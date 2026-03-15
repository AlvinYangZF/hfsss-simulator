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

/* HAL NOR Tests */
static int test_hal_nor(void)
{
    printf("\n=== HAL NOR Tests ===\n");

    struct hal_nor_dev nor;
    int ret;
    u8 write_data[256];
    u8 read_data[256];

    ret = hal_nor_dev_init(&nor, 1024 * 1024, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_dev_init should succeed");
    TEST_ASSERT(nor.size == 1024 * 1024, "NOR size should be 1MB");

    /* Test NOR read - should read all 0xFF (erased state) */
    memset(read_data, 0, sizeof(read_data));
    ret = hal_nor_read(&nor, 0, read_data, sizeof(read_data));
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_read should succeed");
    bool all_ff = true;
    for (u32 i = 0; i < sizeof(read_data); i++) {
        if (read_data[i] != 0xFF) {
            all_ff = false;
            break;
        }
    }
    TEST_ASSERT(all_ff, "NOR should be erased (all 0xFF) after init");

    /* Test NOR write - can only clear bits */
    memset(write_data, 0xAA, sizeof(write_data));
    ret = hal_nor_write(&nor, 0, write_data, sizeof(write_data));
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_write should succeed");

    /* Read back and verify */
    memset(read_data, 0, sizeof(read_data));
    ret = hal_nor_read(&nor, 0, read_data, sizeof(read_data));
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_read should succeed");
    TEST_ASSERT(memcmp(read_data, write_data, sizeof(read_data)) == 0,
                "read data should match written data");

    /* Test NOR erase */
    ret = hal_nor_erase(&nor, 0, HAL_NOR_SECTOR_SIZE);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_erase should succeed");

    /* Verify erased */
    memset(read_data, 0, sizeof(read_data));
    ret = hal_nor_read(&nor, 0, read_data, sizeof(read_data));
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_read after erase should succeed");
    all_ff = true;
    for (u32 i = 0; i < sizeof(read_data); i++) {
        if (read_data[i] != 0xFF) {
            all_ff = false;
            break;
        }
    }
    TEST_ASSERT(all_ff, "NOR should be erased (all 0xFF) after erase");

    hal_nor_dev_cleanup(&nor);

    /* Test NULL handling */
    TEST_ASSERT(hal_nor_dev_init(NULL, 1024 * 1024, NULL) == HFSSS_ERR_INVAL,
                "hal_nor_dev_init with NULL should fail");
    hal_nor_dev_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* HAL Power Tests */
static int test_hal_power(void)
{
    printf("\n=== HAL Power Tests ===\n");

    struct hal_power_ctx power;
    int ret;
    struct hal_power_state_desc desc;

    ret = hal_power_init(&power);
    TEST_ASSERT(ret == HFSSS_OK, "hal_power_init should succeed");

    TEST_ASSERT(hal_power_get_state(&power) == HAL_POWER_PS0, "initial state should be PS0");

    ret = hal_power_set_state(&power, HAL_POWER_PS2);
    TEST_ASSERT(ret == HFSSS_OK, "hal_power_set_state to PS2 should succeed");
    TEST_ASSERT(hal_power_get_state(&power) == HAL_POWER_PS2, "state should be PS2");

    /* Get state descriptor */
    ret = hal_power_get_state_desc(&power, HAL_POWER_PS0, &desc);
    TEST_ASSERT(ret == HFSSS_OK, "hal_power_get_state_desc should succeed");
    TEST_ASSERT(desc.non_operational == false, "PS0 should be operational");

    ret = hal_power_get_state_desc(&power, HAL_POWER_PS4, &desc);
    TEST_ASSERT(ret == HFSSS_OK, "hal_power_get_state_desc PS4 should succeed");
    TEST_ASSERT(desc.non_operational == true, "PS4 should be non-operational");

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
    struct hal_pci_completion comp;
    struct hal_pci_namespace ns_info;
    u32 nsid_list[HAL_PCI_MAX_NAMESPACES];
    u32 count;

    ret = hal_pci_init(&pci);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_init should succeed");

    /* Test command completion submission (REQ-062) */
    memset(&comp, 0, sizeof(comp));
    comp.command_id = 0x1234;
    comp.status = 0;
    comp.result = 0x5678;
    ret = hal_pci_submit_completion(&pci, &comp);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_submit_completion should succeed");
    TEST_ASSERT(hal_pci_get_completion_count(&pci) == 1, "completion count should be 1");

    /* Test poll completion */
    memset(&comp, 0, sizeof(comp));
    ret = hal_pci_poll_completion(&pci, &comp);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_poll_completion should succeed");
    TEST_ASSERT(comp.command_id == 0x1234, "command ID should match");
    TEST_ASSERT(hal_pci_get_completion_count(&pci) == 0, "completion count should be 0 after poll");

    /* Test namespace management (REQ-065) */
    ret = hal_pci_ns_attach(&pci, 1, 100000, 512);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_ns_attach NSID 1 should succeed");
    TEST_ASSERT(hal_pci_ns_get_active_count(&pci) == 1, "active NS count should be 1");

    ret = hal_pci_ns_attach(&pci, 2, 200000, 4096);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_ns_attach NSID 2 should succeed");
    TEST_ASSERT(hal_pci_ns_get_active_count(&pci) == 2, "active NS count should be 2");

    /* Get NS info */
    ret = hal_pci_ns_get_info(&pci, 1, &ns_info);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_ns_get_info NSID 1 should succeed");
    TEST_ASSERT(ns_info.nsid == 1, "NSID should be 1");
    TEST_ASSERT(ns_info.lba_size == 512, "LBA size should be 512");

    /* List NSIDs */
    count = HAL_PCI_MAX_NAMESPACES;
    ret = hal_pci_ns_list(&pci, nsid_list, &count);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_ns_list should succeed");
    TEST_ASSERT(count == 2, "list count should be 2");

    /* Detach NS */
    ret = hal_pci_ns_detach(&pci, 1);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_ns_detach NSID 1 should succeed");
    TEST_ASSERT(hal_pci_ns_get_active_count(&pci) == 1, "active NS count should be 1 after detach");

    hal_pci_cleanup(&pci);

    /* Test NULL handling */
    TEST_ASSERT(hal_pci_init(NULL) == HFSSS_ERR_INVAL,
                "hal_pci_init with NULL should fail");
    hal_pci_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* HAL Tests */
static int test_hal(void)
{
    printf("\n=== HAL Module Tests ===\n");

    struct media_ctx media_ctx;
    struct media_config media_config;
    struct hal_nand_dev nand_dev;
    struct hal_nor_dev nor_dev;
    struct hal_pci_ctx pci_ctx;
    struct hal_power_ctx power_ctx;
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

    /* Initialize all HAL devices */
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

    ret = hal_nor_dev_init(&nor_dev, 1024 * 1024, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_dev_init should succeed");

    ret = hal_pci_init(&pci_ctx);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_init should succeed");

    ret = hal_power_init(&power_ctx);
    TEST_ASSERT(ret == HFSSS_OK, "hal_power_init should succeed");

    /* Initialize HAL context with all devices */
    ret = hal_init_full(&hal_ctx, &nand_dev, &nor_dev, &pci_ctx, &power_ctx);
    TEST_ASSERT(ret == HFSSS_OK, "hal_init_full should succeed");

    /* Test NAND through HAL */
    u8 write_data[4096];
    u8 write_spare[64];
    memset(write_data, 0xBB, sizeof(write_data));
    memset(write_spare, 0xCC, sizeof(write_spare));

    ret = hal_nand_program_sync(&hal_ctx, 0, 0, 0, 0, 0, 0, write_data, write_spare);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nand_program_sync should succeed");

    u8 read_data[4096];
    u8 read_spare[64];
    memset(read_data, 0, sizeof(read_data));
    memset(read_spare, 0, sizeof(read_spare));

    ret = hal_nand_read_sync(&hal_ctx, 0, 0, 0, 0, 0, 0, read_data, read_spare);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nand_read_sync should succeed");
    TEST_ASSERT(memcmp(read_data, write_data, sizeof(read_data)) == 0,
                "read data should match written data");

    /* Test NOR through HAL */
    u8 nor_write[256];
    u8 nor_read[256];
    memset(nor_write, 0x55, sizeof(nor_write));
    ret = hal_nor_write_sync(&hal_ctx, 0, nor_write, sizeof(nor_write));
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_write_sync should succeed");

    memset(nor_read, 0, sizeof(nor_read));
    ret = hal_nor_read_sync(&hal_ctx, 0, nor_read, sizeof(nor_read));
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_read_sync should succeed");
    TEST_ASSERT(memcmp(nor_read, nor_write, sizeof(nor_read)) == 0,
                "NOR read should match write");

    /* Test Power through HAL */
    ret = hal_power_set_state_sync(&hal_ctx, HAL_POWER_PS1);
    TEST_ASSERT(ret == HFSSS_OK, "hal_power_set_state_sync should succeed");
    TEST_ASSERT(hal_power_get_state_sync(&hal_ctx) == HAL_POWER_PS1, "power state should be PS1");

    /* Test stats */
    struct hal_stats stats;
    hal_get_stats(&hal_ctx, &stats);
    TEST_ASSERT(stats.nand_read_count == 1, "NAND read count should be 1");
    TEST_ASSERT(stats.nand_write_count == 1, "NAND write count should be 1");
    TEST_ASSERT(stats.nor_read_count == 1, "NOR read count should be 1");
    TEST_ASSERT(stats.nor_write_count == 1, "NOR write count should be 1");

    /* Cleanup */
    hal_cleanup(&hal_ctx);
    hal_nand_dev_cleanup(&nand_dev);
    hal_nor_dev_cleanup(&nor_dev);
    hal_pci_cleanup(&pci_ctx);
    hal_power_cleanup(&power_ctx);
    media_cleanup(&media_ctx);

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
    test_hal_nor();
    test_hal_power();
    test_hal_pci();
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
