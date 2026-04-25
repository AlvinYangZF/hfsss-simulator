/*
 * bench_channel_worker_cq.c — REQ-045 tier-2 controlled CQ latency bench.
 *
 * Spawns one channel_worker per channel with the opt-in completion queue
 * enabled, drives a mixed read/program workload from a per-channel
 * producer thread, and drains the CQ from a per-channel consumer thread
 * that records (complete_ts_ns - submit_ts_ns) for every popped command.
 * After all channels finish, the harness aggregates samples across
 * channels and reports count, min/max, mean, p50, p99, p99.9 plus
 * wall-clock throughput.
 *
 * Workload mix: 70% READ / 30% PROGRAM. Programs cycle through a fixed
 * range of pre-erased blocks so reads always land on the most-recent
 * programmed page within the same channel. Once a block is fully
 * programmed, it is re-erased and reused. ERASE submissions are
 * accounted in the same op stream so the consumer sees them too.
 *
 * Tunables (env vars):
 *   BENCH_OPS               — ops per channel (default 10000).
 *   BENCH_RATE_OPS_PER_SEC  — pacing rate (0 = unpaced; default 0).
 *   STRESS_RESULTS_FILE     — when set, key=value summary is appended.
 *
 * PASS criterion: every submitted op was observed by the consumer AND
 * the aggregate p99 latency is below the documented budget. Latency
 * here is wall-clock submit→complete, so an unpaced producer that
 * saturates the SPSC ring naturally accrues queue-wait at the head of
 * each op; with ring depth of 256 and the modeled NAND op time the
 * harness observes p99 in the low tens of milliseconds. The budget is
 * deliberately set to 100 ms so the harness catches order-of-magnitude
 * regressions (e.g. an unbounded spin or a latent deadlock surfacing
 * as a multi-second tail) without flaking on natural queue-depth
 * backpressure. Exit 0 on PASS, non-zero otherwise.
 *
 * Standalone — not wired into `make test`. Build via `make bench-cq`.
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/common.h"
#include "controller/channel_worker.h"
#include "media/media.h"

/* -------------------------------------------------------------------- */
/* Configuration                                                        */
/* -------------------------------------------------------------------- */

#define BENCH_DEFAULT_OPS_PER_CH 10000u
#define BENCH_DEFAULT_RATE        0u    /* 0 = unpaced */
#define BENCH_BATCH_CAP           64u
#define BENCH_RING_CAPACITY      256u   /* power of two */
#define BENCH_CQ_CAPACITY        256u   /* power of two */
#define BENCH_READ_PCT            70u
#define BENCH_LATENCY_P99_BUDGET_NS  (100ULL * 1000ULL * 1000ULL)  /* 100 ms */

/* NAND geometry mirroring tests/test_channel_worker.c::make_cfg(). Two
 * channels × 2 planes × 4 blocks × 16 pages keeps the simulated state
 * tiny and lets the harness recycle blocks during the run. */
#define BENCH_CHANNELS         2u
#define BENCH_CHIPS            1u
#define BENCH_DIES             1u
#define BENCH_PLANES           2u
#define BENCH_BLOCKS_PER_PLANE 4u
#define BENCH_PAGES_PER_BLOCK 16u
#define BENCH_PAGE_SIZE     4096u
#define BENCH_SPARE_SIZE     128u

/* Sentinel that "no page has been programmed yet" for a given (plane,
 * block) pair within the channel's working set. */
#define BENCH_PAGE_NONE ((u32)-1)

/* -------------------------------------------------------------------- */
/* Workload op + heap-pooled command                                    */
/* -------------------------------------------------------------------- */

/* Bench cmd wraps the worker cmd plus its data buffer, so heap lifetime
 * spans submit -> drain -> latency record -> free, independent of the
 * producer's stack. */
struct bench_cmd {
    struct channel_cmd ccmd;
    u8                 data[BENCH_PAGE_SIZE];
};

/* -------------------------------------------------------------------- */
/* Per-channel runtime state                                            */
/* -------------------------------------------------------------------- */

struct bench_channel {
    u32                    channel_id;
    struct channel_worker  worker;
    struct media_ctx      *media;
    u32                    target_ops;
    u32                    rate_ops_per_sec;
    pthread_t              prod_thr;
    pthread_t              cons_thr;
    bool                   prod_started;
    bool                   cons_started;
    _Atomic int            prod_done;
    _Atomic u64            submitted;
    _Atomic u64            completed;
    _Atomic int            fatal;            /* 1 if producer/consumer hit a fatal error */
    /* Latency samples — owned by the consumer, sized to target_ops. */
    u64                   *samples;
    u32                    sample_count;
};

/* -------------------------------------------------------------------- */
/* PRNG (xorshift32)                                                    */
/* -------------------------------------------------------------------- */

static u32 xorshift32(u32 *state)
{
    u32 x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void fill_pattern(u8 *buf, u32 size, u32 seed)
{
    u32 s = seed ? seed : 0xDEADBEEFu;
    for (u32 i = 0; i < size; i++) {
        s = xorshift32(&s);
        buf[i] = (u8)(s & 0xFFu);
    }
}

/* -------------------------------------------------------------------- */
/* Env helpers                                                          */
/* -------------------------------------------------------------------- */

static u32 env_u32(const char *key, u32 fallback)
{
    const char *v = getenv(key);
    if (!v || !*v) return fallback;
    long n = strtol(v, NULL, 10);
    if (n <= 0) return fallback;
    return (u32)n;
}

/* -------------------------------------------------------------------- */
/* Working set bookkeeping                                              */
/* -------------------------------------------------------------------- */

/*
 * Per-(plane, block) cursor: how many pages have been programmed within
 * this block since its last erase, and the most recently programmed
 * page index for read targeting. The worker dispatches synchronously,
 * so the producer thread is the sole owner of this state and does not
 * need atomics.
 */
struct bench_block_state {
    u32 next_page;       /* 0..BENCH_PAGES_PER_BLOCK */
    u32 last_programmed; /* BENCH_PAGE_NONE if none yet */
};

struct chan_blocks {
    struct bench_block_state planes[BENCH_PLANES][BENCH_BLOCKS_PER_PLANE];
};

static void chan_blocks_reset(struct chan_blocks *cb)
{
    for (u32 p = 0; p < BENCH_PLANES; p++) {
        for (u32 b = 0; b < BENCH_BLOCKS_PER_PLANE; b++) {
            cb->planes[p][b].next_page       = 0;
            cb->planes[p][b].last_programmed = BENCH_PAGE_NONE;
        }
    }
}

/* Synchronously erase every block in the channel's working set so the
 * producer can immediately program. Runs once before the producer
 * thread is spawned. */
static int prepare_channel_blocks(struct media_ctx *media, u32 ch)
{
    for (u32 plane = 0; plane < BENCH_PLANES; plane++) {
        for (u32 block = 0; block < BENCH_BLOCKS_PER_PLANE; block++) {
            int rc = media_nand_erase(media, ch, 0, 0, plane, block);
            if (rc != HFSSS_OK) {
                fprintf(stderr,
                        "[bench] pre-erase failed ch=%u plane=%u block=%u rc=%d\n",
                        ch, plane, block, rc);
                return rc;
            }
        }
    }
    return HFSSS_OK;
}

/* -------------------------------------------------------------------- */
/* Pacing                                                               */
/* -------------------------------------------------------------------- */

/*
 * If rate > 0, returns the absolute deadline for op index `op_idx`
 * relative to start_ns; the producer sleeps until then. Rate 0 disables
 * pacing entirely.
 */
static u64 deadline_for(u64 start_ns, u32 rate, u64 op_idx)
{
    if (rate == 0) return 0;
    u64 ns_per_op = 1000000000ULL / rate;
    return start_ns + ns_per_op * op_idx;
}

static void wait_until(u64 deadline_ns)
{
    if (deadline_ns == 0) return;
    u64 now = get_time_ns();
    if (now >= deadline_ns) return;
    sleep_ns(deadline_ns - now);
}

/* -------------------------------------------------------------------- */
/* bench_cmd allocation                                                 */
/* -------------------------------------------------------------------- */

static struct bench_cmd *bench_cmd_new(void)
{
    struct bench_cmd *bc = (struct bench_cmd *)calloc(1, sizeof(*bc));
    return bc;
}

static void bench_cmd_free(struct bench_cmd *bc)
{
    free(bc);
}

/* -------------------------------------------------------------------- */
/* Producer                                                             */
/* -------------------------------------------------------------------- */

/*
 * Pick the next op type and fully populate cmd. Returns false if no
 * read target is available yet (consumer hasn't seen any program land).
 * The caller retries with a different op in that case.
 */
static bool fill_program_cmd(struct bench_cmd *bc, u32 ch,
                             struct chan_blocks *cb, u32 *cursor)
{
    /* Scan forward from cursor for any block with free pages. If all
     * blocks are full, re-erase the next one in rotation and reuse. */
    for (u32 sweep = 0; sweep < BENCH_PLANES * BENCH_BLOCKS_PER_PLANE; sweep++) {
        u32 idx   = (*cursor + sweep) % (BENCH_PLANES * BENCH_BLOCKS_PER_PLANE);
        u32 plane = idx / BENCH_BLOCKS_PER_PLANE;
        u32 block = idx % BENCH_BLOCKS_PER_PLANE;
        if (cb->planes[plane][block].next_page < BENCH_PAGES_PER_BLOCK) {
            u32 page = cb->planes[plane][block].next_page++;
            cb->planes[plane][block].last_programmed = page;
            bc->ccmd.op    = CHANNEL_CMD_PROGRAM;
            bc->ccmd.ch    = ch;
            bc->ccmd.chip  = 0;
            bc->ccmd.die   = 0;
            bc->ccmd.plane = plane;
            bc->ccmd.block = block;
            bc->ccmd.page  = page;
            bc->ccmd.data_buf = bc->data;
            fill_pattern(bc->data, BENCH_PAGE_SIZE,
                         (ch << 24) ^ (plane << 16) ^ (block << 8) ^ page);
            *cursor = idx;
            return true;
        }
    }
    /* All blocks are full — issue a synchronous erase on the cursor
     * block via the worker is unsafe here (would deadlock the producer
     * waiting on its own consumer). Instead, encode an ERASE cmd and
     * let the worker run it; mark the block as "in flight" by resetting
     * its cursor immediately so subsequent program ops can target it
     * once the erase completes (the worker dispatches FIFO so by the
     * time the next program for this block reaches the worker, the
     * erase has already returned). */
    u32 idx   = *cursor % (BENCH_PLANES * BENCH_BLOCKS_PER_PLANE);
    u32 plane = idx / BENCH_BLOCKS_PER_PLANE;
    u32 block = idx % BENCH_BLOCKS_PER_PLANE;
    cb->planes[plane][block].next_page       = 0;
    cb->planes[plane][block].last_programmed = BENCH_PAGE_NONE;
    bc->ccmd.op    = CHANNEL_CMD_ERASE;
    bc->ccmd.ch    = ch;
    bc->ccmd.chip  = 0;
    bc->ccmd.die   = 0;
    bc->ccmd.plane = plane;
    bc->ccmd.block = block;
    bc->ccmd.page  = 0;
    bc->ccmd.data_buf = NULL;
    return true;
}

/*
 * Pick a read target from the channel's working set. Returns false if
 * no page has been programmed yet (caller falls back to a program).
 */
static bool fill_read_cmd(struct bench_cmd *bc, u32 ch,
                          struct chan_blocks *cb, u32 *prng)
{
    /* Sample up to N attempts; if none of the random picks is
     * programmed, the caller should fall back to a program. */
    for (int attempt = 0; attempt < 8; attempt++) {
        u32 r     = xorshift32(prng);
        u32 plane = r % BENCH_PLANES;
        u32 block = (r / BENCH_PLANES) % BENCH_BLOCKS_PER_PLANE;
        u32 last  = cb->planes[plane][block].last_programmed;
        if (last == BENCH_PAGE_NONE) continue;
        bc->ccmd.op    = CHANNEL_CMD_READ;
        bc->ccmd.ch    = ch;
        bc->ccmd.chip  = 0;
        bc->ccmd.die   = 0;
        bc->ccmd.plane = plane;
        bc->ccmd.block = block;
        bc->ccmd.page  = last;
        bc->ccmd.data_buf = bc->data;
        return true;
    }
    return false;
}

static void *producer_thread(void *arg)
{
    struct bench_channel *bc  = (struct bench_channel *)arg;
    struct chan_blocks    cb;
    chan_blocks_reset(&cb);
    u32 prng   = 0xC0FFEE00u ^ (bc->channel_id * 0x9E3779B1u);
    u32 cursor = 0;
    u64 start  = get_time_ns();

    for (u32 i = 0; i < bc->target_ops; i++) {
        if (atomic_load(&bc->fatal)) break;

        struct bench_cmd *cmd = bench_cmd_new();
        if (!cmd) {
            fprintf(stderr, "[bench] OOM allocating cmd ch=%u i=%u\n",
                    bc->channel_id, i);
            atomic_store(&bc->fatal, 1);
            break;
        }

        u32 r       = xorshift32(&prng);
        bool is_read = (r % 100u) < BENCH_READ_PCT;
        bool ok      = false;
        if (is_read) {
            ok = fill_read_cmd(cmd, bc->channel_id, &cb, &prng);
        }
        if (!ok) {
            ok = fill_program_cmd(cmd, bc->channel_id, &cb, &cursor);
        }
        if (!ok) {
            bench_cmd_free(cmd);
            atomic_store(&bc->fatal, 1);
            break;
        }

        wait_until(deadline_for(start, bc->rate_ops_per_sec, i));

        /* Submit with retry on BUSY (CQ back-pressure flows through to
         * the submit ring). yield-spin keeps the producer responsive
         * to cleanup. */
        for (;;) {
            int rc = channel_worker_submit(&bc->worker, &cmd->ccmd);
            if (rc == HFSSS_OK) break;
            if (rc == HFSSS_ERR_BUSY) {
                sched_yield();
                continue;
            }
            fprintf(stderr,
                    "[bench] submit failed ch=%u i=%u rc=%d\n",
                    bc->channel_id, i, rc);
            bench_cmd_free(cmd);
            atomic_store(&bc->fatal, 1);
            goto out;
        }
        atomic_fetch_add(&bc->submitted, 1);
    }
out:
    atomic_store(&bc->prod_done, 1);
    return NULL;
}

/* -------------------------------------------------------------------- */
/* Consumer                                                             */
/* -------------------------------------------------------------------- */

static void *consumer_thread(void *arg)
{
    struct bench_channel *bc = (struct bench_channel *)arg;
    struct channel_cmd *batch[BENCH_BATCH_CAP];
    u32 popped_total = 0;

    while (popped_total < bc->target_ops) {
        if (atomic_load(&bc->fatal)) break;
        int n = channel_worker_drain(&bc->worker, batch, BENCH_BATCH_CAP);
        if (n < 0) {
            fprintf(stderr, "[bench] drain ch=%u rc=%d\n",
                    bc->channel_id, n);
            atomic_store(&bc->fatal, 1);
            break;
        }
        if (n == 0) {
            /* Producer finished and CQ is empty — bail. */
            if (atomic_load(&bc->prod_done) &&
                atomic_load(&bc->submitted) == popped_total) {
                break;
            }
            sched_yield();
            continue;
        }
        for (int i = 0; i < n; i++) {
            struct channel_cmd *cc = batch[i];
            struct bench_cmd   *cmd = (struct bench_cmd *)cc;
            u64 lat = cc->complete_ts_ns - cc->submit_ts_ns;
            if (popped_total < bc->target_ops) {
                bc->samples[popped_total] = lat;
            }
            popped_total++;
            atomic_fetch_add(&bc->completed, 1);
            bench_cmd_free(cmd);
        }
    }
    bc->sample_count = popped_total;
    return NULL;
}

/* -------------------------------------------------------------------- */
/* Stats                                                                */
/* -------------------------------------------------------------------- */

struct latency_stats {
    u64 count;
    u64 min_ns;
    u64 max_ns;
    double mean_ns;
    u64 p50_ns;
    u64 p99_ns;
    u64 p999_ns;
};

static int u64_cmp(const void *a, const void *b)
{
    u64 x = *(const u64 *)a;
    u64 y = *(const u64 *)b;
    if (x < y) return -1;
    if (x > y) return  1;
    return 0;
}

static u64 percentile(const u64 *sorted, u64 count, double pct)
{
    if (count == 0) return 0;
    /* Nearest-rank percentile, clamped to last index. */
    u64 idx = (u64)(pct * (double)count);
    if (idx >= count) idx = count - 1;
    return sorted[idx];
}

static void compute_stats(u64 *samples, u64 count, struct latency_stats *out)
{
    memset(out, 0, sizeof(*out));
    if (count == 0) return;
    qsort(samples, count, sizeof(u64), u64_cmp);
    out->count   = count;
    out->min_ns  = samples[0];
    out->max_ns  = samples[count - 1];
    long double sum = 0.0L;
    for (u64 i = 0; i < count; i++) sum += (long double)samples[i];
    out->mean_ns = (double)(sum / (long double)count);
    out->p50_ns  = percentile(samples, count, 0.50);
    out->p99_ns  = percentile(samples, count, 0.99);
    out->p999_ns = percentile(samples, count, 0.999);
}

/* -------------------------------------------------------------------- */
/* Reporting                                                            */
/* -------------------------------------------------------------------- */

static void print_report(const struct latency_stats *s,
                         u32 channels,
                         u32 ops_per_channel,
                         u64 wall_ns,
                         double ops_per_sec,
                         int pass)
{
    double wall_sec = (double)wall_ns / 1e9;
    printf("========================================\n");
    printf("REQ-045 tier-2 CQ latency bench\n");
    printf("========================================\n");
    printf("channels         : %u\n", channels);
    printf("ops_per_channel  : %u\n", ops_per_channel);
    printf("total_ops        : %llu\n", (unsigned long long)s->count);
    printf("wall_time_sec    : %.3f\n", wall_sec);
    printf("ops_per_sec      : %.1f\n", ops_per_sec);
    printf("----------------------------------------\n");
    printf("latency_min_ns   : %llu\n", (unsigned long long)s->min_ns);
    printf("latency_p50_ns   : %llu\n", (unsigned long long)s->p50_ns);
    printf("latency_p99_ns   : %llu\n", (unsigned long long)s->p99_ns);
    printf("latency_p999_ns  : %llu\n", (unsigned long long)s->p999_ns);
    printf("latency_max_ns   : %llu\n", (unsigned long long)s->max_ns);
    printf("latency_mean_ns  : %.1f\n", s->mean_ns);
    printf("----------------------------------------\n");
    printf("result           : %s\n", pass ? "PASS" : "FAIL");
    printf("========================================\n");
}

/*
 * key=value summary for CI consumption. Mirrors the shape used by
 * tests/stress_stability.c::write_results_file so the same scrape
 * pipeline can lift values out of the artifact.
 */
static void write_results_file(const char *path,
                               const struct latency_stats *s,
                               u32 channels,
                               u32 ops_per_channel,
                               u32 expected_total,
                               u64 wall_ns,
                               double ops_per_sec,
                               int pass)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[bench] cannot open results file '%s' (errno=%d)\n",
                path, errno);
        return;
    }
    double wall_sec = (double)wall_ns / 1e9;
    fprintf(f, "result=%s\n",          pass ? "PASS" : "FAIL");
    fprintf(f, "channels=%u\n",        channels);
    fprintf(f, "ops_per_channel=%u\n", ops_per_channel);
    fprintf(f, "expected_total=%u\n",  expected_total);
    fprintf(f, "total_ops=%llu\n",     (unsigned long long)s->count);
    fprintf(f, "wall_time_sec=%.3f\n", wall_sec);
    fprintf(f, "ops_per_sec=%.1f\n",   ops_per_sec);
    fprintf(f, "latency_min_ns=%llu\n",  (unsigned long long)s->min_ns);
    fprintf(f, "latency_p50_ns=%llu\n",  (unsigned long long)s->p50_ns);
    fprintf(f, "latency_p99_ns=%llu\n",  (unsigned long long)s->p99_ns);
    fprintf(f, "latency_p999_ns=%llu\n", (unsigned long long)s->p999_ns);
    fprintf(f, "latency_max_ns=%llu\n",  (unsigned long long)s->max_ns);
    fprintf(f, "latency_mean_ns=%.1f\n", s->mean_ns);
    fprintf(f, "latency_p99_budget_ns=%llu\n",
            (unsigned long long)BENCH_LATENCY_P99_BUDGET_NS);
    fclose(f);
}

/* -------------------------------------------------------------------- */
/* Cleanup helpers                                                      */
/* -------------------------------------------------------------------- */

/*
 * Drain any commands still queued on a channel's CQ after the consumer
 * thread has been joined. Runs before channel_worker_cleanup so we do
 * not leak heap-allocated bench_cmd payloads on early-exit paths.
 */
static void drain_residual(struct bench_channel *bc)
{
    struct channel_cmd *batch[BENCH_BATCH_CAP];
    for (;;) {
        int n = channel_worker_drain(&bc->worker, batch, BENCH_BATCH_CAP);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            bench_cmd_free((struct bench_cmd *)batch[i]);
        }
    }
}

static void teardown_channels(struct bench_channel *channels, u32 n)
{
    /* Signal stop on every worker first so any producer/consumer that
     * is still spinning unblocks promptly. */
    for (u32 i = 0; i < n; i++) {
        atomic_store(&channels[i].fatal, 1);
        channel_worker_stop(&channels[i].worker);
    }
    for (u32 i = 0; i < n; i++) {
        if (channels[i].prod_started) pthread_join(channels[i].prod_thr, NULL);
        if (channels[i].cons_started) pthread_join(channels[i].cons_thr, NULL);
        drain_residual(&channels[i]);
        channel_worker_cleanup(&channels[i].worker);
        free(channels[i].samples);
        channels[i].samples = NULL;
    }
}

/* -------------------------------------------------------------------- */
/* main                                                                 */
/* -------------------------------------------------------------------- */

static struct media_config make_bench_cfg(void)
{
    struct media_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.channel_count       = BENCH_CHANNELS;
    cfg.chips_per_channel   = BENCH_CHIPS;
    cfg.dies_per_chip       = BENCH_DIES;
    cfg.planes_per_die      = BENCH_PLANES;
    cfg.blocks_per_plane    = BENCH_BLOCKS_PER_PLANE;
    cfg.pages_per_block     = BENCH_PAGES_PER_BLOCK;
    cfg.page_size           = BENCH_PAGE_SIZE;
    cfg.spare_size          = BENCH_SPARE_SIZE;
    cfg.nand_type           = NAND_TYPE_SLC;
    cfg.enable_multi_plane  = true;
    return cfg;
}

int main(void)
{
    u32 ops_per_ch = env_u32("BENCH_OPS",              BENCH_DEFAULT_OPS_PER_CH);
    u32 rate       = env_u32("BENCH_RATE_OPS_PER_SEC", BENCH_DEFAULT_RATE);
    const char *results_path = getenv("STRESS_RESULTS_FILE");

    printf("REQ-045 tier-2 CQ bench — channels=%u ops_per_channel=%u rate=%u (%s)\n",
           BENCH_CHANNELS, ops_per_ch, rate,
           rate == 0 ? "unpaced" : "paced");
    printf("workload: %u%% READ / %u%% PROGRAM, page=%u, ring=%u, cq=%u\n",
           BENCH_READ_PCT, 100 - BENCH_READ_PCT,
           BENCH_PAGE_SIZE, BENCH_RING_CAPACITY, BENCH_CQ_CAPACITY);

    struct media_config cfg = make_bench_cfg();
    struct media_ctx media;
    int rc = media_init(&media, &cfg);
    if (rc != HFSSS_OK) {
        fprintf(stderr, "[bench] media_init failed rc=%d\n", rc);
        return 2;
    }

    struct bench_channel channels[BENCH_CHANNELS];
    memset(channels, 0, sizeof(channels));

    /* Pre-erase all blocks before any worker starts so the producer can
     * issue programs immediately. */
    for (u32 ch = 0; ch < BENCH_CHANNELS; ch++) {
        rc = prepare_channel_blocks(&media, ch);
        if (rc != HFSSS_OK) {
            media_cleanup(&media);
            return 2;
        }
    }

    /* Init workers + per-channel state. */
    for (u32 ch = 0; ch < BENCH_CHANNELS; ch++) {
        channels[ch].channel_id       = ch;
        channels[ch].media            = &media;
        channels[ch].target_ops       = ops_per_ch;
        channels[ch].rate_ops_per_sec = rate;
        atomic_store(&channels[ch].prod_done, 0);
        atomic_store(&channels[ch].submitted, 0);
        atomic_store(&channels[ch].completed, 0);
        atomic_store(&channels[ch].fatal,     0);
        channels[ch].samples = (u64 *)calloc(ops_per_ch, sizeof(u64));
        if (!channels[ch].samples) {
            fprintf(stderr, "[bench] OOM allocating samples ch=%u\n", ch);
            teardown_channels(channels, ch);
            media_cleanup(&media);
            return 2;
        }
        rc = channel_worker_init(&channels[ch].worker, ch, &media,
                                 BENCH_RING_CAPACITY, BENCH_CQ_CAPACITY);
        if (rc != HFSSS_OK) {
            fprintf(stderr, "[bench] channel_worker_init ch=%u rc=%d\n", ch, rc);
            teardown_channels(channels, ch);
            media_cleanup(&media);
            return 2;
        }
    }

    /* Spawn producers + consumers. */
    u64 wall_start = get_time_ns();
    for (u32 ch = 0; ch < BENCH_CHANNELS; ch++) {
        if (pthread_create(&channels[ch].cons_thr, NULL,
                           consumer_thread, &channels[ch]) != 0) {
            fprintf(stderr, "[bench] pthread_create consumer ch=%u failed\n", ch);
            teardown_channels(channels, BENCH_CHANNELS);
            media_cleanup(&media);
            return 2;
        }
        channels[ch].cons_started = true;
        if (pthread_create(&channels[ch].prod_thr, NULL,
                           producer_thread, &channels[ch]) != 0) {
            fprintf(stderr, "[bench] pthread_create producer ch=%u failed\n", ch);
            teardown_channels(channels, BENCH_CHANNELS);
            media_cleanup(&media);
            return 2;
        }
        channels[ch].prod_started = true;
    }

    /* Join producers, then consumers. */
    int any_fatal = 0;
    for (u32 ch = 0; ch < BENCH_CHANNELS; ch++) {
        pthread_join(channels[ch].prod_thr, NULL);
        channels[ch].prod_started = false;
        if (atomic_load(&channels[ch].fatal)) any_fatal = 1;
    }
    for (u32 ch = 0; ch < BENCH_CHANNELS; ch++) {
        pthread_join(channels[ch].cons_thr, NULL);
        channels[ch].cons_started = false;
        if (atomic_load(&channels[ch].fatal)) any_fatal = 1;
    }
    u64 wall_end = get_time_ns();
    u64 wall_ns  = wall_end - wall_start;

    /* Aggregate samples. */
    u64 total = 0;
    for (u32 ch = 0; ch < BENCH_CHANNELS; ch++) {
        total += channels[ch].sample_count;
    }
    u64 *agg = (u64 *)calloc(total ? total : 1, sizeof(u64));
    if (!agg) {
        fprintf(stderr, "[bench] OOM aggregating samples (total=%llu)\n",
                (unsigned long long)total);
        teardown_channels(channels, BENCH_CHANNELS);
        media_cleanup(&media);
        return 2;
    }
    u64 cursor = 0;
    for (u32 ch = 0; ch < BENCH_CHANNELS; ch++) {
        memcpy(agg + cursor, channels[ch].samples,
               channels[ch].sample_count * sizeof(u64));
        cursor += channels[ch].sample_count;
    }

    struct latency_stats stats;
    compute_stats(agg, total, &stats);
    free(agg);

    u32 expected_total = ops_per_ch * BENCH_CHANNELS;
    double ops_per_sec = wall_ns ? ((double)total * 1e9 / (double)wall_ns) : 0.0;
    int pass = !any_fatal &&
               (total == expected_total) &&
               (stats.p99_ns < BENCH_LATENCY_P99_BUDGET_NS);

    print_report(&stats, BENCH_CHANNELS, ops_per_ch,
                 wall_ns, ops_per_sec, pass);

    if (results_path && *results_path) {
        write_results_file(results_path, &stats,
                           BENCH_CHANNELS, ops_per_ch,
                           expected_total, wall_ns, ops_per_sec, pass);
    }

    teardown_channels(channels, BENCH_CHANNELS);
    media_cleanup(&media);

    return pass ? 0 : 1;
}
