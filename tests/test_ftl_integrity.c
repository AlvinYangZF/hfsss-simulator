/*
 * test_ftl_integrity.c — targeted data-integrity test for the FTL write/GC/read path.
 *
 * Scenario:
 *   1. Init a tiny FTL (few blocks, small pages) so GC fires quickly.
 *   2. Write a known pattern to LBA 0 and immediately verify it — baseline.
 *   3. Fill the device to force at least one GC cycle.
 *   4. Read LBA 0 back and verify the data is still correct.
 *
 * Before the fix step 4 fails because GC recycles the block that holds LBA 0
 * without updating / invalidating the L2P mapping.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "media/media.h"
#include "hal/hal.h"
#include "ftl/ftl.h"

/* ------------------------------------------------------------------ */
/* Minimal test harness                                                 */
/* ------------------------------------------------------------------ */
static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg) do {                        \
    g_tests_run++;                                         \
    if (cond) {                                            \
        printf("  [PASS] %s\n", (msg));                   \
        g_tests_passed++;                                  \
    } else {                                               \
        printf("  [FAIL] %s\n", (msg));                   \
        g_tests_failed++;                                  \
    }                                                      \
} while (0)

/* ------------------------------------------------------------------ */
/* Device geometry — small enough to stress GC with few writes         */
/* 1 channel, 1 chip, 1 die, 1 plane                                  */
/* 8 blocks × 4 pages = 32 physical pages total                       */
/* gc_threshold=3 so GC fires when free_blocks <= 3                   */
/* ------------------------------------------------------------------ */
#define CH    1
#define CHIP  1
#define DIE   1
#define PLANE 1
#define BLKS  8
#define PGS   4
#define PGSZ  4096

static int setup_device(struct media_ctx *media, struct hal_nand_dev *nand,
                        struct hal_ctx *hal, struct ftl_ctx *ftl)
{
    struct media_config mcfg;
    struct ftl_config   fcfg;
    int ret;

    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.channel_count      = CH;
    mcfg.chips_per_channel  = CHIP;
    mcfg.dies_per_chip      = DIE;
    mcfg.planes_per_die     = PLANE;
    mcfg.blocks_per_plane   = BLKS;
    mcfg.pages_per_block    = PGS;
    mcfg.page_size          = PGSZ;
    mcfg.spare_size         = 64;
    mcfg.nand_type          = NAND_TYPE_TLC;

    ret = media_init(media, &mcfg);
    if (ret != HFSSS_OK) return ret;

    ret = hal_nand_dev_init(nand, CH, CHIP, DIE, PLANE, BLKS, PGS, PGSZ, 64, media);
    if (ret != HFSSS_OK) { media_cleanup(media); return ret; }

    ret = hal_init(hal, nand);
    if (ret != HFSSS_OK) { hal_nand_dev_cleanup(nand); media_cleanup(media); return ret; }

    /* FTL: total_lbas = BLKS*PGS (no OP for simplicity) */
    memset(&fcfg, 0, sizeof(fcfg));
    fcfg.total_lbas         = (u64)(CH * CHIP * DIE * PLANE * BLKS * PGS);
    fcfg.page_size          = PGSZ;
    fcfg.pages_per_block    = PGS;
    fcfg.blocks_per_plane   = BLKS;
    fcfg.planes_per_die     = PLANE;
    fcfg.dies_per_chip      = DIE;
    fcfg.chips_per_channel  = CHIP;
    fcfg.channel_count      = CH;
    fcfg.op_ratio           = 0;
    fcfg.gc_policy          = GC_POLICY_GREEDY;
    fcfg.gc_threshold       = 3;   /* fire GC eagerly */
    fcfg.gc_hiwater         = 5;
    fcfg.gc_lowater         = 1;

    ret = ftl_init(ftl, &fcfg, hal);
    if (ret != HFSSS_OK) {
        hal_cleanup(hal);
        hal_nand_dev_cleanup(nand);
        media_cleanup(media);
    }
    return ret;
}

/* Fill a 4 KiB buffer with a deterministic pattern keyed by lba. */
static void fill_pattern(u8 *buf, u64 lba)
{
    u32 *p = (u32 *)buf;
    u32  s = (u32)(lba ^ 0xDEADBEEFu);
    for (int i = 0; i < (int)(PGSZ / 4); i++) {
        s = s * 1664525u + 1013904223u;
        p[i] = s;
    }
}

static bool check_pattern(const u8 *buf, u64 lba)
{
    u32 *p = (u32 *)buf;
    u32  s = (u32)(lba ^ 0xDEADBEEFu);
    for (int i = 0; i < (int)(PGSZ / 4); i++) {
        s = s * 1664525u + 1013904223u;
        if (p[i] != s) return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Tests                                                                */
/* ------------------------------------------------------------------ */

/*
 * test_baseline — write LBA 0, read back immediately.
 * Must pass regardless of whether GC fires.
 */
static void test_baseline(void)
{
    printf("\n=== Baseline: immediate write-then-read ===\n");

    struct media_ctx    media;
    struct hal_nand_dev nand;
    struct hal_ctx      hal;
    struct ftl_ctx      ftl;
    u8 wbuf[PGSZ], rbuf[PGSZ];
    int ret;

    ret = setup_device(&media, &nand, &hal, &ftl);
    TEST_ASSERT(ret == HFSSS_OK, "device setup should succeed");
    if (ret != HFSSS_OK) return;

    fill_pattern(wbuf, 0);
    ret = ftl_write(&ftl, 0, PGSZ, wbuf);
    TEST_ASSERT(ret == HFSSS_OK, "write LBA 0 should succeed");

    memset(rbuf, 0, sizeof(rbuf));
    ret = ftl_read(&ftl, 0, PGSZ, rbuf);
    TEST_ASSERT(ret == HFSSS_OK, "read LBA 0 should succeed");
    TEST_ASSERT(check_pattern(rbuf, 0), "immediate read-back must match written pattern");

    ftl_cleanup(&ftl);
    hal_cleanup(&hal);
    hal_nand_dev_cleanup(&nand);
    media_cleanup(&media);
}

/*
 * test_post_gc_integrity — write LBA 0, then flood the device to trigger GC,
 * then verify LBA 0 still reads the same data.
 *
 * Before the fix this test FAILS.  After the fix it must PASS.
 */
static void test_post_gc_integrity(void)
{
    printf("\n=== Post-GC data integrity ===\n");

    struct media_ctx    media;
    struct hal_nand_dev nand;
    struct hal_ctx      hal;
    struct ftl_ctx      ftl;
    u8 wbuf[PGSZ], rbuf[PGSZ];
    int ret;

    ret = setup_device(&media, &nand, &hal, &ftl);
    TEST_ASSERT(ret == HFSSS_OK, "device setup should succeed");
    if (ret != HFSSS_OK) return;

    /* Step 1 – write a known pattern to LBA 0 */
    fill_pattern(wbuf, 0);
    ret = ftl_write(&ftl, 0, PGSZ, wbuf);
    TEST_ASSERT(ret == HFSSS_OK, "initial write to LBA 0 should succeed");

    /*
     * Step 2 – flood remaining LBAs with sequential writes.
     * total_lbas = BLKS*PGS = 32.  Writing all 32 LBAs fills every physical
     * page; subsequent re-writes produce invalids that make GC fire.
     * We write each LBA twice so that invalids accumulate and GC is
     * forced to reclaim at least one block.
     */
    u64 total_lbas = (u64)(CH * CHIP * DIE * PLANE * BLKS * PGS);
    for (int pass = 0; pass < 2; pass++) {
        for (u64 lba = 1; lba < total_lbas; lba++) {
            fill_pattern(wbuf, lba * 100 + pass); /* distinct per pass */
            /* Ignore NOSPC — GC may not keep up; we just want GC to fire. */
            ftl_write(&ftl, lba, PGSZ, wbuf);
        }
    }

    /* Step 3 – verify LBA 0 still holds the original pattern */
    fill_pattern(wbuf, 0); /* expected */
    memset(rbuf, 0, sizeof(rbuf));
    ret = ftl_read(&ftl, 0, PGSZ, rbuf);
    TEST_ASSERT(ret == HFSSS_OK, "read LBA 0 after GC should succeed");
    TEST_ASSERT(check_pattern(rbuf, 0),
                "LBA 0 data must survive GC (post-GC integrity check)");

    /* Extra: confirm GC actually ran */
    struct ftl_stats stats;
    ftl_get_stats(&ftl, &stats);
    printf("  [INFO] GC cycles observed: %llu\n", (unsigned long long)stats.gc_count);
    TEST_ASSERT(stats.gc_count > 0, "at least one GC cycle must have occurred");

    ftl_cleanup(&ftl);
    hal_cleanup(&hal);
    hal_nand_dev_cleanup(&nand);
    media_cleanup(&media);
}

/*
 * test_factory_bad_block_prescan — verify that blocks the BBT marks as
 * factory-bad are retired during ftl_init and never enter the allocatable
 * free pool.
 *
 * Geometry: 2ch × 1chip × 1die × 1plane × 128blk × 8pg.
 * The deterministic BBT formula (bbt.c) marks:
 *   ch=0, block=100  (idx=100,  idx%100==0, 100>10, 100<118)
 *   ch=1, block=97   (idx=1000100, idx%100==0, 97>10, 97<118)
 * as factory-bad.  After ftl_init these must be in FTL_BLOCK_BAD state.
 */
static void test_factory_bad_block_prescan(void)
{
    printf("\n=== Factory-bad-block pre-scan ===\n");

#define FBBT_CH    2
#define FBBT_CHIP  1
#define FBBT_DIE   1
#define FBBT_PLANE 1
#define FBBT_BLKS  128
#define FBBT_PGS   8
#define FBBT_PGSZ  4096

    struct media_ctx    media;
    struct hal_nand_dev nand;
    struct hal_ctx      hal;
    struct ftl_ctx      ftl;
    int ret;

    struct media_config mcfg;
    struct ftl_config   fcfg;

    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.channel_count      = FBBT_CH;
    mcfg.chips_per_channel  = FBBT_CHIP;
    mcfg.dies_per_chip      = FBBT_DIE;
    mcfg.planes_per_die     = FBBT_PLANE;
    mcfg.blocks_per_plane   = FBBT_BLKS;
    mcfg.pages_per_block    = FBBT_PGS;
    mcfg.page_size          = FBBT_PGSZ;
    mcfg.spare_size         = 64;
    mcfg.nand_type          = NAND_TYPE_TLC;

    ret = media_init(&media, &mcfg);
    TEST_ASSERT(ret == HFSSS_OK, "factory-bad: media_init should succeed");
    if (ret != HFSSS_OK) return;

    ret = hal_nand_dev_init(&nand, FBBT_CH, FBBT_CHIP, FBBT_DIE, FBBT_PLANE,
                             FBBT_BLKS, FBBT_PGS, FBBT_PGSZ, 64, &media);
    TEST_ASSERT(ret == HFSSS_OK, "factory-bad: hal_nand_dev_init should succeed");
    if (ret != HFSSS_OK) { media_cleanup(&media); return; }

    ret = hal_init(&hal, &nand);
    TEST_ASSERT(ret == HFSSS_OK, "factory-bad: hal_init should succeed");
    if (ret != HFSSS_OK) { hal_nand_dev_cleanup(&nand); media_cleanup(&media); return; }

    /* Use total_lbas well below raw capacity to leave headroom for GC. */
    u64 raw_pages = (u64)FBBT_CH * FBBT_CHIP * FBBT_DIE * FBBT_PLANE *
                    FBBT_BLKS * FBBT_PGS;

    memset(&fcfg, 0, sizeof(fcfg));
    fcfg.total_lbas         = raw_pages * 80 / 100; /* 20% OP */
    fcfg.page_size          = FBBT_PGSZ;
    fcfg.pages_per_block    = FBBT_PGS;
    fcfg.blocks_per_plane   = FBBT_BLKS;
    fcfg.planes_per_die     = FBBT_PLANE;
    fcfg.dies_per_chip      = FBBT_DIE;
    fcfg.chips_per_channel  = FBBT_CHIP;
    fcfg.channel_count      = FBBT_CH;
    fcfg.op_ratio           = 20;
    fcfg.gc_policy          = GC_POLICY_GREEDY;
    fcfg.gc_threshold       = 10;
    fcfg.gc_hiwater         = 20;
    fcfg.gc_lowater         = 5;

    ret = ftl_init(&ftl, &fcfg, &hal);
    TEST_ASSERT(ret == HFSSS_OK, "factory-bad: ftl_init should succeed");
    if (ret != HFSSS_OK) {
        hal_cleanup(&hal);
        hal_nand_dev_cleanup(&nand);
        media_cleanup(&media);
        return;
    }

    /*
     * Confirm that the BBT records the expected factory-bad blocks.
     * hal_ctx_nand_is_bad_block queries the underlying media BBT.
     */
    int bbt_bad_ch0 = hal_ctx_nand_is_bad_block(&hal, 0, 0, 0, 0, 100);
    int bbt_bad_ch1 = hal_ctx_nand_is_bad_block(&hal, 1, 0, 0, 0, 97);
    TEST_ASSERT(bbt_bad_ch0 == 1,
                "factory-bad: BBT must mark ch=0 block=100 as bad");
    TEST_ASSERT(bbt_bad_ch1 == 1,
                "factory-bad: BBT must mark ch=1 block=97 as bad");

    /*
     * After ftl_init pre-scan, the corresponding block descriptors must be
     * in FTL_BLOCK_BAD state — they were removed from the free list.
     */
    struct block_desc *bd_ch0 = block_find_by_coords(&ftl.block_mgr,
                                                      0, 0, 0, 0, 100);
    struct block_desc *bd_ch1 = block_find_by_coords(&ftl.block_mgr,
                                                      1, 0, 0, 0, 97);

    TEST_ASSERT(bd_ch0 != NULL,
                "factory-bad: block descriptor for ch=0 blk=100 must exist");
    TEST_ASSERT(bd_ch1 != NULL,
                "factory-bad: block descriptor for ch=1 blk=97 must exist");

    if (bd_ch0) {
        TEST_ASSERT(bd_ch0->state == FTL_BLOCK_BAD,
                    "factory-bad: ch=0 blk=100 descriptor must be FTL_BLOCK_BAD");
    }
    if (bd_ch1) {
        TEST_ASSERT(bd_ch1->state == FTL_BLOCK_BAD,
                    "factory-bad: ch=1 blk=97 descriptor must be FTL_BLOCK_BAD");
    }

    /*
     * The factory-bad blocks must not appear in the free pool — free_blocks
     * should be (total_blocks - bad_count), not total_blocks.
     */
    u64 total_blocks = (u64)FBBT_CH * FBBT_CHIP * FBBT_DIE * FBBT_PLANE * FBBT_BLKS;
    u64 free_after_prescan = block_get_free_count(&ftl.block_mgr);
    printf("  [INFO] total_blocks=%llu free_after_prescan=%llu\n",
           (unsigned long long)total_blocks,
           (unsigned long long)free_after_prescan);
    TEST_ASSERT(free_after_prescan < total_blocks,
                "factory-bad: free pool must be smaller than total after pre-scan");
    /* 2 bad blocks + FBBT_CH reserved superblocks (1 per channel) */
    TEST_ASSERT(free_after_prescan == total_blocks - 2 - FBBT_CH,
                "factory-bad: exactly 2 blocks must be retired by pre-scan");

    ftl_cleanup(&ftl);
    hal_cleanup(&hal);
    hal_nand_dev_cleanup(&nand);
    media_cleanup(&media);

#undef FBBT_CH
#undef FBBT_CHIP
#undef FBBT_DIE
#undef FBBT_PLANE
#undef FBBT_BLKS
#undef FBBT_PGS
#undef FBBT_PGSZ
}

/*
 * test_cwb_io_error_retirement — verify that when a program to the Current
 * Write Block fails with HFSSS_ERR_IO the FTL retires that block via
 * block_mark_bad and successfully places a subsequent write on a new block.
 *
 * Method:
 *   1. Init a small device with no factory-bad blocks (1ch×1chip×1die×1plane
 *      × 20blk × 4pg).  With this geometry no block satisfies the bad-block
 *      formula (blocks_per_plane-10 == 10, so the range 11..9 is empty).
 *   2. block_alloc from the free list returns the head block, which is blk=19
 *      (last block added in block_mgr_init via block_list_add_head).
 *   3. Mark blk=19 bad in the BBT *after* ftl_init so the pre-scan does not
 *      catch it.  Now hal_nand_program_sync will return HFSSS_ERR_IO when the
 *      FTL tries to write to this block.
 *   4. Call ftl_write(lba=0): all three program retries fail with IO error;
 *      the FTL must call block_mark_bad on blk=19.
 *   5. Assert the descriptor for blk=19 is now FTL_BLOCK_BAD.
 *   6. Call ftl_write(lba=0) again: the FTL allocates a different block and
 *      the write succeeds.
 */
static void test_cwb_io_error_retirement(void)
{
    printf("\n=== CWB IO-error block retirement ===\n");

#define CWB_CH    1
#define CWB_CHIP  1
#define CWB_DIE   1
#define CWB_PLANE 1
#define CWB_BLKS  20
#define CWB_PGS   4
#define CWB_PGSZ  4096

    struct media_ctx    media;
    struct hal_nand_dev nand;
    struct hal_ctx      hal;
    struct ftl_ctx      ftl;
    u8 wbuf[CWB_PGSZ];
    int ret;

    struct media_config mcfg;
    struct ftl_config   fcfg;

    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.channel_count      = CWB_CH;
    mcfg.chips_per_channel  = CWB_CHIP;
    mcfg.dies_per_chip      = CWB_DIE;
    mcfg.planes_per_die     = CWB_PLANE;
    mcfg.blocks_per_plane   = CWB_BLKS;
    mcfg.pages_per_block    = CWB_PGS;
    mcfg.page_size          = CWB_PGSZ;
    mcfg.spare_size         = 64;
    mcfg.nand_type          = NAND_TYPE_TLC;

    ret = media_init(&media, &mcfg);
    TEST_ASSERT(ret == HFSSS_OK, "cwb-io-error: media_init should succeed");
    if (ret != HFSSS_OK) return;

    ret = hal_nand_dev_init(&nand, CWB_CH, CWB_CHIP, CWB_DIE, CWB_PLANE,
                             CWB_BLKS, CWB_PGS, CWB_PGSZ, 64, &media);
    TEST_ASSERT(ret == HFSSS_OK, "cwb-io-error: hal_nand_dev_init should succeed");
    if (ret != HFSSS_OK) { media_cleanup(&media); return; }

    ret = hal_init(&hal, &nand);
    TEST_ASSERT(ret == HFSSS_OK, "cwb-io-error: hal_init should succeed");
    if (ret != HFSSS_OK) { hal_nand_dev_cleanup(&nand); media_cleanup(&media); return; }

    memset(&fcfg, 0, sizeof(fcfg));
    fcfg.total_lbas         = (u64)(CWB_CH * CWB_CHIP * CWB_DIE * CWB_PLANE *
                                    CWB_BLKS * CWB_PGS);
    fcfg.page_size          = CWB_PGSZ;
    fcfg.pages_per_block    = CWB_PGS;
    fcfg.blocks_per_plane   = CWB_BLKS;
    fcfg.planes_per_die     = CWB_PLANE;
    fcfg.dies_per_chip      = CWB_DIE;
    fcfg.chips_per_channel  = CWB_CHIP;
    fcfg.channel_count      = CWB_CH;
    fcfg.op_ratio           = 0;
    fcfg.gc_policy          = GC_POLICY_GREEDY;
    fcfg.gc_threshold       = 3;
    fcfg.gc_hiwater         = 8;
    fcfg.gc_lowater         = 1;

    ret = ftl_init(&ftl, &fcfg, &hal);
    TEST_ASSERT(ret == HFSSS_OK, "cwb-io-error: ftl_init should succeed");
    if (ret != HFSSS_OK) {
        hal_cleanup(&hal);
        hal_nand_dev_cleanup(&nand);
        media_cleanup(&media);
        return;
    }

    /*
     * With 20 blocks, 1 is reserved as a metadata superblock (last block
     * in each channel), leaving 19 free.  block_list_add_head is called in
     * order so blk=18 ends up at the head of the free list — the first
     * ftl_write will allocate blk=18.
     *
     * Mark blk=18 bad in the BBT now, after pre-scan, so the FTL's write
     * attempt to it returns HFSSS_ERR_IO.
     */
    u64 free_before = block_get_free_count(&ftl.block_mgr);
    TEST_ASSERT(free_before == CWB_BLKS - 1,
                "cwb-io-error: all blocks should be free before first write");

    ret = hal_ctx_nand_mark_bad_block(&hal, 0, 0, 0, 0, CWB_BLKS - 2);
    TEST_ASSERT(ret == HFSSS_OK,
                "cwb-io-error: marking blk=18 bad in BBT should succeed");

    /* Confirm BBT registers it as bad before the write attempt. */
    TEST_ASSERT(hal_ctx_nand_is_bad_block(&hal, 0, 0, 0, 0, CWB_BLKS - 2) == 1,
                "cwb-io-error: BBT should report blk=18 as bad");

    /* First write attempt — must fail because blk=18 is now IO-bad. */
    fill_pattern(wbuf, 0);
    ret = ftl_write(&ftl, 0, CWB_PGSZ, wbuf);
    TEST_ASSERT(ret == HFSSS_ERR_IO,
                "cwb-io-error: first write must fail with HFSSS_ERR_IO");

    /* blk=18 descriptor must now be FTL_BLOCK_BAD. */
    struct block_desc *bd = block_find_by_coords(&ftl.block_mgr,
                                                  0, 0, 0, 0, CWB_BLKS - 2);
    TEST_ASSERT(bd != NULL, "cwb-io-error: block descriptor for blk=18 must exist");
    if (bd) {
        TEST_ASSERT(bd->state == FTL_BLOCK_BAD,
                    "cwb-io-error: blk=18 must be FTL_BLOCK_BAD after IO error");
    }

    /* Free count must have dropped by 1 (blk=19 retired, never returned). */
    u64 free_after_err = block_get_free_count(&ftl.block_mgr);
    printf("  [INFO] free_before=%llu free_after_err=%llu\n",
           (unsigned long long)free_before,
           (unsigned long long)free_after_err);
    TEST_ASSERT(free_after_err == free_before - 1,
                "cwb-io-error: free count must decrease by 1 after block retirement");

    /*
     * A subsequent write to the same LBA must succeed because the FTL
     * allocates a different (good) block.
     */
    ret = ftl_write(&ftl, 0, CWB_PGSZ, wbuf);
    TEST_ASSERT(ret == HFSSS_OK,
                "cwb-io-error: retry write to same LBA must succeed on new block");

    /* Confirm the data can be read back correctly. */
    u8 rbuf[CWB_PGSZ];
    memset(rbuf, 0, sizeof(rbuf));
    ret = ftl_read(&ftl, 0, CWB_PGSZ, rbuf);
    TEST_ASSERT(ret == HFSSS_OK,
                "cwb-io-error: read after successful retry write must succeed");
    TEST_ASSERT(memcmp(wbuf, rbuf, CWB_PGSZ) == 0,
                "cwb-io-error: data read after retry must match what was written");

    ftl_cleanup(&ftl);
    hal_cleanup(&hal);
    hal_nand_dev_cleanup(&nand);
    media_cleanup(&media);

#undef CWB_CH
#undef CWB_CHIP
#undef CWB_DIE
#undef CWB_PLANE
#undef CWB_BLKS
#undef CWB_PGS
#undef CWB_PGSZ
}

/*
 * test_nospc_gc_trigger — verify that the NOSPC deadlock fix keeps writes
 * succeeding when the device is nearly full by triggering GC on NOSPC.
 *
 * Fill ~75% of the user capacity, then write another 50% worth of overwrites
 * to generate invalid pages and let GC run.  Every write after the initial
 * fill should either succeed (GC freed space) or return at most one NOSPC
 * before recovering.  The test counts hard, persistent NOSPC failures (two
 * NOSPC in a row to the same LBA) — that must be zero.
 *
 * Geometry: 2ch × 1chip × 1die × 1plane × 64blk × 8pg (1024 physical pages).
 * Factory-bad blocks for this geometry:
 *   ch=0, blk=0: idx=0, 0%100==0 but block <= 10 — safe (excluded).
 *   No other factory-bad in range 11..(64-10-1)=53:
 *     ch=0: idx=blk; blk%100==0 → blk=0 (excluded, <=10); next=100>53 — none.
 *     ch=1: idx=1000003+blk; (1000003+blk)%100==0 → blk=97>53 — none.
 *   Therefore 0 factory-bad blocks, all 128 blocks start free.
 */
static void test_nospc_gc_trigger(void)
{
    printf("\n=== NOSPC GC trigger (no deadlock) ===\n");

#define NOSPC_CH    2
#define NOSPC_CHIP  1
#define NOSPC_DIE   1
#define NOSPC_PLANE 1
#define NOSPC_BLKS  64
#define NOSPC_PGS   8
#define NOSPC_PGSZ  4096

    struct media_ctx    media;
    struct hal_nand_dev nand;
    struct hal_ctx      hal;
    struct ftl_ctx      ftl;
    u8 wbuf[NOSPC_PGSZ];
    int ret;

    struct media_config mcfg;
    struct ftl_config   fcfg;

    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.channel_count      = NOSPC_CH;
    mcfg.chips_per_channel  = NOSPC_CHIP;
    mcfg.dies_per_chip      = NOSPC_DIE;
    mcfg.planes_per_die     = NOSPC_PLANE;
    mcfg.blocks_per_plane   = NOSPC_BLKS;
    mcfg.pages_per_block    = NOSPC_PGS;
    mcfg.page_size          = NOSPC_PGSZ;
    mcfg.spare_size         = 64;
    mcfg.nand_type          = NAND_TYPE_TLC;

    ret = media_init(&media, &mcfg);
    TEST_ASSERT(ret == HFSSS_OK, "nospc-gc: media_init should succeed");
    if (ret != HFSSS_OK) return;

    ret = hal_nand_dev_init(&nand, NOSPC_CH, NOSPC_CHIP, NOSPC_DIE, NOSPC_PLANE,
                             NOSPC_BLKS, NOSPC_PGS, NOSPC_PGSZ, 64, &media);
    TEST_ASSERT(ret == HFSSS_OK, "nospc-gc: hal_nand_dev_init should succeed");
    if (ret != HFSSS_OK) { media_cleanup(&media); return; }

    ret = hal_init(&hal, &nand);
    TEST_ASSERT(ret == HFSSS_OK, "nospc-gc: hal_init should succeed");
    if (ret != HFSSS_OK) { hal_nand_dev_cleanup(&nand); media_cleanup(&media); return; }

    u64 raw_pages  = (u64)NOSPC_CH * NOSPC_CHIP * NOSPC_DIE * NOSPC_PLANE *
                     NOSPC_BLKS * NOSPC_PGS;
    u64 total_lbas = raw_pages * 75 / 100;  /* 25% OP */

    memset(&fcfg, 0, sizeof(fcfg));
    fcfg.total_lbas         = total_lbas;
    fcfg.page_size          = NOSPC_PGSZ;
    fcfg.pages_per_block    = NOSPC_PGS;
    fcfg.blocks_per_plane   = NOSPC_BLKS;
    fcfg.planes_per_die     = NOSPC_PLANE;
    fcfg.dies_per_chip      = NOSPC_DIE;
    fcfg.chips_per_channel  = NOSPC_CHIP;
    fcfg.channel_count      = NOSPC_CH;
    fcfg.op_ratio           = 25;
    fcfg.gc_policy          = GC_POLICY_GREEDY;
    fcfg.gc_threshold       = 8;   /* trigger GC eagerly to keep space available */
    fcfg.gc_hiwater         = 16;
    fcfg.gc_lowater         = 4;

    ret = ftl_init(&ftl, &fcfg, &hal);
    TEST_ASSERT(ret == HFSSS_OK, "nospc-gc: ftl_init should succeed");
    if (ret != HFSSS_OK) {
        hal_cleanup(&hal);
        hal_nand_dev_cleanup(&nand);
        media_cleanup(&media);
        return;
    }

    /* Phase 1: fill ~75% of user LBAs with unique data. */
    u64 fill_target = total_lbas * 75 / 100;
    u64 write_ok = 0;
    for (u64 lba = 0; lba < fill_target; lba++) {
        fill_pattern(wbuf, lba);
        ret = ftl_write(&ftl, lba, NOSPC_PGSZ, wbuf);
        if (ret == HFSSS_OK) {
            write_ok++;
        }
    }
    TEST_ASSERT(write_ok == fill_target,
                "nospc-gc: all writes in fill phase must succeed");

    /*
     * Phase 2: re-write the same LBAs twice (overwrite pass).
     * Each overwrite creates one invalid page, pushing free_blocks down and
     * forcing GC to run.  Track consecutive NOSPC (persistent failure).
     */
    u64 nospc_count  = 0;
    u64 hard_failure = 0; /* NOSPC on two consecutive writes to the same LBA */

    for (int pass = 0; pass < 2; pass++) {
        for (u64 lba = 0; lba < fill_target; lba++) {
            fill_pattern(wbuf, lba + (u64)(pass + 1) * 1000000ULL);
            ret = ftl_write(&ftl, lba, NOSPC_PGSZ, wbuf);
            if (ret == HFSSS_ERR_NOSPC) {
                nospc_count++;
                /* Retry once — GC should have freed space. */
                ret = ftl_write(&ftl, lba, NOSPC_PGSZ, wbuf);
                if (ret == HFSSS_ERR_NOSPC) {
                    hard_failure++;
                }
            }
        }
    }

    printf("  [INFO] nospc_count=%llu hard_failures=%llu\n",
           (unsigned long long)nospc_count,
           (unsigned long long)hard_failure);

    /* Any NOSPC that persists after one GC-triggered retry is a deadlock. */
    TEST_ASSERT(hard_failure == 0,
                "nospc-gc: no persistent NOSPC after GC-triggered retry");

    /* GC must have run at least once during the overwrite phase. */
    struct ftl_stats stats;
    ftl_get_stats(&ftl, &stats);
    printf("  [INFO] gc_cycles=%llu\n", (unsigned long long)stats.gc_count);
    TEST_ASSERT(stats.gc_count > 0,
                "nospc-gc: at least one GC cycle must have occurred");

    ftl_cleanup(&ftl);
    hal_cleanup(&hal);
    hal_nand_dev_cleanup(&nand);
    media_cleanup(&media);

#undef NOSPC_CH
#undef NOSPC_CHIP
#undef NOSPC_DIE
#undef NOSPC_PLANE
#undef NOSPC_BLKS
#undef NOSPC_PGS
#undef NOSPC_PGSZ
}

/* ------------------------------------------------------------------ */
int main(void)
{
    printf("========================================\n");
    printf("HFSSS FTL Integrity Tests\n");
    printf("========================================\n");

    test_baseline();
    test_post_gc_integrity();
    test_factory_bad_block_prescan();
    test_cwb_io_error_retirement();
    test_nospc_gc_trigger();

    printf("\n========================================\n");
    printf("Results: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0) {
        printf(", %d FAILED\n", g_tests_failed);
    } else {
        printf(" — all PASS\n");
    }
    printf("========================================\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
