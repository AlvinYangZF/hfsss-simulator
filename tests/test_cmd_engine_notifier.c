/*
 * Tests for the optional die-ready notifier callback on
 * struct nand_device. Three cases cover the three points in
 * cmd_engine.c where a die transitions to DIE_IDLE:
 *
 *   anchor A — normal completion of a non-cache operation.
 *              Must fire the notifier exactly once per completion.
 *   anchor B — reset force-clear on a busy die.
 *              Must fire the notifier (at least once) on reset.
 *   anchor C — implicit cache-sequence termination at submit
 *              entry, when a non-cache op preempts an open cache
 *              sequence. Must NOT fire the notifier for the
 *              implicit terminate; the new op's own anchor-A
 *              completion is the only wake.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

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

static atomic_int g_notify_count;
static u32 g_last_ch;
static u32 g_last_chip;
static u32 g_last_die;
static struct nand_device *g_last_dev;

static void test_notifier(struct nand_device *dev, u32 ch, u32 chip, u32 die)
{
    atomic_fetch_add(&g_notify_count, 1);
    g_last_dev = dev;
    g_last_ch = ch;
    g_last_chip = chip;
    g_last_die = die;
}

static struct media_config make_cfg(void)
{
    struct media_config cfg = {
        .channel_count = 4,
        .chips_per_channel = 1,
        .dies_per_chip = 1,
        .planes_per_die = 1,
        .blocks_per_plane = 8,
        .pages_per_block = 16,
        .page_size = 4096,
        .spare_size = 64,
        .nand_type = NAND_TYPE_TLC,
    };
    return cfg;
}

static int test_anchor_a_fires_on_normal_completion(void)
{
    printf("\n=== anchor A: normal completion fires notifier ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx ctx;
    int ret = media_init(&ctx, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "anchor-A: media_init OK");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }

    ctx.nand->die_ready_notifier = test_notifier;
    ctx.nand->die_ready_ctx = NULL;
    atomic_store(&g_notify_count, 0);
    g_last_dev = NULL;
    g_last_ch = 0xFFFFFFFFu;
    g_last_chip = 0xFFFFFFFFu;
    g_last_die = 0xFFFFFFFFu;

    /* A program completes via anchor A's "drive to IDLE" branch. */
    u8 buf[4096];
    memset(buf, 0x5A, sizeof(buf));
    ret = media_nand_program(&ctx, 0, 0, 0, 0, 0, 0, buf, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "anchor-A: program submit returns OK");

    int count = atomic_load(&g_notify_count);
    TEST_ASSERT(count == 1, "anchor-A: notifier fires exactly once on completion");
    TEST_ASSERT(g_last_dev == ctx.nand, "anchor-A: notifier received the correct nand_device");
    TEST_ASSERT(g_last_ch == 0 && g_last_chip == 0 && g_last_die == 0,
                "anchor-A: notifier received correct (ch, chip, die) coords");

    media_cleanup(&ctx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

static int test_anchor_b_fires_on_reset(void)
{
    printf("\n=== anchor B: reset force-clear fires notifier ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx ctx;
    int ret = media_init(&ctx, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "anchor-B: media_init OK");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }

    ctx.nand->die_ready_notifier = test_notifier;
    ctx.nand->die_ready_ctx = NULL;
    atomic_store(&g_notify_count, 0);

    /* Open a cache program sequence on (ch=1, chip=0, die=0). The first
     * cache-program leaves the die in DIE_PROG_ARRAY_BUSY with cache_active
     * set, so a subsequent reset must traverse the force-clear path
     * (anchor B). */
    struct nand_cmd_target target = {
        .ch = 1,
        .chip = 0,
        .die = 0,
        .plane_mask = 1u,
        .block = 0,
        .page = 0,
    };
    u8 buf[4096];
    memset(buf, 0xA5, sizeof(buf));
    ret = media_nand_cache_program(&ctx, 1, 0, 0, 0, 0, 0, buf, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "anchor-B: cache_program submit returns OK");

    int after_cache = atomic_load(&g_notify_count);

    /* Reset forces the die back to IDLE through engine_submit_reset's
     * nand_cmd_state_init() under die_lock — anchor B's wake site. */
    ret = nand_cmd_engine_submit_reset(ctx.nand, &target);
    TEST_ASSERT(ret == HFSSS_OK, "anchor-B: reset submit returns OK");

    int after_reset = atomic_load(&g_notify_count);
    TEST_ASSERT(after_reset >= after_cache + 1, "anchor-B: reset fires the notifier at least once");

    media_cleanup(&ctx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

static int test_anchor_c_does_not_fire_on_implicit_cache_terminate(void)
{
    printf("\n=== anchor C: implicit cache terminate must NOT fire notifier ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx ctx;
    int ret = media_init(&ctx, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "anchor-C: media_init OK");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }

    ctx.nand->die_ready_notifier = test_notifier;
    ctx.nand->die_ready_ctx = NULL;
    atomic_store(&g_notify_count, 0);

    /* Open a cache sequence on (ch=2, chip=0, die=0). This program
     * completes synchronously and leaves cache_active=true on the die
     * for the trailing tCBSY window. */
    u8 buf[4096];
    memset(buf, 0xC3, sizeof(buf));
    ret = media_nand_cache_program(&ctx, 2, 0, 0, 0, 0, 0, buf, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "anchor-C: cache_program submit returns OK");

    int after_cache = atomic_load(&g_notify_count);

    /* A non-cache READ on the same die now hits anchor C's
     * "implicit cache terminate" branch at engine_submit entry, which
     * forces state to DIE_IDLE / cache_active=false. That transient
     * IDLE must not fire the notifier. The READ then proceeds and
     * fires anchor A on its own completion — exactly one notify total
     * for the read. */
    u8 rbuf[4096];
    memset(rbuf, 0, sizeof(rbuf));
    ret = media_nand_read(&ctx, 2, 0, 0, 0, 0, 0, rbuf, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "anchor-C: subsequent non-cache read returns OK");

    int after_read = atomic_load(&g_notify_count);
    int delta = after_read - after_cache;
    TEST_ASSERT(delta == 1, "anchor-C: only anchor-A fires for the new op (delta == 1, not 2)");

    media_cleanup(&ctx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

int main(void)
{
    printf("========================================\n");
    printf("NAND cmd_engine die-ready notifier tests\n");
    printf("========================================\n");

    test_anchor_a_fires_on_normal_completion();
    test_anchor_b_fires_on_reset();
    test_anchor_c_does_not_fire_on_implicit_cache_terminate();

    printf("\n========================================\n");
    printf("Notifier Test Summary\n");
    printf("========================================\n");
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    return tests_failed == 0 ? 0 : 1;
}
