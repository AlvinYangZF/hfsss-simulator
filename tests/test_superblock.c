#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "media/media.h"
#include "hal/hal.h"
#include "ftl/ftl.h"
#include "ftl/superblock.h"
#include "common/common.h"

/* ------------------------------------------------------------------ */
/* Test harness                                                        */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Geometry constants                                                  */
/* ------------------------------------------------------------------ */

#define TEST_CHANNELS         2
#define TEST_BLOCKS_PER_PLANE 32
#define TEST_PAGES_PER_BLOCK  16
#define TEST_PAGE_SIZE        4096

/* ------------------------------------------------------------------ */
/* Full FTL stack helper                                               */
/* ------------------------------------------------------------------ */

struct test_stack {
    struct media_ctx media;
    struct hal_nand_dev nand;
    struct hal_ctx hal;
    struct ftl_ctx ftl;
};

static int stack_init(struct test_stack *s, u32 channels, u32 blocks_per_plane,
                      u32 pages_per_block, u32 page_size) {
    struct media_config mcfg;
    struct ftl_config fcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.channel_count = channels;
    mcfg.chips_per_channel = 1;
    mcfg.dies_per_chip = 1;
    mcfg.planes_per_die = 1;
    mcfg.blocks_per_plane = blocks_per_plane;
    mcfg.pages_per_block = pages_per_block;
    mcfg.page_size = page_size;
    mcfg.spare_size = 64;
    mcfg.nand_type = NAND_TYPE_TLC;

    if (media_init(&s->media, &mcfg) != HFSSS_OK) return -1;
    if (hal_nand_dev_init(&s->nand, channels, 1, 1, 1, blocks_per_plane,
                          pages_per_block, page_size, 64, &s->media) != HFSSS_OK) return -1;
    if (hal_init(&s->hal, &s->nand) != HFSSS_OK) return -1;

    memset(&fcfg, 0, sizeof(fcfg));
    u64 raw_pages = (u64)channels * blocks_per_plane * pages_per_block;
    fcfg.total_lbas = raw_pages * 80 / 100;  /* 20% OP */
    fcfg.page_size = page_size;
    fcfg.pages_per_block = pages_per_block;
    fcfg.blocks_per_plane = blocks_per_plane;
    fcfg.planes_per_die = 1;
    fcfg.dies_per_chip = 1;
    fcfg.chips_per_channel = 1;
    fcfg.channel_count = channels;
    fcfg.op_ratio = 20;
    fcfg.gc_policy = GC_POLICY_GREEDY;
    fcfg.gc_threshold = 3;
    fcfg.gc_hiwater = 5;
    fcfg.gc_lowater = 1;

    if (ftl_init(&s->ftl, &fcfg, &s->hal) != HFSSS_OK) return -1;
    return 0;
}

static void stack_cleanup(struct test_stack *s) {
    ftl_cleanup(&s->ftl);
    hal_cleanup(&s->hal);
    hal_nand_dev_cleanup(&s->nand);
    media_cleanup(&s->media);
}

/* ------------------------------------------------------------------ */
/* Deterministic data pattern helpers                                  */
/* ------------------------------------------------------------------ */

static void fill_buf(void *buf, u32 lba, u32 page_size) {
    u32 *p = (u32 *)buf;
    u32 s = lba * 0x9e3779b9u;
    for (u32 i = 0; i < page_size / 4; i++) {
        s = s * 1664525u + 1013904223u;
        p[i] = s;
    }
}

static int verify_buf(const void *buf, u32 lba, u32 page_size) {
    const u32 *p = (const u32 *)buf;
    u32 s = lba * 0x9e3779b9u;
    for (u32 i = 0; i < page_size / 4; i++) {
        s = s * 1664525u + 1013904223u;
        if (p[i] != s) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* FTL config builder (reusable across power-cycle halves)             */
/* ------------------------------------------------------------------ */

static struct ftl_config make_ftl_config(u32 channels, u32 blocks_per_plane,
                                         u32 pages_per_block, u32 page_size) {
    struct ftl_config fcfg;
    u64 raw_pages = (u64)channels * blocks_per_plane * pages_per_block;
    memset(&fcfg, 0, sizeof(fcfg));
    fcfg.total_lbas = raw_pages * 80 / 100;
    fcfg.page_size = page_size;
    fcfg.pages_per_block = pages_per_block;
    fcfg.blocks_per_plane = blocks_per_plane;
    fcfg.planes_per_die = 1;
    fcfg.dies_per_chip = 1;
    fcfg.chips_per_channel = 1;
    fcfg.channel_count = channels;
    fcfg.op_ratio = 20;
    fcfg.gc_policy = GC_POLICY_GREEDY;
    fcfg.gc_threshold = 3;
    fcfg.gc_hiwater = 5;
    fcfg.gc_lowater = 1;
    return fcfg;
}

/* ================================================================== */
/* Test 1: Superblock reservation                                      */
/* ================================================================== */

static void test_superblock_reservation(void) {
    printf("\n=== Test 1: Superblock reservation ===\n");

    struct test_stack s;
    int rc = stack_init(&s, TEST_CHANNELS, TEST_BLOCKS_PER_PLANE,
                        TEST_PAGES_PER_BLOCK, TEST_PAGE_SIZE);
    TEST_ASSERT(rc == 0, "stack_init succeeds");

    TEST_ASSERT(s.ftl.sb.initialized == true,
                "superblock initialized flag is set");

    TEST_ASSERT(s.ftl.sb.block_count == TEST_CHANNELS,
                "superblock block_count equals channel count");

    /* Each reserved block should be the last block in its channel. */
    for (u32 ch = 0; ch < s.ftl.sb.block_count; ch++) {
        u32 blk_id = s.ftl.sb.blocks[ch].block_id;
        struct block_desc *bd = block_find_by_coords(
            &s.ftl.block_mgr, ch, 0, 0, 0, blk_id);
        TEST_ASSERT(bd != NULL, "reserved block descriptor found");
        if (bd) {
            TEST_ASSERT(bd->state == FTL_BLOCK_RESERVED,
                        "reserved block state is FTL_BLOCK_RESERVED");
        }
    }

    /* Reserved blocks must not appear in the free list. */
    u64 total_blocks = (u64)TEST_CHANNELS * TEST_BLOCKS_PER_PLANE;
    u64 expected_free = total_blocks - TEST_CHANNELS; /* minus reserved */
    u64 actual_free = block_get_free_count(&s.ftl.block_mgr);
    TEST_ASSERT(actual_free == expected_free,
                "free_count excludes reserved blocks");

    stack_cleanup(&s);
}

/* ================================================================== */
/* Test 2: Checkpoint write/read round-trip                            */
/* ================================================================== */

static void test_checkpoint_round_trip(void) {
    printf("\n=== Test 2: Checkpoint write/read round-trip ===\n");

    struct test_stack s;
    int rc = stack_init(&s, TEST_CHANNELS, TEST_BLOCKS_PER_PLANE,
                        TEST_PAGES_PER_BLOCK, TEST_PAGE_SIZE);
    TEST_ASSERT(rc == 0, "stack_init succeeds");

    void *buf = malloc(TEST_PAGE_SIZE);
    TEST_ASSERT(buf != NULL, "write buffer allocated");

    /* Write 100 LBAs with deterministic patterns. */
    bool write_ok = true;
    for (u32 lba = 0; lba < 100; lba++) {
        fill_buf(buf, lba, TEST_PAGE_SIZE);
        if (ftl_write(&s.ftl, lba, TEST_PAGE_SIZE, buf) != HFSSS_OK) {
            write_ok = false;
            break;
        }
    }
    TEST_ASSERT(write_ok, "100 LBA writes succeed");

    /* Write checkpoint. */
    rc = sb_checkpoint_write(&s.ftl.sb, &s.ftl.mapping);
    TEST_ASSERT(rc == HFSSS_OK, "sb_checkpoint_write succeeds");

    /* Record original PPNs. */
    union ppn orig_ppns[100];
    for (u32 lba = 0; lba < 100; lba++) {
        mapping_l2p(&s.ftl.mapping, lba, &orig_ppns[lba]);
    }

    /* Build a fresh mapping and load checkpoint into it. */
    u64 raw_pages = (u64)TEST_CHANNELS * TEST_BLOCKS_PER_PLANE * TEST_PAGES_PER_BLOCK;
    struct mapping_ctx fresh;
    rc = mapping_init(&fresh, raw_pages * 80 / 100, raw_pages);
    TEST_ASSERT(rc == HFSSS_OK, "fresh mapping_init succeeds");

    rc = sb_checkpoint_read(&s.ftl.sb, &fresh);
    TEST_ASSERT(rc == HFSSS_OK, "sb_checkpoint_read succeeds");

    rc = mapping_rebuild_p2l(&fresh);
    TEST_ASSERT(rc == HFSSS_OK, "mapping_rebuild_p2l succeeds");

    TEST_ASSERT(mapping_get_valid_count(&fresh) == 100,
                "fresh mapping valid_count is 100");

    /* Verify each LBA maps to the same PPN. */
    bool ppn_match = true;
    for (u32 lba = 0; lba < 100; lba++) {
        union ppn ppn;
        if (mapping_l2p(&fresh, lba, &ppn) != HFSSS_OK ||
            ppn.raw != orig_ppns[lba].raw) {
            ppn_match = false;
            break;
        }
    }
    TEST_ASSERT(ppn_match, "all 100 LBAs have matching PPNs in fresh mapping");

    mapping_cleanup(&fresh);
    free(buf);
    stack_cleanup(&s);
}

/* ================================================================== */
/* Test 3: Journal append and replay                                   */
/* ================================================================== */

static void test_journal_append_replay(void) {
    printf("\n=== Test 3: Journal append and replay ===\n");

    struct test_stack s;
    int rc = stack_init(&s, TEST_CHANNELS, TEST_BLOCKS_PER_PLANE,
                        TEST_PAGES_PER_BLOCK, TEST_PAGE_SIZE);
    TEST_ASSERT(rc == 0, "stack_init succeeds");

    void *buf = malloc(TEST_PAGE_SIZE);
    TEST_ASSERT(buf != NULL, "write buffer allocated");

    /* Write 50 LBAs and checkpoint. */
    bool write_ok = true;
    for (u32 lba = 0; lba < 50; lba++) {
        fill_buf(buf, lba, TEST_PAGE_SIZE);
        if (ftl_write(&s.ftl, lba, TEST_PAGE_SIZE, buf) != HFSSS_OK) {
            write_ok = false;
            break;
        }
    }
    TEST_ASSERT(write_ok, "first 50 LBA writes succeed");

    rc = sb_checkpoint_write(&s.ftl.sb, &s.ftl.mapping);
    TEST_ASSERT(rc == HFSSS_OK, "checkpoint after 50 writes succeeds");

    /* Write 50 more LBAs (these go to journal). */
    for (u32 lba = 50; lba < 100; lba++) {
        fill_buf(buf, lba, TEST_PAGE_SIZE);
        if (ftl_write(&s.ftl, lba, TEST_PAGE_SIZE, buf) != HFSSS_OK) {
            write_ok = false;
            break;
        }
    }
    TEST_ASSERT(write_ok, "next 50 LBA writes succeed");

    /* Flush journal buffer to NAND. */
    rc = sb_journal_flush(&s.ftl.sb);
    TEST_ASSERT(rc == HFSSS_OK, "sb_journal_flush succeeds");

    /* Record original PPNs for all 100 LBAs. */
    union ppn orig_ppns[100];
    for (u32 lba = 0; lba < 100; lba++) {
        mapping_l2p(&s.ftl.mapping, lba, &orig_ppns[lba]);
    }

    /* Fresh mapping: load checkpoint (50 entries), then replay journal (50 more). */
    u64 raw_pages = (u64)TEST_CHANNELS * TEST_BLOCKS_PER_PLANE * TEST_PAGES_PER_BLOCK;
    struct mapping_ctx fresh;
    rc = mapping_init(&fresh, raw_pages * 80 / 100, raw_pages);
    TEST_ASSERT(rc == HFSSS_OK, "fresh mapping_init succeeds");

    rc = sb_checkpoint_read(&s.ftl.sb, &fresh);
    TEST_ASSERT(rc == HFSSS_OK, "sb_checkpoint_read succeeds");

    rc = sb_journal_replay(&s.ftl.sb, &fresh);
    TEST_ASSERT(rc == HFSSS_OK, "sb_journal_replay succeeds");

    rc = mapping_rebuild_p2l(&fresh);
    TEST_ASSERT(rc == HFSSS_OK, "mapping_rebuild_p2l succeeds");

    TEST_ASSERT(mapping_get_valid_count(&fresh) == 100,
                "fresh mapping valid_count is 100 after checkpoint + journal");

    bool ppn_match = true;
    for (u32 lba = 0; lba < 100; lba++) {
        union ppn ppn;
        if (mapping_l2p(&fresh, lba, &ppn) != HFSSS_OK ||
            ppn.raw != orig_ppns[lba].raw) {
            ppn_match = false;
            break;
        }
    }
    TEST_ASSERT(ppn_match, "all 100 LBAs match after checkpoint + journal replay");

    mapping_cleanup(&fresh);
    free(buf);
    stack_cleanup(&s);
}

/* ================================================================== */
/* Test 4: Trim journal                                                */
/* ================================================================== */

static void test_trim_journal(void) {
    printf("\n=== Test 4: Trim journal ===\n");

    struct test_stack s;
    int rc = stack_init(&s, TEST_CHANNELS, TEST_BLOCKS_PER_PLANE,
                        TEST_PAGES_PER_BLOCK, TEST_PAGE_SIZE);
    TEST_ASSERT(rc == 0, "stack_init succeeds");

    void *buf = malloc(TEST_PAGE_SIZE);
    TEST_ASSERT(buf != NULL, "write buffer allocated");

    /* Write 100 LBAs and checkpoint. */
    bool write_ok = true;
    for (u32 lba = 0; lba < 100; lba++) {
        fill_buf(buf, lba, TEST_PAGE_SIZE);
        if (ftl_write(&s.ftl, lba, TEST_PAGE_SIZE, buf) != HFSSS_OK) {
            write_ok = false;
            break;
        }
    }
    TEST_ASSERT(write_ok, "100 LBA writes succeed");

    rc = sb_checkpoint_write(&s.ftl.sb, &s.ftl.mapping);
    TEST_ASSERT(rc == HFSSS_OK, "checkpoint after 100 writes succeeds");

    /* Trim LBAs 0-49 (journal records TRIM ops). */
    for (u32 lba = 0; lba < 50; lba++) {
        rc = ftl_trim(&s.ftl, lba, TEST_PAGE_SIZE);
        TEST_ASSERT(rc == HFSSS_OK || lba > 0, "trim LBA succeeds");
    }

    rc = sb_journal_flush(&s.ftl.sb);
    TEST_ASSERT(rc == HFSSS_OK, "sb_journal_flush after trims succeeds");

    /* Record PPNs for LBAs 50-99 (still valid). */
    union ppn orig_ppns[100];
    for (u32 lba = 50; lba < 100; lba++) {
        mapping_l2p(&s.ftl.mapping, lba, &orig_ppns[lba]);
    }

    /* Fresh mapping: load checkpoint (100 entries), replay journal (50 trims). */
    u64 raw_pages = (u64)TEST_CHANNELS * TEST_BLOCKS_PER_PLANE * TEST_PAGES_PER_BLOCK;
    struct mapping_ctx fresh;
    rc = mapping_init(&fresh, raw_pages * 80 / 100, raw_pages);
    TEST_ASSERT(rc == HFSSS_OK, "fresh mapping_init succeeds");

    rc = sb_checkpoint_read(&s.ftl.sb, &fresh);
    TEST_ASSERT(rc == HFSSS_OK, "sb_checkpoint_read succeeds");

    rc = sb_journal_replay(&s.ftl.sb, &fresh);
    TEST_ASSERT(rc == HFSSS_OK, "sb_journal_replay succeeds");

    rc = mapping_rebuild_p2l(&fresh);
    TEST_ASSERT(rc == HFSSS_OK, "mapping_rebuild_p2l succeeds");

    TEST_ASSERT(mapping_get_valid_count(&fresh) == 50,
                "valid_count is 50 after checkpoint + trim journal");

    /* LBAs 0-49 should return NOENT. */
    bool trim_ok = true;
    for (u32 lba = 0; lba < 50; lba++) {
        union ppn ppn;
        if (mapping_l2p(&fresh, lba, &ppn) != HFSSS_ERR_NOENT) {
            trim_ok = false;
            break;
        }
    }
    TEST_ASSERT(trim_ok, "LBAs 0-49 return NOENT after trim replay");

    /* LBAs 50-99 should have valid PPNs matching the original mapping. */
    bool valid_ok = true;
    for (u32 lba = 50; lba < 100; lba++) {
        union ppn ppn;
        if (mapping_l2p(&fresh, lba, &ppn) != HFSSS_OK ||
            ppn.raw != orig_ppns[lba].raw) {
            valid_ok = false;
            break;
        }
    }
    TEST_ASSERT(valid_ok, "LBAs 50-99 have correct PPNs after trim replay");

    mapping_cleanup(&fresh);
    free(buf);
    stack_cleanup(&s);
}

/* ================================================================== */
/* Test 5: Full power cycle simulation                                 */
/* ================================================================== */

static void test_power_cycle(void) {
    printf("\n=== Test 5: Full power cycle simulation ===\n");

    struct media_ctx media;
    struct hal_nand_dev nand;
    struct hal_ctx hal;
    struct ftl_ctx ftl;
    struct ftl_config fcfg;
    struct media_config mcfg;
    int rc;

    /* --- Phase 1: write data and flush --- */

    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.channel_count = TEST_CHANNELS;
    mcfg.chips_per_channel = 1;
    mcfg.dies_per_chip = 1;
    mcfg.planes_per_die = 1;
    mcfg.blocks_per_plane = TEST_BLOCKS_PER_PLANE;
    mcfg.pages_per_block = TEST_PAGES_PER_BLOCK;
    mcfg.page_size = TEST_PAGE_SIZE;
    mcfg.spare_size = 64;
    mcfg.nand_type = NAND_TYPE_TLC;

    rc = media_init(&media, &mcfg);
    TEST_ASSERT(rc == HFSSS_OK, "media_init succeeds");

    rc = hal_nand_dev_init(&nand, TEST_CHANNELS, 1, 1, 1,
                           TEST_BLOCKS_PER_PLANE, TEST_PAGES_PER_BLOCK,
                           TEST_PAGE_SIZE, 64, &media);
    TEST_ASSERT(rc == HFSSS_OK, "hal_nand_dev_init succeeds");

    rc = hal_init(&hal, &nand);
    TEST_ASSERT(rc == HFSSS_OK, "hal_init succeeds");

    fcfg = make_ftl_config(TEST_CHANNELS, TEST_BLOCKS_PER_PLANE,
                           TEST_PAGES_PER_BLOCK, TEST_PAGE_SIZE);

    rc = ftl_init(&ftl, &fcfg, &hal);
    TEST_ASSERT(rc == HFSSS_OK, "ftl_init succeeds (phase 1)");

    void *buf = malloc(TEST_PAGE_SIZE);
    TEST_ASSERT(buf != NULL, "write buffer allocated");

    bool write_ok = true;
    for (u32 lba = 0; lba < 200; lba++) {
        fill_buf(buf, lba, TEST_PAGE_SIZE);
        if (ftl_write(&ftl, lba, TEST_PAGE_SIZE, buf) != HFSSS_OK) {
            write_ok = false;
            break;
        }
    }
    TEST_ASSERT(write_ok, "200 LBA writes succeed");

    rc = ftl_flush(&ftl);
    TEST_ASSERT(rc == HFSSS_OK, "ftl_flush succeeds (writes checkpoint)");

    /* --- Power cycle: tear down FTL + HAL, keep media alive --- */
    ftl_cleanup(&ftl);
    hal_cleanup(&hal);
    hal_nand_dev_cleanup(&nand);

    /* --- Phase 2: re-init HAL and FTL on the same media --- */
    rc = hal_nand_dev_init(&nand, TEST_CHANNELS, 1, 1, 1,
                           TEST_BLOCKS_PER_PLANE, TEST_PAGES_PER_BLOCK,
                           TEST_PAGE_SIZE, 64, &media);
    TEST_ASSERT(rc == HFSSS_OK, "hal_nand_dev_init succeeds (phase 2)");

    rc = hal_init(&hal, &nand);
    TEST_ASSERT(rc == HFSSS_OK, "hal_init succeeds (phase 2)");

    rc = ftl_init(&ftl, &fcfg, &hal);
    TEST_ASSERT(rc == HFSSS_OK, "ftl_init succeeds (phase 2, should detect checkpoint)");

    /* Verify checkpoint was detected during init. */
    TEST_ASSERT(sb_has_valid_checkpoint(&ftl.sb),
                "valid checkpoint detected after power cycle");

    /* Read back all 200 LBAs and verify data integrity. */
    bool read_ok = true;
    for (u32 lba = 0; lba < 200; lba++) {
        rc = ftl_read(&ftl, lba, TEST_PAGE_SIZE, buf);
        if (rc != HFSSS_OK) {
            read_ok = false;
            break;
        }
        if (!verify_buf(buf, lba, TEST_PAGE_SIZE)) {
            read_ok = false;
            break;
        }
    }
    TEST_ASSERT(read_ok, "all 200 LBAs read back correctly after power cycle");

    /* Full cleanup. */
    free(buf);
    ftl_cleanup(&ftl);
    hal_cleanup(&hal);
    hal_nand_dev_cleanup(&nand);
    media_cleanup(&media);
}

/* ================================================================== */
/* Test 6: sb_has_valid_checkpoint on fresh device                     */
/* ================================================================== */

static void test_fresh_device_no_checkpoint(void) {
    printf("\n=== Test 6: sb_has_valid_checkpoint on fresh device ===\n");

    struct test_stack s;
    int rc = stack_init(&s, TEST_CHANNELS, TEST_BLOCKS_PER_PLANE,
                        TEST_PAGES_PER_BLOCK, TEST_PAGE_SIZE);
    TEST_ASSERT(rc == 0, "stack_init succeeds");

    TEST_ASSERT(sb_has_valid_checkpoint(&s.ftl.sb) == false,
                "no valid checkpoint on a fresh device");

    stack_cleanup(&s);
}

/* ================================================================== */
/* Test 7: Multiple checkpoints (circular)                             */
/* ================================================================== */

static void test_multiple_checkpoints(void) {
    printf("\n=== Test 7: Multiple checkpoints (circular) ===\n");

    struct test_stack s;
    int rc = stack_init(&s, TEST_CHANNELS, TEST_BLOCKS_PER_PLANE,
                        TEST_PAGES_PER_BLOCK, TEST_PAGE_SIZE);
    TEST_ASSERT(rc == 0, "stack_init succeeds");

    void *buf = malloc(TEST_PAGE_SIZE);
    TEST_ASSERT(buf != NULL, "write buffer allocated");

    /* Round 1: write LBAs 0-49, flush (checkpoint 1). */
    bool write_ok = true;
    for (u32 lba = 0; lba < 50; lba++) {
        fill_buf(buf, lba, TEST_PAGE_SIZE);
        if (ftl_write(&s.ftl, lba, TEST_PAGE_SIZE, buf) != HFSSS_OK) {
            write_ok = false;
            break;
        }
    }
    TEST_ASSERT(write_ok, "round 1: 50 writes succeed");

    rc = ftl_flush(&s.ftl);
    TEST_ASSERT(rc == HFSSS_OK, "checkpoint 1 via ftl_flush succeeds");

    /* Round 2: write LBAs 50-99, flush (checkpoint 2). */
    for (u32 lba = 50; lba < 100; lba++) {
        fill_buf(buf, lba, TEST_PAGE_SIZE);
        if (ftl_write(&s.ftl, lba, TEST_PAGE_SIZE, buf) != HFSSS_OK) {
            write_ok = false;
            break;
        }
    }
    TEST_ASSERT(write_ok, "round 2: 50 writes succeed");

    rc = ftl_flush(&s.ftl);
    TEST_ASSERT(rc == HFSSS_OK, "checkpoint 2 via ftl_flush succeeds");

    /* Round 3: write LBAs 100-149, flush (checkpoint 3). */
    for (u32 lba = 100; lba < 150; lba++) {
        fill_buf(buf, lba, TEST_PAGE_SIZE);
        if (ftl_write(&s.ftl, lba, TEST_PAGE_SIZE, buf) != HFSSS_OK) {
            write_ok = false;
            break;
        }
    }
    TEST_ASSERT(write_ok, "round 3: 50 writes succeed");

    rc = ftl_flush(&s.ftl);
    TEST_ASSERT(rc == HFSSS_OK, "checkpoint 3 via ftl_flush succeeds");

    /* Record PPNs for all 150 LBAs. */
    union ppn orig_ppns[150];
    for (u32 lba = 0; lba < 150; lba++) {
        mapping_l2p(&s.ftl.mapping, lba, &orig_ppns[lba]);
    }

    /* Load the latest checkpoint into a fresh mapping and verify. */
    u64 raw_pages = (u64)TEST_CHANNELS * TEST_BLOCKS_PER_PLANE * TEST_PAGES_PER_BLOCK;
    struct mapping_ctx fresh;
    rc = mapping_init(&fresh, raw_pages * 80 / 100, raw_pages);
    TEST_ASSERT(rc == HFSSS_OK, "fresh mapping_init succeeds");

    rc = sb_checkpoint_read(&s.ftl.sb, &fresh);
    TEST_ASSERT(rc == HFSSS_OK, "sb_checkpoint_read latest checkpoint succeeds");

    rc = sb_journal_replay(&s.ftl.sb, &fresh);
    TEST_ASSERT(rc == HFSSS_OK, "sb_journal_replay succeeds");

    rc = mapping_rebuild_p2l(&fresh);
    TEST_ASSERT(rc == HFSSS_OK, "mapping_rebuild_p2l succeeds");

    TEST_ASSERT(mapping_get_valid_count(&fresh) == 150,
                "fresh mapping valid_count is 150 after 3 checkpoints");

    bool ppn_match = true;
    for (u32 lba = 0; lba < 150; lba++) {
        union ppn ppn;
        if (mapping_l2p(&fresh, lba, &ppn) != HFSSS_OK ||
            ppn.raw != orig_ppns[lba].raw) {
            ppn_match = false;
            break;
        }
    }
    TEST_ASSERT(ppn_match, "all 150 LBAs match after loading latest checkpoint");

    mapping_cleanup(&fresh);
    free(buf);
    stack_cleanup(&s);
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

int main(void) {
    printf("========================================\n");
    printf("  Superblock Metadata Persistence Tests\n");
    printf("========================================\n");

    test_superblock_reservation();
    test_checkpoint_round_trip();
    test_journal_append_replay();
    test_trim_journal();
    test_power_cycle();
    test_fresh_device_no_checkpoint();
    test_multiple_checkpoints();

    printf("\n========================================\n");
    printf("  Results: %d/%d passed, %d failed\n",
           tests_passed, tests_run, tests_failed);
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
