#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "ftl/ftl_worker.h"
#include "media/media.h"
#include "hal/hal.h"

static int total_tests = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    total_tests++; \
    if (cond) { printf("  [PASS] %s\n", msg); passed++; } \
    else      { printf("  [FAIL] %s\n", msg); failed++; } \
} while (0)

#define CH    4
#define CHIP  2
#define DIE   1
#define PLANE 1
#define BLKS  16
#define PGS   8
#define PGSZ  4096
#define TOTAL_LBAS 256

struct test_env {
    struct media_ctx     media;
    struct hal_nand_dev  nand;
    struct hal_ctx       hal;
    struct ftl_mt_ctx    mt;
};

static int setup(struct test_env *env)
{
    struct media_config mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.channel_count     = CH;
    mcfg.chips_per_channel = CHIP;
    mcfg.dies_per_chip     = DIE;
    mcfg.planes_per_die    = PLANE;
    mcfg.blocks_per_plane  = BLKS;
    mcfg.pages_per_block   = PGS;
    mcfg.page_size         = PGSZ;
    mcfg.spare_size        = 64;
    mcfg.nand_type         = NAND_TYPE_TLC;

    int ret = media_init(&env->media, &mcfg);
    if (ret != HFSSS_OK) return ret;

    ret = hal_nand_dev_init(&env->nand, CH, CHIP, DIE, PLANE, BLKS, PGS,
                            PGSZ, 64, &env->media);
    if (ret != HFSSS_OK) { media_cleanup(&env->media); return ret; }

    ret = hal_init(&env->hal, &env->nand);
    if (ret != HFSSS_OK) {
        hal_nand_dev_cleanup(&env->nand);
        media_cleanup(&env->media);
        return ret;
    }

    struct ftl_config fcfg;
    memset(&fcfg, 0, sizeof(fcfg));
    fcfg.channel_count     = CH;
    fcfg.chips_per_channel = CHIP;
    fcfg.dies_per_chip     = DIE;
    fcfg.planes_per_die    = PLANE;
    fcfg.blocks_per_plane  = BLKS;
    fcfg.pages_per_block   = PGS;
    fcfg.page_size         = PGSZ;
    fcfg.total_lbas        = TOTAL_LBAS;
    fcfg.op_ratio          = 20;
    fcfg.gc_policy         = GC_POLICY_GREEDY;
    fcfg.gc_threshold      = 5;
    fcfg.gc_hiwater        = 10;
    fcfg.gc_lowater        = 3;

    ret = ftl_mt_init(&env->mt, &fcfg, &env->hal);
    if (ret != HFSSS_OK) {
        hal_cleanup(&env->hal);
        hal_nand_dev_cleanup(&env->nand);
        media_cleanup(&env->media);
        return ret;
    }

    return HFSSS_OK;
}

static void teardown(struct test_env *env)
{
    ftl_mt_cleanup(&env->mt);
    hal_cleanup(&env->hal);
    hal_nand_dev_cleanup(&env->nand);
    media_cleanup(&env->media);
}

static void test_gc_mt_reclaims_blocks(void)
{
    printf("\n=== GC MT: Reclaim blocks after overwrites ===\n");

    struct test_env env;
    memset(&env, 0, sizeof(env));
    int ret = setup(&env);
    TEST_ASSERT(ret == HFSSS_OK, "setup succeeds");
    if (ret != HFSSS_OK) return;

    struct ftl_ctx *ftl = &env.mt.ftl;
    struct taa_ctx *taa = &env.mt.taa;

    uint8_t wbuf[PGSZ];
    int errors = 0;
    for (u32 i = 0; i < TOTAL_LBAS; i++) {
        memset(wbuf, (uint8_t)(i & 0xFF), PGSZ);
        ret = ftl_write_page_mt(ftl, taa, i, wbuf);
        if (ret != HFSSS_OK) errors++;
    }
    TEST_ASSERT(errors == 0, "initial fill succeeds");

    u64 free_before = block_get_free_count(&ftl->block_mgr);
    printf("  Free blocks before overwrite: %" PRIu64 "\n", free_before);

    errors = 0;
    for (u32 i = 0; i < TOTAL_LBAS; i++) {
        memset(wbuf, (uint8_t)((i + 0x55) & 0xFF), PGSZ);
        ret = ftl_write_page_mt(ftl, taa, i, wbuf);
        if (ret != HFSSS_OK) errors++;
    }
    TEST_ASSERT(errors == 0, "overwrite pass succeeds");

    u64 free_after_overwrite = block_get_free_count(&ftl->block_mgr);
    printf("  Free blocks after overwrite: %" PRIu64 "\n", free_after_overwrite);

    int gc_ret = gc_run_mt(&ftl->gc, &ftl->block_mgr, taa, ftl->hal);
    printf("  gc_run_mt returned: %d\n", gc_ret);

    u64 free_after_gc = block_get_free_count(&ftl->block_mgr);
    printf("  Free blocks after GC: %" PRIu64 "\n", free_after_gc);
    TEST_ASSERT(free_after_gc > free_after_overwrite,
                "GC reclaimed at least one block");

    uint8_t rbuf[PGSZ];
    int verify_errors = 0;
    for (u32 i = 0; i < TOTAL_LBAS; i++) {
        uint8_t expected[PGSZ];
        memset(expected, (uint8_t)((i + 0x55) & 0xFF), PGSZ);
        ret = ftl_read_page_mt(ftl, taa, i, rbuf);
        if (ret != HFSSS_OK || memcmp(expected, rbuf, PGSZ) != 0) {
            verify_errors++;
        }
    }
    TEST_ASSERT(verify_errors == 0,
                "data integrity preserved after GC relocation");

    teardown(&env);
}

/*
 * Duty-cycle admit callback that always rejects. Simulates a
 * det_window configured into DW_HOST_IO for the duration of the
 * test, which is what a real caller gets from det_window_admit_gc
 * during the HOST_IO phase.
 */
static int g_always_reject_calls = 0;
static bool always_reject_admit(void *ctx, u64 now_ns)
{
    (void)ctx; (void)now_ns;
    g_always_reject_calls++;
    return false;
}

/*
 * End-to-end test of the REQ-153 duty-cycle consumer: when an admit
 * callback returns false, gc_run_mt must bail out of its per-move
 * loop without performing any page moves. moved_pages must stay at
 * its pre-call value and det_window_rejects must advance.
 *
 * This is the closure signal HLD_02 §11.3 asks for: "No GC page
 * moves occur during HOST_IO window".
 */
static void test_gc_mt_respects_admit_callback_reject(void)
{
    printf("\n=== GC MT: Admit callback reject gates page moves ===\n");

    struct test_env env;
    memset(&env, 0, sizeof(env));
    int ret = setup(&env);
    TEST_ASSERT(ret == HFSSS_OK, "setup succeeds");
    if (ret != HFSSS_OK) return;

    struct ftl_ctx *ftl = &env.mt.ftl;
    struct taa_ctx *taa = &env.mt.taa;

    /* Fill + overwrite to create GC-eligible state (matches the
     * reclaim test setup). We run the baseline GC with no admit cb
     * attached first to confirm there's work to do. */
    uint8_t wbuf[PGSZ];
    for (u32 i = 0; i < TOTAL_LBAS; i++) {
        memset(wbuf, (uint8_t)(i & 0xFF), PGSZ);
        ftl_write_page_mt(ftl, taa, i, wbuf);
    }
    for (u32 i = 0; i < TOTAL_LBAS; i++) {
        memset(wbuf, (uint8_t)((i + 0x55) & 0xFF), PGSZ);
        ftl_write_page_mt(ftl, taa, i, wbuf);
    }

    u64 moved_before = ftl->gc.moved_pages;
    u64 rejects_before = ftl->gc.det_window_rejects;

    /* Attach the always-reject callback and invoke GC. The first
     * per-page admit check in the inner loop must trip the reject,
     * break out of the loop, and leave moved_pages unchanged. */
    g_always_reject_calls = 0;
    gc_attach_admit_cb(&ftl->gc, always_reject_admit, NULL);

    int gc_ret = gc_run_mt(&ftl->gc, &ftl->block_mgr, taa, ftl->hal);
    printf("  gc_run_mt (rejecting admit) returned: %d\n", gc_ret);
    printf("  admit callback invocations: %d\n", g_always_reject_calls);

    u64 moved_after = ftl->gc.moved_pages;
    u64 rejects_after = ftl->gc.det_window_rejects;

    TEST_ASSERT(g_always_reject_calls >= 1,
                "admit callback was consulted at least once per GC run");
    TEST_ASSERT(moved_after == moved_before,
                "no page moves recorded while admit callback rejects");
    TEST_ASSERT(rejects_after == rejects_before + 1,
                "det_window_rejects advanced by one per rejected GC run");

    /* Detach cleanly — flipping off the gate must reset both
     * callback fields to NULL so future GC calls take the default
     * path without dereferencing stale function/context pointers. */
    gc_attach_admit_cb(&ftl->gc, NULL, NULL);
    TEST_ASSERT(ftl->gc.admit_gc_fn  == NULL,
                "gc_attach_admit_cb(NULL, NULL) clears admit_gc_fn");
    TEST_ASSERT(ftl->gc.admit_gc_ctx == NULL,
                "gc_attach_admit_cb(NULL, NULL) clears admit_gc_ctx");

    teardown(&env);
}

int main(void)
{
    printf("========================================\n");
    printf("GC MT (TAA-aware) Tests\n");
    printf("========================================\n");

    test_gc_mt_reclaims_blocks();
    test_gc_mt_respects_admit_callback_reject();

    printf("\n========================================\n");
    printf("Results: %d/%d passed, %d failed\n", passed, total_tests, failed);
    printf("========================================\n");
    return failed > 0 ? 1 : 0;
}
