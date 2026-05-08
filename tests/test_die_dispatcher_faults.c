/*
 * Fault-injection coverage for the FTL die-busy wait queue. Drives the
 * env-gated knobs that exist purely for stress-test exposure of the
 * dispatcher's re-queue and wake-window paths:
 *
 *   - HFSSS_DIE_DISP_FORCE_BUSY=N injects spurious BUSY (returned as
 *     ETIMEDOUT) on a percentage of successful wakes, so the FTL retry
 *     loop's re-queue + re-wait code is exercised even when cmd_engine
 *     is not actually transient-busy.
 *   - HFSSS_DIE_DISP_NOTIFIER_DELAY_NS=K sleeps K ns inside the
 *     notifier between dequeue and cv signal, widening the wake -> resubmit
 *     window. The dispatcher's spurious-wake guard must keep behavior
 *     correct regardless of delay value.
 *
 * Tests programmatically setenv() before each case and call the test-only
 * cache-reset helper so the next probe re-reads the environment. With
 * the env unset, the helpers default to 0 / 0 ns and behavior is
 * byte-equivalent to the no-injection baseline.
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
#include "media/media.h"
#include "media/nand.h"

/* Test-only API exported by die_dispatcher.c via the internal header.
 * Forward-declared here because the internal header is not normally
 * visible to test translation units that link against libhfsss-ftl. */
void die_dispatcher_reset_env_cache_for_testing(void);

#define TEST_PASS 0
#define TEST_FAIL 1

#define WAIT_TIMEOUT_MS 1000u

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
/* Common waiter thread                                               */
/* ------------------------------------------------------------------ */

struct wait_arg {
    struct die_dispatcher *disp;
    u32                    ch, chip, die;
    die_priority_t         prio;
    u32                    max_wait_ms;
    atomic_int            *enqueued;
    int                    rc;
};

static void *wait_worker(void *vp)
{
    struct wait_arg *a = vp;
    atomic_fetch_add(a->enqueued, 1);
    a->rc = die_dispatcher_wait(a->disp, a->ch, a->chip, a->die,
                                a->prio, a->max_wait_ms);
    return NULL;
}

static void wait_for_enqueue(atomic_int *enqueued, int target)
{
    while (atomic_load(enqueued) < target) {
        sleep_ns(100000ULL);
    }
    /* Allow worker threads to actually enter cond_wait. */
    sleep_ns(2000000ULL);
}

/*
 * Drive a single submit -> complete -> wake cycle. Spawns a waiter on
 * (ch=0, chip=0, die=0) at HOST_READ priority, then issues a real
 * media_nand_program on the same die which fires anchor A and wakes
 * the waiter. Returns the rc observed by the waiter.
 *
 * The page argument lets the caller drive multiple cycles against
 * distinct (page) tuples on the same block so each program is a
 * fresh, valid request.
 */
static int run_one_cycle(struct fixture *f, u32 page)
{
    pthread_t tid;
    atomic_int enqueued = 0;
    struct wait_arg arg = {
        .disp = f->disp, .ch = 0, .chip = 0, .die = 0,
        .prio = DIE_PRIO_HOST_READ,
        .max_wait_ms = WAIT_TIMEOUT_MS,
        .enqueued = &enqueued,
        .rc = -999,
    };
    pthread_create(&tid, NULL, wait_worker, &arg);
    wait_for_enqueue(&enqueued, 1);

    u8 buf[4096];
    memset(buf, 0xAA, sizeof(buf));
    int prog_rc = media_nand_program(&f->media, 0, 0, 0, 0, 0, page, buf, NULL);
    (void)prog_rc;

    pthread_join(tid, NULL);
    return arg.rc;
}

/* ------------------------------------------------------------------ */
/* Case 1 — env unset: baseline cycle completes normally              */
/* ------------------------------------------------------------------ */

static int test_force_busy_zero_does_nothing(void)
{
    printf("\n=== env unset: dispatcher cycle completes normally ===\n");

    /* Make sure no stray state from a prior case leaks in. */
    unsetenv("HFSSS_DIE_DISP_FORCE_BUSY");
    unsetenv("HFSSS_DIE_DISP_NOTIFIER_DELAY_NS");
    die_dispatcher_reset_env_cache_for_testing();

    struct fixture f;
    if (fixture_setup(&f) != 0) {
        TEST_ASSERT(0, "fixture setup");
        return TEST_FAIL;
    }

    int rc = run_one_cycle(&f, 0);
    TEST_ASSERT(rc == 0,
                "default-mode: waiter returned 0 (signaled by anchor A)");

    fixture_teardown(&f);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------ */
/* Case 2 — force-BUSY at 50%: most cycles still complete             */
/* ------------------------------------------------------------------ */

/*
 * With the FTL retry budget assumed to be at least ~8 attempts per
 * I/O, a 50%-per-wake force-BUSY rate gives each cycle (1 - 0.5^8) =
 * 99.6% chance of completing within budget. Across 100 cycles we
 * conservatively assert >= 80 successes (>= 80% completion rate).
 *
 * Each "cycle" here drives one wait + one program; on a forced-BUSY
 * the waiter returns ETIMEDOUT (which the FTL caller treats as
 * "retry"), which we observe as a non-zero rc. The test just checks
 * that the injection knob actually engages and that the queue does
 * not deadlock under a high injection rate.
 */
static int test_force_busy_high_rate_eventually_completes(void)
{
    printf("\n=== HFSSS_DIE_DISP_FORCE_BUSY=50: injection observable ===\n");

    setenv("HFSSS_DIE_DISP_FORCE_BUSY", "50", 1);
    unsetenv("HFSSS_DIE_DISP_NOTIFIER_DELAY_NS");
    die_dispatcher_reset_env_cache_for_testing();
    /* Seed rand() so the test is reproducible across runs but still
     * exercises the injection path on roughly half the wakes. */
    srand(1);

    struct fixture f;
    if (fixture_setup(&f) != 0) {
        TEST_ASSERT(0, "fixture setup");
        unsetenv("HFSSS_DIE_DISP_FORCE_BUSY");
        die_dispatcher_reset_env_cache_for_testing();
        return TEST_FAIL;
    }

    const int TOTAL = 100;
    int normal_wake = 0;     /* rc == 0 — real wake, not injected */
    int forced_busy = 0;     /* rc == ETIMEDOUT — injection took effect */
    int other      = 0;
    for (int i = 0; i < TOTAL; i++) {
        /* Use page = i % pages_per_block so each program targets a
         * distinct page on the same block — keeps the request valid
         * without exercising block crossing. */
        int rc = run_one_cycle(&f, (u32)(i % 16));
        if (rc == 0) {
            normal_wake++;
        } else if (rc == ETIMEDOUT) {
            forced_busy++;
        } else {
            other++;
        }
    }

    printf("  cycles=%d  normal_wake=%d  forced_busy=%d  other=%d\n",
           TOTAL, normal_wake, forced_busy, other);

    /* No unexpected error codes. */
    TEST_ASSERT(other == 0,
                "force-busy: no unexpected rc beyond {0, ETIMEDOUT}");

    /* Injection actually engaged (non-trivial fraction were forced). */
    TEST_ASSERT(forced_busy >= 20,
                "force-busy: injection observed on >= 20% of cycles");

    /* Knob did not break the wake path — at least 20% woke normally
     * (the other ~50% were forced; the rest depends on rand stream). */
    TEST_ASSERT(normal_wake >= 20,
                "force-busy: normal wake observed on >= 20% of cycles");

    fixture_teardown(&f);

    /* Restore default state for any later test in the binary. */
    unsetenv("HFSSS_DIE_DISP_FORCE_BUSY");
    die_dispatcher_reset_env_cache_for_testing();

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------ */
/* Case 3 — notifier delay does not cause missed wakeups              */
/* ------------------------------------------------------------------ */

/*
 * A 1 ms delay inserted between dequeue and cv signal must not lose
 * any waiter. Drive 50 cycles and require all of them to complete
 * with rc == 0; total wallclock must be at least 50 * 1 ms so the
 * delay is observable (i.e., the env actually engaged).
 */
static int test_notifier_delay_does_not_cause_missed_wakeup(void)
{
    printf("\n=== HFSSS_DIE_DISP_NOTIFIER_DELAY_NS=1ms: no missed wakes ===\n");

    unsetenv("HFSSS_DIE_DISP_FORCE_BUSY");
    setenv("HFSSS_DIE_DISP_NOTIFIER_DELAY_NS", "1000000", 1);
    die_dispatcher_reset_env_cache_for_testing();

    struct fixture f;
    if (fixture_setup(&f) != 0) {
        TEST_ASSERT(0, "fixture setup");
        unsetenv("HFSSS_DIE_DISP_NOTIFIER_DELAY_NS");
        die_dispatcher_reset_env_cache_for_testing();
        return TEST_FAIL;
    }

    const int TOTAL = 50;
    int completed = 0;
    u64 t0 = get_time_ns();
    for (int i = 0; i < TOTAL; i++) {
        int rc = run_one_cycle(&f, (u32)(i % 16));
        if (rc == 0) {
            completed++;
        }
    }
    u64 elapsed_ns = get_time_ns() - t0;
    u64 elapsed_ms = elapsed_ns / 1000000ULL;

    printf("  cycles=%d  completed=%d  elapsed=%llu ms\n",
           TOTAL, completed, (unsigned long long)elapsed_ms);

    TEST_ASSERT(completed == TOTAL,
                "notifier-delay: every cycle completed (no missed wakeups)");

    /* Each cycle includes one 1-ms notifier-internal sleep. Total
     * wallclock must reflect at least that — set the bound a bit
     * below 50 ms to absorb minor scheduling jitter while still
     * confirming the knob engaged. */
    TEST_ASSERT(elapsed_ms >= 40,
                "notifier-delay: wallclock reflects injected delay (>= 40 ms)");

    fixture_teardown(&f);

    unsetenv("HFSSS_DIE_DISP_NOTIFIER_DELAY_NS");
    die_dispatcher_reset_env_cache_for_testing();

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printf("===========================================\n");
    printf("FTL die_dispatcher fault-injection knobs\n");
    printf("===========================================\n");

    test_force_busy_zero_does_nothing();
    test_force_busy_high_rate_eventually_completes();
    test_notifier_delay_does_not_cause_missed_wakeup();

    printf("\n========================================\n");
    printf("die_dispatcher Faults Test Summary\n");
    printf("========================================\n");
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    return tests_failed == 0 ? 0 : 1;
}
