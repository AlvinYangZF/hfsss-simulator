#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "common/log.h"
#include "media/media.h"
#include "hal/hal.h"
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
} while(0)

/* Mapping Tests */
static int test_mapping(void)
{
    printf("\n=== Mapping Tests ===\n");

    struct mapping_ctx ctx;
    union ppn ppn;
    u64 lba;
    int ret;

    ret = mapping_init(&ctx, 1024, 4096);
    TEST_ASSERT(ret == HFSSS_OK, "mapping_init should succeed");

    /* Test insert */
    ppn.raw = 0x12345678;
    ret = mapping_insert(&ctx, 100, ppn);
    TEST_ASSERT(ret == HFSSS_OK, "mapping_insert should succeed");

    /* Test L2P lookup */
    union ppn ppn_out;
    ret = mapping_l2p(&ctx, 100, &ppn_out);
    TEST_ASSERT(ret == HFSSS_OK, "mapping_l2p should succeed");
    TEST_ASSERT(ppn_out.raw == ppn.raw, "L2P should return correct PPN");

    /* Test P2L lookup */
    ret = mapping_p2l(&ctx, ppn, &lba);
    TEST_ASSERT(ret == HFSSS_OK, "mapping_p2l should succeed");
    TEST_ASSERT(lba == 100, "P2L should return correct LBA");

    /* Test valid count */
    TEST_ASSERT(mapping_get_valid_count(&ctx) == 1, "valid count should be 1");

    /* Test update */
    union ppn ppn_new, ppn_old;
    ppn_new.raw = 0xABCDEF01;
    ret = mapping_update(&ctx, 100, ppn_new, &ppn_old);
    TEST_ASSERT(ret == HFSSS_OK, "mapping_update should succeed");
    TEST_ASSERT(ppn_old.raw == ppn.raw, "old PPN should match");

    /* Verify update */
    ret = mapping_l2p(&ctx, 100, &ppn_out);
    TEST_ASSERT(ret == HFSSS_OK, "mapping_l2p after update should succeed");
    TEST_ASSERT(ppn_out.raw == ppn_new.raw, "L2P should return new PPN");

    /* Test remove */
    ret = mapping_remove(&ctx, 100);
    TEST_ASSERT(ret == HFSSS_OK, "mapping_remove should succeed");
    TEST_ASSERT(mapping_get_valid_count(&ctx) == 0, "valid count should be 0 after remove");

    /* Test L2P after remove should fail */
    ret = mapping_l2p(&ctx, 100, &ppn_out);
    TEST_ASSERT(ret == HFSSS_ERR_NOENT, "mapping_l2p after remove should fail");

    mapping_cleanup(&ctx);

    /* Test NULL handling */
    TEST_ASSERT(mapping_init(NULL, 1024, 4096) == HFSSS_ERR_INVAL,
                "mapping_init with NULL should fail");
    mapping_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Block Manager Tests */
static int test_block_mgr(void)
{
    printf("\n=== Block Manager Tests ===\n");

    struct block_mgr mgr;
    struct block_desc *block;
    int ret;

    ret = block_mgr_init(&mgr, 1, 1, 1, 1, 10);
    TEST_ASSERT(ret == HFSSS_OK, "block_mgr_init should succeed");

    /* Test free count */
    TEST_ASSERT(block_get_free_count(&mgr) == 10, "initial free count should be 10");

    /* Test allocate */
    block = block_alloc(&mgr);
    TEST_ASSERT(block != NULL, "block_alloc should succeed");
    TEST_ASSERT(block->state == FTL_BLOCK_OPEN, "block should be open");
    TEST_ASSERT(block_get_free_count(&mgr) == 9, "free count should be 9");

    /* Test mark closed */
    ret = block_mark_closed(&mgr, block);
    TEST_ASSERT(ret == HFSSS_OK, "block_mark_closed should succeed");
    TEST_ASSERT(block->state == FTL_BLOCK_CLOSED, "block should be closed");

    /* Test find victim */
    block = block_find_victim(&mgr, GC_POLICY_GREEDY);
    TEST_ASSERT(block != NULL, "block_find_victim should find a block");

    /* Test free */
    ret = block_free(&mgr, block);
    TEST_ASSERT(ret == HFSSS_OK, "block_free should succeed");
    TEST_ASSERT(block_get_free_count(&mgr) == 10, "free count should be 10 again");

    block_mgr_cleanup(&mgr);

    /* Test NULL handling */
    TEST_ASSERT(block_mgr_init(NULL, 1, 1, 1, 1, 10) == HFSSS_ERR_INVAL,
                "block_mgr_init with NULL should fail");
    block_mgr_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* GC Tests */
static int test_gc(void)
{
    printf("\n=== GC Tests ===\n");

    struct gc_ctx ctx;
    int ret;

    ret = gc_init(&ctx, GC_POLICY_GREEDY, 5, 10, 2);
    TEST_ASSERT(ret == HFSSS_OK, "gc_init should succeed");

    /* Test initial stats */
    u64 gc_count, moved_pages, reclaimed_blocks;
    gc_get_stats(&ctx, &gc_count, &moved_pages, &reclaimed_blocks);
    TEST_ASSERT(gc_count == 0, "initial gc_count should be 0");
    TEST_ASSERT(moved_pages == 0, "initial moved_pages should be 0");
    TEST_ASSERT(reclaimed_blocks == 0, "initial reclaimed_blocks should be 0");

    /* Test should trigger */
    TEST_ASSERT(gc_should_trigger(&ctx, 3) == true, "should trigger when free <= threshold");
    TEST_ASSERT(gc_should_trigger(&ctx, 10) == false, "should not trigger when free > threshold");

    gc_cleanup(&ctx);

    /* Test NULL handling */
    TEST_ASSERT(gc_init(NULL, GC_POLICY_GREEDY, 5, 10, 2) == HFSSS_ERR_INVAL,
                "gc_init with NULL should fail");
    gc_cleanup(NULL);
    gc_get_stats(NULL, NULL, NULL, NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* FTL Tests */
static int test_ftl(void)
{
    printf("\n=== FTL Tests ===\n");

    struct media_ctx media_ctx;
    struct media_config media_config;
    struct hal_nand_dev nand_dev;
    struct hal_ctx hal_ctx;
    struct ftl_ctx ftl_ctx;
    struct ftl_config ftl_config;
    int ret;

    /* Initialize media layer */
    memset(&media_config, 0, sizeof(media_config));
    media_config.channel_count = 2;
    media_config.chips_per_channel = 1;
    media_config.dies_per_chip = 1;
    media_config.planes_per_die = 2;
    media_config.blocks_per_plane = 20;
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

    /* Initialize HAL */
    ret = hal_init(&hal_ctx, &nand_dev);
    TEST_ASSERT(ret == HFSSS_OK, "hal_init should succeed");

    /* Initialize FTL config */
    memset(&ftl_config, 0, sizeof(ftl_config));
    ftl_config.total_lbas = 1024;
    ftl_config.page_size = media_config.page_size;
    ftl_config.pages_per_block = media_config.pages_per_block;
    ftl_config.blocks_per_plane = media_config.blocks_per_plane;
    ftl_config.planes_per_die = media_config.planes_per_die;
    ftl_config.dies_per_chip = media_config.dies_per_chip;
    ftl_config.chips_per_channel = media_config.chips_per_channel;
    ftl_config.channel_count = media_config.channel_count;
    ftl_config.op_ratio = 10;
    ftl_config.gc_policy = GC_POLICY_GREEDY;
    ftl_config.gc_threshold = 5;
    ftl_config.gc_hiwater = 10;
    ftl_config.gc_lowater = 2;

    /* Initialize FTL */
    ret = ftl_init(&ftl_ctx, &ftl_config, &hal_ctx);
    TEST_ASSERT(ret == HFSSS_OK, "ftl_init should succeed");

    /* Test write */
    u8 write_data[4096];
    memset(write_data, 0xAA, sizeof(write_data));

    ret = ftl_write(&ftl_ctx, 0, 4096, write_data);
    TEST_ASSERT(ret == HFSSS_OK, "ftl_write should succeed");

    /* Test read back */
    u8 read_data[4096];
    memset(read_data, 0, sizeof(read_data));

    ret = ftl_read(&ftl_ctx, 0, 4096, read_data);
    TEST_ASSERT(ret == HFSSS_OK, "ftl_read should succeed");
    TEST_ASSERT(memcmp(read_data, write_data, sizeof(read_data)) == 0,
                "read data should match written data");

    /* Test trim */
    ret = ftl_trim(&ftl_ctx, 0, 4096);
    TEST_ASSERT(ret == HFSSS_OK, "ftl_trim should succeed");

    /* Test flush */
    ret = ftl_flush(&ftl_ctx);
    TEST_ASSERT(ret == HFSSS_OK, "ftl_flush should succeed");

    /* Test stats */
    struct ftl_stats stats;
    ftl_get_stats(&ftl_ctx, &stats);
    TEST_ASSERT(stats.write_count == 1, "write_count should be 1");
    TEST_ASSERT(stats.read_count == 1, "read_count should be 1");
    TEST_ASSERT(stats.trim_count == 1, "trim_count should be 1");

    /* Reset stats */
    ftl_reset_stats(&ftl_ctx);
    ftl_get_stats(&ftl_ctx, &stats);
    TEST_ASSERT(stats.write_count == 0, "write_count should be 0 after reset");

    /* Cleanup */
    ftl_cleanup(&ftl_ctx);
    hal_cleanup(&hal_ctx);
    hal_nand_dev_cleanup(&nand_dev);
    media_cleanup(&media_ctx);

    /* Test NULL handling */
    TEST_ASSERT(ftl_init(NULL, &ftl_config, &hal_ctx) == HFSSS_ERR_INVAL,
                "ftl_init with NULL ctx should fail");
    TEST_ASSERT(ftl_init(&ftl_ctx, NULL, &hal_ctx) == HFSSS_ERR_INVAL,
                "ftl_init with NULL config should fail");
    TEST_ASSERT(ftl_init(&ftl_ctx, &ftl_config, NULL) == HFSSS_ERR_INVAL,
                "ftl_init with NULL hal should fail");
    ftl_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Main */
int main(void)
{
    printf("========================================\n");
    printf("HFSSS FTL Module Tests\n");
    printf("========================================\n");

    int result = 0;

    (void)result; /* Suppress unused variable warning */

    test_mapping();
    test_block_mgr();
    test_gc();
    test_ftl();

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
