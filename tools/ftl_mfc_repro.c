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
 *   --planes N    Planes per die, 1-4 (default 1). Configures the FTL stack
 *                 with a multi-plane NAND topology — raises raw capacity
 *                 proportionally and widens the (ch,chip,die,plane) tuple
 *                 space in cmd_engine, but does NOT trigger the multi-plane
 *                 PROG/ERASE opcode path (that requires plane_mask > 1, which
 *                 only tests/test_media_multi_plane_concurrency.c sets).
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
    /* In dispatcher mode the producer issues writes without waiting for
     * completion, so a same-lba read can race ahead of the prior write
     * and observe the old contents. The producer increments this counter
     * under the stripe lock at submit-time and the reaper decrements it
     * at completion. Reads issued while the counter is non-zero have
     * indeterminate ordering vs the in-flight write and skip CRC verify
     * to avoid false-positive mismatches. Legacy mode never increments
     * this field. */
    uint16_t pending_writes;
    /* Monotonic generation counter, incremented under the stripe lock by
     * every write completion (legacy + dispatcher). Readers snapshot the
     * gen at submit time and re-read it at verify time; any difference
     * means a concurrent write completed during the read window, so the
     * read's data + the current g_crc_map state aren't ordered with each
     * other and verify must be skipped. Eliminates false-positive
     * MISMATCH counts that are bookkeeping artifacts, not FTL bugs. */
    uint32_t gen;
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

/* -------------------------------------------------------------------------
 * Dispatcher-mode inflight tracking. The legacy path stores per-IO state
 * on the submitting thread's stack and waits inline; the dispatcher path
 * fires writes/reads without waiting, so per-IO state has to live on the
 * heap until the reaper sees the matching completion.
 * ------------------------------------------------------------------------- */
struct dispatcher_op {
    uint64_t              handle;
    int                   op_type;        /* IO_OP_READ or IO_OP_WRITE */
    uint32_t              lba;
    uint32_t              stripe;
    uint8_t               *buf;            /* heap wbuf for write, rbuf for read */
    uint32_t              new_crc;         /* write only: CRC committed on success */
    bool                  verify_skipped;  /* read only: pending writes ⇒ skip verify */
    /* Read only: snapshot of g_crc_map[lba].valid taken under stripe lock at
     * submit time. NOENT on a never-written LBA is a workload-warmup artifact
     * (no L2P mapping yet), not a race; only errors with valid_at_submit==true
     * are counted as real signal. */
    bool                  valid_at_submit;
    /* Read only: snapshot of g_crc_map[lba].gen at submit time. Verify
     * compares this against the current gen; a delta means another writer
     * landed during our read window so skip verify to avoid false MISMATCH. */
    uint32_t              gen_at_submit;
    struct dispatcher_op *next;
};

static pthread_mutex_t       g_dispatcher_mu = PTHREAD_MUTEX_INITIALIZER;
static struct dispatcher_op *g_dispatcher_buckets[INFLIGHT_BUCKETS];

/* Log only the first few non-OK statuses verbatim — useful for pinning
 * the failing op type to a tuple — but cap to avoid drowning the log
 * once a race fires repeatedly. Separate caps for read vs write so a
 * burst of NOENT on unwritten LBAs (legitimate, not a race) does not
 * starve the log of any genuine WRITE errors that follow. */
#define ERROR_LOG_CAP_READ  16u
#define ERROR_LOG_CAP_WRITE 64u
static atomic_uint_fast64_t g_logged_read_errors;
static atomic_uint_fast64_t g_logged_write_errors;
static atomic_uint_fast64_t g_write_errors;

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

static void dispatcher_insert(struct dispatcher_op *op)
{
    pthread_mutex_lock(&g_dispatcher_mu);
    op->next = g_dispatcher_buckets[op->handle % INFLIGHT_BUCKETS];
    g_dispatcher_buckets[op->handle % INFLIGHT_BUCKETS] = op;
    pthread_mutex_unlock(&g_dispatcher_mu);
}

static struct dispatcher_op *dispatcher_remove(uint64_t h)
{
    struct dispatcher_op *found = NULL;
    pthread_mutex_lock(&g_dispatcher_mu);
    struct dispatcher_op **pp = &g_dispatcher_buckets[h % INFLIGHT_BUCKETS];
    while (*pp) {
        if ((*pp)->handle == h) {
            found = *pp;
            *pp = found->next;
            found->next = NULL;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_dispatcher_mu);
    return found;
}

/* -------------------------------------------------------------------------
 * Reaper thread: single consumer of ftl_mt_poll_completion. Handles both
 * legacy mode (waiter lookup, signal owning thread) and dispatcher mode
 * (no waiter; reaper performs verify and CRC commit itself).
 * ------------------------------------------------------------------------- */
static volatile bool g_reaper_stop = false;

struct reaper_arg {
    struct ftl_mt_ctx    *mt;
    atomic_uint_fast64_t *total_ops;
    atomic_uint_fast64_t *mismatch_count;
    atomic_uint_fast64_t *error_count;
};

static void reaper_handle_dispatcher_op(struct dispatcher_op *op,
                                        int                   cpl_status,
                                        atomic_uint_fast64_t *total_ops,
                                        atomic_uint_fast64_t *mismatch_count,
                                        atomic_uint_fast64_t *error_count)
{
    if (cpl_status != HFSSS_OK) {
        if (op->op_type == IO_OP_WRITE) {
            atomic_fetch_add(error_count, 1u);
            atomic_fetch_add(&g_write_errors, 1u);
            if (atomic_fetch_add(&g_logged_write_errors, 1u) < ERROR_LOG_CAP_WRITE) {
                fprintf(stderr, "ERR op=WRITE lba=%" PRIu32 " status=%d\n",
                        op->lba, cpl_status);
            }
        } else if (op->valid_at_submit) {
            atomic_fetch_add(error_count, 1u);
            if (atomic_fetch_add(&g_logged_read_errors, 1u) < ERROR_LOG_CAP_READ) {
                fprintf(stderr, "ERR op=READ lba=%" PRIu32 " status=%d\n",
                        op->lba, cpl_status);
            }
        }
        /* else: read NOENT on a never-written LBA — workload-warmup artifact,
         * not a race signal; suppress to keep the baseline meaningful. */
    }
    if (op->op_type == IO_OP_WRITE) {
        pthread_mutex_lock(&g_locks[op->stripe].mu);
        if (cpl_status == HFSSS_OK) {
            g_crc_map[op->lba].crc = op->new_crc;
            g_crc_map[op->lba].valid = true;
            g_crc_map[op->lba].gen++;
        }
        if (g_crc_map[op->lba].pending_writes > 0u) {
            g_crc_map[op->lba].pending_writes--;
        }
        pthread_mutex_unlock(&g_locks[op->stripe].mu);
        if (cpl_status == HFSSS_OK) {
            atomic_fetch_add(total_ops, 1u);
        }
    } else { /* IO_OP_READ */
        if (cpl_status == HFSSS_OK && !op->verify_skipped) {
            pthread_mutex_lock(&g_locks[op->stripe].mu);
            struct crc_entry e = g_crc_map[op->lba];
            pthread_mutex_unlock(&g_locks[op->stripe].mu);
            if (e.valid && e.gen == op->gen_at_submit) {
                uint32_t got = local_crc32c(op->buf, GEO_PGSZ);
                if (got != e.crc) {
                    fprintf(stderr,
                            "MISMATCH lba=%" PRIu32
                            " expected=0x%08" PRIx32
                            " got=0x%08" PRIx32 "\n",
                            op->lba, e.crc, got);
                    atomic_fetch_add(mismatch_count, 1u);
                }
            }
            /* else: gen advanced during read window — concurrent writer
             * landed; data and current crc_map aren't ordered, skip verify. */
        }
        if (cpl_status == HFSSS_OK) {
            atomic_fetch_add(total_ops, 1u);
        }
    }
    free(op->buf);
    free(op);
}

static void *reaper_fn(void *arg)
{
    struct reaper_arg *ra = (struct reaper_arg *)arg;
    struct ftl_mt_ctx *mt = ra->mt;
    struct io_completion cpl;
    while (!g_reaper_stop) {
        if (ftl_mt_poll_completion(mt, &cpl)) {
            /* Try legacy waiter table first; fall through to the
             * dispatcher table if absent. Only one of the two paths is
             * active in any given run. */
            struct waiter *w = inflight_remove(cpl.nbd_handle);
            if (w) {
                pthread_mutex_lock(&w->mu);
                w->status = cpl.status;
                w->completed = true;
                pthread_cond_signal(&w->cv);
                pthread_mutex_unlock(&w->mu);
                continue;
            }
            struct dispatcher_op *op = dispatcher_remove(cpl.nbd_handle);
            if (op) {
                reaper_handle_dispatcher_op(op, cpl.status,
                                            ra->total_ops, ra->mismatch_count,
                                            ra->error_count);
                continue;
            }
            /* Unknown handle — shouldn't happen, skip rather than crash. */
        } else {
            sched_yield();
        }
    }
    /* Drain any completions still in flight at shutdown. */
    while (ftl_mt_poll_completion(mt, &cpl)) {
        struct waiter *w = inflight_remove(cpl.nbd_handle);
        if (w) {
            pthread_mutex_lock(&w->mu);
            w->status = cpl.status;
            w->completed = true;
            pthread_cond_signal(&w->cv);
            pthread_mutex_unlock(&w->mu);
            continue;
        }
        struct dispatcher_op *op = dispatcher_remove(cpl.nbd_handle);
        if (op) {
            reaper_handle_dispatcher_op(op, cpl.status,
                                        ra->total_ops, ra->mismatch_count,
                                        ra->error_count);
        }
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
    atomic_uint_fast64_t     *error_count;
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
            /* Snapshot valid + gen + pending_writes under stripe lock at
             * submit time. The combination filters all race windows:
             *  - pending_at_submit > 0    → write in flight when we issued
             *  - e.pending_writes > 0     → write started after our submit
             *  - e.gen != gen_at_submit   → write completed during our read
             *  - !e.valid                 → never written (warmup artifact)
             * Any of these means the read+crc_map ordering is undefined, so
             * verify is skipped to avoid bookkeeping false-positive MISMATCH. */
            pthread_mutex_lock(&g_locks[stripe].mu);
            bool     valid_at_submit   = g_crc_map[lba].valid;
            uint32_t gen_at_submit     = g_crc_map[lba].gen;
            bool     pending_at_submit = (g_crc_map[lba].pending_writes > 0u);
            pthread_mutex_unlock(&g_locks[stripe].mu);

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
                if (e.valid && e.gen == gen_at_submit &&
                    !pending_at_submit && e.pending_writes == 0u) {
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
            } else if (valid_at_submit) {
                atomic_fetch_add(wa->error_count, 1u);
                if (atomic_fetch_add(&g_logged_read_errors, 1u) < ERROR_LOG_CAP_READ) {
                    fprintf(stderr, "ERR op=READ lba=%" PRIu32 " status=%d\n",
                            lba, status);
                }
            }
        } else {
            memset(wbuf, (uint8_t)(lba & 0xFFu), GEO_PGSZ);
            wbuf[0] = (uint8_t)((lba >> 8) & 0xFFu);
            wbuf[1] = (uint8_t)((lba >> 16) & 0xFFu);
            uint32_t salt = (uint32_t)rand_r(&seed);
            memcpy(wbuf + 4, &salt, sizeof(salt));

            uint32_t new_crc = local_crc32c(wbuf, GEO_PGSZ);

            /* Mark this LBA as having an in-flight write so concurrent
             * readers can skip verify (the read+crc_map ordering is
             * undefined while a write is mid-execution). Decremented at
             * completion under the same lock. */
            pthread_mutex_lock(&g_locks[stripe].mu);
            g_crc_map[lba].pending_writes++;
            pthread_mutex_unlock(&g_locks[stripe].mu);

            struct io_request req;
            memset(&req, 0, sizeof(req));
            req.opcode = IO_OP_WRITE;
            req.lba    = lba;
            req.count  = 1;
            req.data   = wbuf;
            int status = submit_and_wait(mt, &req);
            pthread_mutex_lock(&g_locks[stripe].mu);
            if (status == HFSSS_OK) {
                g_crc_map[lba].crc = new_crc;
                g_crc_map[lba].valid = true;
                g_crc_map[lba].gen++;
            }
            if (g_crc_map[lba].pending_writes > 0u) {
                g_crc_map[lba].pending_writes--;
            }
            pthread_mutex_unlock(&g_locks[stripe].mu);
            if (status == HFSSS_OK) {
                local_ops++;
            } else {
                atomic_fetch_add(wa->error_count, 1u);
                atomic_fetch_add(&g_write_errors, 1u);
                if (atomic_fetch_add(&g_logged_write_errors, 1u) < ERROR_LOG_CAP_WRITE) {
                    fprintf(stderr, "ERR op=WRITE lba=%" PRIu32 " status=%d\n",
                            lba, status);
                }
            }
        }
    }

    atomic_fetch_add(wa->total_ops, local_ops);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Dispatcher producer: fires writes/reads back-to-back, no per-IO wait.
 * Mirrors the NBD async pipeline producer (1 SQ thread; FTL workers fan
 * out via SPSC rings; reaper drains the completion queue).
 * ------------------------------------------------------------------------- */
struct dispatcher_arg {
    struct env           *env;
    uint32_t              total_lbas;
    int                   rwmix;
    volatile bool        *stop;
    unsigned int          seed;
};

static void *dispatcher_producer_fn(void *arg)
{
    struct dispatcher_arg *da  = (struct dispatcher_arg *)arg;
    struct ftl_mt_ctx     *mt  = &da->env->mt;
    unsigned int           seed = da->seed;

    while (!(*da->stop)) {
        uint32_t lba     = (uint32_t)((uint64_t)rand_r(&seed) % da->total_lbas);
        bool     do_read = ((rand_r(&seed) % 100) < da->rwmix);
        uint32_t stripe  = lba >> STRIPE_SHIFT;
        if (stripe >= g_num_stripes) {
            stripe = g_num_stripes - 1u;
        }

        uint64_t handle = (uint64_t)atomic_fetch_add(&g_next_handle, 1u) + 1u;
        struct dispatcher_op *op = (struct dispatcher_op *)calloc(1, sizeof(*op));
        if (!op) {
            sched_yield();
            continue;
        }
        op->handle = handle;
        op->lba    = lba;
        op->stripe = stripe;

        if (do_read) {
            op->op_type = IO_OP_READ;
            op->buf     = (uint8_t *)malloc(GEO_PGSZ);
            if (!op->buf) {
                free(op);
                sched_yield();
                continue;
            }
            pthread_mutex_lock(&g_locks[stripe].mu);
            op->verify_skipped  = (g_crc_map[lba].pending_writes > 0u);
            op->valid_at_submit = g_crc_map[lba].valid;
            op->gen_at_submit   = g_crc_map[lba].gen;
            pthread_mutex_unlock(&g_locks[stripe].mu);

            struct io_request req;
            memset(&req, 0, sizeof(req));
            req.opcode      = IO_OP_READ;
            req.lba         = lba;
            req.count       = 1;
            req.data        = op->buf;
            req.nbd_handle  = handle;
            dispatcher_insert(op);
            uint32_t wid = (uint32_t)(lba % FTL_NUM_WORKERS);
            pthread_mutex_lock(&g_worker_submit_mu[wid]);
            while (!ftl_mt_submit(mt, &req)) {
                pthread_mutex_unlock(&g_worker_submit_mu[wid]);
                sched_yield();
                pthread_mutex_lock(&g_worker_submit_mu[wid]);
            }
            pthread_mutex_unlock(&g_worker_submit_mu[wid]);
        } else {
            op->op_type = IO_OP_WRITE;
            op->buf     = (uint8_t *)malloc(GEO_PGSZ);
            if (!op->buf) {
                free(op);
                sched_yield();
                continue;
            }
            memset(op->buf, (uint8_t)(lba & 0xFFu), GEO_PGSZ);
            op->buf[0] = (uint8_t)((lba >> 8) & 0xFFu);
            op->buf[1] = (uint8_t)((lba >> 16) & 0xFFu);
            uint32_t salt = (uint32_t)rand_r(&seed);
            memcpy(op->buf + 4, &salt, sizeof(salt));
            op->new_crc = local_crc32c(op->buf, GEO_PGSZ);

            pthread_mutex_lock(&g_locks[stripe].mu);
            g_crc_map[lba].pending_writes++;
            pthread_mutex_unlock(&g_locks[stripe].mu);

            struct io_request req;
            memset(&req, 0, sizeof(req));
            req.opcode      = IO_OP_WRITE;
            req.lba         = lba;
            req.count       = 1;
            req.data        = op->buf;
            req.nbd_handle  = handle;
            dispatcher_insert(op);
            uint32_t wid = (uint32_t)(lba % FTL_NUM_WORKERS);
            pthread_mutex_lock(&g_worker_submit_mu[wid]);
            while (!ftl_mt_submit(mt, &req)) {
                pthread_mutex_unlock(&g_worker_submit_mu[wid]);
                sched_yield();
                pthread_mutex_lock(&g_worker_submit_mu[wid]);
            }
            pthread_mutex_unlock(&g_worker_submit_mu[wid]);
        }
    }
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
 * Reproducer mode — legacy uses N independent submitters each waiting
 * inline; dispatcher uses 1 producer firing fire-and-forget through the
 * NBD-style (1 SQ + N FTL workers + 1 reaper) topology.
 * ------------------------------------------------------------------------- */
enum repro_mode {
    REPRO_MODE_LEGACY = 0,
    REPRO_MODE_DISPATCHER,
};

/* -------------------------------------------------------------------------
 * CLI argument parsing
 * ------------------------------------------------------------------------- */
static void parse_args(int argc, char **argv,
                       int *threads, int *rwmix, int *duration,
                       uint32_t *lbas, uint32_t *planes,
                       enum repro_mode *mode)
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
        } else if (strcmp(argv[i], "--mode") == 0) {
            if (strcmp(argv[i + 1], "legacy") == 0) {
                *mode = REPRO_MODE_LEGACY;
            } else if (strcmp(argv[i + 1], "dispatcher") == 0) {
                *mode = REPRO_MODE_DISPATCHER;
            } else {
                fprintf(stderr, "error: --mode must be 'legacy' or 'dispatcher'\n");
                exit(1);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    int             threads  = DEFAULT_THREADS;
    int             rwmix    = DEFAULT_RWMIX;
    int             duration = DEFAULT_DURATION;
    uint32_t        lbas     = DEFAULT_LBAS;
    uint32_t        planes   = GEO_PLANE_DEFAULT;
    enum repro_mode mode     = REPRO_MODE_LEGACY;

    parse_args(argc, argv, &threads, &rwmix, &duration, &lbas, &planes, &mode);

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

    printf("ftl_mfc_repro: mode=%s threads=%d rwmix=%d%% duration=%ds lbas=%u planes=%u\n",
           (mode == REPRO_MODE_DISPATCHER) ? "dispatcher" : "legacy",
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

    atomic_uint_fast64_t total_ops      = 0;
    atomic_uint_fast64_t mismatch_count = 0;
    atomic_uint_fast64_t error_count    = 0;
    volatile bool        stop_flag      = false;

    struct reaper_arg ra = {
        .mt             = &e->mt,
        .total_ops      = &total_ops,
        .mismatch_count = &mismatch_count,
        .error_count    = &error_count,
    };
    pthread_t reaper_tid;
    if (pthread_create(&reaper_tid, NULL, reaper_fn, &ra) != 0) {
        fprintf(stderr, "error: pthread_create reaper failed\n");
        ftl_mt_stop(&e->mt);
        env_cleanup(e);
        free(e);
        free(g_crc_map);
        free(g_locks);
        return 1;
    }

    pthread_t            *tids   = NULL;
    struct worker_arg    *was    = NULL;
    pthread_t             producer_tid = 0;
    struct dispatcher_arg da_arg;
    bool                  producer_started = false;

    if (mode == REPRO_MODE_LEGACY) {
        tids = (pthread_t *)calloc((size_t)threads, sizeof(pthread_t));
        was  = (struct worker_arg *)calloc((size_t)threads, sizeof(*was));
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
            was[i].error_count    = &error_count;
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
    } else {
        /* Dispatcher: 1 producer, --threads ignored on the harness side
         * (FTL_NUM_WORKERS workers fan out behind ftl_mt internally). */
        da_arg.env        = e;
        da_arg.total_lbas = lbas;
        da_arg.rwmix      = rwmix;
        da_arg.stop       = &stop_flag;
        da_arg.seed       = 12345u;
        if (pthread_create(&producer_tid, NULL,
                           dispatcher_producer_fn, &da_arg) != 0) {
            fprintf(stderr, "error: pthread_create producer failed\n");
            g_reaper_stop = true;
            pthread_join(reaper_tid, NULL);
            ftl_mt_stop(&e->mt);
            env_cleanup(e);
            free(e);
            free(g_crc_map);
            free(g_locks);
            return 1;
        }
        producer_started = true;
    }

    sleep((unsigned int)duration);
    stop_flag = true;

    if (mode == REPRO_MODE_LEGACY) {
        for (int i = 0; i < threads; i++) {
            pthread_join(tids[i], NULL);
        }
    } else if (producer_started) {
        pthread_join(producer_tid, NULL);
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
    uint64_t errs = (uint64_t)atomic_load(&error_count);
    double   rate = (ops > 0u) ? ((double)mmis / (double)ops) : 0.0;
    double   err_rate = (ops + errs > 0u)
                        ? ((double)errs / (double)(ops + errs)) : 0.0;

    uint64_t werrs = (uint64_t)atomic_load(&g_write_errors);
    uint64_t rerrs = errs - werrs;
    printf("ops=%" PRIu64 " mismatches=%" PRIu64
           " errors=%" PRIu64 " (write=%" PRIu64 " read=%" PRIu64 ")"
           " mmis_rate=%.2e err_rate=%.2e\n",
           ops, mmis, errs, werrs, rerrs, rate, err_rate);

    free(tids);
    free(was);
    free(g_crc_map);
    free(g_locks);
    free(e);

    return (mmis == 0u && errs == 0u) ? 0 : 2;
}
