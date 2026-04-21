#include <pthread.h>
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
    rc = channel_worker_init(&w, 0, &media, 64);
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
    rc = channel_worker_init(&w, 0, &media, 32);
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
 * Ring-full back-pressure: when the SPSC ring is saturated,
 * channel_worker_submit returns HFSSS_ERR_BUSY rather than blocking.
 * Uses a small ring (capacity 2) and submits faster than the worker
 * can drain by pre-filling before yielding.
 *
 * The worker thread is running and draining, so we can't deterministically
 * hit "full" via a timing race. Instead we spoof: stop the worker, fill
 * the ring, and assert the (N+1)th submit fails. Worker is never
 * restarted; cleanup drains the ring via free (not dispatch) which is
 * acceptable for this test.
 */
static int test_full_ring_returns_busy(void)
{
    printf("\n=== channel_worker: submit returns BUSY when ring is full ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    int rc = media_init(&media, &cfg);
    TEST_ASSERT(rc == HFSSS_OK, "media_init OK");

    struct channel_worker w;
    rc = channel_worker_init(&w, 0, &media, 2);
    TEST_ASSERT(rc == HFSSS_OK, "channel_worker_init OK (cap=2)");

    /* Signal stop so the worker stops dequeuing. It may still drain one
     * command that was already pulled; we accept that and overshoot. */
    channel_worker_stop(&w);
    pthread_join(w.thread, NULL);
    w.thread_started = false;

    struct channel_cmd c1, c2, c3;
    memset(&c1, 0, sizeof(c1));
    memset(&c2, 0, sizeof(c2));
    memset(&c3, 0, sizeof(c3));
    c1.op = CHANNEL_CMD_ERASE;
    c2.op = CHANNEL_CMD_ERASE;
    c3.op = CHANNEL_CMD_ERASE;

    TEST_ASSERT(channel_worker_submit(&w, &c1) == HFSSS_OK, "submit 1 OK (slot 1/2)");
    TEST_ASSERT(channel_worker_submit(&w, &c2) == HFSSS_OK, "submit 2 OK (slot 2/2)");
    TEST_ASSERT(channel_worker_submit(&w, &c3) == HFSSS_ERR_BUSY, "submit 3 returns BUSY (full)");

    channel_worker_cleanup(&w);
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
    TEST_ASSERT(channel_worker_init(&w0, 0, &media, 16) == HFSSS_OK, "worker 0 init");
    TEST_ASSERT(channel_worker_init(&w1, 1, &media, 16) == HFSSS_OK, "worker 1 init");

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

    TEST_ASSERT(channel_worker_init(NULL, 0, NULL, 16) == HFSSS_ERR_INVAL, "init(NULL, 0, NULL, _) INVAL");
    channel_worker_stop(NULL);
    channel_worker_cleanup(NULL);
    TEST_ASSERT(channel_worker_submit(NULL, NULL) == HFSSS_ERR_INVAL, "submit(NULL, NULL) INVAL");
    TEST_ASSERT(channel_cmd_wait(NULL) == HFSSS_ERR_INVAL, "wait(NULL) INVAL");
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
    test_two_workers_isolated();
    test_null_inputs();

    printf("\n========================================\n");
    printf("Tests run:    %d\n", tests_run);
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);
    printf("========================================\n");
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}
