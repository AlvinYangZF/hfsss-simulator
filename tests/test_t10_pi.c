#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/common.h"
#include "ftl/t10_pi.h"

/* Test counters */
static int total_tests = 0;
static int passed_tests = 0;
static int failed_tests = 0;

#define TEST_ASSERT(cond, msg) do { \
    total_tests++; \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        passed_tests++; \
    } else { \
        printf("  [FAIL] %s\n", msg); \
        failed_tests++; \
    } \
} while (0)

static void print_separator(void)
{
    printf("========================================\n");
}

static void test_pi_type1_guard_tag(void)
{
    u8 data[4096];
    struct t10_pi_tuple pi;
    int ret;
    int i;

    print_separator();
    printf("Test: T10 PI Type 1 Guard Tag\n");
    print_separator();

    /* Fill with a known pattern */
    for (i = 0; i < 4096; i++) {
        data[i] = (u8)(i & 0xFF);
    }

    ret = pi_generate(&pi, data, 4096, 42, PI_TYPE_1);
    TEST_ASSERT(ret == HFSSS_OK, "pi_generate Type 1 succeeds");

    ret = pi_verify(&pi, data, 4096, 42, PI_TYPE_1);
    TEST_ASSERT(ret == HFSSS_OK, "pi_verify Type 1 with correct data succeeds");

    /* Corrupt one byte */
    data[2048] ^= 0x01;
    ret = pi_verify(&pi, data, 4096, 42, PI_TYPE_1);
    TEST_ASSERT(ret == HFSSS_ERR_PI_GUARD,
                "pi_verify detects guard tag mismatch after corruption");

    printf("\n");
}

static void test_pi_type1_ref_tag(void)
{
    u8 data[4096];
    struct t10_pi_tuple pi;
    int ret;
    int i;

    print_separator();
    printf("Test: T10 PI Type 1 Reference Tag\n");
    print_separator();

    for (i = 0; i < 4096; i++) {
        data[i] = (u8)(i & 0xFF);
    }

    ret = pi_generate(&pi, data, 4096, 100, PI_TYPE_1);
    TEST_ASSERT(ret == HFSSS_OK, "pi_generate with LBA 100");

    /* Verify with wrong LBA should fail */
    ret = pi_verify(&pi, data, 4096, 200, PI_TYPE_1);
    TEST_ASSERT(ret == HFSSS_ERR_PI_REFTAG,
                "pi_verify detects reference tag mismatch (wrong LBA)");

    /* Verify with correct LBA should pass */
    ret = pi_verify(&pi, data, 4096, 100, PI_TYPE_1);
    TEST_ASSERT(ret == HFSSS_OK,
                "pi_verify with correct LBA succeeds");

    printf("\n");
}

static void test_pi_gc_preservation(void)
{
    u8 data[4096];
    struct t10_pi_tuple pi_src, pi_dst;
    int ret;
    int i;

    print_separator();
    printf("Test: T10 PI Preservation During GC\n");
    print_separator();

    for (i = 0; i < 4096; i++) {
        data[i] = (u8)((i * 7 + 3) & 0xFF);
    }

    /* Generate PI for source page */
    ret = pi_generate(&pi_src, data, 4096, 500, PI_TYPE_1);
    TEST_ASSERT(ret == HFSSS_OK, "generate PI for source page");

    /* Simulate GC: copy PI metadata without regeneration */
    memcpy(&pi_dst, &pi_src, sizeof(struct t10_pi_tuple));

    /* Verify copied PI against original data */
    ret = pi_verify(&pi_dst, data, 4096, 500, PI_TYPE_1);
    TEST_ASSERT(ret == HFSSS_OK,
                "PI preserved across GC migration (copy + verify)");

    printf("\n");
}

static void test_pi_disabled_namespace(void)
{
    u8 data[512];
    struct t10_pi_tuple pi;
    int ret;

    print_separator();
    printf("Test: PI Disabled Namespace\n");
    print_separator();

    memset(data, 0xAA, sizeof(data));

    /* PI_TYPE_NONE should reject generation */
    ret = pi_generate(&pi, data, 512, 0, PI_TYPE_NONE);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL,
                "pi_generate with PI_TYPE_NONE returns INVAL");

    /* PI_TYPE_NONE should reject verification */
    ret = pi_verify(&pi, data, 512, 0, PI_TYPE_NONE);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL,
                "pi_verify with PI_TYPE_NONE returns INVAL");

    /* Type 3 should not check ref_tag */
    ret = pi_generate(&pi, data, 512, 10, PI_TYPE_3);
    TEST_ASSERT(ret == HFSSS_OK, "pi_generate Type 3 succeeds");

    /* Type 3 verify with any LBA should pass (ref_tag not checked) */
    ret = pi_verify(&pi, data, 512, 999, PI_TYPE_3);
    TEST_ASSERT(ret == HFSSS_OK,
                "Type 3 verify ignores ref_tag (any LBA passes)");

    printf("\n");
}

int main(void)
{
    print_separator();
    printf("HFSSS T10 DIF/PI Tests\n");
    print_separator();
    printf("\n");

    test_pi_type1_guard_tag();
    test_pi_type1_ref_tag();
    test_pi_gc_preservation();
    test_pi_disabled_namespace();

    print_separator();
    printf("Test Summary\n");
    print_separator();
    printf("  Total:  %d\n", total_tests);
    printf("  Passed: %d\n", passed_tests);
    printf("  Failed: %d\n", failed_tests);
    print_separator();

    if (failed_tests == 0) {
        printf("\n[SUCCESS] All tests passed!\n");
        return 0;
    } else {
        printf("\n[FAILURE] Some tests failed!\n");
        return 1;
    }
}
