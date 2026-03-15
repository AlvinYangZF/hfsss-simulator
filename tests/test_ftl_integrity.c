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

/* ------------------------------------------------------------------ */
int main(void)
{
    printf("========================================\n");
    printf("HFSSS FTL Integrity Tests\n");
    printf("========================================\n");

    test_baseline();
    test_post_gc_integrity();

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
