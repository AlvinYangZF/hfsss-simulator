#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "pcie/prp.h"
#include "common/common.h"

/* Simple test framework */
static int g_total = 0;
static int g_passed = 0;
static int g_failed = 0;

#define TEST_ASSERT(cond, name) do { \
    g_total++; \
    if (cond) { \
        g_passed++; \
        printf("  PASS: %s\n", name); \
    } else { \
        g_failed++; \
        printf("  FAIL: %s (line %d)\n", name, __LINE__); \
    } \
} while (0)

/* Allocate a page-aligned buffer of 'pages' pages */
static char *alloc_pages(uint32_t pages)
{
    return aligned_alloc(NVME_PAGE_SIZE, (size_t)pages * NVME_PAGE_SIZE);
}

/* Test 1: Single-page transfer (xfer <= one page remainder): 1 segment */
static void test_single_page_partial(void)
{
    char *page = alloc_pages(1);
    uint64_t prp1 = (uint64_t)(uintptr_t)page + 512; /* offset 512 in page */
    uint32_t xfer_len = 512; /* fits within remain_in_page1 = 4096-512 = 3584 */

    struct prp_list pl;
    int rc = prp_build_list(prp1, 0, xfer_len, NVME_PAGE_SIZE, &pl);
    TEST_ASSERT(rc == HFSSS_OK, "test1: single-page-partial returns OK");
    TEST_ASSERT(pl.nseg == 1, "test1: single-page-partial has 1 segment");
    TEST_ASSERT(pl.segs[0].addr == prp1, "test1: seg0.addr == prp1");
    TEST_ASSERT(pl.segs[0].len == xfer_len, "test1: seg0.len == xfer_len");
    TEST_ASSERT(pl.total_len == xfer_len, "test1: total_len correct");

    free(page);
}

/* Test 2: Single-page transfer at page start (offset=0): 1 segment */
static void test_single_page_aligned(void)
{
    char *page = alloc_pages(1);
    uint64_t prp1 = (uint64_t)(uintptr_t)page; /* offset 0 */
    uint32_t xfer_len = NVME_PAGE_SIZE; /* exactly one page */

    struct prp_list pl;
    int rc = prp_build_list(prp1, 0, xfer_len, NVME_PAGE_SIZE, &pl);
    TEST_ASSERT(rc == HFSSS_OK, "test2: aligned single-page returns OK");
    TEST_ASSERT(pl.nseg == 1, "test2: aligned single-page has 1 segment");
    TEST_ASSERT(pl.segs[0].addr == prp1, "test2: seg0.addr == prp1");
    TEST_ASSERT(pl.segs[0].len == NVME_PAGE_SIZE, "test2: seg0.len == page_size");
    TEST_ASSERT(pl.total_len == NVME_PAGE_SIZE, "test2: total_len == page_size");

    free(page);
}

/* Test 3: Transfer crossing into second page: 2 segments, PRP2 direct */
static void test_two_page_crossing(void)
{
    char *page1 = alloc_pages(1);
    char *page2 = alloc_pages(1);
    uint64_t prp1 = (uint64_t)(uintptr_t)page1 + 1024; /* offset 1024 */
    uint64_t prp2 = (uint64_t)(uintptr_t)page2;
    /* remain_in_page1 = 4096 - 1024 = 3072, want xfer > 3072 but <= 3072+4096 */
    uint32_t xfer_len = 3072 + 512; /* 3584 bytes */

    struct prp_list pl;
    int rc = prp_build_list(prp1, prp2, xfer_len, NVME_PAGE_SIZE, &pl);
    TEST_ASSERT(rc == HFSSS_OK, "test3: two-page crossing returns OK");
    TEST_ASSERT(pl.nseg == 2, "test3: two-page crossing has 2 segments");
    TEST_ASSERT(pl.segs[0].addr == prp1, "test3: seg0.addr == prp1");
    TEST_ASSERT(pl.segs[0].len == 3072, "test3: seg0.len == remain_in_page1");
    TEST_ASSERT(pl.segs[1].addr == prp2, "test3: seg1.addr == prp2");
    TEST_ASSERT(pl.segs[1].len == 512, "test3: seg1.len == remainder");
    TEST_ASSERT(pl.total_len == xfer_len, "test3: total_len correct");

    free(page1);
    free(page2);
}

/* Test 4: Two-page transfer exact: 2 segments */
static void test_two_page_exact(void)
{
    char *page1 = alloc_pages(1);
    char *page2 = alloc_pages(1);
    uint64_t prp1 = (uint64_t)(uintptr_t)page1; /* offset 0 */
    uint64_t prp2 = (uint64_t)(uintptr_t)page2;
    /* remain = 4096, xfer = 8192 = 4096 + 4096 */
    uint32_t xfer_len = 2 * NVME_PAGE_SIZE;

    struct prp_list pl;
    int rc = prp_build_list(prp1, prp2, xfer_len, NVME_PAGE_SIZE, &pl);
    TEST_ASSERT(rc == HFSSS_OK, "test4: two-page exact returns OK");
    TEST_ASSERT(pl.nseg == 2, "test4: two-page exact has 2 segments");
    TEST_ASSERT(pl.segs[0].len == NVME_PAGE_SIZE, "test4: seg0.len == page_size");
    TEST_ASSERT(pl.segs[1].len == NVME_PAGE_SIZE, "test4: seg1.len == page_size");
    TEST_ASSERT(pl.total_len == xfer_len, "test4: total_len == 2*page_size");

    free(page1);
    free(page2);
}

/* Test 5: Three-page transfer: PRP2 is list pointer, 3 segments */
static void test_three_page_prp_list(void)
{
    char *page1 = alloc_pages(1);
    char *page2 = alloc_pages(1);
    char *page3 = alloc_pages(1);
    uint64_t *prp_array = malloc(PRP_MAX_SEGMENTS * sizeof(uint64_t));

    prp_array[0] = (uint64_t)(uintptr_t)page2;
    prp_array[1] = (uint64_t)(uintptr_t)page3;

    uint64_t prp1 = (uint64_t)(uintptr_t)page1; /* offset 0 */
    uint64_t prp2 = (uint64_t)(uintptr_t)prp_array;
    uint32_t xfer_len = 3 * NVME_PAGE_SIZE;

    struct prp_list pl;
    int rc = prp_build_list(prp1, prp2, xfer_len, NVME_PAGE_SIZE, &pl);
    TEST_ASSERT(rc == HFSSS_OK, "test5: three-page prp-list returns OK");
    TEST_ASSERT(pl.nseg == 3, "test5: three-page prp-list has 3 segments");
    TEST_ASSERT(pl.segs[0].addr == prp1, "test5: seg0 == page1");
    TEST_ASSERT(pl.segs[1].addr == (uint64_t)(uintptr_t)page2, "test5: seg1 == page2");
    TEST_ASSERT(pl.segs[2].addr == (uint64_t)(uintptr_t)page3, "test5: seg2 == page3");
    TEST_ASSERT(pl.total_len == xfer_len, "test5: total_len == 3*page_size");

    free(page1);
    free(page2);
    free(page3);
    free(prp_array);
}

/* Test 6: Five-page transfer: 5 segments from PRP list */
static void test_five_page_prp_list(void)
{
    char *pages[5];
    for (int i = 0; i < 5; i++)
        pages[i] = alloc_pages(1);

    uint64_t *prp_array = malloc(PRP_MAX_SEGMENTS * sizeof(uint64_t));
    /* prp_array holds pages[1..4] (page[0] is covered by prp1 directly) */
    for (int i = 0; i < 4; i++)
        prp_array[i] = (uint64_t)(uintptr_t)pages[i + 1];

    uint64_t prp1 = (uint64_t)(uintptr_t)pages[0];
    uint64_t prp2 = (uint64_t)(uintptr_t)prp_array;
    uint32_t xfer_len = 5 * NVME_PAGE_SIZE;

    struct prp_list pl;
    int rc = prp_build_list(prp1, prp2, xfer_len, NVME_PAGE_SIZE, &pl);
    TEST_ASSERT(rc == HFSSS_OK, "test6: five-page prp-list returns OK");
    TEST_ASSERT(pl.nseg == 5, "test6: five-page prp-list has 5 segments");
    TEST_ASSERT(pl.total_len == xfer_len, "test6: total_len == 5*page_size");

    for (int i = 0; i < 5; i++)
        free(pages[i]);
    free(prp_array);
}

/* Test 7: prp_build_list with xfer_len=0: returns OK, nseg=0 */
static void test_zero_xfer_len(void)
{
    char *page = alloc_pages(1);
    uint64_t prp1 = (uint64_t)(uintptr_t)page;

    struct prp_list pl;
    int rc = prp_build_list(prp1, 0, 0, NVME_PAGE_SIZE, &pl);
    TEST_ASSERT(rc == HFSSS_OK, "test7: xfer_len=0 returns OK");
    TEST_ASSERT(pl.nseg == 0, "test7: xfer_len=0 has nseg==0");
    TEST_ASSERT(pl.total_len == 0, "test7: xfer_len=0 has total_len==0");

    free(page);
}

/* Test 8: prp_validate: all page-aligned (except first) -> true */
static void test_prp_validate_valid(void)
{
    char *page1 = alloc_pages(1);
    char *page2 = alloc_pages(1);
    char *page3 = alloc_pages(1);

    struct prp_list pl;
    memset(&pl, 0, sizeof(pl));
    pl.segs[0].addr = (uint64_t)(uintptr_t)page1 + 128; /* unaligned first is OK */
    pl.segs[0].len  = NVME_PAGE_SIZE - 128;
    pl.segs[1].addr = (uint64_t)(uintptr_t)page2; /* page-aligned */
    pl.segs[1].len  = NVME_PAGE_SIZE;
    pl.segs[2].addr = (uint64_t)(uintptr_t)page3; /* page-aligned */
    pl.segs[2].len  = NVME_PAGE_SIZE;
    pl.nseg = 3;
    pl.total_len = (NVME_PAGE_SIZE - 128) + NVME_PAGE_SIZE + NVME_PAGE_SIZE;

    TEST_ASSERT(prp_validate(&pl, NVME_PAGE_SIZE) == true,
                "test8: valid prp_list -> true");

    free(page1);
    free(page2);
    free(page3);
}

/* Test 9: prp_validate: non-aligned middle entry -> false */
static void test_prp_validate_invalid(void)
{
    char *page1 = alloc_pages(1);
    char *page2 = alloc_pages(1);

    struct prp_list pl;
    memset(&pl, 0, sizeof(pl));
    pl.segs[0].addr = (uint64_t)(uintptr_t)page1;
    pl.segs[0].len  = NVME_PAGE_SIZE;
    pl.segs[1].addr = (uint64_t)(uintptr_t)page2 + 64; /* misaligned */
    pl.segs[1].len  = NVME_PAGE_SIZE - 64;
    pl.nseg = 2;
    pl.total_len = NVME_PAGE_SIZE + (NVME_PAGE_SIZE - 64);

    TEST_ASSERT(prp_validate(&pl, NVME_PAGE_SIZE) == false,
                "test9: misaligned middle entry -> false");

    free(page1);
    free(page2);
}

/* Test 10: prp_copy_from_host: single segment, data matches */
static void test_copy_from_host_single(void)
{
    char *page = alloc_pages(1);
    const char pattern[] = "Hello, PRP!";
    uint32_t len = (uint32_t)strlen(pattern);
    memcpy(page, pattern, len);

    struct prp_list pl;
    memset(&pl, 0, sizeof(pl));
    pl.segs[0].addr = (uint64_t)(uintptr_t)page;
    pl.segs[0].len  = len;
    pl.nseg = 1;
    pl.total_len = len;

    char flat[64];
    int rc = prp_copy_from_host(&pl, flat, len);
    TEST_ASSERT(rc == HFSSS_OK, "test10: copy_from_host single returns OK");
    TEST_ASSERT(memcmp(flat, pattern, len) == 0,
                "test10: copy_from_host single data matches");

    free(page);
}

/* Test 11: prp_copy_from_host: two segments, data matches across boundary */
static void test_copy_from_host_two_segs(void)
{
    char *page1 = alloc_pages(1);
    char *page2 = alloc_pages(1);
    memset(page1, 0xAA, NVME_PAGE_SIZE);
    memset(page2, 0xBB, NVME_PAGE_SIZE);

    uint32_t seg1_len = 100;
    uint32_t seg2_len = 200;
    uint32_t total = seg1_len + seg2_len;

    struct prp_list pl;
    memset(&pl, 0, sizeof(pl));
    pl.segs[0].addr = (uint64_t)(uintptr_t)page1;
    pl.segs[0].len  = seg1_len;
    pl.segs[1].addr = (uint64_t)(uintptr_t)page2;
    pl.segs[1].len  = seg2_len;
    pl.nseg = 2;
    pl.total_len = total;

    char flat[300];
    int rc = prp_copy_from_host(&pl, flat, total);
    TEST_ASSERT(rc == HFSSS_OK, "test11: copy_from_host two-seg returns OK");

    bool ok = true;
    for (uint32_t i = 0; i < seg1_len; i++)
        if ((unsigned char)flat[i] != 0xAA) { ok = false; break; }
    for (uint32_t i = seg1_len; ok && i < total; i++)
        if ((unsigned char)flat[i] != 0xBB) { ok = false; break; }
    TEST_ASSERT(ok, "test11: copy_from_host two-seg data matches");

    free(page1);
    free(page2);
}

/* Test 12: prp_copy_to_host: single segment, data written correctly */
static void test_copy_to_host_single(void)
{
    char *page = alloc_pages(1);
    memset(page, 0, NVME_PAGE_SIZE);

    const char flat[] = "WriteTest";
    uint32_t len = (uint32_t)strlen(flat);

    struct prp_list pl;
    memset(&pl, 0, sizeof(pl));
    pl.segs[0].addr = (uint64_t)(uintptr_t)page;
    pl.segs[0].len  = len;
    pl.nseg = 1;
    pl.total_len = len;

    int rc = prp_copy_to_host(&pl, flat, len);
    TEST_ASSERT(rc == HFSSS_OK, "test12: copy_to_host single returns OK");
    TEST_ASSERT(memcmp(page, flat, len) == 0,
                "test12: copy_to_host single data written correctly");

    free(page);
}

/* Test 13: prp_copy_to_host: two segments, scatter written correctly */
static void test_copy_to_host_two_segs(void)
{
    char *page1 = alloc_pages(1);
    char *page2 = alloc_pages(1);
    memset(page1, 0, NVME_PAGE_SIZE);
    memset(page2, 0, NVME_PAGE_SIZE);

    uint32_t seg1_len = 150;
    uint32_t seg2_len = 250;
    uint32_t total = seg1_len + seg2_len;

    char flat[400];
    memset(flat, 0xCC, seg1_len);
    memset(flat + seg1_len, 0xDD, seg2_len);

    struct prp_list pl;
    memset(&pl, 0, sizeof(pl));
    pl.segs[0].addr = (uint64_t)(uintptr_t)page1;
    pl.segs[0].len  = seg1_len;
    pl.segs[1].addr = (uint64_t)(uintptr_t)page2;
    pl.segs[1].len  = seg2_len;
    pl.nseg = 2;
    pl.total_len = total;

    int rc = prp_copy_to_host(&pl, flat, total);
    TEST_ASSERT(rc == HFSSS_OK, "test13: copy_to_host two-seg returns OK");

    bool ok = true;
    for (uint32_t i = 0; i < seg1_len; i++)
        if ((unsigned char)page1[i] != 0xCC) { ok = false; break; }
    for (uint32_t i = 0; ok && i < seg2_len; i++)
        if ((unsigned char)page2[i] != 0xDD) { ok = false; break; }
    TEST_ASSERT(ok, "test13: copy_to_host two-seg data scatter written correctly");

    free(page1);
    free(page2);
}

/* Test 14: prp_copy_from_host: flat_len mismatch -> HFSSS_ERR_INVAL */
static void test_copy_from_host_len_mismatch(void)
{
    char *page = alloc_pages(1);

    struct prp_list pl;
    memset(&pl, 0, sizeof(pl));
    pl.segs[0].addr = (uint64_t)(uintptr_t)page;
    pl.segs[0].len  = 512;
    pl.nseg = 1;
    pl.total_len = 512;

    char flat[1024];
    int rc = prp_copy_from_host(&pl, flat, 1024); /* mismatch */
    TEST_ASSERT(rc == HFSSS_ERR_INVAL,
                "test14: copy_from_host len mismatch -> HFSSS_ERR_INVAL");

    free(page);
}

/* Test 15: prp_copy_to_host: flat_len mismatch -> HFSSS_ERR_INVAL */
static void test_copy_to_host_len_mismatch(void)
{
    char *page = alloc_pages(1);
    memset(page, 0, NVME_PAGE_SIZE);

    struct prp_list pl;
    memset(&pl, 0, sizeof(pl));
    pl.segs[0].addr = (uint64_t)(uintptr_t)page;
    pl.segs[0].len  = 512;
    pl.nseg = 1;
    pl.total_len = 512;

    char flat[256];
    int rc = prp_copy_to_host(&pl, flat, 256); /* mismatch */
    TEST_ASSERT(rc == HFSSS_ERR_INVAL,
                "test15: copy_to_host len mismatch -> HFSSS_ERR_INVAL");

    free(page);
}

/* Test 16: total_len in prp_list equals sum of segment lengths */
static void test_total_len_equals_sum(void)
{
    char *pages[4];
    for (int i = 0; i < 4; i++)
        pages[i] = alloc_pages(1);

    uint64_t *prp_array = malloc(PRP_MAX_SEGMENTS * sizeof(uint64_t));
    prp_array[0] = (uint64_t)(uintptr_t)pages[1];
    prp_array[1] = (uint64_t)(uintptr_t)pages[2];
    prp_array[2] = (uint64_t)(uintptr_t)pages[3];

    uint64_t prp1 = (uint64_t)(uintptr_t)pages[0];
    uint64_t prp2 = (uint64_t)(uintptr_t)prp_array;
    uint32_t xfer_len = 4 * NVME_PAGE_SIZE;

    struct prp_list pl;
    int rc = prp_build_list(prp1, prp2, xfer_len, NVME_PAGE_SIZE, &pl);
    TEST_ASSERT(rc == HFSSS_OK, "test16: four-page build OK");

    uint64_t sum = 0;
    for (uint32_t i = 0; i < pl.nseg; i++)
        sum += pl.segs[i].len;
    TEST_ASSERT(sum == pl.total_len, "test16: total_len == sum of seg lengths");
    TEST_ASSERT(pl.total_len == xfer_len, "test16: total_len == xfer_len");

    for (int i = 0; i < 4; i++)
        free(pages[i]);
    free(prp_array);
}

/* Test 17: NULL safety: prp_build_list(NULL out) -> HFSSS_ERR_INVAL */
static void test_null_out(void)
{
    char *page = alloc_pages(1);
    uint64_t prp1 = (uint64_t)(uintptr_t)page;
    int rc = prp_build_list(prp1, 0, 512, NVME_PAGE_SIZE, NULL);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL,
                "test17: prp_build_list NULL out -> HFSSS_ERR_INVAL");
    free(page);
}

/* Test 18: NULL safety: prp_copy_from_host(NULL pl) -> HFSSS_ERR_INVAL */
static void test_null_pl(void)
{
    char flat[64];
    int rc = prp_copy_from_host(NULL, flat, 64);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL,
                "test18: prp_copy_from_host NULL pl -> HFSSS_ERR_INVAL");
}

/* Bonus test 19: NULL buf in copy_to_host -> HFSSS_ERR_INVAL */
static void test_null_flat_buf(void)
{
    char *page = alloc_pages(1);
    struct prp_list pl;
    memset(&pl, 0, sizeof(pl));
    pl.segs[0].addr = (uint64_t)(uintptr_t)page;
    pl.segs[0].len  = 512;
    pl.nseg = 1;
    pl.total_len = 512;

    int rc = prp_copy_to_host(&pl, NULL, 512);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL,
                "test19: prp_copy_to_host NULL flat_buf -> HFSSS_ERR_INVAL");
    free(page);
}

/* Bonus test 20: prp_validate with NULL pl -> false */
static void test_prp_validate_null(void)
{
    TEST_ASSERT(prp_validate(NULL, NVME_PAGE_SIZE) == false,
                "test20: prp_validate NULL pl -> false");
}

int main(void)
{
    printf("========================================\n");
    printf("test_prp: PRP List Parser Tests\n");
    printf("========================================\n");

    test_single_page_partial();
    test_single_page_aligned();
    test_two_page_crossing();
    test_two_page_exact();
    test_three_page_prp_list();
    test_five_page_prp_list();
    test_zero_xfer_len();
    test_prp_validate_valid();
    test_prp_validate_invalid();
    test_copy_from_host_single();
    test_copy_from_host_two_segs();
    test_copy_to_host_single();
    test_copy_to_host_two_segs();
    test_copy_from_host_len_mismatch();
    test_copy_to_host_len_mismatch();
    test_total_len_equals_sum();
    test_null_out();
    test_null_pl();
    test_null_flat_buf();
    test_prp_validate_null();

    printf("----------------------------------------\n");
    printf("Total: %d, Passed: %d, Failed: %d\n",
           g_total, g_passed, g_failed);
    if (g_failed == 0)
        printf("SUCCESS: All tests passed!\n");
    else
        printf("FAILURE: %d test(s) failed!\n", g_failed);
    printf("========================================\n");

    return (g_failed == 0) ? 0 : 1;
}
