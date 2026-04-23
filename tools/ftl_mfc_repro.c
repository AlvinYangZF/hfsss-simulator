/*
 * ftl_mfc_repro.c — Multi-threaded FTL load generator with data-integrity
 * verification.  Drives the FTL API directly (no NBD, no QEMU, no fio) using
 * MFC-equivalent workload parameters and validates every written sector with
 * a CRC32C comparison on read-back.
 *
 * Concurrency model:
 *   - N worker threads each submit one I/O and block on their own condvar
 *     waiting for the matching completion.
 *   - Submission takes a per-worker-ring mutex, so up to FTL_NUM_WORKERS
 *     submissions proceed in parallel (matching the ring-index partition of
 *     LBA % FTL_NUM_WORKERS used by ftl_mt_submit).
 *   - A dedicated reaper thread is the sole caller of ftl_mt_poll_completion.
 *     It looks up the completion's nbd_handle in a shared inflight table and
 *     wakes the owning worker's condvar.
 *   - This keeps ftl_mt_submit / ftl_mt_poll_completion SPSC-correct while
 *     exposing real concurrency to the FTL worker path.
 *
 * Usage:
 *   ftl_mfc_repro [--threads N] [--rwmix N] [--duration N] [--lbas N]
 *                 [--planes N]
 *
 *   --threads N   Worker thread count (default 64)
 *   --rwmix  N    Read percentage 0-100 (default 70)
 *   --duration N  Run duration in seconds (default 120)
 *   --lbas   N    Total LBA space (default 131072 = 512 MiB @ 4K)
 *   --planes N    Planes per die, 1-4 (default 1). Exercises the multi-plane
 *                 geometry added in PR #99 (REQ-042) — raises raw capacity
 *                 proportionally.
 *
 * Exit code: 0 if mismatches == 0, 2 otherwise.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>

#include "media/media.h"
#include "hal/hal.h"
#include "ftl/ftl_worker.h"

/* -------------------------------------------------------------------------
 * Geometry — scaled up from test_mt_ftl.c to hold 512 MiB working set:
 * 4ch * 4chip * 2die * 1plane * 64blk * 64pg * 4096B = 1 GiB raw capacity.
 * ------------------------------------------------------------------------- */
#define GEO_CH    4
#define GEO_CHIP  4
#define GEO_DIE   2
#define GEO_PLANE_DEFAULT 1
#define GEO_PLANE_MAX     4
#define GEO_BLKS  64
#define GEO_PGS   64
#define GEO_PGSZ  4096

/* Stripe-lock granularity: one mutex per 1024 LBAs */
#define STRIPE_SHIFT 10u
#define STRIPE_SIZE  (1u << STRIPE_SHIFT)
#define MAX_STRIPES  4096u

/* Default CLI parameters */
#define DEFAULT_THREADS  64
#define DEFAULT_RWMIX    70
#define DEFAULT_DURATION 120
#define DEFAULT_LBAS     131072u

/* Inflight hash table sizing */
#define INFLIGHT_BUCKETS 1024u

/* -------------------------------------------------------------------------
 * Local CRC32C — Castagnoli polynomial.  Table is built exactly once via
 * pthread_once so the lazy init is race-free across worker threads.
 * ------------------------------------------------------------------------- */
#define CRC32C_POLY 0x82F63B78U
static uint32_t crc_table[256];
static pthread_once_t crc_table_once = PTHREAD_ONCE_INIT;

static void crc_table_init(void)
{
    for (int n = 0; n < 256; n++) {
        uint32_t c = (uint32_t)n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? (CRC32C_POLY ^ (c >> 1)) : (c >> 1);
        }
        crc_table[n] = c;
    }
}

static uint32_t local_crc32c(const void *buf, size_t len)
{
    pthread_once(&crc_table_once, crc_table_init);
    uint32_t crc = 0xFFFFFFFFU;
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++) {
        crc = crc_table[(crc ^ p[i]) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

/* -------------------------------------------------------------------------
 * CRC map — per-LBA expected CRC + explicit "valid" bit so CRC==0 is not
 * mistaken for "never written".
 * ------------------------------------------------------------------------- */
struct crc_entry {
    uint32_t crc;
    bool     valid;
};

static struct crc_entry *g_crc_map;
struct stripe_lock {
    pthread_mutex_t mu;
};
static struct stripe_lock *g_locks;
static uint32_t            g_num_stripes;

/* -------------------------------------------------------------------------
 * Inflight-request correlation: each submission gets a unique nbd_handle
 * and a per-submission waiter.  The reaper thread looks up the waiter by
 * nbd_handle and signals it when the completion arrives.
 * ------------------------------------------------------------------------- */
struct waiter {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    bool            completed;
    int             status;
};

struct inflight_slot {
    uint64_t              handle;
    struct waiter        *w;
    struct inflight_slot *next;
};

static atomic_uint_fast64_t g_next_handle;
static pthread_mutex_t      g_inflight_mu = PTHREAD_MUTEX_INITIALIZER;
static struct inflight_slot *g_inflight_buckets[INFLIGHT_BUCKETS];

/* Per-worker producer lock: ftl_mt_submit's ring is SPSC, so even though
 * ftl_mt_submit is reentrant across different wids, two callers for the
 * same wid must serialize.  Using 1 lock per worker preserves
 * FTL_NUM_WORKERS-way parallelism on the producer side. */
static pthread_mutex_t g_worker_submit_mu[FTL_NUM_WORKERS];

static void inflight_insert(uint64_t h, struct waiter *w)
{
    struct inflight_slot *s = (struct inflight_slot *)malloc(sizeof(*s));
    if (!s) abort();
    s->handle = h;
    s->w = w;
    pthread_mutex_lock(&g_inflight_mu);
    s->next = g_inflight_buckets[h % INFLIGHT_BUCKETS];
    g_inflight_buckets[h % INFLIGHT_BUCKETS] = s;
    pthread_mutex_unlock(&g_inflight_mu);
}

static struct waiter *inflight_remove(uint64_t h)
{
    struct waiter *w = NULL;
    pthread_mutex_lock(&g_inflight_mu);
    struct inflight_slot **pp = &g_inflight_buckets[h % INFLIGHT_BUCKETS];
    while (*pp) {
        if ((*pp)->handle == h) {
            struct inflight_slot *s = *pp;
            w = s->w;
            *pp = s->next;
            free(s);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_inflight_mu);
    return w;
}

/* -------------------------------------------------------------------------
 * Reaper thread: single consumer of ftl_mt_poll_completion.
 * ------------------------------------------------------------------------- */
static volatile bool g_reaper_stop = false;

static void *reaper_fn(void *arg)
{
    struct ftl_mt_ctx *mt = (struct ftl_mt_ctx *)arg;
    struct io_completion cpl;
    while (!g_reaper_stop) {
        if (ftl_mt_poll_completion(mt, &cpl)) {
            struct waiter *w = inflight_remove(cpl.nbd_handle);
            if (!w) {
                /* Completion arrived for an unknown handle — shouldn't
                 * happen under this harness, but skip rather than crash. */
                continue;
            }
            pthread_mutex_lock(&w->mu);
            w->status = cpl.status;
            w->completed = true;
            pthread_cond_signal(&w->cv);
            pthread_mutex_unlock(&w->mu);
        } else {
            sched_yield();
        }
    }
    /* Drain any completions still in flight at shutdown. */
    while (ftl_mt_poll_completion(mt, &cpl)) {
        struct waiter *w = inflight_remove(cpl.nbd_handle);
        if (!w) continue;
        pthread_mutex_lock(&w->mu);
        w->status = cpl.status;
        w->completed = true;
        pthread_cond_signal(&w->cv);
        pthread_mutex_unlock(&w->mu);
    }
    return NULL;
}

/* Submit one request and block on its completion.  Returns cpl.status. */
static int submit_and_wait(struct ftl_mt_ctx *mt, struct io_request *req)
{
    struct waiter w;
    pthread_mutex_init(&w.mu, NULL);
    pthread_cond_init(&w.cv, NULL);
    w.completed = false;
    w.status = 0;

    uint64_t h = atomic_fetch_add(&g_next_handle, 1) + 1u;
    req->nbd_handle = h;
    inflight_insert(h, &w);

    uint32_t wid = (uint32_t)(req->lba % FTL_NUM_WORKERS);
    pthread_mutex_lock(&g_worker_submit_mu[wid]);
    while (!ftl_mt_submit(mt, req)) {
        pthread_mutex_unlock(&g_worker_submit_mu[wid]);
        sched_yield();
        pthread_mutex_lock(&g_worker_submit_mu[wid]);
    }
    pthread_mutex_unlock(&g_worker_submit_mu[wid]);

    pthread_mutex_lock(&w.mu);
    while (!w.completed) {
        pthread_cond_wait(&w.cv, &w.mu);
    }
    pthread_mutex_unlock(&w.mu);

    int status = w.status;
    pthread_mutex_destroy(&w.mu);
    pthread_cond_destroy(&w.cv);
    return status;
}

/* -------------------------------------------------------------------------
 * FTL environment
 * ------------------------------------------------------------------------- */
struct env {
    struct media_ctx    media;
    struct hal_nand_dev nand;
    struct hal_ctx      hal;
    struct ftl_mt_ctx   mt;
};

/* Worker thread arguments */
struct worker_arg {
    struct env               *env;
    uint32_t                  total_lbas;
    int                       rwmix;
    volatile bool            *stop;
    atomic_uint_fast64_t     *total_ops;
    atomic_uint_fast64_t     *mismatch_count;
    unsigned int              seed;
};

/* -------------------------------------------------------------------------
 * Worker thread body
 * ------------------------------------------------------------------------- */
static void *worker_fn(void *arg)
{
    struct worker_arg *wa = (struct worker_arg *)arg;
    struct ftl_mt_ctx *mt = &wa->env->mt;
    unsigned int       seed = wa->seed;
    uint8_t            wbuf[GEO_PGSZ];
    uint8_t            rbuf[GEO_PGSZ];
    uint64_t           local_ops = 0;

    while (!(*wa->stop)) {
        uint32_t lba     = (uint32_t)((uint64_t)rand_r(&seed) % wa->total_lbas);
        bool     do_read = ((rand_r(&seed) % 100) < wa->rwmix);
        uint32_t stripe  = lba >> STRIPE_SHIFT;
        if (stripe >= g_num_stripes) {
            stripe = g_num_stripes - 1u;
        }

        if (do_read) {
            struct io_request req;
            memset(&req, 0, sizeof(req));
            req.opcode = IO_OP_READ;
            req.lba    = lba;
            req.count  = 1;
            req.data   = rbuf;
            int status = submit_and_wait(mt, &req);
            if (status == HFSSS_OK) {
                pthread_mutex_lock(&g_locks[stripe].mu);
                struct crc_entry e = g_crc_map[lba];
                pthread_mutex_unlock(&g_locks[stripe].mu);
                if (e.valid) {
                    uint32_t got = local_crc32c(rbuf, GEO_PGSZ);
                    if (got != e.crc) {
                        fprintf(stderr,
                                "MISMATCH lba=%" PRIu32
                                " expected=0x%08" PRIx32
                                " got=0x%08" PRIx32 "\n",
                                lba, e.crc, got);
                        atomic_fetch_add(wa->mismatch_count, 1u);
                    }
                }
                local_ops++;
            }
        } else {
            memset(wbuf, (uint8_t)(lba & 0xFFu), GEO_PGSZ);
            wbuf[0] = (uint8_t)((lba >> 8) & 0xFFu);
            wbuf[1] = (uint8_t)((lba >> 16) & 0xFFu);
            uint32_t salt = (uint32_t)rand_r(&seed);
            memcpy(wbuf + 4, &salt, sizeof(salt));

            uint32_t new_crc = local_crc32c(wbuf, GEO_PGSZ);

            struct io_request req;
            memset(&req, 0, sizeof(req));
            req.opcode = IO_OP_WRITE;
            req.lba    = lba;
            req.count  = 1;
            req.data   = wbuf;
            int status = submit_and_wait(mt, &req);
            if (status == HFSSS_OK) {
                pthread_mutex_lock(&g_locks[stripe].mu);
                g_crc_map[lba].crc = new_crc;
                g_crc_map[lba].valid = true;
                pthread_mutex_unlock(&g_locks[stripe].mu);
                local_ops++;
            }
        }
    }

    atomic_fetch_add(wa->total_ops, local_ops);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Environment init/cleanup
 * ------------------------------------------------------------------------- */
static int env_init(struct env *e, uint32_t total_lbas, uint32_t planes)
{
    struct media_config mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.channel_count     = GEO_CH;
    mcfg.chips_per_channel = GEO_CHIP;
    mcfg.dies_per_chip     = GEO_DIE;
    mcfg.planes_per_die    = planes;
    mcfg.blocks_per_plane  = GEO_BLKS;
    mcfg.pages_per_block   = GEO_PGS;
    mcfg.page_size         = GEO_PGSZ;
    mcfg.spare_size        = 64;
    mcfg.nand_type         = NAND_TYPE_TLC;

    int ret = media_init(&e->media, &mcfg);
    if (ret != HFSSS_OK) {
        fprintf(stderr, "media_init failed: %d\n", ret);
        return ret;
    }

    ret = hal_nand_dev_init(&e->nand,
                            GEO_CH, GEO_CHIP, GEO_DIE, planes,
                            GEO_BLKS, GEO_PGS, GEO_PGSZ, 64, &e->media);
    if (ret != HFSSS_OK) {
        fprintf(stderr, "hal_nand_dev_init failed: %d\n", ret);
        media_cleanup(&e->media);
        return ret;
    }

    ret = hal_init(&e->hal, &e->nand);
    if (ret != HFSSS_OK) {
        fprintf(stderr, "hal_init failed: %d\n", ret);
        hal_nand_dev_cleanup(&e->nand);
        media_cleanup(&e->media);
        return ret;
    }

    struct ftl_config fcfg;
    memset(&fcfg, 0, sizeof(fcfg));
    fcfg.channel_count     = GEO_CH;
    fcfg.chips_per_channel = GEO_CHIP;
    fcfg.dies_per_chip     = GEO_DIE;
    fcfg.planes_per_die    = planes;
    fcfg.blocks_per_plane  = GEO_BLKS;
    fcfg.pages_per_block   = GEO_PGS;
    fcfg.page_size         = GEO_PGSZ;
    fcfg.total_lbas        = total_lbas;
    fcfg.op_ratio          = 20;
    fcfg.gc_policy         = GC_POLICY_GREEDY;
    fcfg.gc_threshold      = 5;
    fcfg.gc_hiwater        = 10;
    fcfg.gc_lowater        = 3;

    ret = ftl_mt_init(&e->mt, &fcfg, &e->hal);
    if (ret != HFSSS_OK) {
        fprintf(stderr, "ftl_mt_init failed: %d\n", ret);
        hal_cleanup(&e->hal);
        hal_nand_dev_cleanup(&e->nand);
        media_cleanup(&e->media);
        return ret;
    }

    return HFSSS_OK;
}

static void env_cleanup(struct env *e)
{
    ftl_mt_cleanup(&e->mt);
    hal_cleanup(&e->hal);
    hal_nand_dev_cleanup(&e->nand);
    media_cleanup(&e->media);
}

/* -------------------------------------------------------------------------
 * CLI argument parsing
 * ------------------------------------------------------------------------- */
static void parse_args(int argc, char **argv,
                       int *threads, int *rwmix, int *duration,
                       uint32_t *lbas, uint32_t *planes)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--threads") == 0) {
            *threads = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--rwmix") == 0) {
            *rwmix = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--duration") == 0) {
            *duration = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--lbas") == 0) {
            *lbas = (uint32_t)atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--planes") == 0) {
            *planes = (uint32_t)atoi(argv[i + 1]);
        }
    }
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    int      threads  = DEFAULT_THREADS;
    int      rwmix    = DEFAULT_RWMIX;
    int      duration = DEFAULT_DURATION;
    uint32_t lbas     = DEFAULT_LBAS;
    uint32_t planes   = GEO_PLANE_DEFAULT;

    parse_args(argc, argv, &threads, &rwmix, &duration, &lbas, &planes);

    if (threads < 1 || threads > 4096) {
        fprintf(stderr, "error: --threads must be 1-4096\n");
        return 1;
    }
    if (rwmix < 0 || rwmix > 100) {
        fprintf(stderr, "error: --rwmix must be 0-100\n");
        return 1;
    }
    if (duration < 1) {
        fprintf(stderr, "error: --duration must be >= 1\n");
        return 1;
    }
    if (lbas < 1u) {
        fprintf(stderr, "error: --lbas must be >= 1\n");
        return 1;
    }
    if (planes < 1u || planes > GEO_PLANE_MAX) {
        fprintf(stderr, "error: --planes must be 1-%u\n", GEO_PLANE_MAX);
        return 1;
    }

    printf("ftl_mfc_repro: threads=%d rwmix=%d%% duration=%ds lbas=%u planes=%u\n",
           threads, rwmix, duration, lbas, planes);

    g_crc_map = (struct crc_entry *)calloc(lbas, sizeof(struct crc_entry));
    if (!g_crc_map) {
        fprintf(stderr, "error: calloc crc_map failed\n");
        return 1;
    }

    g_num_stripes = (lbas + STRIPE_SIZE - 1u) / STRIPE_SIZE;
    if (g_num_stripes > MAX_STRIPES) {
        g_num_stripes = MAX_STRIPES;
    }
    g_locks = (struct stripe_lock *)calloc(g_num_stripes, sizeof(*g_locks));
    if (!g_locks) {
        fprintf(stderr, "error: calloc stripe_locks failed\n");
        free(g_crc_map);
        return 1;
    }
    for (uint32_t i = 0; i < g_num_stripes; i++) {
        pthread_mutex_init(&g_locks[i].mu, NULL);
    }

    for (int i = 0; i < FTL_NUM_WORKERS; i++) {
        pthread_mutex_init(&g_worker_submit_mu[i], NULL);
    }

    struct env *e = (struct env *)calloc(1, sizeof(*e));
    if (!e) {
        fprintf(stderr, "error: calloc env failed\n");
        free(g_crc_map);
        free(g_locks);
        return 1;
    }

    int ret = env_init(e, lbas, planes);
    if (ret != HFSSS_OK) {
        free(e);
        free(g_crc_map);
        free(g_locks);
        return 1;
    }

    ret = ftl_mt_start(&e->mt);
    if (ret != HFSSS_OK) {
        fprintf(stderr, "error: ftl_mt_start failed: %d\n", ret);
        env_cleanup(e);
        free(e);
        free(g_crc_map);
        free(g_locks);
        return 1;
    }

    pthread_t reaper_tid;
    if (pthread_create(&reaper_tid, NULL, reaper_fn, &e->mt) != 0) {
        fprintf(stderr, "error: pthread_create reaper failed\n");
        ftl_mt_stop(&e->mt);
        env_cleanup(e);
        free(e);
        free(g_crc_map);
        free(g_locks);
        return 1;
    }

    atomic_uint_fast64_t total_ops      = 0;
    atomic_uint_fast64_t mismatch_count = 0;
    volatile bool        stop_flag      = false;

    pthread_t         *tids = (pthread_t *)calloc((size_t)threads, sizeof(pthread_t));
    struct worker_arg *was  = (struct worker_arg *)calloc((size_t)threads,
                                                           sizeof(*was));
    if (!tids || !was) {
        fprintf(stderr, "error: calloc thread arrays failed\n");
        g_reaper_stop = true;
        pthread_join(reaper_tid, NULL);
        ftl_mt_stop(&e->mt);
        env_cleanup(e);
        free(e);
        free(g_crc_map);
        free(g_locks);
        free(tids);
        free(was);
        return 1;
    }

    for (int i = 0; i < threads; i++) {
        was[i].env            = e;
        was[i].total_lbas     = lbas;
        was[i].rwmix          = rwmix;
        was[i].stop           = &stop_flag;
        was[i].total_ops      = &total_ops;
        was[i].mismatch_count = &mismatch_count;
        was[i].seed           = (unsigned int)(12345u + (unsigned int)i);

        if (pthread_create(&tids[i], NULL, worker_fn, &was[i]) != 0) {
            fprintf(stderr, "error: pthread_create failed for thread %d\n", i);
            stop_flag = true;
            for (int j = 0; j < i; j++) {
                pthread_join(tids[j], NULL);
            }
            g_reaper_stop = true;
            pthread_join(reaper_tid, NULL);
            ftl_mt_stop(&e->mt);
            env_cleanup(e);
            free(e);
            free(g_crc_map);
            free(g_locks);
            free(tids);
            free(was);
            return 1;
        }
    }

    sleep((unsigned int)duration);
    stop_flag = true;

    for (int i = 0; i < threads; i++) {
        pthread_join(tids[i], NULL);
    }

    g_reaper_stop = true;
    pthread_join(reaper_tid, NULL);

    ftl_mt_stop(&e->mt);
    env_cleanup(e);

    for (uint32_t i = 0; i < g_num_stripes; i++) {
        pthread_mutex_destroy(&g_locks[i].mu);
    }
    for (int i = 0; i < FTL_NUM_WORKERS; i++) {
        pthread_mutex_destroy(&g_worker_submit_mu[i]);
    }

    uint64_t ops  = (uint64_t)atomic_load(&total_ops);
    uint64_t mmis = (uint64_t)atomic_load(&mismatch_count);
    double   rate = (ops > 0u) ? ((double)mmis / (double)ops) : 0.0;

    printf("ops=%" PRIu64 " mismatches=%" PRIu64 " rate=%.2e\n",
           ops, mmis, rate);

    free(tids);
    free(was);
    free(g_crc_map);
    free(g_locks);
    free(e);

    return (mmis == 0u) ? 0 : 2;
}
