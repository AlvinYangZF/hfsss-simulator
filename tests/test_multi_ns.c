/*
 * test_multi_ns.c - Tests for multi-namespace management (REQ-166..REQ-170)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "common/common.h"
#include "ftl/ns_mapping.h"
#include "ftl/ns_gc.h"

static int total = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    total++; \
    if (cond) { printf("  [PASS] %s\n", msg); passed++; } \
    else       { printf("  [FAIL] %s\n", msg); failed++; } \
} while (0)

static void separator(void) {
    printf("========================================\n");
}

/* ------------------------------------------------------------------
 * NS Mapping Manager Tests
 * ------------------------------------------------------------------ */

static void test_mgr_init_cleanup(void) {
    separator();
    printf("Test: ns_mapping_mgr_init / cleanup\n");
    separator();

    struct ns_mapping_mgr mgr;
    int ret = ns_mapping_mgr_init(&mgr, 1000000, NS_POOL_SHARED);
    TEST_ASSERT(ret == HFSSS_OK, "mgr init returns OK");
    TEST_ASSERT(mgr.initialized, "mgr.initialized set");
    TEST_ASSERT(mgr.total_lbas == 1000000, "total_lbas = 1000000");
    TEST_ASSERT(mgr.allocated_lbas == 0, "allocated_lbas = 0");
    TEST_ASSERT(mgr.active_count == 0, "active_count = 0");
    TEST_ASSERT(mgr.default_mode == NS_POOL_SHARED, "default_mode = SHARED");

    ns_mapping_mgr_cleanup(&mgr);
    TEST_ASSERT(!mgr.initialized, "initialized cleared after cleanup");

    /* NULL guard */
    ret = ns_mapping_mgr_init(NULL, 1000000, NS_POOL_SHARED);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "init NULL returns INVAL");

    /* Zero LBAs */
    ret = ns_mapping_mgr_init(&mgr, 0, NS_POOL_SHARED);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "init zero lbas returns INVAL");
}

static void test_create_single_ns(void) {
    separator();
    printf("Test: create single namespace\n");
    separator();

    struct ns_mapping_mgr mgr;
    ns_mapping_mgr_init(&mgr, 1000000, NS_POOL_SHARED);

    int ret = ns_mapping_create(&mgr, 1, 500000);
    TEST_ASSERT(ret == HFSSS_OK, "create NS-1 returns OK");
    TEST_ASSERT(mgr.active_count == 1, "active_count = 1");
    TEST_ASSERT(mgr.allocated_lbas == 500000, "allocated_lbas = 500000");

    struct ns_mapping_ctx info;
    ret = ns_mapping_get_info(&mgr, 1, &info);
    TEST_ASSERT(ret == HFSSS_OK, "get_info NS-1 returns OK");
    TEST_ASSERT(info.nsid == 1, "info.nsid = 1");
    TEST_ASSERT(info.lba_start == 0, "info.lba_start = 0");
    TEST_ASSERT(info.lba_count == 500000, "info.lba_count = 500000");
    TEST_ASSERT(info.active, "info.active = true");
    TEST_ASSERT(info.valid_pages == 0, "info.valid_pages = 0");
    TEST_ASSERT(info.invalid_pages == 0, "info.invalid_pages = 0");

    u64 free_lbas = ns_mapping_get_free_lbas(&mgr);
    TEST_ASSERT(free_lbas == 500000, "free_lbas = 500000");

    ns_mapping_mgr_cleanup(&mgr);
}

static void test_create_multiple_ns(void) {
    separator();
    printf("Test: create multiple namespaces\n");
    separator();

    struct ns_mapping_mgr mgr;
    ns_mapping_mgr_init(&mgr, 1000000, NS_POOL_SHARED);

    int ret = ns_mapping_create(&mgr, 1, 300000);
    TEST_ASSERT(ret == HFSSS_OK, "create NS-1 OK");

    ret = ns_mapping_create(&mgr, 2, 300000);
    TEST_ASSERT(ret == HFSSS_OK, "create NS-2 OK");

    ret = ns_mapping_create(&mgr, 3, 200000);
    TEST_ASSERT(ret == HFSSS_OK, "create NS-3 OK");

    TEST_ASSERT(mgr.active_count == 3, "active_count = 3");
    TEST_ASSERT(mgr.allocated_lbas == 800000, "allocated_lbas = 800000");

    struct ns_mapping_ctx info;
    ns_mapping_get_info(&mgr, 1, &info);
    TEST_ASSERT(info.lba_start == 0, "NS-1 lba_start = 0");
    TEST_ASSERT(info.lba_count == 300000, "NS-1 lba_count = 300000");

    ns_mapping_get_info(&mgr, 2, &info);
    TEST_ASSERT(info.lba_start == 300000, "NS-2 lba_start = 300000");
    TEST_ASSERT(info.lba_count == 300000, "NS-2 lba_count = 300000");

    ns_mapping_get_info(&mgr, 3, &info);
    TEST_ASSERT(info.lba_start == 600000, "NS-3 lba_start = 600000");
    TEST_ASSERT(info.lba_count == 200000, "NS-3 lba_count = 200000");

    u64 free_lbas = ns_mapping_get_free_lbas(&mgr);
    TEST_ASSERT(free_lbas == 200000, "free_lbas = 200000");

    ns_mapping_mgr_cleanup(&mgr);
}

static void test_delete_ns(void) {
    separator();
    printf("Test: delete namespace, capacity reclaimed\n");
    separator();

    struct ns_mapping_mgr mgr;
    ns_mapping_mgr_init(&mgr, 1000000, NS_POOL_SHARED);

    ns_mapping_create(&mgr, 1, 400000);
    ns_mapping_create(&mgr, 2, 300000);

    TEST_ASSERT(mgr.active_count == 2, "active_count = 2 before delete");
    TEST_ASSERT(mgr.allocated_lbas == 700000, "allocated before delete = 700000");

    int ret = ns_mapping_delete(&mgr, 1);
    TEST_ASSERT(ret == HFSSS_OK, "delete NS-1 returns OK");
    TEST_ASSERT(mgr.active_count == 1, "active_count = 1 after delete");
    TEST_ASSERT(mgr.allocated_lbas == 300000, "allocated after delete = 300000");

    u64 free_lbas = ns_mapping_get_free_lbas(&mgr);
    TEST_ASSERT(free_lbas == 700000, "free_lbas = 700000 after delete");

    /* Verify NS-1 is gone */
    struct ns_mapping_ctx info;
    ret = ns_mapping_get_info(&mgr, 1, &info);
    TEST_ASSERT(ret == HFSSS_ERR_NOENT, "get_info deleted NS returns NOENT");

    ns_mapping_mgr_cleanup(&mgr);
}

static void test_create_exceed_capacity(void) {
    separator();
    printf("Test: create NS exceeding capacity\n");
    separator();

    struct ns_mapping_mgr mgr;
    ns_mapping_mgr_init(&mgr, 1000000, NS_POOL_SHARED);

    ns_mapping_create(&mgr, 1, 600000);

    int ret = ns_mapping_create(&mgr, 2, 500000);
    TEST_ASSERT(ret == HFSSS_ERR_NOSPC, "exceed capacity returns NOSPC");
    TEST_ASSERT(mgr.active_count == 1, "active_count unchanged on failure");

    ns_mapping_mgr_cleanup(&mgr);
}

static void test_create_duplicate_nsid(void) {
    separator();
    printf("Test: create duplicate NSID\n");
    separator();

    struct ns_mapping_mgr mgr;
    ns_mapping_mgr_init(&mgr, 1000000, NS_POOL_SHARED);

    ns_mapping_create(&mgr, 1, 100000);

    int ret = ns_mapping_create(&mgr, 1, 100000);
    TEST_ASSERT(ret == HFSSS_ERR_EXIST, "duplicate NSID returns EXIST");

    ns_mapping_mgr_cleanup(&mgr);
}

static void test_delete_nonexistent(void) {
    separator();
    printf("Test: delete non-existent NS\n");
    separator();

    struct ns_mapping_mgr mgr;
    ns_mapping_mgr_init(&mgr, 1000000, NS_POOL_SHARED);

    int ret = ns_mapping_delete(&mgr, 99);
    TEST_ASSERT(ret == HFSSS_ERR_NOENT, "delete non-existent returns NOENT");

    ns_mapping_mgr_cleanup(&mgr);
}

static void test_ns_format(void) {
    separator();
    printf("Test: NS format resets stats, preserves allocation\n");
    separator();

    struct ns_mapping_mgr mgr;
    ns_mapping_mgr_init(&mgr, 1000000, NS_POOL_SHARED);

    ns_mapping_create(&mgr, 1, 500000);

    /* Simulate some writes by directly modifying the context */
    mutex_lock(&mgr.lock, 0);
    int slot = -1;
    for (u32 i = 0; i < NS_MAX_NAMESPACES; i++) {
        if (mgr.namespaces[i].active && mgr.namespaces[i].nsid == 1) {
            slot = (int)i;
            break;
        }
    }
    TEST_ASSERT(slot >= 0, "found NS-1 slot");
    mgr.namespaces[slot].valid_pages = 1000;
    mgr.namespaces[slot].invalid_pages = 200;
    mutex_unlock(&mgr.lock);

    struct ns_mapping_ctx info;
    ns_mapping_get_info(&mgr, 1, &info);
    TEST_ASSERT(info.valid_pages == 1000, "valid_pages = 1000 before format");
    TEST_ASSERT(info.invalid_pages == 200, "invalid_pages = 200 before format");

    int ret = ns_mapping_format(&mgr, 1);
    TEST_ASSERT(ret == HFSSS_OK, "format returns OK");

    ns_mapping_get_info(&mgr, 1, &info);
    TEST_ASSERT(info.valid_pages == 0, "valid_pages = 0 after format");
    TEST_ASSERT(info.invalid_pages == 0, "invalid_pages = 0 after format");
    TEST_ASSERT(info.lba_count == 500000, "lba_count preserved after format");
    TEST_ASSERT(info.lba_start == 0, "lba_start preserved after format");
    TEST_ASSERT(info.active, "active preserved after format");

    ns_mapping_mgr_cleanup(&mgr);
}

static void test_32_ns_limit(void) {
    separator();
    printf("Test: 32 NS boundary limit\n");
    separator();

    struct ns_mapping_mgr mgr;
    /* Each NS gets 100 LBAs, total = 32 * 100 + 100 spare */
    ns_mapping_mgr_init(&mgr, 3300, NS_POOL_SHARED);

    int ret;
    for (u32 i = 1; i <= 32; i++) {
        ret = ns_mapping_create(&mgr, i, 100);
        TEST_ASSERT(ret == HFSSS_OK, "create NS within 32 limit");
    }

    TEST_ASSERT(mgr.active_count == 32, "active_count = 32");

    /* 33rd should fail (no free slot) */
    ret = ns_mapping_create(&mgr, 33, 100);
    TEST_ASSERT(ret == HFSSS_ERR_NOSPC, "33rd NS returns NOSPC");

    ns_mapping_mgr_cleanup(&mgr);
}

/* ------------------------------------------------------------------
 * GC Coordinator Tests
 * ------------------------------------------------------------------ */

static void test_gc_init_cleanup(void) {
    separator();
    printf("Test: ns_gc_coordinator init / cleanup\n");
    separator();

    struct ns_gc_coordinator coord;
    int ret = ns_gc_coordinator_init(&coord, 1000, 0.3, 0.8, 50);
    TEST_ASSERT(ret == HFSSS_OK, "gc coordinator init OK");
    TEST_ASSERT(coord.initialized, "coord.initialized set");
    TEST_ASSERT(coord.total_gc_budget == 1000, "total_gc_budget = 1000");
    TEST_ASSERT(coord.min_budget_per_ns == 50, "min_budget_per_ns = 50");

    ns_gc_coordinator_cleanup(&coord);
    TEST_ASSERT(!coord.initialized, "initialized cleared after cleanup");

    /* NULL guard */
    ret = ns_gc_coordinator_init(NULL, 1000, 0.3, 0.8, 50);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "init NULL returns INVAL");

    /* Zero budget */
    ret = ns_gc_coordinator_init(&coord, 0, 0.3, 0.8, 50);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "init zero budget returns INVAL");
}

static void test_gc_register_unregister(void) {
    separator();
    printf("Test: GC register / unregister NS\n");
    separator();

    struct ns_gc_coordinator coord;
    ns_gc_coordinator_init(&coord, 1000, 0.3, 0.8, 50);

    int ret = ns_gc_register_ns(&coord, 1, 1000);
    TEST_ASSERT(ret == HFSSS_OK, "register NS-1 OK");

    ret = ns_gc_register_ns(&coord, 2, 2000);
    TEST_ASSERT(ret == HFSSS_OK, "register NS-2 OK");

    /* Duplicate */
    ret = ns_gc_register_ns(&coord, 1, 500);
    TEST_ASSERT(ret == HFSSS_ERR_EXIST, "duplicate register returns EXIST");

    /* Unregister */
    ret = ns_gc_unregister_ns(&coord, 1);
    TEST_ASSERT(ret == HFSSS_OK, "unregister NS-1 OK");

    /* Unregister non-existent */
    ret = ns_gc_unregister_ns(&coord, 99);
    TEST_ASSERT(ret == HFSSS_ERR_NOENT, "unregister non-existent returns NOENT");

    /* Re-register after unregister */
    ret = ns_gc_register_ns(&coord, 1, 500);
    TEST_ASSERT(ret == HFSSS_OK, "re-register NS-1 after unregister OK");

    ns_gc_coordinator_cleanup(&coord);
}

static void test_gc_urgency_calculation(void) {
    separator();
    printf("Test: GC urgency calculation\n");
    separator();

    /* High free blocks -> low urgency */
    /* free_ratio = 900/1000 = 0.9, invalid_ratio = 0
     * score = (1 - 0.9) * 0.6 + 0 * 0.4 = 0.06 */
    double score = ns_gc_calculate_urgency(900, 1000, 0, 100);
    TEST_ASSERT(score < 0.1, "high free blocks -> low urgency");

    /* Low free blocks -> high urgency */
    /* free_ratio = 50/1000 = 0.05, invalid_ratio = 500/1000 = 0.5
     * score = (1 - 0.05) * 0.6 + 0.5 * 0.4 = 0.57 + 0.2 = 0.77 */
    score = ns_gc_calculate_urgency(50, 1000, 500, 500);
    TEST_ASSERT(score > 0.7, "low free blocks -> high urgency");

    /* Zero total blocks */
    score = ns_gc_calculate_urgency(0, 0, 0, 0);
    TEST_ASSERT(score == 0.0, "zero total blocks -> 0.0 urgency");

    /* All blocks free, no pages */
    score = ns_gc_calculate_urgency(1000, 1000, 0, 0);
    TEST_ASSERT(score == 0.0, "all free -> 0.0 urgency");

    /* No free blocks, all pages invalid */
    /* free_ratio = 0, invalid_ratio = 1.0
     * score = 1.0 * 0.6 + 1.0 * 0.4 = 1.0 */
    score = ns_gc_calculate_urgency(0, 1000, 1000, 0);
    TEST_ASSERT(fabs(score - 1.0) < 0.001, "no free + all invalid -> 1.0");
}

static void test_gc_budget_allocation(void) {
    separator();
    printf("Test: GC budget allocation proportional to urgency\n");
    separator();

    struct ns_gc_coordinator coord;
    ns_gc_coordinator_init(&coord, 1000, 0.1, 0.8, 50);

    ns_gc_register_ns(&coord, 1, 1000);
    ns_gc_register_ns(&coord, 2, 1000);

    /* NS-1: high urgency, NS-2: moderate urgency */
    ns_gc_update_urgency(&coord, 1, 50, 1000, 500, 500);
    ns_gc_update_urgency(&coord, 2, 500, 1000, 100, 400);

    u32 budgets[NS_GC_MAX_NS];
    int ret = ns_gc_allocate_budget(&coord, budgets);
    TEST_ASSERT(ret == HFSSS_OK, "allocate budget returns OK");

    /* Find which slots were used */
    u32 budget_ns1 = 0, budget_ns2 = 0;
    for (u32 i = 0; i < NS_GC_MAX_NS; i++) {
        if (coord.urgency[i].active && coord.urgency[i].nsid == 1) {
            budget_ns1 = budgets[i];
        }
        if (coord.urgency[i].active && coord.urgency[i].nsid == 2) {
            budget_ns2 = budgets[i];
        }
    }

    TEST_ASSERT(budget_ns1 > 0, "NS-1 gets budget > 0");
    TEST_ASSERT(budget_ns2 > 0, "NS-2 gets budget > 0");
    TEST_ASSERT(budget_ns1 > budget_ns2, "higher urgency NS gets more budget");
    TEST_ASSERT(budget_ns1 >= 50, "NS-1 budget >= min_budget");
    TEST_ASSERT(budget_ns2 >= 50, "NS-2 budget >= min_budget");

    u32 total_budget = budget_ns1 + budget_ns2;
    TEST_ASSERT(total_budget <= 1000, "total budget <= configured total");

    ns_gc_coordinator_cleanup(&coord);
}

static void test_waf_tracking(void) {
    separator();
    printf("Test: WAF tracking\n");
    separator();

    struct ns_gc_coordinator coord;
    ns_gc_coordinator_init(&coord, 1000, 0.3, 0.8, 50);

    ns_gc_register_ns(&coord, 1, 1000);

    /* No writes yet */
    double waf = ns_gc_get_waf(&coord, 1);
    TEST_ASSERT(fabs(waf - 1.0) < 0.001, "WAF = 1.0 with no writes");

    /* Host writes 100 pages, NAND writes 150 (includes GC) */
    ns_gc_record_write(&coord, 1, 100, 150);
    waf = ns_gc_get_waf(&coord, 1);
    TEST_ASSERT(fabs(waf - 1.5) < 0.001, "WAF = 1.5 (150/100)");

    /* Accumulate more */
    ns_gc_record_write(&coord, 1, 100, 250);
    waf = ns_gc_get_waf(&coord, 1);
    /* total host = 200, total nand = 400 -> WAF = 2.0 */
    TEST_ASSERT(fabs(waf - 2.0) < 0.001, "WAF = 2.0 (400/200)");

    /* Non-existent NS */
    waf = ns_gc_get_waf(&coord, 99);
    TEST_ASSERT(fabs(waf - 1.0) < 0.001, "WAF = 1.0 for non-existent NS");

    ns_gc_coordinator_cleanup(&coord);
}

static void test_multi_ns_isolation(void) {
    separator();
    printf("Test: multi-NS isolation\n");
    separator();

    struct ns_mapping_mgr mgr;
    ns_mapping_mgr_init(&mgr, 1000000, NS_POOL_SHARED);
    ns_mapping_create(&mgr, 1, 500000);
    ns_mapping_create(&mgr, 2, 300000);

    /* Simulate writes to NS-1 only */
    mutex_lock(&mgr.lock, 0);
    for (u32 i = 0; i < NS_MAX_NAMESPACES; i++) {
        if (mgr.namespaces[i].active && mgr.namespaces[i].nsid == 1) {
            mgr.namespaces[i].valid_pages = 5000;
            mgr.namespaces[i].invalid_pages = 100;
            break;
        }
    }
    mutex_unlock(&mgr.lock);

    struct ns_mapping_ctx info1, info2;
    ns_mapping_get_info(&mgr, 1, &info1);
    ns_mapping_get_info(&mgr, 2, &info2);

    TEST_ASSERT(info1.valid_pages == 5000, "NS-1 valid_pages = 5000");
    TEST_ASSERT(info1.invalid_pages == 100, "NS-1 invalid_pages = 100");
    TEST_ASSERT(info2.valid_pages == 0, "NS-2 valid_pages = 0 (isolated)");
    TEST_ASSERT(info2.invalid_pages == 0, "NS-2 invalid_pages = 0 (isolated)");

    /* GC coordinator isolation */
    struct ns_gc_coordinator coord;
    ns_gc_coordinator_init(&coord, 1000, 0.1, 0.8, 50);
    ns_gc_register_ns(&coord, 1, 1000);
    ns_gc_register_ns(&coord, 2, 1000);

    ns_gc_record_write(&coord, 1, 100, 200);

    double waf1 = ns_gc_get_waf(&coord, 1);
    double waf2 = ns_gc_get_waf(&coord, 2);
    TEST_ASSERT(fabs(waf1 - 2.0) < 0.001, "NS-1 WAF = 2.0");
    TEST_ASSERT(fabs(waf2 - 1.0) < 0.001, "NS-2 WAF = 1.0 (no writes)");

    ns_gc_coordinator_cleanup(&coord);
    ns_mapping_mgr_cleanup(&mgr);
}

/* ------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------ */

int main(void) {
    printf("\n");
    separator();
    printf("HFSSS Multi-Namespace Management Tests\n");
    separator();
    printf("\n");

    /* NS mapping tests */
    test_mgr_init_cleanup();
    test_create_single_ns();
    test_create_multiple_ns();
    test_delete_ns();
    test_create_exceed_capacity();
    test_create_duplicate_nsid();
    test_delete_nonexistent();
    test_ns_format();
    test_32_ns_limit();

    /* GC coordinator tests */
    test_gc_init_cleanup();
    test_gc_register_unregister();
    test_gc_urgency_calculation();
    test_gc_budget_allocation();
    test_waf_tracking();
    test_multi_ns_isolation();

    printf("\n");
    separator();
    printf("Multi-NS Test Results: %d/%d passed, %d failed\n",
           passed, total, failed);
    separator();
    printf("\n");

    return failed > 0 ? 1 : 0;
}
