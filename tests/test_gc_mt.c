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

int main(void)
{
    printf("========================================\n");
    printf("GC MT (TAA-aware) Tests\n");
    printf("========================================\n");

    test_gc_mt_reclaims_blocks();

    printf("\n========================================\n");
    printf("Results: %d/%d passed, %d failed\n", passed, total_tests, failed);
    printf("========================================\n");
    return failed > 0 ? 1 : 0;
}
