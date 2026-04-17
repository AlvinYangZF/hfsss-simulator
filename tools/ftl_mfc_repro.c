/*
 * ftl_mfc_repro.c — Multi-threaded FTL load generator with data-integrity
 * verification.  Drives the FTL API directly (no NBD, no QEMU, no fio) using
 * MFC-equivalent workload parameters and validates every written sector with
 * a CRC32C comparison on read-back.
 *
 * Usage:
 *   ftl_mfc_repro [--threads N] [--rwmix N] [--duration N] [--lbas N]
 *
 *   --threads N   Worker thread count (default 64)
 *   --rwmix  N    Read percentage 0-100 (default 70)
 *   --duration N  Run duration in seconds (default 120)
 *   --lbas   N    Total LBA space (default 131072 = 512 MiB @ 4K)
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
#define GEO_PLANE 1
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

/* -------------------------------------------------------------------------
 * Local CRC32C — Castagnoli polynomial.  Table is built at first call so
 * there are no large static initializers that could contain typos.
 * Always compiled in so the tool never depends on HFSSS_DEBUG_TRACE.
 * ------------------------------------------------------------------------- */
static uint32_t local_crc32c(const void *buf, size_t len)
{
    /* Castagnoli reflected polynomial */
#define CRC32C_POLY 0x82F63B78U
    static uint32_t table[256];
    static int      table_ready = 0;

    if (!table_ready) {
        for (int n = 0; n < 256; n++) {
            uint32_t c = (uint32_t)n;
            for (int k = 0; k < 8; k++) {
                c = (c & 1u) ? (CRC32C_POLY ^ (c >> 1)) : (c >> 1);
            }
            table[n] = c;
        }
        table_ready = 1;
    }
#undef CRC32C_POLY

    uint32_t crc = 0xFFFFFFFFU;
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ p[i]) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

/* -------------------------------------------------------------------------
 * Shared state
 * ------------------------------------------------------------------------- */

/* Cache-line padded stripe lock */
struct stripe_lock {
    pthread_mutex_t mu;
};

/* CRC map: entry 0 means "never written" */
static volatile uint32_t *g_crc_map;
static struct stripe_lock *g_locks;
static uint32_t            g_num_stripes;

/*
 * ftl_mt_submit/poll_completion use per-worker SPSC rings: only one caller
 * thread may submit/poll at a time.  A single global mutex serializes all
 * submit+poll pairs so the SPSC contract is upheld.
 */
static pthread_mutex_t g_dispatch_mu = PTHREAD_MUTEX_INITIALIZER;

/* FTL environment */
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
            req.opcode     = IO_OP_READ;
            req.lba        = lba;
            req.count      = 1;
            req.data       = rbuf;
            req.nbd_handle = (uint64_t)lba;

            struct io_completion cpl;
            pthread_mutex_lock(&g_dispatch_mu);
            while (!ftl_mt_submit(mt, &req)) {
                sched_yield();
            }
            while (!ftl_mt_poll_completion(mt, &cpl)) {
                sched_yield();
            }
            pthread_mutex_unlock(&g_dispatch_mu);

            if (cpl.status == HFSSS_OK) {
                pthread_mutex_lock(&g_locks[stripe].mu);
                uint32_t expected = g_crc_map[lba];
                pthread_mutex_unlock(&g_locks[stripe].mu);

                if (expected != 0u) {
                    uint32_t got = local_crc32c(rbuf, GEO_PGSZ);
                    if (got != expected) {
                        fprintf(stderr,
                                "MISMATCH lba=%" PRIu32
                                " expected=0x%08" PRIx32
                                " got=0x%08" PRIx32 "\n",
                                lba, expected, got);
                        atomic_fetch_add(wa->mismatch_count, 1u);
                    }
                }
                local_ops++;
            }
        } else {
            /* Build a unique pattern per write */
            memset(wbuf, (uint8_t)(lba & 0xFFu), GEO_PGSZ);
            wbuf[0] = (uint8_t)((lba >> 8) & 0xFFu);
            wbuf[1] = (uint8_t)((lba >> 16) & 0xFFu);
            uint32_t salt = (uint32_t)rand_r(&seed);
            memcpy(wbuf + 4, &salt, sizeof(salt));

            uint32_t new_crc = local_crc32c(wbuf, GEO_PGSZ);

            struct io_request req;
            memset(&req, 0, sizeof(req));
            req.opcode     = IO_OP_WRITE;
            req.lba        = lba;
            req.count      = 1;
            req.data       = wbuf;
            req.nbd_handle = (uint64_t)lba;

            struct io_completion cpl;
            pthread_mutex_lock(&g_dispatch_mu);
            while (!ftl_mt_submit(mt, &req)) {
                sched_yield();
            }
            while (!ftl_mt_poll_completion(mt, &cpl)) {
                sched_yield();
            }
            pthread_mutex_unlock(&g_dispatch_mu);

            if (cpl.status == HFSSS_OK) {
                pthread_mutex_lock(&g_locks[stripe].mu);
                g_crc_map[lba] = new_crc;
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
static int env_init(struct env *e, uint32_t total_lbas)
{
    struct media_config mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.channel_count     = GEO_CH;
    mcfg.chips_per_channel = GEO_CHIP;
    mcfg.dies_per_chip     = GEO_DIE;
    mcfg.planes_per_die    = GEO_PLANE;
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
                            GEO_CH, GEO_CHIP, GEO_DIE, GEO_PLANE,
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
    fcfg.planes_per_die    = GEO_PLANE;
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
                       uint32_t *lbas)
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

    parse_args(argc, argv, &threads, &rwmix, &duration, &lbas);

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

    printf("ftl_mfc_repro: threads=%d rwmix=%d%% duration=%ds lbas=%u\n",
           threads, rwmix, duration, lbas);

    /* Allocate CRC map */
    g_crc_map = (volatile uint32_t *)calloc(lbas, sizeof(uint32_t));
    if (!g_crc_map) {
        fprintf(stderr, "error: calloc crc_map failed\n");
        return 1;
    }

    /* Allocate stripe locks */
    g_num_stripes = (lbas + STRIPE_SIZE - 1u) / STRIPE_SIZE;
    if (g_num_stripes > MAX_STRIPES) {
        g_num_stripes = MAX_STRIPES;
    }
    g_locks = (struct stripe_lock *)calloc(g_num_stripes, sizeof(*g_locks));
    if (!g_locks) {
        fprintf(stderr, "error: calloc stripe_locks failed\n");
        free((void *)g_crc_map);
        return 1;
    }
    for (uint32_t i = 0; i < g_num_stripes; i++) {
        pthread_mutex_init(&g_locks[i].mu, NULL);
    }

    /* Init FTL environment */
    struct env *e = (struct env *)calloc(1, sizeof(*e));
    if (!e) {
        fprintf(stderr, "error: calloc env failed\n");
        free((void *)g_crc_map);
        free(g_locks);
        return 1;
    }

    int ret = env_init(e, lbas);
    if (ret != HFSSS_OK) {
        free(e);
        free((void *)g_crc_map);
        free(g_locks);
        return 1;
    }

    ret = ftl_mt_start(&e->mt);
    if (ret != HFSSS_OK) {
        fprintf(stderr, "error: ftl_mt_start failed: %d\n", ret);
        env_cleanup(e);
        free(e);
        free((void *)g_crc_map);
        free(g_locks);
        return 1;
    }

    /* Launch worker threads */
    atomic_uint_fast64_t total_ops      = 0;
    atomic_uint_fast64_t mismatch_count = 0;
    volatile bool        stop_flag      = false;

    pthread_t        *tids = (pthread_t *)calloc((size_t)threads, sizeof(pthread_t));
    struct worker_arg *was = (struct worker_arg *)calloc((size_t)threads,
                                                          sizeof(*was));
    if (!tids || !was) {
        fprintf(stderr, "error: calloc thread arrays failed\n");
        ftl_mt_stop(&e->mt);
        env_cleanup(e);
        free(e);
        free((void *)g_crc_map);
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
            ftl_mt_stop(&e->mt);
            env_cleanup(e);
            free(e);
            free((void *)g_crc_map);
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

    ftl_mt_stop(&e->mt);
    env_cleanup(e);

    for (uint32_t i = 0; i < g_num_stripes; i++) {
        pthread_mutex_destroy(&g_locks[i].mu);
    }

    uint64_t ops  = (uint64_t)atomic_load(&total_ops);
    uint64_t mmis = (uint64_t)atomic_load(&mismatch_count);
    double   rate = (ops > 0u) ? ((double)mmis / (double)ops) : 0.0;

    printf("ops=%" PRIu64 " mismatches=%" PRIu64 " rate=%.2e\n",
           ops, mmis, rate);

    free(tids);
    free(was);
    free((void *)g_crc_map);
    free(g_locks);
    free(e);

    return (mmis == 0u) ? 0 : 2;
}
