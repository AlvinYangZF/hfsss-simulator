/*
 * L3 integration tests for the FTL die-busy wait queue against the real
 * cmd_engine. Each case spins up a real nand_device via media_init,
 * installs the dispatcher, and drives end-to-end submit -> complete ->
 * notify cycles through the real anchor A / anchor B paths in cmd_engine.
 *
 *   - Anchor A: a normal program completion fires the notifier and wakes
 *     a queued waiter on the matching die.
 *   - Anchor B: a reset against an open cache sequence fires the notifier
 *     via the force-clear path and wakes a queued waiter.
 *   - Anchor C: the implicit cache-terminate transient at submit entry
 *     does not wake. Only the new op's anchor-A completion wakes.
 *   - Multi-waiter priority: three waiters at different priorities woken
 *     by three completions emerge in T1 -> T2 -> T4 order.
 *
 * Production code is exercised unmodified: the dispatcher installs itself
 * as dev->die_ready_notifier and listens to anchors fired by cmd_engine.
 *
 * All dispatcher_wait calls use a millisecond timeout so a broken test
 * cannot hang the parent make test invocation.
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/common.h"
#include "ftl/die_dispatcher.h"
#include "media/cmd_engine.h"
#include "media/cmd_state.h"
#include "media/media.h"
#include "media/nand.h"
#include "media/nand_profile.h"

#define TEST_PASS 0
#define TEST_FAIL 1

#define WAIT_TIMEOUT_MS 1000u
#define WAIT_BOUND_MS   500u

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                                                 \
    do {                                                                       \
        tests_run++;                                                           \
        if (cond) {                                                            \
            printf("  [PASS] %s\n", msg);                                      \
            tests_passed++;                                                    \
        } else {                                                               \
            printf("  [FAIL] %s\n", msg);                                      \
            tests_failed++;                                                    \
        }                                                                      \
    } while (0)

/* ------------------------------------------------------------------ */
/* Fixture                                                            */
/* ------------------------------------------------------------------ */

struct fixture {
    struct media_ctx       media;
    struct die_dispatcher *disp;
};

static struct media_config make_cfg(void)
{
    struct media_config cfg = {
        .channel_count     = 4,
        .chips_per_channel = 1,
        .dies_per_chip     = 1,
        .planes_per_die    = 1,
        .blocks_per_plane  = 8,
        .pages_per_block   = 16,
        .page_size         = 4096,
        .spare_size        = 64,
        .nand_type         = NAND_TYPE_TLC,
    };
    return cfg;
}

static int fixture_setup(struct fixture *f)
{
    memset(f, 0, sizeof(*f));
    struct media_config cfg = make_cfg();
    if (media_init(&f->media, &cfg) != HFSSS_OK) {
        return -1;
    }
    f->disp = die_dispatcher_create(f->media.nand);
    if (!f->disp) {
        media_cleanup(&f->media);
        return -1;
    }
    return 0;
}

static void fixture_teardown(struct fixture *f)
{
    if (f->disp) {
        die_dispatcher_destroy(f->disp);
        f->disp = NULL;
    }
    media_cleanup(&f->media);
}

/* ------------------------------------------------------------------ */
/* Notifier-wrapper helpers (used by anchor-C test only)              */
/* ------------------------------------------------------------------ */

/*
 * Counter wrapper: reads the dispatcher pointer from dev->die_ready_ctx,
 * forwards to die_dispatcher_on_die_ready, and increments a counter so the
 * test can observe how many notifications fired across the test interval.
 */
static atomic_int g_notify_count;

static void counter_wrapper_notifier(struct nand_device *dev,
                                     u32 ch, u32 chip, u32 die)
{
    atomic_fetch_add(&g_notify_count, 1);
    die_dispatcher_on_die_ready(dev, ch, chip, die);
}

/* ------------------------------------------------------------------ */
/* Common waiter thread                                               */
/* ------------------------------------------------------------------ */

struct wait_arg {
    struct die_dispatcher *disp;
    u32                    ch, chip, die;
    die_priority_t         prio;
    u32                    max_wait_ms;
    atomic_int            *enqueued;
    atomic_int            *wake_index;     /* incremented at wake; recorded */
    int                    wake_order;     /* assigned wake_index value */
    int                    rc;
    u64                    wake_ns;
};

static void *wait_worker(void *vp)
{
    struct wait_arg *a = vp;
    atomic_fetch_add(a->enqueued, 1);
    a->rc = die_dispatcher_wait(a->disp, a->ch, a->chip, a->die,
                                a->prio, a->max_wait_ms);
    a->wake_ns = get_time_ns();
    if (a->wake_index) {
        a->wake_order = atomic_fetch_add(a->wake_index, 1);
    }
    return NULL;
}

static void wait_for_enqueue(atomic_int *enqueued, int target)
{
    while (atomic_load(enqueued) < target) {
        sleep_ns(100000ULL);
    }
    /* Give the threads a moment to actually enter cond_wait. */
    sleep_ns(2000000ULL);
}

/* ------------------------------------------------------------------ */
/* Case 1 — submit -> complete -> notify cycle (anchor A)             */
/* ------------------------------------------------------------------ */

static int test_submit_complete_notify_cycle_anchor_a(void)
{
    printf("\n=== anchor A: submit -> complete -> notify wakes waiter ===\n");

    struct fixture f;
    if (fixture_setup(&f) != 0) {
        TEST_ASSERT(0, "fixture setup");
        return TEST_FAIL;
    }

    /* The dispatcher_create() must have installed the notifier hook. */
    TEST_ASSERT(f.media.nand->die_ready_notifier == die_dispatcher_on_die_ready,
                "dispatcher_create installed die_ready_notifier");
    TEST_ASSERT(f.media.nand->die_ready_ctx == f.disp,
                "dispatcher_create installed die_ready_ctx");

    pthread_t tid;
    atomic_int enqueued = 0;
    struct wait_arg arg = {
        .disp = f.disp, .ch = 0, .chip = 0, .die = 0,
        .prio = DIE_PRIO_HOST_READ,
        .max_wait_ms = WAIT_TIMEOUT_MS,
        .enqueued = &enqueued,
        .wake_index = NULL,
        .wake_order = -1,
        .rc = -999,
        .wake_ns = 0,
    };
    pthread_create(&tid, NULL, wait_worker, &arg);
    wait_for_enqueue(&enqueued, 1);

    u64 submit_start_ns = get_time_ns();

    /* The program completes synchronously inside cmd_engine and fires
     * anchor A's notifier inline, which wakes the queued waiter. */
    u8 buf[4096];
    memset(buf, 0x5A, sizeof(buf));
    int prog_rc = media_nand_program(&f.media, 0, 0, 0, 0, 0, 0, buf, NULL);
    TEST_ASSERT(prog_rc == HFSSS_OK, "anchor-A: program submit returns OK");

    pthread_join(tid, NULL);

    TEST_ASSERT(arg.rc == 0,
                "anchor-A: waiter returned 0 (signaled, not timeout, not shutdown)");

    u64 elapsed_ms = (arg.wake_ns - submit_start_ns) / 1000000ULL;
    TEST_ASSERT(elapsed_ms < WAIT_BOUND_MS,
                "anchor-A: waiter woke within wall-clock bound");

    fixture_teardown(&f);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------ */
/* Case 2 — reset force-clear (anchor B) wakes waiter                 */
/* ------------------------------------------------------------------ */

static int test_reset_notify_anchor_b(void)
{
    printf("\n=== anchor B: reset force-clear wakes waiter ===\n");

    struct fixture f;
    if (fixture_setup(&f) != 0) {
        TEST_ASSERT(0, "fixture setup");
        return TEST_FAIL;
    }

    /* Open a cache program on (ch=1, chip=0, die=0). The first cache
     * program completes its xfer phase but leaves cache_active=true on
     * the die for the trailing tCBSY window — a subsequent reset must
     * traverse the force-clear path that fires anchor B. */
    u8 buf[4096];
    memset(buf, 0xA5, sizeof(buf));
    int cache_rc = media_nand_cache_program(&f.media, 1, 0, 0, 0, 0, 0, buf, NULL);
    TEST_ASSERT(cache_rc == HFSSS_OK, "anchor-B: cache_program submit returns OK");

    /* Cache program's own anchor-A may have fired; ignore that wake by
     * queueing the waiter only AFTER cache submit returns. */
    pthread_t tid;
    atomic_int enqueued = 0;
    struct wait_arg arg = {
        .disp = f.disp, .ch = 1, .chip = 0, .die = 0,
        .prio = DIE_PRIO_HOST_READ,
        .max_wait_ms = WAIT_TIMEOUT_MS,
        .enqueued = &enqueued,
        .wake_index = NULL,
        .wake_order = -1,
        .rc = -999,
        .wake_ns = 0,
    };
    pthread_create(&tid, NULL, wait_worker, &arg);
    wait_for_enqueue(&enqueued, 1);

    u64 submit_start_ns = get_time_ns();

    struct nand_cmd_target target = {
        .ch = 1,
        .chip = 0,
        .die = 0,
        .plane_mask = 1u,
        .block = 0,
        .page = 0,
    };
    int reset_rc = nand_cmd_engine_submit_reset(f.media.nand, &target);
    TEST_ASSERT(reset_rc == HFSSS_OK, "anchor-B: reset submit returns OK");

    pthread_join(tid, NULL);

    TEST_ASSERT(arg.rc == 0,
                "anchor-B: waiter returned 0 (woken by reset's anchor B)");

    u64 elapsed_ms = (arg.wake_ns - submit_start_ns) / 1000000ULL;
    TEST_ASSERT(elapsed_ms < WAIT_BOUND_MS,
                "anchor-B: waiter woke within wall-clock bound");

    fixture_teardown(&f);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------ */
/* Case 3 — anchor C does not wake (only the read's anchor A does)    */
/* ------------------------------------------------------------------ */

/*
 * A non-cache READ submitted while a cache sequence is open hits anchor
 * C's "implicit cache terminate" branch at submit entry. That transient
 * IDLE must not fire the notifier; only the read's own anchor-A
 * completion fires. We install a counter wrapper so we can observe the
 * exact number of notifier invocations across the read.
 */
static int test_anchor_c_does_not_wake_waiter(void)
{
    printf("\n=== anchor C: implicit cache terminate does NOT wake waiter ===\n");

    struct fixture f;
    if (fixture_setup(&f) != 0) {
        TEST_ASSERT(0, "fixture setup");
        return TEST_FAIL;
    }

    /* Replace the dispatcher's notifier with a counter wrapper that
     * forwards to die_dispatcher_on_die_ready. The dispatcher's ctx is
     * still in dev->die_ready_ctx, so forwarded calls reach the right
     * dispatcher. */
    f.media.nand->die_ready_notifier = counter_wrapper_notifier;
    /* die_ready_ctx already points to f.disp from dispatcher_create. */
    atomic_store(&g_notify_count, 0);

    /* Open the cache program on (ch=2, chip=0, die=0). This fires
     * anchor A on the cache program's own completion (count goes to 1). */
    u8 buf[4096];
    memset(buf, 0xC3, sizeof(buf));
    int cache_rc = media_nand_cache_program(&f.media, 2, 0, 0, 0, 0, 0, buf, NULL);
    TEST_ASSERT(cache_rc == HFSSS_OK, "anchor-C: cache_program submit returns OK");

    int after_cache = atomic_load(&g_notify_count);

    /* Queue a waiter on the same die. */
    pthread_t tid;
    atomic_int enqueued = 0;
    struct wait_arg arg = {
        .disp = f.disp, .ch = 2, .chip = 0, .die = 0,
        .prio = DIE_PRIO_HOST_READ,
        .max_wait_ms = WAIT_TIMEOUT_MS,
        .enqueued = &enqueued,
        .wake_index = NULL,
        .wake_order = -1,
        .rc = -999,
        .wake_ns = 0,
    };
    pthread_create(&tid, NULL, wait_worker, &arg);
    wait_for_enqueue(&enqueued, 1);

    /* A non-cache READ on the same die hits anchor C at submit entry.
     * If anchor C were wired to the notifier, the waiter would wake on
     * the transient IDLE. The expected behaviour is that anchor C does
     * NOT fire and only anchor A on the read's completion does — so the
     * notifier increments by exactly 1 across this read. */
    u8 rbuf[4096];
    memset(rbuf, 0, sizeof(rbuf));
    int read_rc = media_nand_read(&f.media, 2, 0, 0, 0, 0, 0, rbuf, NULL);
    TEST_ASSERT(read_rc == HFSSS_OK, "anchor-C: subsequent read submit returns OK");

    pthread_join(tid, NULL);

    int after_read = atomic_load(&g_notify_count);
    int delta = after_read - after_cache;
    TEST_ASSERT(delta == 1,
                "anchor-C: only anchor A fires for the read (delta == 1)");
    TEST_ASSERT(arg.rc == 0,
                "anchor-C: waiter wakes (via the read's anchor A)");

    /* Restore the bare dispatcher notifier so teardown does not chase a
     * stale wrapper across the destroy path. */
    f.media.nand->die_ready_notifier = die_dispatcher_on_die_ready;

    fixture_teardown(&f);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------ */
/* Case 4 — multi-waiter priority order under real cmd_engine timing  */
/* ------------------------------------------------------------------ */

#define PRIO_DIE_CH 3u

/*
 * Three waiters on the same die at different priorities (T4, T2, T1),
 * woken by three sequential program completions. Expected wake order is
 * T1 -> T2 -> T4 (highest priority first). Between completions the
 * test waits for the previous waiter to fully unwind so the next
 * anchor-A fire targets a known queue state.
 */
static int test_multi_waiter_priority_order(void)
{
    printf("\n=== multi-waiter priority: T1 wakes before T2 wakes before T4 ===\n");

    struct fixture f;
    if (fixture_setup(&f) != 0) {
        TEST_ASSERT(0, "fixture setup");
        return TEST_FAIL;
    }

    pthread_t tid_t4, tid_t2, tid_t1;
    atomic_int enqueued = 0;
    atomic_int wake_index = 0;

    struct wait_arg a_t4 = {
        .disp = f.disp, .ch = PRIO_DIE_CH, .chip = 0, .die = 0,
        .prio = DIE_PRIO_GC_IDLE,           /* T4 */
        .max_wait_ms = WAIT_TIMEOUT_MS,
        .enqueued = &enqueued,
        .wake_index = &wake_index,
        .wake_order = -1, .rc = -999, .wake_ns = 0,
    };
    struct wait_arg a_t2 = {
        .disp = f.disp, .ch = PRIO_DIE_CH, .chip = 0, .die = 0,
        .prio = DIE_PRIO_HOST_READ,         /* T2 */
        .max_wait_ms = WAIT_TIMEOUT_MS,
        .enqueued = &enqueued,
        .wake_index = &wake_index,
        .wake_order = -1, .rc = -999, .wake_ns = 0,
    };
    struct wait_arg a_t1 = {
        .disp = f.disp, .ch = PRIO_DIE_CH, .chip = 0, .die = 0,
        .prio = DIE_PRIO_GC_CRITICAL,       /* T1 */
        .max_wait_ms = WAIT_TIMEOUT_MS,
        .enqueued = &enqueued,
        .wake_index = &wake_index,
        .wake_order = -1, .rc = -999, .wake_ns = 0,
    };

    /*
     * Enqueue order is T4, T2, T1 — explicitly NOT priority order — so a
     * priority-blind FIFO would observe T4 -> T2 -> T1 wake order. The
     * priority queue must reorder to T1 -> T2 -> T4. We sleep briefly
     * between thread creates to ensure the dispatcher sees the inserts
     * in source-code order.
     */
    pthread_create(&tid_t4, NULL, wait_worker, &a_t4);
    sleep_ns(1000000ULL);
    pthread_create(&tid_t2, NULL, wait_worker, &a_t2);
    sleep_ns(1000000ULL);
    pthread_create(&tid_t1, NULL, wait_worker, &a_t1);
    wait_for_enqueue(&enqueued, 3);

    /*
     * Drive three program completions. Each anchor-A fires the notifier
     * which pops the highest-priority waiter. We sleep briefly between
     * submits so the just-woken waiter unwinds before the next fire —
     * this keeps the wake_index assignment matched to the wake order.
     */
    u8 buf[4096];
    memset(buf, 0xD1, sizeof(buf));

    int prog_rc;
    /* All three programs target the same die but distinct (block, page)
     * tuples so each request is a fresh, valid program. Plane stays 0 to
     * match the planes_per_die=1 fixture geometry. */
    prog_rc = media_nand_program(&f.media, PRIO_DIE_CH, 0, 0, 0, 0, 0, buf, NULL);
    TEST_ASSERT(prog_rc == HFSSS_OK, "priority: 1st program submit OK");
    sleep_ns(20000000ULL);   /* 20 ms — let the just-woken waiter unwind */

    prog_rc = media_nand_program(&f.media, PRIO_DIE_CH, 0, 0, 0, 0, 1, buf, NULL);
    TEST_ASSERT(prog_rc == HFSSS_OK, "priority: 2nd program submit OK");
    sleep_ns(20000000ULL);

    prog_rc = media_nand_program(&f.media, PRIO_DIE_CH, 0, 0, 0, 0, 2, buf, NULL);
    TEST_ASSERT(prog_rc == HFSSS_OK, "priority: 3rd program submit OK");

    pthread_join(tid_t1, NULL);
    pthread_join(tid_t2, NULL);
    pthread_join(tid_t4, NULL);

    /* All three waiters must have woken with rc == 0. */
    TEST_ASSERT(a_t1.rc == 0, "priority: T1 (GC_CRITICAL) wake rc=0");
    TEST_ASSERT(a_t2.rc == 0, "priority: T2 (HOST_READ)  wake rc=0");
    TEST_ASSERT(a_t4.rc == 0, "priority: T4 (GC_IDLE)    wake rc=0");

    /* Wake order: T1 woke first (index 0), T2 second (1), T4 last (2). */
    TEST_ASSERT(a_t1.wake_order == 0,
                "priority: T1 (GC_CRITICAL) woke first");
    TEST_ASSERT(a_t2.wake_order == 1,
                "priority: T2 (HOST_READ) woke second");
    TEST_ASSERT(a_t4.wake_order == 2,
                "priority: T4 (GC_IDLE) woke last");

    fixture_teardown(&f);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printf("===========================================\n");
    printf("FTL die_dispatcher L3 cmd_engine integration\n");
    printf("===========================================\n");

    test_submit_complete_notify_cycle_anchor_a();
    test_reset_notify_anchor_b();
    test_anchor_c_does_not_wake_waiter();
    test_multi_waiter_priority_order();

    printf("\n========================================\n");
    printf("die_dispatcher L3 Test Summary\n");
    printf("========================================\n");
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    return tests_failed == 0 ? 0 : 1;
}
