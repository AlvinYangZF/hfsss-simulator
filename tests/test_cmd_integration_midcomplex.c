/*
 * NAND command-engine integration tests (Phase 7 mid-complex tier).
 *
 * Two scenarios combining >= 2 phases. Heavier cases (multi-plane ×
 * suspend × cache combinations) live in test_cmd_integration_heavy.c.
 *
 *   IS-02: non-conflicting reads during SUSPENDED_PROG / SUSPENDED_ERASE
 *          must succeed and return correct data; the engine must not
 *          serialize them behind the suspended op's remaining time.
 *          (Phase 2 reads + Phase 3 suspend)
 *
 *   IS-03: reset interrupting PROG / multi-plane ERASE must stamp the
 *          target page(s)/block(s) with the DEAD abort pattern so the
 *          effect of the aborted command is distinguishable from both
 *          a clean erase (0xFF) and a valid program. Verifies the die
 *          returns to DIE_IDLE with clean accounting and that the
 *          engine accepts new commands afterwards.
 *          (Phase 1 reset-abort + Phase 4 multi-plane + reset-abort
 *           data-policy extension)
 *
 * IS-09 (ONFI strict vs Toggle relaxed plane addressing) is deferred
 * to Phase 7 T5 because the engine does not yet consult mp_rules from
 * the active profile; T5 will wire that alongside the ONFI 3.5 /
 * Toggle opcode-bitmap divergence.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "common/common.h"
#include "media/cmd_engine.h"
#include "media/cmd_legality.h"
#include "media/cmd_state.h"
#include "media/media.h"
#include "media/nand.h"
#include "media/nand_identity.h"
#include "media/nand_profile.h"

#define TEST_PASS 0
#define TEST_FAIL 1

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                                                 \
    do {                                                                       \
        tests_run++;                                                           \
        if (cond) {                                                            \
            printf("  [PASS] %s\n", msg);                                      \
            tests_passed++;                                                    \
        } else {                                                               \
            printf("  [FAIL] %s\n", msg);                                      \
            tests_failed++;                                                    \
        }                                                                      \
    } while (0)

/* ------------------------------------------------------------------------ */
/* Shared helpers. Same patterns as test_cmd_integration_basic.c but        */
/* enriched with multi-plane program/erase workers for IS-03.               */
/* ------------------------------------------------------------------------ */

struct worker_ctx {
    struct media_ctx *ctx;
    u32 plane;
    u32 block;
    u32 page;
    u8 fill;
    int rc;
};

static void *prog_worker(void *arg)
{
    struct worker_ctx *w = arg;
    u8 buf[4096];
    memset(buf, w->fill, sizeof(buf));
    w->rc = media_nand_program(w->ctx, 0, 0, 0, w->plane, w->block, w->page, buf, NULL);
    return NULL;
}

struct mp_erase_worker_ctx {
    struct media_ctx *ctx;
    u32 plane_mask;
    u32 block;
    int rc;
};

static void *mp_erase_worker(void *arg)
{
    struct mp_erase_worker_ctx *w = arg;
    w->rc = media_nand_multi_plane_erase(w->ctx, 0, 0, 0, w->plane_mask, w->block);
    return NULL;
}

static bool wait_for_state(struct media_ctx *ctx, enum nand_die_state want, u64 timeout_ns)
{
    u64 deadline = get_time_ns() + timeout_ns;
    while (get_time_ns() < deadline) {
        struct nand_status_enhanced enh;
        if (media_nand_read_status_enhanced(ctx, 0, 0, 0, &enh) == HFSSS_OK && enh.state == want) {
            return true;
        }
    }
    return false;
}

static struct media_config make_cfg_two_plane(void)
{
    struct media_config cfg = {
        .channel_count = 1,
        .chips_per_channel = 1,
        .dies_per_chip = 1,
        .planes_per_die = 2,
        .blocks_per_plane = 32,
        .pages_per_block = 16,
        .page_size = 4096,
        .spare_size = 64,
        .nand_type = NAND_TYPE_TLC,
        .profile_explicit = true,
        .profile_id = NAND_PROFILE_GENERIC_ONFI_TLC,
        .enable_multi_plane = true,
    };
    return cfg;
}

/* Does the byte stream look like the abort-pattern (DE AD DE AD ...) ? */
static bool is_abort_pattern(const u8 *buf, u32 len)
{
    if (!buf || len < 4) {
        return false;
    }
    for (u32 i = 0; i < len; i++) {
        u8 expect = (i & 1u) ? NAND_ABORT_PATTERN_BYTE_LO : NAND_ABORT_PATTERN_BYTE_HI;
        if (buf[i] != expect) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* IS-02: non-conflicting reads during suspend                               */
/* ------------------------------------------------------------------------ */

static int test_is02_suspended_legal_reads(void)
{
    printf("\n=== IS-02: non-conflicting reads during suspend ===\n");

    struct media_config cfg = make_cfg_two_plane();
    struct media_ctx ctx;
    int ret = media_init(&ctx, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "IS-02: media_init");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }

    /* Seed two non-conflicting pages the suspend-time reads will
     * target. These are on (plane=0, block=3, page=5) and
     * (plane=1, block=4, page=0) — both distinct from the suspended
     * program's target. */
    u8 seed_a[4096], seed_b[4096];
    memset(seed_a, 0xBB, sizeof(seed_a));
    memset(seed_b, 0xCC, sizeof(seed_b));
    TEST_ASSERT(media_nand_program(&ctx, 0, 0, 0, 0, 3, 5, seed_a, NULL) == HFSSS_OK, "IS-02: seed page A");
    TEST_ASSERT(media_nand_program(&ctx, 0, 0, 0, 1, 4, 0, seed_b, NULL) == HFSSS_OK, "IS-02: seed page B");

    /* Start a program we will suspend. Target (plane=0, block=7, page=0). */
    struct worker_ctx prog = {.ctx = &ctx, .plane = 0, .block = 7, .page = 0, .fill = 0x99, .rc = -1};
    pthread_t prog_thr;
    pthread_create(&prog_thr, NULL, prog_worker, &prog);
    TEST_ASSERT(wait_for_state(&ctx, DIE_PROG_ARRAY_BUSY, 5000000000ULL), "IS-02: entered PROG_ARRAY_BUSY");

    TEST_ASSERT(media_nand_program_suspend(&ctx, 0, 0, 0) == HFSSS_OK, "IS-02: program_suspend OK");
    TEST_ASSERT(wait_for_state(&ctx, DIE_SUSPENDED_PROG, 5000000000ULL), "IS-02: entered SUSPENDED_PROG");

    /* Read A: different plane+block+page from the suspended target.
     * Should complete fast and return the seeded data. */
    u64 t0 = get_time_ns();
    u8 rd_a[4096];
    int rc_a = media_nand_read(&ctx, 0, 0, 0, 0, 3, 5, rd_a, NULL);
    u64 dur_a = get_time_ns() - t0;
    TEST_ASSERT(rc_a == HFSSS_OK, "IS-02: non-conflicting read A succeeds during suspend");
    TEST_ASSERT(rd_a[0] == 0xBB && rd_a[4095] == 0xBB, "IS-02: non-conflicting read A returns seeded data");

    /* Read B: different plane from the suspended target. Should also
     * succeed. */
    u8 rd_b[4096];
    int rc_b = media_nand_read(&ctx, 0, 0, 0, 1, 4, 0, rd_b, NULL);
    TEST_ASSERT(rc_b == HFSSS_OK, "IS-02: non-conflicting read B (different plane) succeeds during suspend");
    TEST_ASSERT(rd_b[0] == 0xCC, "IS-02: non-conflicting read B returns seeded data");

    /* Sanity: the read did not push us past the suspended program's
     * remaining time. A naive serialization bug would make the read
     * cost the full tR + remaining tPROG, which for TLC is in the
     * millisecond range. A healthy suspend path returns in tens of
     * microseconds. Budget is generous (1 ms) so CI timing noise is
     * not flaky. */
    TEST_ASSERT(dur_a < 1000000ULL,
                "IS-02: suspended read does not block on the suspended program's remaining time");

    /* State must still be SUSPENDED_PROG after the reads. */
    struct nand_status_enhanced enh;
    TEST_ASSERT(media_nand_read_status_enhanced(&ctx, 0, 0, 0, &enh) == HFSSS_OK, "IS-02: status after reads OK");
    TEST_ASSERT(enh.state == DIE_SUSPENDED_PROG, "IS-02: still SUSPENDED_PROG after non-conflicting reads");
    TEST_ASSERT(enh.suspend_count == 1, "IS-02: suspend_count unchanged by non-conflicting reads");

    TEST_ASSERT(media_nand_program_resume(&ctx, 0, 0, 0) == HFSSS_OK, "IS-02: program_resume OK");
    pthread_join(prog_thr, NULL);
    TEST_ASSERT(prog.rc == HFSSS_OK, "IS-02: suspended program completes OK after resume");

    /* Verify the resumed program actually landed its bytes. */
    u8 rd_tgt[4096];
    TEST_ASSERT(media_nand_read(&ctx, 0, 0, 0, 0, 7, 0, rd_tgt, NULL) == HFSSS_OK, "IS-02: read resumed target OK");
    TEST_ASSERT(rd_tgt[0] == 0x99 && rd_tgt[4095] == 0x99, "IS-02: resumed target carries programmed data");

    media_cleanup(&ctx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------------ */
/* IS-03: reset-abort stamps the DEAD pattern on target pages                */
/* ------------------------------------------------------------------------ */

static int test_is03_reset_abort_pattern_program(void)
{
    printf("\n=== IS-03a: reset aborts in-flight program, page gets DEAD pattern ===\n");

    struct media_config cfg = make_cfg_two_plane();
    struct media_ctx ctx;
    int ret = media_init(&ctx, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "IS-03a: media_init");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }

    /* Baseline: the target page starts PAGE_FREE / 0xFF. We start a
     * program to plane=0, block=9, page=3 with pattern 0x77, then reset
     * mid-ARRAY_BUSY. Expectation: the page carries DEAD not 0x77 and
     * not 0xFF; the adjacent untouched page at (plane=0, block=9,
     * page=4) is still 0xFF. */
    struct worker_ctx prog = {.ctx = &ctx, .plane = 0, .block = 9, .page = 3, .fill = 0x77, .rc = -1};
    pthread_t prog_thr;
    pthread_create(&prog_thr, NULL, prog_worker, &prog);
    TEST_ASSERT(wait_for_state(&ctx, DIE_PROG_ARRAY_BUSY, 5000000000ULL), "IS-03a: entered PROG_ARRAY_BUSY");

    /* Submit reset via the engine (media has no wrapper for reset). */
    struct nand_cmd_target reset_tgt = {.ch = 0, .chip = 0, .die = 0, .plane_mask = 0x01, .block = 0, .page = 0};
    TEST_ASSERT(nand_cmd_engine_submit_reset(ctx.nand, &reset_tgt) == HFSSS_OK, "IS-03a: reset accepted mid-flight");
    pthread_join(prog_thr, NULL);
    TEST_ASSERT(prog.rc != HFSSS_OK, "IS-03a: aborted program returns non-OK");

    /* Die should be back to IDLE with a clean state. */
    struct nand_status_enhanced enh_after;
    TEST_ASSERT(media_nand_read_status_enhanced(&ctx, 0, 0, 0, &enh_after) == HFSSS_OK, "IS-03a: status after reset OK");
    TEST_ASSERT(enh_after.state == DIE_IDLE, "IS-03a: die returned to DIE_IDLE");
    TEST_ASSERT(enh_after.suspend_count == 0, "IS-03a: suspend_count cleared after reset");

    /* Target page carries DEAD. Use direct NAND access because
     * media_nand_read would rewrite via the active PAGE_INVALID check. */
    struct nand_page *p_tgt = nand_get_page(ctx.nand, 0, 0, 0, 0, 9, 3);
    TEST_ASSERT(p_tgt != NULL, "IS-03a: target page pointer");
    TEST_ASSERT(p_tgt->state == PAGE_INVALID, "IS-03a: target page marked PAGE_INVALID");
    TEST_ASSERT(is_abort_pattern(p_tgt->data, cfg.page_size), "IS-03a: target page contains DEAD pattern");
    TEST_ASSERT(p_tgt->dirty == true, "IS-03a: target page dirty flag set for checkpoint");

    /*
     * Containing block must also be dirty: media_save_incremental short-
     * circuits on !blk->dirty and never reaches the page level. Before
     * Phase 7 review round 1 commit 2, the stamp path only set page->dirty,
     * so a reset-aborted page on a previously-clean block would be dropped
     * by an incremental checkpoint and come back as stale on reload. Block
     * 9 was never programmed before this test, so its clean-to-dirty
     * transition here is the exact regression guard for that bug.
     */
    struct nand_block *blk_tgt = nand_get_block(ctx.nand, 0, 0, 0, 0, 9);
    TEST_ASSERT(blk_tgt != NULL, "IS-03a: target block pointer");
    TEST_ASSERT(blk_tgt->dirty == true, "IS-03a: target block dirty flag set by abort-stamp");

    /* Adjacent non-target page untouched (PAGE_FREE / 0xFF). */
    struct nand_page *p_adj = nand_get_page(ctx.nand, 0, 0, 0, 0, 9, 4);
    TEST_ASSERT(p_adj != NULL, "IS-03a: adjacent page pointer");
    TEST_ASSERT(p_adj->state == PAGE_FREE, "IS-03a: adjacent page still PAGE_FREE");
    TEST_ASSERT(p_adj->data[0] == 0xFF && p_adj->data[cfg.page_size - 1] == 0xFF,
                "IS-03a: adjacent page still 0xFF");

    /* Engine accepts a fresh program on a clean block after abort. */
    u8 redo[4096];
    memset(redo, 0x55, sizeof(redo));
    int rc_redo = media_nand_program(&ctx, 0, 0, 0, 0, 10, 0, redo, NULL);
    TEST_ASSERT(rc_redo == HFSSS_OK, "IS-03a: engine accepts new program after reset");
    u8 rd_back[4096];
    TEST_ASSERT(media_nand_read(&ctx, 0, 0, 0, 0, 10, 0, rd_back, NULL) == HFSSS_OK, "IS-03a: read-back new program");
    TEST_ASSERT(rd_back[0] == 0x55, "IS-03a: read-back matches post-reset program");

    media_cleanup(&ctx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

static int test_is03_reset_abort_pattern_mp_erase(void)
{
    printf("\n=== IS-03b: reset aborts multi-plane erase, all block pages get DEAD ===\n");

    struct media_config cfg = make_cfg_two_plane();
    struct media_ctx ctx;
    int ret = media_init(&ctx, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "IS-03b: media_init");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }

    /* Seed a recognizable pattern in block=5 on both planes so the
     * post-abort content is distinguishable from both the original
     * programmed bytes and a clean erase. */
    const u32 target_block = 5;
    u8 seed0[4096], seed1[4096];
    memset(seed0, 0xA0, sizeof(seed0));
    memset(seed1, 0xB1, sizeof(seed1));
    TEST_ASSERT(media_nand_program(&ctx, 0, 0, 0, 0, target_block, 0, seed0, NULL) == HFSSS_OK, "IS-03b: seed p0");
    TEST_ASSERT(media_nand_program(&ctx, 0, 0, 0, 1, target_block, 0, seed1, NULL) == HFSSS_OK, "IS-03b: seed p1");

    /* Start the multi-plane erase of block=5 across planes 0+1. */
    struct mp_erase_worker_ctx er = {.ctx = &ctx, .plane_mask = 0x3, .block = target_block, .rc = -1};
    pthread_t er_thr;
    pthread_create(&er_thr, NULL, mp_erase_worker, &er);
    TEST_ASSERT(wait_for_state(&ctx, DIE_ERASE_ARRAY_BUSY, 5000000000ULL), "IS-03b: entered ERASE_ARRAY_BUSY");

    /* Reset with plane_mask matching the erase's mask so the abort
     * stamp covers both planes. */
    struct nand_cmd_target reset_tgt = {
        .ch = 0, .chip = 0, .die = 0, .plane_mask = 0x03, .block = 0, .page = 0};
    TEST_ASSERT(nand_cmd_engine_submit_reset(ctx.nand, &reset_tgt) == HFSSS_OK, "IS-03b: reset accepted mid-erase");
    pthread_join(er_thr, NULL);
    TEST_ASSERT(er.rc != HFSSS_OK, "IS-03b: aborted multi-plane erase returns non-OK");

    struct nand_status_enhanced enh_after;
    TEST_ASSERT(media_nand_read_status_enhanced(&ctx, 0, 0, 0, &enh_after) == HFSSS_OK, "IS-03b: status after reset OK");
    TEST_ASSERT(enh_after.state == DIE_IDLE, "IS-03b: die returned to DIE_IDLE");

    /* Every page in both target blocks should carry the abort pattern.
     * Spot-check the first, mid, and last page of each plane's block. */
    const u32 mid = cfg.pages_per_block / 2;
    const u32 last = cfg.pages_per_block - 1;
    for (u32 plane = 0; plane < 2; plane++) {
        for (u32 pg_idx = 0; pg_idx == 0 || pg_idx == mid || pg_idx == last; pg_idx = (pg_idx == 0) ? mid : last) {
            struct nand_page *pg = nand_get_page(ctx.nand, 0, 0, 0, plane, target_block, pg_idx);
            char msg[96];
            snprintf(msg, sizeof(msg), "IS-03b: p%u block=%u page=%u PAGE_INVALID", plane, target_block, pg_idx);
            TEST_ASSERT(pg && pg->state == PAGE_INVALID, msg);
            snprintf(msg, sizeof(msg), "IS-03b: p%u block=%u page=%u DEAD pattern", plane, target_block, pg_idx);
            TEST_ASSERT(pg && is_abort_pattern(pg->data, cfg.page_size), msg);
            if (pg_idx == last) {
                break;
            }
        }
    }

    /* Both target blocks must carry a dirty flag so an incremental
     * checkpoint picks them up. Pre-programming at the top of the
     * test already sets blk->dirty; the point here is that the
     * abort-stamp path must not CLEAR or leave stale the block dirty
     * bit, and must ensure it is set when the erase would otherwise
     * clean the block. */
    for (u32 plane = 0; plane < 2; plane++) {
        struct nand_block *blk = nand_get_block(ctx.nand, 0, 0, 0, plane, target_block);
        char msg[96];
        snprintf(msg, sizeof(msg), "IS-03b: p%u target block dirty after abort-stamp", plane);
        TEST_ASSERT(blk && blk->dirty == true, msg);
    }

    /* Sanity: a page outside the erased block is NOT stamped. */
    struct nand_page *p_outside = nand_get_page(ctx.nand, 0, 0, 0, 0, target_block + 1, 0);
    TEST_ASSERT(p_outside != NULL, "IS-03b: adjacent block page pointer");
    TEST_ASSERT(!is_abort_pattern(p_outside->data, cfg.page_size), "IS-03b: adjacent block does not get DEAD");

    /* Adjacent block was never touched by the aborted erase — it must
     * remain clean so incremental checkpoint does not copy data for
     * blocks the reset did not affect. */
    struct nand_block *blk_adj = nand_get_block(ctx.nand, 0, 0, 0, 0, target_block + 1);
    TEST_ASSERT(blk_adj != NULL, "IS-03b: adjacent block pointer");
    TEST_ASSERT(blk_adj->dirty == false, "IS-03b: adjacent block stays clean after abort-stamp");

    /* Engine continues to accept commands after the abort. */
    int rc_erase2 = media_nand_erase(&ctx, 0, 0, 0, 0, target_block + 2);
    TEST_ASSERT(rc_erase2 == HFSSS_OK, "IS-03b: engine accepts new erase after reset");

    media_cleanup(&ctx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

int main(void)
{
    printf("========================================\n");
    printf("NAND Command Engine Integration — Mid\n");
    printf("========================================\n");

    test_is02_suspended_legal_reads();
    test_is03_reset_abort_pattern_program();
    test_is03_reset_abort_pattern_mp_erase();

    printf("\n========================================\n");
    printf("Integration Mid Summary\n");
    printf("========================================\n");
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    return tests_failed == 0 ? 0 : 1;
}
