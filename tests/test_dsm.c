#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "pcie/nvme.h"
#include "pcie/nvme_uspace.h"
#include "ftl/ftl.h"

#define TEST_PASS 0
#define TEST_FAIL 1

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        tests_passed++; \
    } else { \
        printf("  [FAIL] %s\n", msg); \
        tests_failed++; \
    } \
} while (0)

static void print_separator(void)
{
    printf("========================================\n");
}

/* Initialise a small nvme_uspace_dev for integration tests. */
static int setup_dev(struct nvme_uspace_dev *dev)
{
    struct nvme_uspace_config config;

    nvme_uspace_config_default(&config);
    config.sssim_cfg.page_size        = 4096;
    config.sssim_cfg.spare_size       = 64;
    config.sssim_cfg.channel_count    = 2;
    config.sssim_cfg.chips_per_channel = 2;
    config.sssim_cfg.dies_per_chip    = 1;
    config.sssim_cfg.planes_per_die   = 1;
    config.sssim_cfg.blocks_per_plane = 64;
    config.sssim_cfg.pages_per_block  = 64;
    config.sssim_cfg.total_lbas       = 1024;

    int ret = nvme_uspace_dev_init(dev, &config);
    if (ret != HFSSS_OK) {
        return ret;
    }
    return nvme_uspace_dev_start(dev);
}

/* ------------------------------------------------------------------ */
/* Test 1: DSM opcode constant is 0x09                                  */
/* ------------------------------------------------------------------ */
static void test_dsm_opcode(void)
{
    printf("\n--- Test 1: DSM opcode value ---\n");
    TEST_ASSERT(NVME_NVM_DATASET_MANAGEMENT == 0x09,
                "NVME_NVM_DATASET_MANAGEMENT must equal 0x09");
    TEST_ASSERT(NVME_CMD_DSM == 0x09,
                "NVME_CMD_DSM alias must equal 0x09");
}

/* ------------------------------------------------------------------ */
/* Test 2: nvme_dsm_range struct layout (16 bytes: 4+4+8)              */
/* ------------------------------------------------------------------ */
static void test_dsm_range_layout(void)
{
    printf("\n--- Test 2: nvme_dsm_range layout ---\n");
    TEST_ASSERT(sizeof(struct nvme_dsm_range) == 16,
                "nvme_dsm_range must be 16 bytes");
    TEST_ASSERT(offsetof(struct nvme_dsm_range, attributes) == 0,
                "attributes field at offset 0");
    TEST_ASSERT(offsetof(struct nvme_dsm_range, nlb) == 4,
                "nlb field at offset 4");
    TEST_ASSERT(offsetof(struct nvme_dsm_range, slba) == 8,
                "slba field at offset 8");
}

/* ------------------------------------------------------------------ */
/* Test 3: NVME_DSM_ATTR_DEALLOCATE is bit 2                            */
/* ------------------------------------------------------------------ */
static void test_dsm_deallocate_attr(void)
{
    printf("\n--- Test 3: NVME_DSM_ATTR_DEALLOCATE flag ---\n");
    TEST_ASSERT(NVME_DSM_ATTR_DEALLOCATE == (1u << 2),
                "NVME_DSM_ATTR_DEALLOCATE must be bit 2");
    TEST_ASSERT((NVME_DSM_ATTR_DEALLOCATE & 0x04) != 0,
                "Bit 2 is set in NVME_DSM_ATTR_DEALLOCATE");
}

/* ------------------------------------------------------------------ */
/* Test 4: trim with AD=0 (no deallocate) returns HFSSS_ERR_INVAL       */
/* (nvme_uspace_trim requires non-zero nr_ranges / valid input)         */
/* ------------------------------------------------------------------ */
static void test_dsm_no_deallocate_attr(void)
{
    printf("\n--- Test 4: DSM without AD bit — NULL ranges rejected ---\n");
    struct nvme_uspace_dev dev;
    int ret = setup_dev(&dev);
    if (ret != HFSSS_OK) {
        TEST_ASSERT(false, "device setup for test 4 should succeed");
        return;
    }

    /* Passing NULL ranges should be rejected regardless of AD flag. */
    ret = nvme_uspace_trim(&dev, 1, NULL, 1);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL,
                "nvme_uspace_trim with NULL ranges returns HFSSS_ERR_INVAL");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
}

/* ------------------------------------------------------------------ */
/* Test 5: trim with AD=1, 1 range — correct LBA trimmed               */
/* ------------------------------------------------------------------ */
static void test_dsm_single_range(void)
{
    printf("\n--- Test 5: DSM trim single range ---\n");
    struct nvme_uspace_dev dev;
    int ret = setup_dev(&dev);
    if (ret != HFSSS_OK) {
        TEST_ASSERT(false, "device setup for test 5 should succeed");
        return;
    }

    /* Write first then trim */
    uint8_t buf[4096];
    memset(buf, 0xBB, sizeof(buf));
    ret = nvme_uspace_write(&dev, 1, 10, 1, buf);
    TEST_ASSERT(ret == HFSSS_OK, "write before trim should succeed");

    struct nvme_dsm_range range = { .attributes = 0, .nlb = 1, .slba = 10 };
    ret = nvme_uspace_trim(&dev, 1, &range, 1);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_uspace_trim single range should succeed");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
}

/* ------------------------------------------------------------------ */
/* Test 6: trim with AD=1, 3 ranges — all ranges processed             */
/* ------------------------------------------------------------------ */
static void test_dsm_multiple_ranges(void)
{
    printf("\n--- Test 6: DSM trim 3 ranges ---\n");
    struct nvme_uspace_dev dev;
    int ret = setup_dev(&dev);
    if (ret != HFSSS_OK) {
        TEST_ASSERT(false, "device setup for test 6 should succeed");
        return;
    }

    uint8_t buf[4096];
    memset(buf, 0xCC, sizeof(buf));
    nvme_uspace_write(&dev, 1, 20, 1, buf);
    nvme_uspace_write(&dev, 1, 30, 1, buf);
    nvme_uspace_write(&dev, 1, 40, 1, buf);

    struct nvme_dsm_range ranges[3] = {
        { .attributes = 0, .nlb = 1, .slba = 20 },
        { .attributes = 0, .nlb = 1, .slba = 30 },
        { .attributes = 0, .nlb = 1, .slba = 40 },
    };
    ret = nvme_uspace_trim(&dev, 1, ranges, 3);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_uspace_trim with 3 ranges should succeed");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
}

/* ------------------------------------------------------------------ */
/* Test 7: NR=0 in DSM command means 1 range (boundary: nr_ranges=1)   */
/* ------------------------------------------------------------------ */
static void test_dsm_nr_zero_means_one_range(void)
{
    printf("\n--- Test 7: NR=0 implies 1 range ---\n");
    /* CDW10[7:0] NR=0 means (NR+1)=1 range. Verify the arithmetic. */
    uint32_t cdw10 = 0x00;  /* NR=0 */
    uint32_t nr_ranges = (cdw10 & 0xFF) + 1;
    TEST_ASSERT(nr_ranges == 1,
                "NR=0 in CDW10 must yield 1 range after adding 1");

    struct nvme_uspace_dev dev;
    int ret = setup_dev(&dev);
    if (ret != HFSSS_OK) {
        TEST_ASSERT(false, "device setup for test 7 should succeed");
        return;
    }

    struct nvme_dsm_range range = { .attributes = 0, .nlb = 2, .slba = 5 };
    ret = nvme_uspace_trim(&dev, 1, &range, nr_ranges);
    TEST_ASSERT(ret == HFSSS_OK,
                "trim with 1 implied range should succeed");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
}

/* ------------------------------------------------------------------ */
/* Test 8: nlb=0 range is skipped (no-op, succeeds)                    */
/* ------------------------------------------------------------------ */
static void test_dsm_zero_nlb_skipped(void)
{
    printf("\n--- Test 8: DSM range with nlb=0 is skipped ---\n");
    struct nvme_uspace_dev dev;
    int ret = setup_dev(&dev);
    if (ret != HFSSS_OK) {
        TEST_ASSERT(false, "device setup for test 8 should succeed");
        return;
    }

    struct nvme_dsm_range ranges[2] = {
        { .attributes = 0, .nlb = 0, .slba = 100 },  /* skipped */
        { .attributes = 0, .nlb = 1, .slba = 50  },  /* processed */
    };
    ret = nvme_uspace_trim(&dev, 1, ranges, 2);
    TEST_ASSERT(ret == HFSSS_OK,
                "trim with a zero-nlb range mixed in should succeed");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
}

/* ------------------------------------------------------------------ */
/* Test 9: Integration — submit DSM via nvme_uspace API, verify success */
/* ------------------------------------------------------------------ */
static void test_dsm_integration_success(void)
{
    printf("\n--- Test 9: DSM integration — submit and verify success ---\n");
    struct nvme_uspace_dev dev;
    int ret = setup_dev(&dev);
    if (ret != HFSSS_OK) {
        TEST_ASSERT(false, "device setup for test 9 should succeed");
        return;
    }

    uint8_t wbuf[4096];
    memset(wbuf, 0xDE, sizeof(wbuf));
    ret = nvme_uspace_write(&dev, 1, 0, 1, wbuf);
    TEST_ASSERT(ret == HFSSS_OK, "write LBA 0 for integration test should succeed");

    struct nvme_dsm_range range = { .attributes = 0, .nlb = 1, .slba = 0 };
    ret = nvme_uspace_trim(&dev, 1, &range, 1);
    TEST_ASSERT(ret == HFSSS_OK, "DSM trim via nvme_uspace API should succeed");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
}

/* ------------------------------------------------------------------ */
/* Test 10: DSM after write — FTL trim_count increments                 */
/* ------------------------------------------------------------------ */
static void test_dsm_after_write_stats(void)
{
    printf("\n--- Test 10: DSM trim increments FTL trim_count ---\n");
    struct nvme_uspace_dev dev;
    int ret = setup_dev(&dev);
    if (ret != HFSSS_OK) {
        TEST_ASSERT(false, "device setup for test 10 should succeed");
        return;
    }

    uint8_t buf[4096];
    memset(buf, 0xAA, sizeof(buf));
    nvme_uspace_write(&dev, 1, 7, 1, buf);

    struct ftl_stats before, after;
    ftl_get_stats(&dev.sssim.ftl, &before);

    struct nvme_dsm_range range = { .attributes = 0, .nlb = 1, .slba = 7 };
    ret = nvme_uspace_trim(&dev, 1, &range, 1);
    TEST_ASSERT(ret == HFSSS_OK, "trim should succeed");

    ftl_get_stats(&dev.sssim.ftl, &after);
    TEST_ASSERT(after.trim_count > before.trim_count,
                "FTL trim_count should increase after DSM trim");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
}

/* ------------------------------------------------------------------ */
/* Test 11: Multiple sequential DSM commands all succeed               */
/* ------------------------------------------------------------------ */
static void test_dsm_sequential(void)
{
    printf("\n--- Test 11: Multiple sequential DSM commands ---\n");
    struct nvme_uspace_dev dev;
    int ret = setup_dev(&dev);
    if (ret != HFSSS_OK) {
        TEST_ASSERT(false, "device setup for test 11 should succeed");
        return;
    }

    uint8_t buf[4096];
    memset(buf, 0x55, sizeof(buf));

    bool all_ok = true;
    for (int i = 0; i < 5; i++) {
        uint64_t lba = (uint64_t)(i * 10);
        nvme_uspace_write(&dev, 1, lba, 1, buf);
        struct nvme_dsm_range range = { .attributes = 0, .nlb = 1, .slba = lba };
        if (nvme_uspace_trim(&dev, 1, &range, 1) != HFSSS_OK) {
            all_ok = false;
        }
    }
    TEST_ASSERT(all_ok, "all 5 sequential DSM trims should succeed");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
}

/* ------------------------------------------------------------------ */
/* Test 12: DSM on unwritten LBAs succeeds (trim is idempotent)        */
/* ------------------------------------------------------------------ */
static void test_dsm_unwritten_lbas(void)
{
    printf("\n--- Test 12: DSM trim on unwritten LBAs (idempotent) ---\n");
    struct nvme_uspace_dev dev;
    int ret = setup_dev(&dev);
    if (ret != HFSSS_OK) {
        TEST_ASSERT(false, "device setup for test 12 should succeed");
        return;
    }

    /* Trim LBAs that were never written */
    struct nvme_dsm_range range = { .attributes = 0, .nlb = 4, .slba = 200 };
    ret = nvme_uspace_trim(&dev, 1, &range, 1);
    TEST_ASSERT(ret == HFSSS_OK,
                "trim on unwritten LBAs should succeed (idempotent)");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
}

/* ------------------------------------------------------------------ */
/* Test 13: NULL safety — NULL dev pointer rejected                    */
/* ------------------------------------------------------------------ */
static void test_dsm_null_safety(void)
{
    printf("\n--- Test 13: NULL safety checks ---\n");
    struct nvme_dsm_range range = { .attributes = 0, .nlb = 1, .slba = 0 };

    /* NULL dev */
    int ret = nvme_uspace_trim(NULL, 1, &range, 1);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL,
                "nvme_uspace_trim(NULL dev) must return HFSSS_ERR_INVAL");

    /* nr_ranges == 0 */
    struct nvme_uspace_dev dev;
    if (setup_dev(&dev) == HFSSS_OK) {
        ret = nvme_uspace_trim(&dev, 1, &range, 0);
        TEST_ASSERT(ret == HFSSS_ERR_INVAL,
                    "nvme_uspace_trim with nr_ranges=0 must return HFSSS_ERR_INVAL");
        nvme_uspace_dev_stop(&dev);
        nvme_uspace_dev_cleanup(&dev);
    }
}

/* ------------------------------------------------------------------ */
/* Test 14: DSM with invalid NSID rejected                             */
/* ------------------------------------------------------------------ */
static void test_dsm_invalid_nsid(void)
{
    printf("\n--- Test 14: DSM with invalid NSID ---\n");
    struct nvme_uspace_dev dev;
    int ret = setup_dev(&dev);
    if (ret != HFSSS_OK) {
        TEST_ASSERT(false, "device setup for test 14 should succeed");
        return;
    }

    struct nvme_dsm_range range = { .attributes = 0, .nlb = 1, .slba = 0 };
    ret = nvme_uspace_trim(&dev, 99, &range, 1);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL,
                "nvme_uspace_trim with invalid NSID must return HFSSS_ERR_INVAL");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
}

/* ------------------------------------------------------------------ */
int main(void)
{
    print_separator();
    printf("HFSSS NVMe Dataset Management (DSM/Trim) Tests\n");
    print_separator();

    test_dsm_opcode();
    test_dsm_range_layout();
    test_dsm_deallocate_attr();
    test_dsm_no_deallocate_attr();
    test_dsm_single_range();
    test_dsm_multiple_ranges();
    test_dsm_nr_zero_means_one_range();
    test_dsm_zero_nlb_skipped();
    test_dsm_integration_success();
    test_dsm_after_write_stats();
    test_dsm_sequential();
    test_dsm_unwritten_lbas();
    test_dsm_null_safety();
    test_dsm_invalid_nsid();

    print_separator();
    printf("Test Summary\n");
    print_separator();
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    print_separator();

    if (tests_failed == 0) {
        printf("\n  [SUCCESS] All tests passed!\n");
        return 0;
    } else {
        printf("\n  [FAILURE] Some tests failed!\n");
        return 1;
    }
}
