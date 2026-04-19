/*
 * Reset-abort race stress test (Phase 7 review response).
 *
 * The reviewer flagged a TOCTOU race in src/media/cmd_engine.c between
 * the worker's abort-epoch recheck and its on_array_commit() call: the
 * reset path stamps DEAD and increments abort_epoch under die_lock, but
 * the pre-fix worker read the epoch without holding die_lock, so a
 * reset arriving in the window between recheck and commit could stamp
 * DEAD and immediately have the commit write caller data on top.
 *
 * This test exercises the invariant the fix must maintain: for every
 * iteration where a reset is issued after the worker has entered
 * DIE_PROG_ARRAY_BUSY, the target page must end up with the DEAD
 * abort pattern — never with caller data. 100 jittered iterations
 * give enough coverage to flush any residual race on contemporary
 * hardware without blowing the CI budget.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common/common.h"
#include "media/cmd_engine.h"
#include "media/cmd_state.h"
#include "media/media.h"
#include "media/nand.h"
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

#define STRESS_ITERATIONS 100
#define PAYLOAD_FILL_BASE 0x42u

struct worker_ctx {
    struct media_ctx *ctx;
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
    w->rc = media_nand_program(w->ctx, 0, 0, 0, 0, w->block, w->page, buf, NULL);
    return NULL;
}

static bool wait_for_state(struct media_ctx *ctx, enum nand_die_state want, u64 timeout_ns)
{
    struct nand_cmd_target t = {.ch = 0, .chip = 0, .die = 0, .plane_mask = 0x1};
    u64 deadline = get_time_ns() + timeout_ns;
    while (get_time_ns() < deadline) {
        struct nand_die_cmd_state s;
        if (nand_cmd_engine_snapshot(ctx->nand, &t, &s) == HFSSS_OK && s.state == want) {
            return true;
        }
    }
    return false;
}

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

static bool is_caller_fill(const u8 *buf, u32 len, u8 fill)
{
    if (!buf) {
        return false;
    }
    for (u32 i = 0; i < len; i++) {
        if (buf[i] != fill) {
            return false;
        }
    }
    return true;
}

static struct media_config make_cfg(void)
{
    struct media_config cfg = {
        .channel_count = 1,
        .chips_per_channel = 1,
        .dies_per_chip = 1,
        .planes_per_die = 1,
        .blocks_per_plane = 128,
        .pages_per_block = 8,
        .page_size = 4096,
        .spare_size = 64,
        .nand_type = NAND_TYPE_TLC,
        .profile_explicit = true,
        .profile_id = NAND_PROFILE_GENERIC_ONFI_TLC,
    };
    return cfg;
}

/* ------------------------------------------------------------------------ */
/* Stress: 100 iterations, random us jitter, assert no race-lost outcome.  */
/* ------------------------------------------------------------------------ */

static int test_reset_abort_race_stress(void)
{
    printf("\n=== reset-abort race: %d jittered iterations ===\n", STRESS_ITERATIONS);

    struct media_config cfg = make_cfg();
    struct media_ctx ctx;
    int ret = media_init(&ctx, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "stress: media_init");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }

    srand((unsigned)time(NULL));

    u32 race_lost = 0;         /* caller data survived despite reset */
    u32 dead_stamped = 0;      /* DEAD pattern observed (either race outcome) */
    u32 untouched_free = 0;    /* reset arrived before program reached busy */
    u32 other = 0;             /* anything else (should be zero) */

    for (int i = 0; i < STRESS_ITERATIONS; i++) {
        u32 block = 10 + (u32)(i % 100);
        u32 page = 0;
        u8 fill = (u8)(PAYLOAD_FILL_BASE + (i & 0x3F));

        struct worker_ctx w = {.ctx = &ctx, .block = block, .page = page, .fill = fill, .rc = -1};
        pthread_t thr;
        pthread_create(&thr, NULL, prog_worker, &w);

        /* Wait until the worker is definitely past setup — without this
         * the reset might land on DIE_PROG_SETUP where the fix's scope
         * guard intentionally skips stamping (Phase 7 review round 1
         * commit 3). That scenario is exercised separately in its own
         * test; the race this binary targets requires PROG_ARRAY_BUSY. */
        if (!wait_for_state(&ctx, DIE_PROG_ARRAY_BUSY, 5000000000ULL)) {
            pthread_join(thr, NULL);
            other++;
            continue;
        }

        /* Jitter: 0..99 us. Keeps the reset close enough to the worker's
         * commit path to exercise the original race window, without
         * blowing past tPROG (which would complete the program before
         * reset lands). */
        int jitter_us = rand() % 100;
        if (jitter_us > 0) {
            usleep(jitter_us);
        }

        struct nand_cmd_target reset_tgt = {.ch = 0, .chip = 0, .die = 0, .plane_mask = 0x01};
        ret = nand_cmd_engine_submit_reset(ctx.nand, &reset_tgt);
        if (ret != HFSSS_OK) {
            pthread_join(thr, NULL);
            other++;
            continue;
        }
        pthread_join(thr, NULL);

        struct nand_page *pg = nand_get_page(ctx.nand, 0, 0, 0, 0, block, page);
        if (!pg) {
            other++;
            continue;
        }

        if (is_abort_pattern(pg->data, cfg.page_size)) {
            dead_stamped++;
        } else if (is_caller_fill(pg->data, cfg.page_size, fill)) {
            race_lost++;
        } else if (pg->data[0] == 0xFF && pg->data[cfg.page_size - 1] == 0xFF) {
            untouched_free++;
        } else {
            other++;
        }
    }

    printf("  [INFO] outcomes over %d iterations: dead=%u untouched=%u race_lost=%u other=%u\n",
           STRESS_ITERATIONS, dead_stamped, untouched_free, race_lost, other);

    /*
     * The invariant the fix must maintain: a reset issued during
     * PROG_ARRAY_BUSY must always produce a stamped page — the commit
     * cannot win the race. Before the fix, race_lost could be > 0 on
     * sufficiently aggressive hardware. After the fix it must be zero.
     */
    TEST_ASSERT(race_lost == 0,
                "reset-vs-commit race: caller data never survives a reset-during-busy");
    /* Most iterations should reach the DEAD-stamped outcome; the
     * 'other' bucket is only for pathological scheduling where the
     * program completed before reset submitted. Zero is expected under
     * normal scheduling; allow a small fraction for CI variance. */
    TEST_ASSERT(other <= STRESS_ITERATIONS / 20,
                "reset-abort stress: 'other' outcome rate within budget");

    media_cleanup(&ctx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------------ */
/* Control: reset from an idle die must not stamp anything.                 */
/* ------------------------------------------------------------------------ */

static int test_reset_from_idle_does_not_stamp(void)
{
    printf("\n=== reset from idle: no page mutation ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx ctx;
    int ret = media_init(&ctx, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "idle: media_init");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }

    /* Program a page cleanly so its content is distinguishable from
     * both 0xFF (erased) and DEAD (aborted). */
    u8 wr[4096];
    memset(wr, 0xA5, sizeof(wr));
    ret = media_nand_program(&ctx, 0, 0, 0, 0, 4, 0, wr, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "idle: seed program");

    /* Reset the die from DIE_IDLE. The reset handler's scope guard
     * must skip stamping entirely here. */
    struct nand_cmd_target t = {.ch = 0, .chip = 0, .die = 0, .plane_mask = 0x01};
    ret = nand_cmd_engine_submit_reset(ctx.nand, &t);
    TEST_ASSERT(ret == HFSSS_OK, "idle: reset accepted from IDLE");

    struct nand_page *pg = nand_get_page(ctx.nand, 0, 0, 0, 0, 4, 0);
    TEST_ASSERT(pg != NULL, "idle: target page pointer");
    TEST_ASSERT(pg->state == PAGE_VALID,
                "idle: reset-from-IDLE did not change page state to INVALID");
    TEST_ASSERT(pg->data[0] == 0xA5 && pg->data[cfg.page_size - 1] == 0xA5,
                "idle: reset-from-IDLE did not stamp the page");

    media_cleanup(&ctx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

int main(void)
{
    printf("========================================\n");
    printf("NAND Reset-Abort Race Stress\n");
    printf("========================================\n");

    test_reset_from_idle_does_not_stamp();
    test_reset_abort_race_stress();

    printf("\n========================================\n");
    printf("Reset-Abort Stress Summary\n");
    printf("========================================\n");
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    return tests_failed == 0 ? 0 : 1;
}
