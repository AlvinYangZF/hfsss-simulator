#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/common.h"
#include "controller/channel_worker.h"
#include "media/media.h"

#define TEST_PASS 0
#define TEST_FAIL 1

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                                                                                         \
    do {                                                                                                               \
        tests_run++;                                                                                                   \
        if (cond) {                                                                                                    \
            printf("  [PASS] %s\n", msg);                                                                              \
            tests_passed++;                                                                                            \
        } else {                                                                                                       \
            printf("  [FAIL] %s\n", msg);                                                                              \
            tests_failed++;                                                                                            \
        }                                                                                                              \
    } while (0)

static struct media_config make_cfg(void)
{
    struct media_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.channel_count = 2;
    cfg.chips_per_channel = 1;
    cfg.dies_per_chip = 1;
    cfg.planes_per_die = 2;
    cfg.blocks_per_plane = 4;
    cfg.pages_per_block = 16;
    cfg.page_size = 4096;
    cfg.spare_size = 128;
    cfg.nand_type = NAND_TYPE_SLC;
    cfg.enable_multi_plane = true;
    return cfg;
}

static void fill(void *buf, u32 n, u8 seed)
{
    u8 *p = (u8 *)buf;
    for (u32 i = 0; i < n; i++) {
        p[i] = (u8)((seed + i) & 0xFF);
    }
}

/* Submit one ERASE on (ch=0,chip=0,die=0,plane=0,block=K) and return
 * the prepared cmd. Caller fills program/read forms inline. The block
 * id is a parameter so each test exercises different physical state
 * and avoids retry-on-already-erased-block surprises. */
static void prep_erase_cmd(struct channel_cmd *cmd, u32 ch, u32 block)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->op    = CHANNEL_CMD_ERASE;
    cmd->ch    = ch;
    cmd->chip  = 0;
    cmd->die   = 0;
    cmd->plane = 0;
    cmd->block = block;
}

/*
 * Completion sink used by the tests. The worker fires on_complete from
 * its own thread; tests verify the hook was called by incrementing the
 * counter, then optionally block on channel_cmd_wait to observe
 * status.
 */
struct sink {
    _Atomic int fired_count;
    _Atomic int last_status;
};

static void sink_cb(struct channel_cmd *cmd, void *ctx)
{
    struct sink *s = (struct sink *)ctx;
    atomic_store(&s->last_status, cmd->status);
    atomic_fetch_add(&s->fired_count, 1);
}

/*
 * REQ-044 core: init → submit → wait → cleanup produces exactly one
 * dispatch. The media op itself is an ERASE on block 0, which the
 * synchronous media path already supports; we're just proving that
 * the worker thread was the one to run it and that the async
 * completion path fires.
 */
static int test_submit_wait_completion_cb(void)
{
    printf("\n=== channel_worker: single submit + completion cb ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    int rc = media_init(&media, &cfg);
    TEST_ASSERT(rc == HFSSS_OK, "media_init OK");

    struct channel_worker w;
    rc = channel_worker_init(&w, 0, &media, 64, 0);
    TEST_ASSERT(rc == HFSSS_OK, "channel_worker_init OK");

    struct sink sink;
    atomic_store(&sink.fired_count, 0);
    atomic_store(&sink.last_status, -1);

    struct channel_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = CHANNEL_CMD_ERASE;
    cmd.ch = 0;
    cmd.chip = 0;
    cmd.die = 0;
    cmd.plane = 0;
    cmd.block = 0;
    cmd.page = 0;
    cmd.on_complete = sink_cb;
    cmd.cb_ctx = &sink;

    rc = channel_worker_submit(&w, &cmd);
    TEST_ASSERT(rc == HFSSS_OK, "submit ERASE returns OK");

    int status = channel_cmd_wait(&cmd);
    TEST_ASSERT(status == HFSSS_OK, "waited cmd status is OK");
    TEST_ASSERT(atomic_load(&sink.fired_count) == 1, "completion callback fired exactly once");
    TEST_ASSERT(atomic_load(&sink.last_status) == HFSSS_OK, "callback observed status OK");
    TEST_ASSERT(atomic_load(&w.submitted) == 1, "worker submitted counter == 1");
    TEST_ASSERT(atomic_load(&w.completed) == 1, "worker completed counter == 1");

    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/*
 * End-to-end data path via async submission: ERASE → PROGRAM → READ,
 * all dispatched through the worker thread. Verifies that payload
 * round-trips across the queue boundary without corruption and that
 * completion counters advance monotonically.
 */
static int test_erase_program_read_roundtrip(void)
{
    printf("\n=== channel_worker: erase→program→read via async path ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    int rc = media_init(&media, &cfg);
    TEST_ASSERT(rc == HFSSS_OK, "media_init OK");

    struct channel_worker w;
    rc = channel_worker_init(&w, 0, &media, 32, 0);
    TEST_ASSERT(rc == HFSSS_OK, "channel_worker_init OK");

    /* ERASE plane 0 block 0 */
    struct channel_cmd ec;
    memset(&ec, 0, sizeof(ec));
    ec.op = CHANNEL_CMD_ERASE;
    TEST_ASSERT(channel_worker_submit(&w, &ec) == HFSSS_OK, "submit ERASE");
    TEST_ASSERT(channel_cmd_wait(&ec) == HFSSS_OK, "erase completed OK");

    /* PROGRAM page 3 with a known pattern */
    u8 *src = (u8 *)malloc(cfg.page_size);
    fill(src, cfg.page_size, 0x5A);
    struct channel_cmd pc;
    memset(&pc, 0, sizeof(pc));
    pc.op = CHANNEL_CMD_PROGRAM;
    pc.page = 3;
    pc.data_buf = src;
    TEST_ASSERT(channel_worker_submit(&w, &pc) == HFSSS_OK, "submit PROGRAM");
    TEST_ASSERT(channel_cmd_wait(&pc) == HFSSS_OK, "program completed OK");

    /* READ back page 3 */
    u8 *dst = (u8 *)malloc(cfg.page_size);
    memset(dst, 0, cfg.page_size);
    struct channel_cmd rc_cmd;
    memset(&rc_cmd, 0, sizeof(rc_cmd));
    rc_cmd.op = CHANNEL_CMD_READ;
    rc_cmd.page = 3;
    rc_cmd.data_buf = dst;
    TEST_ASSERT(channel_worker_submit(&w, &rc_cmd) == HFSSS_OK, "submit READ");
    TEST_ASSERT(channel_cmd_wait(&rc_cmd) == HFSSS_OK, "read completed OK");

    TEST_ASSERT(memcmp(src, dst, cfg.page_size) == 0, "read payload matches programmed pattern");
    TEST_ASSERT(atomic_load(&w.completed) == 3, "worker completed 3 commands");

    free(src);
    free(dst);
    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/*
 * Ring-full back-pressure under load: flood a live worker with a burst
 * of synchronous ERASE ops (each drains ~one tBERS worth of wall-clock
 * in the media engine) and confirm at least one submit is refused with
 * HFSSS_ERR_BUSY, i.e. the producer observes real ring pressure rather
 * than a manipulated lifecycle. Every accepted command is then awaited
 * so no leaked work remains when cleanup joins the worker.
 */
#define BURST_COUNT 64u
static int test_full_ring_returns_busy(void)
{
    printf("\n=== channel_worker: submit returns BUSY under real ring pressure ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    int rc = media_init(&media, &cfg);
    TEST_ASSERT(rc == HFSSS_OK, "media_init OK");

    struct channel_worker w;
    rc = channel_worker_init(&w, 0, &media, 2, 0);
    TEST_ASSERT(rc == HFSSS_OK, "channel_worker_init OK (cap=2)");

    static struct channel_cmd burst[BURST_COUNT];
    memset(burst, 0, sizeof(burst));

    u32 accepted = 0;
    u32 busy = 0;
    for (u32 i = 0; i < BURST_COUNT; i++) {
        burst[i].op = CHANNEL_CMD_ERASE;
        burst[i].ch = 0;
        burst[i].block = i % cfg.blocks_per_plane;
        int r = channel_worker_submit(&w, &burst[i]);
        if (r == HFSSS_OK) {
            accepted++;
        } else if (r == HFSSS_ERR_BUSY) {
            busy++;
        } else {
            TEST_ASSERT(false, "unexpected submit return code");
        }
    }

    TEST_ASSERT(busy > 0, "at least one submit refused with BUSY under flood");
    TEST_ASSERT(accepted + busy == BURST_COUNT, "every submit returned OK or BUSY");

    /* Drain every accepted command so cleanup does not join over
     * in-flight work. */
    for (u32 i = 0; i < BURST_COUNT; i++) {
        if (atomic_load(&burst[i].done) || burst[i].status != 0) {
            continue;
        }
    }
    u32 drained = 0;
    for (u32 i = 0; i < BURST_COUNT; i++) {
        /* Any cmd not accepted never had its done flag flipped from 0;
         * cmds not placed in the ring are simply skipped by waiting on
         * a bounded number of accepted ones. Walk the whole array; for
         * never-submitted entries the wait would spin forever, so gate
         * the wait on whether the worker's submitted counter surpasses
         * the submission index. */
        (void)drained;
    }
    /* Simpler drain: poll until the worker's completed counter catches
     * up with accepted. */
    while (atomic_load(&w.completed) < accepted) {
        sched_yield();
    }

    TEST_ASSERT(atomic_load(&w.completed) == accepted, "worker completed exactly the accepted submits");

    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/*
 * Stop-lifecycle: once channel_worker_stop has been signalled, any
 * further submit is rejected with HFSSS_ERR_BUSY. Rejects closes the
 * window in which a late producer could orphan a command on a ring
 * the consumer is about to abandon.
 */
static int test_submit_after_stop_returns_busy(void)
{
    printf("\n=== channel_worker: submit rejected after stop ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    int rc = media_init(&media, &cfg);
    TEST_ASSERT(rc == HFSSS_OK, "media_init OK");

    struct channel_worker w;
    rc = channel_worker_init(&w, 0, &media, 16, 0);
    TEST_ASSERT(rc == HFSSS_OK, "channel_worker_init OK");

    channel_worker_stop(&w);

    struct channel_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = CHANNEL_CMD_ERASE;

    TEST_ASSERT(channel_worker_submit(&w, &cmd) == HFSSS_ERR_BUSY, "submit after stop returns BUSY");

    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/*
 * Cross-channel cmd rejection: a worker bound to channel 0 refuses to
 * drive commands addressed to channel 1. The command still completes
 * via the normal path with status == HFSSS_ERR_INVAL so the submitter
 * can detect the misrouting.
 */
static int test_cross_channel_cmd_rejected(void)
{
    printf("\n=== channel_worker: cross-channel command rejected ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    int rc = media_init(&media, &cfg);
    TEST_ASSERT(rc == HFSSS_OK, "media_init OK (2 channels)");

    struct channel_worker w0;
    rc = channel_worker_init(&w0, 0, &media, 8, 0);
    TEST_ASSERT(rc == HFSSS_OK, "worker 0 init");

    struct channel_cmd miscmd;
    memset(&miscmd, 0, sizeof(miscmd));
    miscmd.op = CHANNEL_CMD_ERASE;
    miscmd.ch = 1; /* wrong channel for worker 0 */
    miscmd.block = 0;

    TEST_ASSERT(channel_worker_submit(&w0, &miscmd) == HFSSS_OK, "submit cross-channel cmd");
    TEST_ASSERT(channel_cmd_wait(&miscmd) == HFSSS_ERR_INVAL, "cross-channel cmd fails with INVAL");

    channel_worker_cleanup(&w0);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/*
 * Two workers on distinct channels run independently: each drains its
 * own ring, each worker's completed counter advances only for commands
 * it owns. Verifies channel_id isolation and confirms the runtime
 * scales to multi-channel config.
 */
static int test_two_workers_isolated(void)
{
    printf("\n=== channel_worker: two workers on distinct channels ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    int rc = media_init(&media, &cfg);
    TEST_ASSERT(rc == HFSSS_OK, "media_init OK (2 channels)");

    struct channel_worker w0, w1;
    TEST_ASSERT(channel_worker_init(&w0, 0, &media, 16, 0) == HFSSS_OK, "worker 0 init");
    TEST_ASSERT(channel_worker_init(&w1, 1, &media, 16, 0) == HFSSS_OK, "worker 1 init");

    struct channel_cmd e0, e1;
    memset(&e0, 0, sizeof(e0));
    memset(&e1, 0, sizeof(e1));
    e0.op = CHANNEL_CMD_ERASE;
    e0.ch = 0;
    e0.block = 0;
    e1.op = CHANNEL_CMD_ERASE;
    e1.ch = 1;
    e1.block = 0;

    TEST_ASSERT(channel_worker_submit(&w0, &e0) == HFSSS_OK, "submit to worker 0");
    TEST_ASSERT(channel_worker_submit(&w1, &e1) == HFSSS_OK, "submit to worker 1");

    TEST_ASSERT(channel_cmd_wait(&e0) == HFSSS_OK, "e0 completes OK");
    TEST_ASSERT(channel_cmd_wait(&e1) == HFSSS_OK, "e1 completes OK");

    TEST_ASSERT(atomic_load(&w0.completed) == 1, "worker 0 completed == 1");
    TEST_ASSERT(atomic_load(&w1.completed) == 1, "worker 1 completed == 1");

    channel_worker_cleanup(&w0);
    channel_worker_cleanup(&w1);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/*
 * NULL-input hardening. None of these should crash or return OK.
 */
static int test_null_inputs(void)
{
    printf("\n=== channel_worker: NULL-input hardening ===\n");

    TEST_ASSERT(channel_worker_init(NULL, 0, NULL, 16, 0) == HFSSS_ERR_INVAL, "init(NULL, 0, NULL, _) INVAL");
    channel_worker_stop(NULL);
    channel_worker_cleanup(NULL);
    TEST_ASSERT(channel_worker_submit(NULL, NULL) == HFSSS_ERR_INVAL, "submit(NULL, NULL) INVAL");
    TEST_ASSERT(channel_cmd_wait(NULL) == HFSSS_ERR_INVAL, "wait(NULL) INVAL");
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* REQ-045: submit 4 cmds with CQ enabled, drain, assert order +
 * timestamps. Tests the hot-path delivery shape end to end. */
static int test_cq_basic_drain(void)
{
    printf("\n=== channel_worker: CQ basic drain ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    int rc = media_init(&media, &cfg);
    TEST_ASSERT(rc == HFSSS_OK, "media_init OK");

    struct channel_worker w;
    rc = channel_worker_init(&w, 0, &media, 16, 16);
    TEST_ASSERT(rc == HFSSS_OK, "init with cq_capacity=16 OK");

    struct channel_cmd cmds[4];
    for (int i = 0; i < 4; i++) {
        prep_erase_cmd(&cmds[i], 0, (u32)i);
        TEST_ASSERT(channel_worker_submit(&w, &cmds[i]) == HFSSS_OK,
                    "submit OK");
    }

    /* Spin-drain until we've collected all 4. The worker may interleave
     * dispatch + CQ push with our drain calls, so accumulate. */
    struct channel_cmd *out[8] = {0};
    int drained = 0;
    for (int spins = 0; spins < 100000 && drained < 4; spins++) {
        int n = channel_worker_drain(&w, &out[drained], (u32)(8 - drained));
        TEST_ASSERT(n >= 0, "drain returns non-negative");
        drained += n;
        if (n == 0) sched_yield();
    }
    TEST_ASSERT(drained == 4, "drained exactly 4 commands");

    /* SPSC FIFO: the order of CQ pops mirrors worker dispatch order,
     * which mirrors submit order in this single-producer test. */
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT(out[i] == &cmds[i],
                    "CQ pops in submit order (FIFO)");
        TEST_ASSERT(cmds[i].submit_ts_ns > 0,
                    "submit_ts_ns populated");
        TEST_ASSERT(cmds[i].complete_ts_ns >= cmds[i].submit_ts_ns,
                    "complete_ts_ns >= submit_ts_ns");
        TEST_ASSERT(atomic_load(&cmds[i].done) == 1,
                    "done flag set");
    }

    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* REQ-045: drain on an idle CQ-enabled worker returns 0, not an error. */
static int test_cq_empty_drain(void)
{
    printf("\n=== channel_worker: CQ empty drain returns 0 ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    media_init(&media, &cfg);

    struct channel_worker w;
    int rc = channel_worker_init(&w, 0, &media, 16, 8);
    TEST_ASSERT(rc == HFSSS_OK, "init OK");

    struct channel_cmd *out[4] = {0};
    int n = channel_worker_drain(&w, out, 4);
    TEST_ASSERT(n == 0, "empty drain returns 0");

    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* REQ-045: submit > cq_capacity commands while the consumer drains
 * slowly. No completion may be lost; total drained must equal total
 * submitted; per-cmd timestamps must be monotone. */
static int test_cq_batching_under_slow_consumer(void)
{
    printf("\n=== channel_worker: CQ batching with slow consumer ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    media_init(&media, &cfg);

    struct channel_worker w;
    /* CQ capacity 4 forces back-pressure since we'll submit 16. */
    int rc = channel_worker_init(&w, 0, &media, 16, 4);
    TEST_ASSERT(rc == HFSSS_OK, "init cq_capacity=4 OK");

    enum { N = 16 };
    struct channel_cmd cmds[N];
    for (int i = 0; i < N; i++) {
        prep_erase_cmd(&cmds[i], 0, (u32)i);
        /* Submit ring is 16; CQ is 4; worker stalls on CQ-full when it
         * tries to push past slot 3 until the test drains. We submit
         * eagerly and let the worker stall — that exercises the spin-
         * retry path. */
        TEST_ASSERT(channel_worker_submit(&w, &cmds[i]) == HFSSS_OK,
                    "submit OK");
    }

    /* Drain in small batches to keep the CQ alternating between full
     * and partial. Bound the total spin so a stuck worker fails the
     * test instead of hanging it. */
    struct channel_cmd *out[N] = {0};
    int drained = 0;
    for (int spins = 0; spins < 1000000 && drained < N; spins++) {
        struct channel_cmd *batch[2];
        int n = channel_worker_drain(&w, batch, 2);
        for (int i = 0; i < n; i++) {
            out[drained++] = batch[i];
        }
        if (n == 0) sched_yield();
    }
    TEST_ASSERT(drained == N,
                "drained == submitted under back-pressure");

    /* Per-cmd: each cmd timestamp pair is internally consistent. */
    int bad_pair = 0;
    for (int i = 0; i < N; i++) {
        if (cmds[i].submit_ts_ns == 0 ||
            cmds[i].complete_ts_ns < cmds[i].submit_ts_ns) {
            bad_pair++;
        }
    }
    TEST_ASSERT(bad_pair == 0,
                "every cmd has submit_ts_ns > 0 and complete_ts_ns >= submit_ts_ns");

    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* REQ-045: with the consumer drained-out-never, the submit ring
 * eventually returns HFSSS_ERR_BUSY because the worker stalls
 * spin-retrying the full CQ. Proves back-pressure flows from CQ
 * full into submit-ring full. */
static int test_cq_back_pressure_to_submit_ring(void)
{
    printf("\n=== channel_worker: CQ back-pressure to submit ring ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    media_init(&media, &cfg);

    struct channel_worker w;
    int rc = channel_worker_init(&w, 0, &media, 4, 2);
    TEST_ASSERT(rc == HFSSS_OK, "init small ring + tiny cq OK");

    /* Submit until BUSY. Without back-pressure (or with silent drops)
     * we'd never see BUSY. The submit ring is 4; CQ is 2; on a normal
     * machine the worker fills the CQ within the first 2 ops, then
     * spins on CQ-full, then the producer fills the submit ring. */
    enum { CAP = 64 };
    struct channel_cmd cmds[CAP];
    int got_busy = 0;
    int submitted = 0;
    for (int i = 0; i < CAP; i++) {
        prep_erase_cmd(&cmds[i], 0, (u32)(i & 3)); /* recycle blocks */
        rc = channel_worker_submit(&w, &cmds[i]);
        if (rc == HFSSS_ERR_BUSY) { got_busy = 1; break; }
        TEST_ASSERT(rc == HFSSS_OK, "submit OK pre-BUSY");
        submitted++;
    }
    TEST_ASSERT(got_busy == 1, "submit ring eventually returns BUSY");

    /* Drain everything to release the worker so cleanup doesn't hang. */
    struct channel_cmd *out[CAP] = {0};
    int drained = 0;
    for (int spins = 0; spins < 1000000 && drained < submitted; spins++) {
        int n = channel_worker_drain(&w, out + drained, (u32)(CAP - drained));
        if (n > 0) drained += n;
        else sched_yield();
    }

    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* REQ-045: submitting a cmd with a non-NULL on_complete to a
 * CQ-enabled worker returns HFSSS_ERR_INVAL. */
static int test_cq_rejects_on_complete(void)
{
    printf("\n=== channel_worker: CQ rejects on_complete ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    media_init(&media, &cfg);

    struct channel_worker w;
    int rc = channel_worker_init(&w, 0, &media, 8, 4);
    TEST_ASSERT(rc == HFSSS_OK, "init cq=4 OK");

    struct sink s;
    atomic_store(&s.fired_count, 0);
    atomic_store(&s.last_status, 0);

    struct channel_cmd cmd;
    prep_erase_cmd(&cmd, 0, 0);
    cmd.on_complete = sink_cb;
    cmd.cb_ctx = &s;

    rc = channel_worker_submit(&w, &cmd);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL,
                "submit rejects on_complete in CQ mode");

    /* And the canary callback never fired. */
    TEST_ASSERT(atomic_load(&s.fired_count) == 0,
                "on_complete did not fire on rejected submit");

    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* REQ-045: channel_cmd_wait on a cmd whose owner is a CQ-enabled
 * worker returns HFSSS_ERR_INVAL. The cmd still completes through
 * the CQ; this just refuses the legacy poll path. */
static int test_cq_rejects_wait(void)
{
    printf("\n=== channel_worker: CQ rejects channel_cmd_wait ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    media_init(&media, &cfg);

    struct channel_worker w;
    int rc = channel_worker_init(&w, 0, &media, 8, 4);
    TEST_ASSERT(rc == HFSSS_OK, "init cq=4 OK");

    struct channel_cmd cmd;
    prep_erase_cmd(&cmd, 0, 0);
    rc = channel_worker_submit(&w, &cmd);
    TEST_ASSERT(rc == HFSSS_OK, "submit OK without on_complete");

    /* Refused immediately; not a busy-spin. */
    int wait_rc = channel_cmd_wait(&cmd);
    TEST_ASSERT(wait_rc == HFSSS_ERR_INVAL,
                "wait rejected for CQ-mode cmd");

    /* Drain so cleanup is clean. */
    struct channel_cmd *out[2] = {0};
    for (int spins = 0; spins < 100000; spins++) {
        if (channel_worker_drain(&w, out, 2) > 0) break;
        sched_yield();
    }

    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* REQ-045: drain on a worker initialised with cq_capacity=0 returns
 * HFSSS_ERR_INVAL. Confirms the legacy/back-compat path stays
 * unambiguously distinct. */
static int test_cq_disabled_drain_rejects(void)
{
    printf("\n=== channel_worker: drain on cq=0 worker rejects ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    media_init(&media, &cfg);

    struct channel_worker w;
    int rc = channel_worker_init(&w, 0, &media, 8, 0);
    TEST_ASSERT(rc == HFSSS_OK, "init cq=0 OK");

    struct channel_cmd *out[2] = {0};
    int n = channel_worker_drain(&w, out, 2);
    TEST_ASSERT(n == HFSSS_ERR_INVAL,
                "drain rejected when CQ disabled");

    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* REQ-045: timestamps are recorded unconditionally — even on the
 * legacy wait/callback path, NOT only when CQ is enabled. */
static int test_timestamps_present_on_legacy_path(void)
{
    printf("\n=== channel_worker: timestamps on cq=0 wait path ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    media_init(&media, &cfg);

    struct channel_worker w;
    int rc = channel_worker_init(&w, 0, &media, 8, 0);
    TEST_ASSERT(rc == HFSSS_OK, "init cq=0 OK");

    struct channel_cmd cmd;
    prep_erase_cmd(&cmd, 0, 0);
    rc = channel_worker_submit(&w, &cmd);
    TEST_ASSERT(rc == HFSSS_OK, "submit OK");

    int wait_rc = channel_cmd_wait(&cmd);
    TEST_ASSERT(wait_rc == HFSSS_OK || wait_rc == 0,
                "wait completes (legacy path still works)");
    TEST_ASSERT(cmd.submit_ts_ns > 0,
                "submit_ts_ns set on legacy path");
    TEST_ASSERT(cmd.complete_ts_ns >= cmd.submit_ts_ns,
                "complete_ts_ns >= submit_ts_ns on legacy path");

    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

int main(void)
{
    printf("========================================\n");
    printf("   Channel Worker (REQ-044 / REQ-045)   \n");
    printf("========================================\n");

    test_submit_wait_completion_cb();
    test_erase_program_read_roundtrip();
    test_full_ring_returns_busy();
    test_submit_after_stop_returns_busy();
    test_cross_channel_cmd_rejected();
    test_two_workers_isolated();
    test_null_inputs();
    test_cq_basic_drain();
    test_cq_empty_drain();
    test_cq_batching_under_slow_consumer();
    test_cq_back_pressure_to_submit_ring();
    test_cq_rejects_on_complete();
    test_cq_rejects_wait();
    test_cq_disabled_drain_rejects();
    test_timestamps_present_on_legacy_path();

    printf("\n========================================\n");
    printf("Tests run:    %d\n", tests_run);
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);
    printf("========================================\n");
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}
