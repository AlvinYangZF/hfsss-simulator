#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/common.h"
#include "ftl/t10_pi.h"
#include "hal/hal.h"
#include "media/media.h"

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

/* End-to-end PI-through-GC-migration test (REQ-157).
 *
 * Verifies that when a live page is migrated between blocks (the core GC
 * operation), its spare-area T10 PI tuple survives the read/program
 * round-trip through the HAL. If the HAL or media layer ever drops the
 * spare during a GC-style copy, `pi_verify` at the destination will fail
 * on guard/ref_tag mismatch and the invariant is lost. */
static void test_pi_gc_preservation(void)
{
    u8 page_buf[4096];
    u8 spare_buf[64];
    struct t10_pi_tuple pi_generated, pi_read_back;
    struct media_ctx media_ctx;
    struct media_config media_config;
    struct hal_ctx hal_ctx;
    struct hal_nand_dev nand_dev;
    struct hal_nor_dev nor_dev;
    struct hal_pci_ctx pci_ctx;
    struct hal_power_ctx power_ctx;
    const u64 kLba = 500;
    int ret;

    print_separator();
    printf("Test: T10 PI Preservation Through GC-style Page Migration (REQ-157)\n");
    print_separator();

    memset(&media_config, 0, sizeof(media_config));
    media_config.channel_count     = 1;
    media_config.chips_per_channel = 1;
    media_config.dies_per_chip     = 1;
    media_config.planes_per_die    = 1;
    media_config.blocks_per_plane  = 4;
    media_config.pages_per_block   = 4;
    media_config.page_size         = 4096;
    media_config.spare_size        = 64;
    media_config.nand_type         = NAND_TYPE_TLC;

    ret = media_init(&media_ctx, &media_config);
    TEST_ASSERT(ret == HFSSS_OK, "media_init for GC-PI test");

    ret = hal_nand_dev_init(&nand_dev, 1, 1, 1, 1, 4, 4, 4096, 64, &media_ctx);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nand_dev_init for GC-PI test");

    ret = hal_nor_dev_init(&nor_dev, 1024 * 1024, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_dev_init for GC-PI test");
    ret = hal_pci_init(&pci_ctx);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_init for GC-PI test");
    ret = hal_power_init(&power_ctx);
    TEST_ASSERT(ret == HFSSS_OK, "hal_power_init for GC-PI test");
    ret = hal_init_full(&hal_ctx, &nand_dev, &nor_dev, &pci_ctx, &power_ctx);
    TEST_ASSERT(ret == HFSSS_OK, "hal_init_full for GC-PI test");

    /* 1. Build a page with Type-1 PI and stash the tuple in the spare. */
    for (int i = 0; i < 4096; i++) {
        page_buf[i] = (u8)((i * 7 + 3) & 0xFF);
    }
    ret = pi_generate(&pi_generated, page_buf, 4096, kLba, PI_TYPE_1);
    TEST_ASSERT(ret == HFSSS_OK, "generate PI for source page");

    memset(spare_buf, 0xFF, sizeof(spare_buf));
    memcpy(spare_buf, &pi_generated, sizeof(pi_generated));

    /* 2. Program the source page + spare into block 0, page 0. */
    ret = hal_nand_program_sync(&hal_ctx, 0, 0, 0, 0, 0, 0,
                                page_buf, spare_buf);
    TEST_ASSERT(ret == HFSSS_OK, "program source page + PI spare");

    /* 3. GC read: fetch the live page + spare. */
    u8 gc_page[4096];
    u8 gc_spare[64];
    memset(gc_spare, 0, sizeof(gc_spare));
    ret = hal_nand_read_sync(&hal_ctx, 0, 0, 0, 0, 0, 0,
                             gc_page, gc_spare);
    TEST_ASSERT(ret == HFSSS_OK, "GC read of source page + spare");
    TEST_ASSERT(memcmp(gc_page, page_buf, 4096) == 0,
                "data round-trips through HAL read/program");
    TEST_ASSERT(memcmp(gc_spare, spare_buf, sizeof(pi_generated)) == 0,
                "spare round-trips through HAL read/program");

    /* 4. GC program: migrate those exact bytes into a different block. */
    ret = hal_nand_program_sync(&hal_ctx, 0, 0, 0, 0, /*block=*/2, /*page=*/0,
                                gc_page, gc_spare);
    TEST_ASSERT(ret == HFSSS_OK, "GC program to destination block");

    /* 5. Read from the destination and reconstruct the PI tuple. */
    u8 dst_page[4096];
    u8 dst_spare[64];
    memset(dst_spare, 0, sizeof(dst_spare));
    ret = hal_nand_read_sync(&hal_ctx, 0, 0, 0, 0, 2, 0,
                             dst_page, dst_spare);
    TEST_ASSERT(ret == HFSSS_OK, "read destination page after GC migrate");

    memcpy(&pi_read_back, dst_spare, sizeof(pi_read_back));

    /* 6. The Type-1 PI tuple must still validate against the LBA. */
    ret = pi_verify(&pi_read_back, dst_page, 4096, kLba, PI_TYPE_1);
    TEST_ASSERT(ret == HFSSS_OK,
                "pi_verify passes on migrated page (PI survived GC copy)");

    hal_cleanup(&hal_ctx);
    hal_nand_dev_cleanup(&nand_dev);
    hal_nor_dev_cleanup(&nor_dev);
    hal_pci_cleanup(&pci_ctx);
    hal_power_cleanup(&power_ctx);
    media_cleanup(&media_ctx);

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
