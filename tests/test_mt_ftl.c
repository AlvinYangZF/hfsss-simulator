#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <inttypes.h>
#include <sched.h>
#include "ftl/ftl_worker.h"
#include "media/media.h"
#include "hal/hal.h"

static int total_tests = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    total_tests++; \
    if (cond) { printf("  [PASS] %s\n", msg); passed++; } \
    else      { printf("  [FAIL] %s\n", msg); failed++; } \
} while (0)

/* Small geometry for fast testing:
 * 4ch * 2chip * 1die * 1plane * 64blk * 64pg * 4096B
 * = 32768 pages = 128 MB raw */
#define CH    4
#define CHIP  2
#define DIE   1
#define PLANE 1
#define BLKS  64
#define PGS   64
#define PGSZ  4096
#define TOTAL_LBAS 4096

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

static void test_mt_init_start_stop(void)
{
    printf("\n=== MT FTL Init/Start/Stop ===\n");

    struct test_env env;
    memset(&env, 0, sizeof(env));

    int ret = setup(&env);
    TEST_ASSERT(ret == HFSSS_OK, "setup succeeds");
    TEST_ASSERT(env.mt.initialized, "mt.initialized is true");

    ret = ftl_mt_start(&env.mt);
    TEST_ASSERT(ret == HFSSS_OK, "ftl_mt_start succeeds");

    usleep(10000);

    ftl_mt_stop(&env.mt);
    TEST_ASSERT(1, "ftl_mt_stop completes without hang");

    teardown(&env);
}

static void test_mt_single_io(void)
{
    printf("\n=== MT FTL Single I/O ===\n");

    struct test_env env;
    memset(&env, 0, sizeof(env));
    setup(&env);
    ftl_mt_start(&env.mt);

    uint8_t wbuf[PGSZ], rbuf[PGSZ];
    memset(wbuf, 0xAB, PGSZ);

    /* Write LBA 0 */
    struct io_request wreq;
    memset(&wreq, 0, sizeof(wreq));
    wreq.opcode = IO_OP_WRITE;
    wreq.lba = 0;
    wreq.count = 1;
    wreq.data = wbuf;
    wreq.nbd_handle = 1;

    while (!ftl_mt_submit(&env.mt, &wreq)) { sched_yield(); }

    struct io_completion cpl;
    while (!ftl_mt_poll_completion(&env.mt, &cpl)) { sched_yield(); }
    TEST_ASSERT(cpl.status == HFSSS_OK, "write LBA 0 succeeds");
    TEST_ASSERT(cpl.nbd_handle == 1, "handle echoed correctly");

    /* Read LBA 0 */
    memset(rbuf, 0, PGSZ);
    struct io_request rreq;
    memset(&rreq, 0, sizeof(rreq));
    rreq.opcode = IO_OP_READ;
    rreq.lba = 0;
    rreq.count = 1;
    rreq.data = rbuf;
    rreq.nbd_handle = 2;

    while (!ftl_mt_submit(&env.mt, &rreq)) { sched_yield(); }
    while (!ftl_mt_poll_completion(&env.mt, &cpl)) { sched_yield(); }
    TEST_ASSERT(cpl.status == HFSSS_OK, "read LBA 0 succeeds");
    TEST_ASSERT(memcmp(wbuf, rbuf, PGSZ) == 0, "data verify LBA 0");

    ftl_mt_stop(&env.mt);
    teardown(&env);
}

static void test_mt_multi_lba(void)
{
    printf("\n=== MT FTL Multi-LBA Write+Verify ===\n");

    struct test_env env;
    memset(&env, 0, sizeof(env));
    setup(&env);
    ftl_mt_start(&env.mt);

    uint8_t wbuf[PGSZ], rbuf[PGSZ];
    int errors = 0;
    uint32_t num_lbas = 100;

    /* Write 100 LBAs with unique patterns */
    for (uint32_t i = 0; i < num_lbas; i++) {
        memset(wbuf, (uint8_t)(i & 0xFF), PGSZ);
        wbuf[0] = (uint8_t)((i >> 8) & 0xFF);

        struct io_request req;
        memset(&req, 0, sizeof(req));
        req.opcode = IO_OP_WRITE;
        req.lba = i;
        req.count = 1;
        req.data = wbuf;
        req.nbd_handle = i;

        while (!ftl_mt_submit(&env.mt, &req)) { sched_yield(); }
        struct io_completion cpl;
        while (!ftl_mt_poll_completion(&env.mt, &cpl)) { sched_yield(); }
        if (cpl.status != HFSSS_OK) errors++;
    }
    TEST_ASSERT(errors == 0, "100 writes all succeed");

    /* Read back and verify */
    int verify_errors = 0;
    for (uint32_t i = 0; i < num_lbas; i++) {
        uint8_t expected[PGSZ];
        memset(expected, (uint8_t)(i & 0xFF), PGSZ);
        expected[0] = (uint8_t)((i >> 8) & 0xFF);

        memset(rbuf, 0, PGSZ);
        struct io_request req;
        memset(&req, 0, sizeof(req));
        req.opcode = IO_OP_READ;
        req.lba = i;
        req.count = 1;
        req.data = rbuf;
        req.nbd_handle = i + 1000;

        while (!ftl_mt_submit(&env.mt, &req)) { sched_yield(); }
        struct io_completion cpl;
        while (!ftl_mt_poll_completion(&env.mt, &cpl)) { sched_yield(); }
        if (cpl.status != HFSSS_OK || memcmp(expected, rbuf, PGSZ) != 0) {
            verify_errors++;
        }
    }
    TEST_ASSERT(verify_errors == 0, "100 read-back verifications pass");

    u64 taa_lookups, taa_conflicts;
    taa_get_stats(&env.mt.taa, &taa_lookups, &taa_conflicts);
    printf("  TAA: %" PRIu64 " lookups, %" PRIu64 " conflicts\n",
           taa_lookups, taa_conflicts);

    ftl_mt_stop(&env.mt);
    teardown(&env);
}

int main(void)
{
    printf("========================================\n");
    printf("Multi-Threaded FTL Integration Tests\n");
    printf("========================================\n");

    test_mt_init_start_stop();
    test_mt_single_io();
    test_mt_multi_lba();

    printf("\n========================================\n");
    printf("Results: %d/%d passed, %d failed\n", passed, total_tests, failed);
    printf("========================================\n");
    return failed > 0 ? 1 : 0;
}
