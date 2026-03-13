#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "common/log.h"
#include "media/media.h"
#include "hal/hal.h"

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
} while(0)

/* HAL NAND Device Tests */
static int test_hal_nand_dev(void)
{
    printf("\n=== HAL NAND Device Tests ===\n");

    struct hal_nand_dev dev;
    int ret;

    ret = hal_nand_dev_init(&dev, 2, 2, 2, 2, 10, 8, 4096, 64, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nand_dev_init should succeed");

    TEST_ASSERT(dev.channel_count == 2, "channel count should be 2");
    TEST_ASSERT(dev.chips_per_channel == 2, "chips per channel should be 2");
    TEST_ASSERT(dev.page_size == 4096, "page size should be 4096");

    hal_nand_dev_cleanup(&dev);

    /* Test NULL handling */
    TEST_ASSERT(hal_nand_dev_init(NULL, 2, 2, 2, 2, 10, 8, 4096, 64, NULL) == HFSSS_ERR_INVAL,
                "hal_nand_dev_init with NULL should fail");
    hal_nand_dev_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* HAL Tests */
static int test_hal(void)
{
    printf("\n=== HAL Module Tests ===\n");

    struct media_ctx media_ctx;
    struct media_config media_config;
    struct hal_nand_dev nand_dev;
    struct hal_ctx hal_ctx;
    int ret;

    /* Initialize media layer first */
    memset(&media_config, 0, sizeof(media_config));
    media_config.channel_count = 1;
    media_config.chips_per_channel = 1;
    media_config.dies_per_chip = 1;
    media_config.planes_per_die = 1;
    media_config.blocks_per_plane = 10;
    media_config.pages_per_block = 8;
    media_config.page_size = 4096;
    media_config.spare_size = 64;
    media_config.nand_type = NAND_TYPE_TLC;

    ret = media_init(&media_ctx, &media_config);
    TEST_ASSERT(ret == HFSSS_OK, "media_init should succeed");

    /* Initialize HAL NAND device */
    ret = hal_nand_dev_init(&nand_dev, media_config.channel_count,
                           media_config.chips_per_channel,
                           media_config.dies_per_chip,
                           media_config.planes_per_die,
                           media_config.blocks_per_plane,
                           media_config.pages_per_block,
                           media_config.page_size,
                           media_config.spare_size,
                           &media_ctx);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nand_dev_init should succeed");

    /* Initialize HAL context */
    ret = hal_init(&hal_ctx, &nand_dev);
    TEST_ASSERT(ret == HFSSS_OK, "hal_init should succeed");

    /* Test program through HAL */
    u8 write_data[4096];
    u8 write_spare[64];
    memset(write_data, 0xBB, sizeof(write_data));
    memset(write_spare, 0xCC, sizeof(write_spare));

    ret = hal_nand_program_sync(&hal_ctx, 0, 0, 0, 0, 0, 0, write_data, write_spare);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nand_program_sync should succeed");

    /* Test read through HAL */
    u8 read_data[4096];
    u8 read_spare[64];
    memset(read_data, 0, sizeof(read_data));
    memset(read_spare, 0, sizeof(read_spare));

    ret = hal_nand_read_sync(&hal_ctx, 0, 0, 0, 0, 0, 0, read_data, read_spare);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nand_read_sync should succeed");
    TEST_ASSERT(memcmp(read_data, write_data, sizeof(read_data)) == 0,
                "read data should match written data");

    /* Test erase through HAL */
    ret = hal_nand_erase_sync(&hal_ctx, 0, 0, 0, 0, 0);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nand_erase_sync should succeed");

    /* Test erase count */
    u32 ec = hal_ctx_nand_get_erase_count(&hal_ctx, 0, 0, 0, 0, 0);
    TEST_ASSERT(ec == 1, "erase count should be 1");

    /* Test stats */
    struct hal_stats stats;
    hal_get_stats(&hal_ctx, &stats);
    TEST_ASSERT(stats.nand_read_count == 1, "read count should be 1");
    TEST_ASSERT(stats.nand_write_count == 1, "write count should be 1");
    TEST_ASSERT(stats.nand_erase_count == 1, "erase count should be 1");

    /* Reset stats */
    hal_reset_stats(&hal_ctx);
    hal_get_stats(&hal_ctx, &stats);
    TEST_ASSERT(stats.nand_read_count == 0, "read count should be 0 after reset");

    /* Cleanup */
    hal_cleanup(&hal_ctx);
    hal_nand_dev_cleanup(&nand_dev);
    media_cleanup(&media_ctx);

    /* Test NULL handling */
    TEST_ASSERT(hal_init(NULL, &nand_dev) == HFSSS_ERR_INVAL,
                "hal_init with NULL ctx should fail");
    TEST_ASSERT(hal_init(&hal_ctx, NULL) == HFSSS_ERR_INVAL,
                "hal_init with NULL nand_dev should fail");
    hal_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* HAL Power Tests */
static int test_hal_power(void)
{
    printf("\n=== HAL Power Tests ===\n");

    struct hal_power_ctx power;
    int ret;

    ret = hal_power_init(&power);
    TEST_ASSERT(ret == HFSSS_OK, "hal_power_init should succeed");

    TEST_ASSERT(hal_power_get_state(&power) == HAL_POWER_ACTIVE, "initial state should be ACTIVE");

    ret = hal_power_set_state(&power, HAL_POWER_IDLE);
    TEST_ASSERT(ret == HFSSS_OK, "hal_power_set_state should succeed");
    TEST_ASSERT(hal_power_get_state(&power) == HAL_POWER_IDLE, "state should be IDLE");

    hal_power_cleanup(&power);

    /* Test NULL handling */
    TEST_ASSERT(hal_power_init(NULL) == HFSSS_ERR_INVAL,
                "hal_power_init with NULL should fail");
    hal_power_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* HAL PCI Tests */
static int test_hal_pci(void)
{
    printf("\n=== HAL PCI Tests ===\n");

    struct hal_pci_ctx pci;
    int ret;

    ret = hal_pci_init(&pci);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_init should succeed");
    TEST_ASSERT(pci.initialized == true, "pci should be initialized");

    hal_pci_cleanup(&pci);

    /* Test NULL handling */
    TEST_ASSERT(hal_pci_init(NULL) == HFSSS_ERR_INVAL,
                "hal_pci_init with NULL should fail");
    hal_pci_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* HAL NOR Tests */
static int test_hal_nor(void)
{
    printf("\n=== HAL NOR Tests ===\n");

    struct hal_nor_dev nor;
    int ret;

    ret = hal_nor_dev_init(&nor, 1024 * 1024, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_dev_init should succeed");
    TEST_ASSERT(nor.size == 1024 * 1024, "NOR size should be 1MB");

    /* NOR operations should return NOTSUPP (placeholder) */
    u8 data[256];
    ret = hal_nor_read(&nor, 0, data, sizeof(data));
    TEST_ASSERT(ret == HFSSS_ERR_NOTSUPP, "hal_nor_read should return NOTSUPP");

    hal_nor_dev_cleanup(&nor);

    /* Test NULL handling */
    TEST_ASSERT(hal_nor_dev_init(NULL, 1024 * 1024, NULL) == HFSSS_ERR_INVAL,
                "hal_nor_dev_init with NULL should fail");
    hal_nor_dev_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Main */
int main(void)
{
    printf("========================================\n");
    printf("HFSSS HAL Module Tests\n");
    printf("========================================\n");

    int result = 0;

    (void)result; /* Suppress unused variable warning */

    test_hal_nand_dev();
    test_hal_power();
    test_hal_pci();
    test_hal_nor();
    test_hal();

    printf("\n========================================\n");
    printf("Test Summary\n");
    printf("========================================\n");
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    if (tests_failed == 0) {
        printf("\n  [SUCCESS] All tests passed!\n");
        return 0;
    } else {
        printf("\n  [FAILURE] Some tests failed!\n");
        return 1;
    }
}
