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

/*
 * test_block_mark_bad_from_free — verify that block_mark_bad removes a block
 * from the free list and permanently marks it FTL_BLOCK_BAD.
 *
 * Steps:
 *   1. Init a 10-block manager.
 *   2. Alloc one block (moves it to open list, free_count = 9).
 *   3. Return that block to the free list via block_free (free_count = 10).
 *   4. Confirm the descriptor is FREE.
 *   5. Call block_mark_bad on that descriptor.
 *   6. Assert: state == FTL_BLOCK_BAD, free_count == 9.
 *   7. Assert: block_alloc never returns that descriptor again.
 */
static void test_block_mark_bad_from_free(void)
{
    printf("\n=== block_mark_bad removes block from free list ===\n");

    struct block_mgr mgr;
    int ret;

    ret = block_mgr_init(&mgr, 1, 1, 1, 1, 10);
    TEST_ASSERT(ret == HFSSS_OK, "block_mgr_init should succeed");

    /* Alloc and immediately return to free list. */
    struct block_desc *target = block_alloc(&mgr);
    TEST_ASSERT(target != NULL, "block_alloc must return a descriptor");
    if (!target) { block_mgr_cleanup(&mgr); return; }

    TEST_ASSERT(target->state == FTL_BLOCK_OPEN, "allocated block state should be OPEN");

    ret = block_free(&mgr, target);
    TEST_ASSERT(ret == HFSSS_OK, "block_free should succeed");
    TEST_ASSERT(target->state == FTL_BLOCK_FREE, "block state after free should be FREE");
    TEST_ASSERT(block_get_free_count(&mgr) == 10, "free count should be 10 after returning block");

    /* Now retire it permanently via block_mark_bad. */
    ret = block_mark_bad(&mgr, target);
    TEST_ASSERT(ret == HFSSS_OK, "block_mark_bad should succeed");
    TEST_ASSERT(target->state == FTL_BLOCK_BAD, "block state must be BAD after block_mark_bad");

    u64 free_after_bad = block_get_free_count(&mgr);
    TEST_ASSERT(free_after_bad == 9,
                "free count must decrease by 1 after block_mark_bad on free block");

    /*
     * Drain all remaining allocatable blocks.  The retired block must never
     * appear among them.
     */
    bool target_reallocated = false;
    for (int i = 0; i < 9; i++) {
        struct block_desc *b = block_alloc(&mgr);
        if (b == target) {
            target_reallocated = true;
        }
        /* Put each block back so subsequent allocs keep working. */
        if (b) {
            block_free(&mgr, b);
        }
    }
    TEST_ASSERT(!target_reallocated,
                "bad block must never be returned by block_alloc");

    /*
     * Calling block_mark_bad on an already-BAD block must be idempotent and
     * must not change the free count.
     */
    ret = block_mark_bad(&mgr, target);
    TEST_ASSERT(ret == HFSSS_OK, "second block_mark_bad on BAD block should be idempotent");
    TEST_ASSERT(block_get_free_count(&mgr) == 9,
                "free count must be unchanged after idempotent block_mark_bad");

    block_mgr_cleanup(&mgr);
}

/*
 * test_gc_flush_dst_closes_block — verify that gc_flush_dst closes the
 * persistent GC destination block (ctx->dst_block becomes NULL) and moves
 * the block from the open list to the closed list.
 *
 * The test exercises gc_flush_dst at the FTL level: after running enough
 * writes to trigger at least one GC cycle, call ftl_flush (which internally
 * calls gc_flush_dst) and check that the gc_ctx's dst_block pointer is NULL
 * and the block_mgr's closed_blocks count increased.
 */
static void test_gc_flush_dst_closes_block(void)
{
    printf("\n=== gc_flush_dst closes persistent GC destination block ===\n");

    /*
     * Geometry: 1ch × 1chip × 1die × 1plane × 16blk × 4pg.
     * No factory-bad blocks (16 blocks, blocks_per_plane-10=6, range 11..5
     * is empty so the bad-block formula never fires).
     * gc_threshold=3 so GC fires when free_blocks <= 3.
     */
#define GFDT_CH    1
#define GFDT_CHIP  1
#define GFDT_DIE   1
#define GFDT_PLANE 1
#define GFDT_BLKS  16
#define GFDT_PGS   4
#define GFDT_PGSZ  4096

    struct media_ctx    media;
    struct hal_nand_dev nand;
    struct hal_ctx      hal;
    struct ftl_ctx      ftl;
    u8 wbuf[GFDT_PGSZ];
    int ret;

    struct media_config mcfg;
    struct ftl_config   fcfg;

    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.channel_count      = GFDT_CH;
    mcfg.chips_per_channel  = GFDT_CHIP;
    mcfg.dies_per_chip      = GFDT_DIE;
    mcfg.planes_per_die     = GFDT_PLANE;
    mcfg.blocks_per_plane   = GFDT_BLKS;
    mcfg.pages_per_block    = GFDT_PGS;
    mcfg.page_size          = GFDT_PGSZ;
    mcfg.spare_size         = 64;
    mcfg.nand_type          = NAND_TYPE_TLC;

    ret = media_init(&media, &mcfg);
    TEST_ASSERT(ret == HFSSS_OK, "gc-flush-dst: media_init should succeed");
    if (ret != HFSSS_OK) return;

    ret = hal_nand_dev_init(&nand, GFDT_CH, GFDT_CHIP, GFDT_DIE, GFDT_PLANE,
                             GFDT_BLKS, GFDT_PGS, GFDT_PGSZ, 64, &media);
    TEST_ASSERT(ret == HFSSS_OK, "gc-flush-dst: hal_nand_dev_init should succeed");
    if (ret != HFSSS_OK) { media_cleanup(&media); return; }

    ret = hal_init(&hal, &nand);
    TEST_ASSERT(ret == HFSSS_OK, "gc-flush-dst: hal_init should succeed");
    if (ret != HFSSS_OK) { hal_nand_dev_cleanup(&nand); media_cleanup(&media); return; }

    memset(&fcfg, 0, sizeof(fcfg));
    fcfg.total_lbas         = (u64)(GFDT_CH * GFDT_CHIP * GFDT_DIE * GFDT_PLANE *
                                    GFDT_BLKS * GFDT_PGS);
    fcfg.page_size          = GFDT_PGSZ;
    fcfg.pages_per_block    = GFDT_PGS;
    fcfg.blocks_per_plane   = GFDT_BLKS;
    fcfg.planes_per_die     = GFDT_PLANE;
    fcfg.dies_per_chip      = GFDT_DIE;
    fcfg.chips_per_channel  = GFDT_CHIP;
    fcfg.channel_count      = GFDT_CH;
    fcfg.op_ratio           = 0;
    fcfg.gc_policy          = GC_POLICY_GREEDY;
    fcfg.gc_threshold       = 3;
    fcfg.gc_hiwater         = 6;
    fcfg.gc_lowater         = 1;

    ret = ftl_init(&ftl, &fcfg, &hal);
    TEST_ASSERT(ret == HFSSS_OK, "gc-flush-dst: ftl_init should succeed");
    if (ret != HFSSS_OK) {
        hal_cleanup(&hal);
        hal_nand_dev_cleanup(&nand);
        media_cleanup(&media);
        return;
    }

    /*
     * Flood the device with two passes of writes so GC is forced to run and
     * allocate a persistent destination block (ctx->gc.dst_block).
     */
    u64 total_lbas = fcfg.total_lbas;
    for (int pass = 0; pass < 2; pass++) {
        for (u64 lba = 0; lba < total_lbas; lba++) {
            memset(wbuf, (int)(lba + pass), sizeof(wbuf));
            ftl_write(&ftl, lba, GFDT_PGSZ, wbuf); /* ignore NOSPC */
        }
    }

    /*
     * After writing, check how many GC cycles occurred.  If GC ran, it
     * likely left dst_block open.  We verify by inspecting the gc_ctx
     * directly before and after ftl_flush.
     */
    struct ftl_stats stats_before;
    ftl_get_stats(&ftl, &stats_before);
    printf("  [INFO] gc_cycles before flush: %llu\n",
           (unsigned long long)stats_before.gc_count);

    bool dst_was_open_before_flush = (ftl.gc.dst_block != NULL);
    u64 closed_before = ftl.block_mgr.closed_blocks;

    /*
     * ftl_flush calls gc_flush_dst internally.  After the call:
     *   - ftl.gc.dst_block must be NULL.
     *   - If a destination block was open, closed_blocks must increase by 1.
     */
    ret = ftl_flush(&ftl);
    TEST_ASSERT(ret == HFSSS_OK, "gc-flush-dst: ftl_flush should succeed");

    TEST_ASSERT(ftl.gc.dst_block == NULL,
                "gc-flush-dst: gc_ctx.dst_block must be NULL after ftl_flush");

    if (dst_was_open_before_flush) {
        u64 closed_after = ftl.block_mgr.closed_blocks;
        printf("  [INFO] closed_blocks before=%llu after=%llu\n",
               (unsigned long long)closed_before,
               (unsigned long long)closed_after);
        TEST_ASSERT(closed_after == closed_before + 1,
                    "gc-flush-dst: closed_blocks must increase by 1 when dst_block is flushed");
    } else {
        printf("  [INFO] no open dst_block before flush "
               "(GC may not have run or already filled it); "
               "testing gc_flush_dst via gc_ctx directly\n");

        /*
         * If no destination block was left open after the write phase,
         * verify gc_flush_dst's contract directly on a fresh manager/gc_ctx
         * pair without going through the full FTL stack.
         */
        struct block_mgr mgr2;
        struct gc_ctx    gc2;

        ret = block_mgr_init(&mgr2, 1, 1, 1, 1, 8);
        TEST_ASSERT(ret == HFSSS_OK, "gc-flush-dst-direct: block_mgr_init should succeed");

        ret = gc_init(&gc2, GC_POLICY_GREEDY, 2, 6, 1);
        TEST_ASSERT(ret == HFSSS_OK, "gc-flush-dst-direct: gc_init should succeed");

        /* Simulate GC having allocated an open destination block. */
        struct block_desc *dst = block_alloc(&mgr2);
        TEST_ASSERT(dst != NULL, "gc-flush-dst-direct: block_alloc for simulated dst should succeed");

        if (dst) {
            gc2.dst_block = dst;
            gc2.dst_page  = 2;

            u64 closed_before2 = mgr2.closed_blocks;

            gc_flush_dst(&gc2, &mgr2);

            TEST_ASSERT(gc2.dst_block == NULL,
                        "gc-flush-dst-direct: dst_block must be NULL after gc_flush_dst");
            TEST_ASSERT(gc2.dst_page == 0,
                        "gc-flush-dst-direct: dst_page must be 0 after gc_flush_dst");
            TEST_ASSERT(dst->state == FTL_BLOCK_CLOSED,
                        "gc-flush-dst-direct: flushed dst block state must be FTL_BLOCK_CLOSED");
            TEST_ASSERT(mgr2.closed_blocks == closed_before2 + 1,
                        "gc-flush-dst-direct: closed_blocks must increase by 1 after flush");
        }

        /* Second call with no open dst must be a no-op. */
        u64 closed_noop = mgr2.closed_blocks;
        gc_flush_dst(&gc2, &mgr2);
        TEST_ASSERT(mgr2.closed_blocks == closed_noop,
                    "gc-flush-dst-direct: second gc_flush_dst call must be a no-op");

        gc_cleanup(&gc2);
        block_mgr_cleanup(&mgr2);
    }

    ftl_cleanup(&ftl);
    hal_cleanup(&hal);
    hal_nand_dev_cleanup(&nand);
    media_cleanup(&media);

#undef GFDT_CH
#undef GFDT_CHIP
#undef GFDT_DIE
#undef GFDT_PLANE
#undef GFDT_BLKS
#undef GFDT_PGS
#undef GFDT_PGSZ
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
    u64 gc_count, moved_pages, reclaimed_blocks, gc_write_pages;
    gc_get_stats(&ctx, &gc_count, &moved_pages, &reclaimed_blocks, &gc_write_pages);
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
    gc_get_stats(NULL, NULL, NULL, NULL, NULL);

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
    test_block_mark_bad_from_free();
    test_gc_flush_dst_closes_block();

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
