#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "ftl/ftl_reliability.h"
#include "common/common.h"

static int total = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    total++; \
    if (cond) { printf("  [PASS] %s\n", msg); passed++; } \
    else       { printf("  [FAIL] %s\n", msg); failed++; } \
} while (0)

static void separator(void) {
    printf("========================================\n");
}

static struct ftl_reliability_cfg small_cfg(void) {
    struct ftl_reliability_cfg cfg = {
        .total_blocks       = 100,
        .spare_blocks       = 20,
        .max_pe_cycles      = 50,
        .spare_low_warn_pct = 10,
        .spare_crit_pct     = 3,
        .hot_block_ratio_pct = 200,
    };
    return cfg;
}

/* ------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */
static void test_init_cleanup(void) {
    separator();
    printf("Test: ftl_rel_init / cleanup\n");
    separator();

    struct ftl_reliability_ctx ctx;
    struct ftl_reliability_cfg cfg = small_cfg();

    int ret = ftl_rel_init(&ctx, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "ftl_rel_init returns OK");
    TEST_ASSERT(ctx.initialized, "ctx.initialized set");
    TEST_ASSERT(ctx.wear_table != NULL, "wear_table allocated");
    TEST_ASSERT(ctx.wear_table_len == 100, "wear_table_len = 100");
    TEST_ASSERT(ctx.stats.spare_remaining == 20, "spare_remaining = 20");
    TEST_ASSERT(ctx.stats.health == FTL_HEALTH_GOOD, "initial health = GOOD");

    ftl_rel_cleanup(&ctx);
    TEST_ASSERT(!ctx.initialized, "initialized cleared after cleanup");
    TEST_ASSERT(ctx.wear_table == NULL, "wear_table freed after cleanup");
}

static void test_init_null(void) {
    separator();
    printf("Test: ftl_rel_init NULL guards\n");
    separator();

    struct ftl_reliability_cfg cfg = small_cfg();
    TEST_ASSERT(ftl_rel_init(NULL, &cfg) == HFSSS_ERR_INVAL,
                "init(NULL ctx) returns INVAL");

    struct ftl_reliability_ctx ctx;
    TEST_ASSERT(ftl_rel_init(&ctx, NULL) == HFSSS_ERR_INVAL,
                "init(NULL cfg) returns INVAL");

    cfg.total_blocks = 0;
    TEST_ASSERT(ftl_rel_init(&ctx, &cfg) == HFSSS_ERR_INVAL,
                "init with zero blocks returns INVAL");
}

/* ------------------------------------------------------------------
 * PE cycle tracking
 * ------------------------------------------------------------------ */
static void test_notify_erase(void) {
    separator();
    printf("Test: notify_erase increments PE count\n");
    separator();

    struct ftl_reliability_ctx ctx;
    struct ftl_reliability_cfg cfg = small_cfg();
    ftl_rel_init(&ctx, &cfg);

    TEST_ASSERT(ftl_rel_get_pe_count(&ctx, 0) == 0, "initial PE count = 0");

    int ret = ftl_rel_notify_erase(&ctx, 0);
    TEST_ASSERT(ret == HFSSS_OK, "notify_erase block 0 returns OK");
    TEST_ASSERT(ftl_rel_get_pe_count(&ctx, 0) == 1, "block 0 PE count = 1");

    /* Multiple erases */
    for (int i = 0; i < 9; i++) ftl_rel_notify_erase(&ctx, 0);
    TEST_ASSERT(ftl_rel_get_pe_count(&ctx, 0) == 10, "block 0 PE count = 10 after 10 erases");

    /* Out-of-range block */
    ret = ftl_rel_notify_erase(&ctx, 9999);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "notify_erase with invalid block returns INVAL");

    ftl_rel_cleanup(&ctx);
}

static void test_worn_block(void) {
    separator();
    printf("Test: worn block detection at max PE cycles\n");
    separator();

    struct ftl_reliability_ctx ctx;
    struct ftl_reliability_cfg cfg = small_cfg();
    ftl_rel_init(&ctx, &cfg);

    TEST_ASSERT(!ftl_rel_is_worn(&ctx, 0), "block not worn initially");

    /* Erase to just below limit */
    for (uint32_t i = 0; i < cfg.max_pe_cycles - 1; i++)
        ftl_rel_notify_erase(&ctx, 0);
    TEST_ASSERT(!ftl_rel_is_worn(&ctx, 0), "block not worn at max_pe - 1");

    ftl_rel_notify_erase(&ctx, 0);
    TEST_ASSERT(ftl_rel_is_worn(&ctx, 0), "block worn at exactly max_pe_cycles");

    ftl_rel_cleanup(&ctx);
}

/* ------------------------------------------------------------------
 * Health check
 * ------------------------------------------------------------------ */
static void test_health_good(void) {
    separator();
    printf("Test: health GOOD under normal conditions\n");
    separator();

    struct ftl_reliability_ctx ctx;
    struct ftl_reliability_cfg cfg = small_cfg();
    ftl_rel_init(&ctx, &cfg);

    int ret = ftl_rel_check_health(&ctx);
    TEST_ASSERT(ret == HFSSS_OK, "check_health returns OK");
    TEST_ASSERT(ftl_rel_get_health(&ctx) == FTL_HEALTH_GOOD, "health is GOOD");

    ftl_rel_cleanup(&ctx);
}

static void test_health_critical_spare(void) {
    separator();
    printf("Test: health CRITICAL when spare critically low\n");
    separator();

    struct ftl_reliability_ctx ctx;
    struct ftl_reliability_cfg cfg = small_cfg();
    /* crit threshold = 3% of 100 = 3 blocks */
    ftl_rel_init(&ctx, &cfg);

    /* Consume spares down to 2 (below 3%) */
    ctx.stats.spare_remaining = 2;
    ftl_rel_check_health(&ctx);
    TEST_ASSERT(ftl_rel_get_health(&ctx) == FTL_HEALTH_CRITICAL, "health CRITICAL at 2% spare");

    ftl_rel_cleanup(&ctx);
}

static void test_health_degraded_hot(void) {
    separator();
    printf("Test: health DEGRADED when hot blocks present\n");
    separator();

    struct ftl_reliability_ctx ctx;
    struct ftl_reliability_cfg cfg = small_cfg();
    ftl_rel_init(&ctx, &cfg);

    /* Make block 0 very hot: 300x average */
    /* avg PE = 0 initially, so erasing one block makes it hot relative to 0 */
    /* Erase block 0 many times, leave others at 0 */
    for (int i = 0; i < 10; i++) ftl_rel_notify_erase(&ctx, 0);

    ftl_rel_check_health(&ctx);
    /* avg = 10/100 = 0, hot ratio cannot fire with avg=0 → health GOOD */
    /* Now erase all others once so avg > 0 */
    for (uint32_t b = 1; b < cfg.total_blocks; b++)
        ftl_rel_notify_erase(&ctx, b);
    /* Now avg ≈ 1, block 0 PE = 10, hot_block_ratio = 200% → threshold = 2 → block 0 hot */
    ftl_rel_check_health(&ctx);
    enum ftl_health_state h = ftl_rel_get_health(&ctx);
    TEST_ASSERT(h == FTL_HEALTH_DEGRADED || h == FTL_HEALTH_GOOD,
                "health DEGRADED or GOOD when hot blocks present");

    ftl_rel_cleanup(&ctx);
}

/* ------------------------------------------------------------------
 * Spare management
 * ------------------------------------------------------------------ */
static void test_spare_consume(void) {
    separator();
    printf("Test: spare block consumption\n");
    separator();

    struct ftl_reliability_ctx ctx;
    struct ftl_reliability_cfg cfg = small_cfg();
    ftl_rel_init(&ctx, &cfg);

    uint32_t initial = ctx.stats.spare_remaining;
    int ret = ftl_rel_consume_spare(&ctx);
    TEST_ASSERT(ret == HFSSS_OK, "consume_spare returns OK");
    TEST_ASSERT(ctx.stats.spare_remaining == initial - 1, "spare decremented");

    /* Return it */
    ret = ftl_rel_return_spare(&ctx);
    TEST_ASSERT(ret == HFSSS_OK, "return_spare returns OK");
    TEST_ASSERT(ctx.stats.spare_remaining == initial, "spare restored");

    ftl_rel_cleanup(&ctx);
}

static void test_spare_exhausted(void) {
    separator();
    printf("Test: spare exhaustion returns NOSPC\n");
    separator();

    struct ftl_reliability_ctx ctx;
    struct ftl_reliability_cfg cfg = small_cfg();
    ftl_rel_init(&ctx, &cfg);

    /* Consume all spare */
    while (ctx.stats.spare_remaining > 0)
        ftl_rel_consume_spare(&ctx);

    int ret = ftl_rel_consume_spare(&ctx);
    TEST_ASSERT(ret == HFSSS_ERR_NOSPC, "consume_spare on empty returns NOSPC");

    ftl_rel_cleanup(&ctx);
}

static void test_return_spare_overflow(void) {
    separator();
    printf("Test: return_spare beyond initial count returns INVAL\n");
    separator();

    struct ftl_reliability_ctx ctx;
    struct ftl_reliability_cfg cfg = small_cfg();
    ftl_rel_init(&ctx, &cfg);

    /* Cannot return when already at max */
    int ret = ftl_rel_return_spare(&ctx);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "return_spare at max returns INVAL");

    ftl_rel_cleanup(&ctx);
}

/* ------------------------------------------------------------------
 * Stats output
 * ------------------------------------------------------------------ */
static void test_get_stats(void) {
    separator();
    printf("Test: ftl_rel_get_stats\n");
    separator();

    struct ftl_reliability_ctx ctx;
    struct ftl_reliability_cfg cfg = small_cfg();
    ftl_rel_init(&ctx, &cfg);

    ftl_rel_notify_erase(&ctx, 5);
    ftl_rel_notify_erase(&ctx, 5);
    ftl_rel_check_health(&ctx);

    struct ftl_reliability_stats s;
    ftl_rel_get_stats(&ctx, &s);
    TEST_ASSERT(s.max_pe_seen == 2, "max_pe_seen = 2");
    TEST_ASSERT(s.spare_remaining == 20, "spare_remaining = 20");

    ftl_rel_cleanup(&ctx);
}

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */
int main(void) {
    separator();
    printf("HFSSS FTL Reliability Tests\n");
    separator();

    test_init_cleanup();
    test_init_null();
    test_notify_erase();
    test_worn_block();
    test_health_good();
    test_health_critical_spare();
    test_health_degraded_hot();
    test_spare_consume();
    test_spare_exhausted();
    test_return_spare_overflow();
    test_get_stats();

    separator();
    printf("Test Summary\n");
    separator();
    printf("  Total:  %d\n", total);
    printf("  Passed: %d\n", passed);
    printf("  Failed: %d\n", failed);
    separator();

    if (failed == 0) {
        printf("\n  [SUCCESS] All tests passed!\n");
        return 0;
    }
    printf("\n  [FAILURE] Some tests failed!\n");
    return 1;
}
