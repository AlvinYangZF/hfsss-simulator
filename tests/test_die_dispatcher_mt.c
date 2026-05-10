/*
 * L2 multi-thread tests for the FTL die-busy wait queue. Each case spawns
 * worker threads that call die_dispatcher_wait against a real dispatcher
 * installed on a real nand_device, while a mock cmd_engine in the test
 * harness drives completions by calling the registered notifier directly.
 *
 *   - Concurrent enqueue + signal: 8 threads block on the same die; 8 mock
 *     completions wake them; every wake corresponds to exactly one waiter.
 *   - Lost-wakeup defense: notifier called on an empty queue is a no-op;
 *     a subsequent waiter still wakes on the next notifier call.
 *   - Shutdown cancellation: 8 threads block; destroy releases all of them
 *     with HFSSS_ERR_SHUTDOWN.
 *   - Timed wait: a single waiter with max_wait_ms=100 returns ETIMEDOUT
 *     when the notifier never fires, and self-removes from the queue so a
 *     second waiter still wakes normally.
 *   - Lock-order stress: many enqueue threads + many notifier threads on
 *     different dies for ~1 second — no hangs, no crashes.
 *
 * The test harness joins worker threads before tearing down media/dispatcher
 * memory; this matches the precondition documented on die_dispatcher_destroy.
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmd_engine_mock.h"
#include "common/common.h"
#include "ftl/die_dispatcher.h"
#include "media/media.h"
#include "media/nand.h"

#define TEST_PASS 0
#define TEST_FAIL 1

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
/* Test fixture                                                       */
/* ------------------------------------------------------------------ */

struct fixture {
    struct media_ctx       media;
    struct die_dispatcher *disp;
    struct mock_engine    *mock;
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
    f->mock = mock_engine_create(f->media.nand);
    if (!f->mock) {
        die_dispatcher_destroy(f->disp);
        media_cleanup(&f->media);
        return -1;
    }
    return 0;
}

static void fixture_teardown(struct fixture *f)
{
    if (f->mock) {
        mock_engine_destroy(f->mock);
        f->mock = NULL;
    }
    if (f->disp) {
        die_dispatcher_destroy(f->disp);
        f->disp = NULL;
    }
    media_cleanup(&f->media);
}

/* ------------------------------------------------------------------ */
/* Case 1 — concurrent enqueue + 1:1 wakeup                           */
/* ------------------------------------------------------------------ */

#define WAKE_THREAD_COUNT 8

struct wake_arg {
    struct die_dispatcher *disp;
    u32                    ch, chip, die;
    atomic_int            *enqueued;
    atomic_int            *woke;
    int                    rc;
};

static void *wake_worker(void *vp)
{
    struct wake_arg *a = vp;
    atomic_fetch_add(a->enqueued, 1);
    a->rc = die_dispatcher_wait(a->disp, a->ch, a->chip, a->die,
                                DIE_PRIO_HOST_READ, /*max_wait_ms=*/0);
    if (a->rc == 0) {
        atomic_fetch_add(a->woke, 1);
    }
    return NULL;
}

static int test_signal_wakes_exactly_one_waiter(void)
{
    printf("\n=== one notifier call wakes exactly one waiter (x8) ===\n");

    struct fixture f;
    if (fixture_setup(&f) != 0) {
        TEST_ASSERT(0, "fixture setup");
        return TEST_FAIL;
    }

    pthread_t tids[WAKE_THREAD_COUNT];
    struct wake_arg args[WAKE_THREAD_COUNT];
    atomic_int enqueued = 0;
    atomic_int woke = 0;

    for (int i = 0; i < WAKE_THREAD_COUNT; i++) {
        args[i].disp = f.disp;
        args[i].ch = 0; args[i].chip = 0; args[i].die = 0;
        args[i].enqueued = &enqueued;
        args[i].woke = &woke;
        args[i].rc = -999;
        pthread_create(&tids[i], NULL, wake_worker, &args[i]);
    }

    /* Wait for every worker to register before firing completions, so
     * each fire corresponds to a guaranteed-queued waiter. */
    while (atomic_load(&enqueued) < WAKE_THREAD_COUNT) {
        sleep_ns(100000ULL);
    }
    /* Give the threads a moment to actually enter cond_wait. */
    sleep_ns(2000000ULL);

    for (int i = 0; i < WAKE_THREAD_COUNT; i++) {
        mock_engine_complete(f.media.nand, 0, 0, 0);
        /* Tiny gap so each completion races against a different waker. */
        sleep_ns(50000ULL);
    }

    for (int i = 0; i < WAKE_THREAD_COUNT; i++) {
        pthread_join(tids[i], NULL);
    }

    int total_woke = atomic_load(&woke);
    TEST_ASSERT(total_woke == WAKE_THREAD_COUNT,
                "exactly N waiters woke for N completions");

    int rc_zero = 0;
    for (int i = 0; i < WAKE_THREAD_COUNT; i++) {
        if (args[i].rc == 0) {
            rc_zero++;
        }
    }
    TEST_ASSERT(rc_zero == WAKE_THREAD_COUNT,
                "every wait returned 0 (no timeout, no shutdown)");

    fixture_teardown(&f);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------ */
/* Case 2 — lost-wakeup defense                                       */
/* ------------------------------------------------------------------ */

static int test_lost_wakeup_defense_on_empty_queue(void)
{
    printf("\n=== notifier on empty queue is a no-op; later waiter wakes ===\n");

    struct fixture f;
    if (fixture_setup(&f) != 0) {
        TEST_ASSERT(0, "fixture setup");
        return TEST_FAIL;
    }

    /* Fire on an empty queue. Must not crash; nothing to wake. */
    mock_engine_complete(f.media.nand, 1, 0, 0);
    TEST_ASSERT(1, "empty-queue notifier returns cleanly");

    /* Now enqueue a waiter; second notifier call must wake it. */
    pthread_t tid;
    struct wake_arg arg = {
        .disp = f.disp, .ch = 1, .chip = 0, .die = 0,
        .rc = -999,
    };
    atomic_int enqueued = 0;
    atomic_int woke = 0;
    arg.enqueued = &enqueued;
    arg.woke = &woke;
    pthread_create(&tid, NULL, wake_worker, &arg);

    while (atomic_load(&enqueued) < 1) {
        sleep_ns(100000ULL);
    }
    sleep_ns(2000000ULL);

    mock_engine_complete(f.media.nand, 1, 0, 0);
    pthread_join(tid, NULL);

    TEST_ASSERT(arg.rc == 0, "subsequent waiter woke with rc=0");

    fixture_teardown(&f);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------ */
/* Case 3 — shutdown cancels active waiters                           */
/* ------------------------------------------------------------------ */

#define SHUTDOWN_THREAD_COUNT 8

struct shutdown_arg {
    struct die_dispatcher *disp;
    u32                    ch, chip, die;
    atomic_int            *enqueued;
    int                    rc;
};

static void *shutdown_worker(void *vp)
{
    struct shutdown_arg *a = vp;
    atomic_fetch_add(a->enqueued, 1);
    a->rc = die_dispatcher_wait(a->disp, a->ch, a->chip, a->die,
                                DIE_PRIO_HOST_READ, /*max_wait_ms=*/0);
    return NULL;
}

static int test_shutdown_cancels_active_waiters(void)
{
    printf("\n=== die_dispatcher_destroy cancels all active waiters ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx mctx;
    int ret = media_init(&mctx, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "media_init OK");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }
    struct die_dispatcher *disp = die_dispatcher_create(mctx.nand);
    TEST_ASSERT(disp != NULL, "dispatcher_create OK");

    pthread_t tids[SHUTDOWN_THREAD_COUNT];
    struct shutdown_arg args[SHUTDOWN_THREAD_COUNT];
    atomic_int enqueued = 0;
    for (int i = 0; i < SHUTDOWN_THREAD_COUNT; i++) {
        args[i].disp = disp;
        args[i].ch = 2; args[i].chip = 0; args[i].die = 0;
        args[i].enqueued = &enqueued;
        args[i].rc = -999;
        pthread_create(&tids[i], NULL, shutdown_worker, &args[i]);
    }
    while (atomic_load(&enqueued) < SHUTDOWN_THREAD_COUNT) {
        sleep_ns(100000ULL);
    }
    sleep_ns(2000000ULL);

    /*
     * Destroy fires the cancellation signal under each queue's lock;
     * each blocked waiter exits its cv loop with shutdown=true. Joining
     * before media_cleanup ensures no thread is still in dispatcher_wait
     * when the dispatcher's queue memory is freed.
     */
    die_dispatcher_destroy(disp);

    for (int i = 0; i < SHUTDOWN_THREAD_COUNT; i++) {
        pthread_join(tids[i], NULL);
    }

    int shutdown_count = 0;
    for (int i = 0; i < SHUTDOWN_THREAD_COUNT; i++) {
        if (args[i].rc == HFSSS_ERR_SHUTDOWN) {
            shutdown_count++;
        }
    }
    TEST_ASSERT(shutdown_count == SHUTDOWN_THREAD_COUNT,
                "every waiter returned HFSSS_ERR_SHUTDOWN");

    media_cleanup(&mctx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------ */
/* Case 4 — timed wait times out and self-removes                     */
/* ------------------------------------------------------------------ */

struct timed_arg {
    struct die_dispatcher *disp;
    u32                    ch, chip, die;
    u32                    max_wait_ms;
    atomic_int            *enqueued;
    int                    rc;
};

static void *timed_worker(void *vp)
{
    struct timed_arg *a = vp;
    atomic_fetch_add(a->enqueued, 1);
    a->rc = die_dispatcher_wait(a->disp, a->ch, a->chip, a->die,
                                DIE_PRIO_HOST_READ, a->max_wait_ms);
    return NULL;
}

static int test_timed_wait_times_out(void)
{
    printf("\n=== timed wait returns ETIMEDOUT and self-removes ===\n");

    struct fixture f;
    if (fixture_setup(&f) != 0) {
        TEST_ASSERT(0, "fixture setup");
        return TEST_FAIL;
    }

    /* First waiter: 100 ms timeout, no notifier — must time out. */
    pthread_t t1;
    atomic_int enq1 = 0;
    struct timed_arg a1 = {
        .disp = f.disp, .ch = 3, .chip = 0, .die = 0,
        .max_wait_ms = 100, .enqueued = &enq1, .rc = -999,
    };
    pthread_create(&t1, NULL, timed_worker, &a1);
    pthread_join(t1, NULL);

    TEST_ASSERT(a1.rc == ETIMEDOUT,
                "100 ms wait with no notifier returns ETIMEDOUT");

    /*
     * Now a second waiter on the same die. If the first waiter had failed
     * to self-remove, it would still be at the head and the second would
     * never wake (the notifier would pop and signal a stale stack frame).
     * Confirming the second waiter wakes proves the queue is empty.
     */
    pthread_t t2;
    atomic_int enq2 = 0;
    struct wake_arg a2 = {
        .disp = f.disp, .ch = 3, .chip = 0, .die = 0,
        .enqueued = &enq2,
    };
    atomic_int woke2 = 0;
    a2.woke = &woke2;
    a2.rc = -999;
    pthread_create(&t2, NULL, wake_worker, &a2);

    while (atomic_load(&enq2) < 1) {
        sleep_ns(100000ULL);
    }
    sleep_ns(2000000ULL);

    mock_engine_complete(f.media.nand, 3, 0, 0);
    pthread_join(t2, NULL);

    TEST_ASSERT(a2.rc == 0,
                "second waiter wakes (timed-out waiter cleaned up)");

    fixture_teardown(&f);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------ */
/* Case 5 — lock-order stress                                         */
/* ------------------------------------------------------------------ */

#define STRESS_WAIT_THREADS    8
#define STRESS_NOTIFY_THREADS  4
#define STRESS_DURATION_MS     1000

struct stress_state {
    struct die_dispatcher *disp;
    struct nand_device    *dev;
    u32                    channel_count;
    atomic_bool            stop;
    atomic_int             waits_total;
    atomic_int             notifies_total;
};

static void *stress_wait_thread(void *vp)
{
    struct stress_state *s = vp;
    while (!atomic_load(&s->stop)) {
        u32 ch = (u32)(rand() % s->channel_count);
        int rc = die_dispatcher_wait(s->disp, ch, 0, 0,
                                     DIE_PRIO_HOST_READ, /*max_wait_ms=*/5);
        atomic_fetch_add(&s->waits_total, 1);
        (void)rc;
    }
    return NULL;
}

static void *stress_notify_thread(void *vp)
{
    struct stress_state *s = vp;
    while (!atomic_load(&s->stop)) {
        u32 ch = (u32)(rand() % s->channel_count);
        mock_engine_complete(s->dev, ch, 0, 0);
        atomic_fetch_add(&s->notifies_total, 1);
        sleep_ns(10000ULL);
    }
    return NULL;
}

static int test_lock_order_stress(void)
{
    printf("\n=== lock-order stress: concurrent waits + notifies (1s) ===\n");

    struct fixture f;
    if (fixture_setup(&f) != 0) {
        TEST_ASSERT(0, "fixture setup");
        return TEST_FAIL;
    }

    struct stress_state s = {
        .disp = f.disp,
        .dev = f.media.nand,
        .channel_count = f.media.nand->channel_count,
    };
    atomic_init(&s.stop, false);
    atomic_init(&s.waits_total, 0);
    atomic_init(&s.notifies_total, 0);

    pthread_t waits[STRESS_WAIT_THREADS];
    pthread_t notifies[STRESS_NOTIFY_THREADS];
    for (int i = 0; i < STRESS_WAIT_THREADS; i++) {
        pthread_create(&waits[i], NULL, stress_wait_thread, &s);
    }
    for (int i = 0; i < STRESS_NOTIFY_THREADS; i++) {
        pthread_create(&notifies[i], NULL, stress_notify_thread, &s);
    }

    sleep_ns((u64)STRESS_DURATION_MS * 1000000ULL);
    atomic_store(&s.stop, true);

    for (int i = 0; i < STRESS_WAIT_THREADS; i++) {
        pthread_join(waits[i], NULL);
    }
    for (int i = 0; i < STRESS_NOTIFY_THREADS; i++) {
        pthread_join(notifies[i], NULL);
    }

    int waits_total = atomic_load(&s.waits_total);
    int notifies_total = atomic_load(&s.notifies_total);
    printf("    (stress completed: %d waits, %d notifies)\n",
           waits_total, notifies_total);
    TEST_ASSERT(waits_total > 0,
                "stress drove at least one wait without hanging");
    TEST_ASSERT(notifies_total > 0,
                "stress drove at least one notify without hanging");

    fixture_teardown(&f);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printf("========================================\n");
    printf("FTL die_dispatcher L2 multi-thread tests\n");
    printf("========================================\n");

    test_signal_wakes_exactly_one_waiter();
    test_lost_wakeup_defense_on_empty_queue();
    test_shutdown_cancels_active_waiters();
    test_timed_wait_times_out();
    test_lock_order_stress();

    printf("\n========================================\n");
    printf("die_dispatcher L2 Test Summary\n");
    printf("========================================\n");
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    return tests_failed == 0 ? 0 : 1;
}
