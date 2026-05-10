/*
 * L1 unit tests for the FTL die-busy wait queue. Single-threaded, no
 * cmd_engine; exercises the pure data-structure layer:
 *
 *   - create/destroy installs and uninstalls the cmd_engine notifier hook
 *   - tier-priority pop order
 *   - WFQ deficit-round-robin alternation between two slots in T2
 *   - EMA inter-arrival-time tracking and host-pressure predicate
 *   - T4 yield decision under simulated host burst
 *   - gc_trigger_t -> die_priority_t mapping table
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "common/common.h"
#include "ftl/die_dispatcher.h"
#include "media/media.h"
#include "media/nand.h"

/* Pull in the internal types via the dispatcher's private header. */
#include "../src/ftl/die_dispatcher_internal.h"

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

static struct media_config make_cfg(void)
{
    struct media_config cfg = {
        .channel_count = 4,
        .chips_per_channel = 1,
        .dies_per_chip = 1,
        .planes_per_die = 1,
        .blocks_per_plane = 8,
        .pages_per_block = 16,
        .page_size = 4096,
        .spare_size = 64,
        .nand_type = NAND_TYPE_TLC,
    };
    return cfg;
}

/* ------------------------------------------------------------------ */

static int test_create_destroy_is_clean(void)
{
    printf("\n=== create/destroy installs and uninstalls notifier hook ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx ctx;
    int ret = media_init(&ctx, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "media_init OK");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }

    /* Hook starts NULL on a fresh device. */
    TEST_ASSERT(ctx.nand->die_ready_notifier == NULL,
                "notifier is NULL before create");
    TEST_ASSERT(ctx.nand->die_ready_ctx == NULL,
                "ctx is NULL before create");

    struct die_dispatcher *d = die_dispatcher_create(ctx.nand);
    TEST_ASSERT(d != NULL, "create returns non-NULL dispatcher");
    TEST_ASSERT(ctx.nand->die_ready_notifier != NULL,
                "create installs the notifier");
    TEST_ASSERT(ctx.nand->die_ready_ctx == d,
                "create stores dispatcher in die_ready_ctx");

    die_dispatcher_destroy(d);
    TEST_ASSERT(ctx.nand->die_ready_notifier == NULL,
                "destroy uninstalls the notifier");
    TEST_ASSERT(ctx.nand->die_ready_ctx == NULL,
                "destroy clears die_ready_ctx");

    media_cleanup(&ctx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------ */

static int test_pop_picks_highest_tier_first(void)
{
    printf("\n=== pop_next picks highest tier first (T1 > T2 > T4) ===\n");

    struct die_waitqueue q;
    die_waitqueue_init(&q);

    struct die_waiter w_idle, w_read, w_critical;
    die_waiter_init(&w_idle, DIE_PRIO_GC_IDLE);       /* T4 */
    die_waiter_init(&w_read, DIE_PRIO_HOST_READ);     /* T2 slot 0 */
    die_waiter_init(&w_critical, DIE_PRIO_GC_CRITICAL); /* T1 */

    die_waitqueue_enqueue(&q, &w_idle);
    die_waitqueue_enqueue(&q, &w_read);
    die_waitqueue_enqueue(&q, &w_critical);

    /* T1 wins first regardless of insertion order. */
    struct die_waiter *first = die_waitqueue_pop_next(&q);
    TEST_ASSERT(first == &w_critical, "T1 (GC_CRITICAL) pops first");

    /* T2 is next. */
    struct die_waiter *second = die_waitqueue_pop_next(&q);
    TEST_ASSERT(second == &w_read, "T2 (HOST_READ) pops second");

    /* T4 is last; with no host IO recorded, host_bursting is false so it
     * fires. */
    struct die_waiter *third = die_waitqueue_pop_next(&q);
    TEST_ASSERT(third == &w_idle,
                "T4 (GC_IDLE) pops third (host quiet)");

    /* Empty queue. */
    TEST_ASSERT(die_waitqueue_pop_next(&q) == NULL,
                "empty queue returns NULL");

    die_waiter_cleanup(&w_idle);
    die_waiter_cleanup(&w_read);
    die_waiter_cleanup(&w_critical);
    die_waitqueue_cleanup(&q);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------ */

static int test_t2_wfq_alternates_host_read_and_force_gc(void)
{
    printf("\n=== T2 WFQ alternates HOST_READ and GC_FORCE under 1:1 ===\n");

    struct die_waitqueue q;
    die_waitqueue_init(&q);

    struct die_waiter waiters[8];
    die_priority_t order[8] = {
        DIE_PRIO_HOST_READ, DIE_PRIO_GC_FORCE,
        DIE_PRIO_HOST_READ, DIE_PRIO_GC_FORCE,
        DIE_PRIO_HOST_READ, DIE_PRIO_GC_FORCE,
        DIE_PRIO_HOST_READ, DIE_PRIO_GC_FORCE,
    };
    for (int i = 0; i < 8; i++) {
        die_waiter_init(&waiters[i], order[i]);
        die_waitqueue_enqueue(&q, &waiters[i]);
    }

    int read_count = 0;
    int force_count = 0;
    int last_class = -1;
    int alternations = 0;
    for (int i = 0; i < 8; i++) {
        struct die_waiter *w = die_waitqueue_pop_next(&q);
        if (!w) {
            break;
        }
        int cls = (w->prio == DIE_PRIO_HOST_READ) ? 0 : 1;
        if (cls == 0) {
            read_count++;
        } else {
            force_count++;
        }
        if (last_class != -1 && cls != last_class) {
            alternations++;
        }
        last_class = cls;
    }

    TEST_ASSERT(read_count == 4, "exactly 4 HOST_READ waiters popped");
    TEST_ASSERT(force_count == 4, "exactly 4 GC_FORCE waiters popped");
    TEST_ASSERT(alternations >= 6,
                "at least 6 alternations in 8 pops under 1:1 quantum");

    for (int i = 0; i < 8; i++) {
        die_waiter_cleanup(&waiters[i]);
    }
    die_waitqueue_cleanup(&q);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------ */

static int test_ema_iat_decays_correctly(void)
{
    printf("\n=== EMA IAT tracks host IO inter-arrival time ===\n");

    struct die_waitqueue q;
    die_waitqueue_init(&q);

    /* Drive 10 IOs at 100us inter-arrival time. EMA should converge to
     * something near 100,000 ns (allow a wide tolerance for the smoothing
     * transient). */
    u64 t = 1000000ULL;
    for (int i = 0; i < 10; i++) {
        die_waitqueue_record_host_io(&q, t);
        t += 100000ULL;
    }
    TEST_ASSERT(q.ema_iat_ns > 80000.0 && q.ema_iat_ns < 200000.0,
                "EMA converges in (80us, 200us) range");
    TEST_ASSERT(die_waitqueue_host_bursting(&q, t) == true,
                "host_bursting reports true under 100us cadence");

    /* Skip 100 ms forward without any new IOs. The grace check expires
     * (gap >> burst_grace_ns) and host_bursting must report false even
     * though the EMA itself is still small. */
    t += 100ULL * 1000ULL * 1000ULL;
    TEST_ASSERT(die_waitqueue_host_bursting(&q, t) == false,
                "host_bursting reports false after 100ms gap");

    die_waitqueue_cleanup(&q);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------ */

static int test_t4_yields_under_burst(void)
{
    printf("\n=== T4 yields when host is bursting; fires when host quiet ===\n");

    struct die_waitqueue q;
    die_waitqueue_init(&q);

    struct die_waiter w_idle;
    die_waiter_init(&w_idle, DIE_PRIO_GC_IDLE);
    die_waitqueue_enqueue(&q, &w_idle);

    /* Drive a synthetic host burst against the queue's notion of "now".
     * Use real wall-clock so pop_next's internal get_time_ns() sees a
     * recent last_host_ts. */
    u64 now = get_time_ns();
    /* Two samples 100us apart let EMA reach a high-rate value. */
    die_waitqueue_record_host_io(&q, now - 100000ULL);
    die_waitqueue_record_host_io(&q, now);

    /* Host burst is fresh — pop must yield. */
    struct die_waiter *first = die_waitqueue_pop_next(&q);
    TEST_ASSERT(first == NULL,
                "T4 waiter held back while host is bursting");

    /* Force host quiet by clearing the last-host timestamp. */
    q.last_host_ts_ns = 0;
    q.ema_iat_ns = 0.0;

    struct die_waiter *second = die_waitqueue_pop_next(&q);
    TEST_ASSERT(second == &w_idle,
                "T4 waiter dispatched once host is quiet");

    die_waiter_cleanup(&w_idle);
    die_waitqueue_cleanup(&q);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------ */

static int test_gc_trigger_to_priority_mapping(void)
{
    printf("\n=== gc_trigger_t -> die_priority_t mapping ===\n");

    TEST_ASSERT(die_dispatcher_prio_for_gc(GC_TRIGGER_FREE_SB_LOW)
                    == DIE_PRIO_GC_CRITICAL,
                "FREE_SB_LOW -> GC_CRITICAL");
    TEST_ASSERT(die_dispatcher_prio_for_gc(GC_TRIGGER_READ_DISTURB)
                    == DIE_PRIO_GC_FORCE,
                "READ_DISTURB -> GC_FORCE");
    TEST_ASSERT(die_dispatcher_prio_for_gc(GC_TRIGGER_WEAR_LEVELING)
                    == DIE_PRIO_GC_FORCE,
                "WEAR_LEVELING -> GC_FORCE");
    TEST_ASSERT(die_dispatcher_prio_for_gc(GC_TRIGGER_HOST_WRITE)
                    == DIE_PRIO_GC_NORMAL,
                "HOST_WRITE -> GC_NORMAL");
    TEST_ASSERT(die_dispatcher_prio_for_gc(GC_TRIGGER_IDLE)
                    == DIE_PRIO_GC_IDLE,
                "IDLE -> GC_IDLE");

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printf("========================================\n");
    printf("FTL die_dispatcher L1 unit tests\n");
    printf("========================================\n");

    test_create_destroy_is_clean();
    test_pop_picks_highest_tier_first();
    test_t2_wfq_alternates_host_read_and_force_gc();
    test_ema_iat_decays_correctly();
    test_t4_yields_under_burst();
    test_gc_trigger_to_priority_mapping();

    printf("\n========================================\n");
    printf("die_dispatcher Unit Test Summary\n");
    printf("========================================\n");
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    return tests_failed == 0 ? 0 : 1;
}
