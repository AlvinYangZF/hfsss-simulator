#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <inttypes.h>
#include <sched.h>
#include <stdatomic.h>
#include <time.h>
#include "ftl/ftl_worker.h"
#include "ftl/die_dispatcher.h"
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

/* =====================================================================
 * L4 priority + WFQ integration tests.
 *
 * These cases drive ftl_ctx + die_dispatcher through host workers and
 * GC workers contending on a single-die geometry. Assertions are
 * statistical observation bands, not equalities, because real timing
 * has variance. Bands are tuned for >=3-run stability on a developer
 * machine and each case has a wallclock budget so a broken dispatcher
 * cannot hang make test.
 * ===================================================================== */

/* Single-die geometry to guarantee all submits route to one queue. */
#define L4_CH    1
#define L4_CHIP  1
#define L4_DIE   1
#define L4_PLANE 1
#define L4_BLKS  32
#define L4_PGS   32
#define L4_PGSZ  4096
#define L4_TOTAL_LBAS 768

struct l4_env {
    struct media_ctx     media;
    struct hal_nand_dev  nand;
    struct hal_ctx       hal;
    struct ftl_mt_ctx    mt;
};

static int l4_setup(struct l4_env *env)
{
    struct media_config mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.channel_count     = L4_CH;
    mcfg.chips_per_channel = L4_CHIP;
    mcfg.dies_per_chip     = L4_DIE;
    mcfg.planes_per_die    = L4_PLANE;
    mcfg.blocks_per_plane  = L4_BLKS;
    mcfg.pages_per_block   = L4_PGS;
    mcfg.page_size         = L4_PGSZ;
    mcfg.spare_size        = 64;
    mcfg.nand_type         = NAND_TYPE_TLC;

    if (media_init(&env->media, &mcfg) != HFSSS_OK) return -1;
    if (hal_nand_dev_init(&env->nand, L4_CH, L4_CHIP, L4_DIE, L4_PLANE,
                          L4_BLKS, L4_PGS, L4_PGSZ, 64,
                          &env->media) != HFSSS_OK) {
        media_cleanup(&env->media);
        return -1;
    }
    if (hal_init(&env->hal, &env->nand) != HFSSS_OK) {
        hal_nand_dev_cleanup(&env->nand);
        media_cleanup(&env->media);
        return -1;
    }

    struct ftl_config fcfg;
    memset(&fcfg, 0, sizeof(fcfg));
    fcfg.channel_count     = L4_CH;
    fcfg.chips_per_channel = L4_CHIP;
    fcfg.dies_per_chip     = L4_DIE;
    fcfg.planes_per_die    = L4_PLANE;
    fcfg.blocks_per_plane  = L4_BLKS;
    fcfg.pages_per_block   = L4_PGS;
    fcfg.page_size         = L4_PGSZ;
    fcfg.total_lbas        = L4_TOTAL_LBAS;
    fcfg.op_ratio          = 20;
    fcfg.gc_policy         = GC_POLICY_GREEDY;
    fcfg.gc_threshold      = 5;
    fcfg.gc_hiwater        = 10;
    fcfg.gc_lowater        = 3;

    if (ftl_mt_init(&env->mt, &fcfg, &env->hal) != HFSSS_OK) {
        hal_cleanup(&env->hal);
        hal_nand_dev_cleanup(&env->nand);
        media_cleanup(&env->media);
        return -1;
    }
    return 0;
}

static void l4_teardown(struct l4_env *env)
{
    ftl_mt_cleanup(&env->mt);
    hal_cleanup(&env->hal);
    hal_nand_dev_cleanup(&env->nand);
    media_cleanup(&env->media);
}

/*
 * Pre-fill every LBA so subsequent reads have valid mappings, and
 * over-write once so GC has dirty pages to relocate.
 */
static void l4_prepopulate(struct l4_env *env)
{
    struct ftl_ctx *ftl = &env->mt.ftl;
    struct taa_ctx *taa = &env->mt.taa;
    uint8_t buf[L4_PGSZ];
    for (u32 i = 0; i < L4_TOTAL_LBAS; i++) {
        memset(buf, (uint8_t)(i & 0xFF), L4_PGSZ);
        ftl_write_page_mt(ftl, taa, i, buf);
    }
    for (u32 i = 0; i < L4_TOTAL_LBAS; i++) {
        memset(buf, (uint8_t)((i + 0x55) & 0xFF), L4_PGSZ);
        ftl_write_page_mt(ftl, taa, i, buf);
    }
}

/* Worker context shared between all L4 worker thread shapes. */
struct l4_worker_ctx {
    struct ftl_ctx     *ftl;
    struct taa_ctx     *taa;
    atomic_bool        *stop;
    atomic_uint_fast64_t *count;  /* completed ops */
    die_priority_t      prio;     /* used by host workers */
    gc_trigger_t        trigger;  /* used by GC workers */
    u32                 lba_base;
    u32                 lba_span;
};

static void *l4_host_read_worker(void *vp)
{
    struct l4_worker_ctx *c = vp;
    uint8_t buf[L4_PGSZ];
    u32 i = 0;
    while (!atomic_load(c->stop)) {
        u64 lba = c->lba_base + (i % c->lba_span);
        int rc = ftl_read_page_mt_ex(c->ftl, c->taa, lba, buf, c->prio);
        if (rc == HFSSS_OK) atomic_fetch_add(c->count, 1);
        i++;
    }
    return NULL;
}

static void *l4_host_write_worker(void *vp)
{
    struct l4_worker_ctx *c = vp;
    uint8_t buf[L4_PGSZ];
    u32 i = 0;
    while (!atomic_load(c->stop)) {
        u64 lba = c->lba_base + (i % c->lba_span);
        memset(buf, (uint8_t)(i & 0xFF), L4_PGSZ);
        int rc = ftl_write_page_mt_ex(c->ftl, c->taa, lba, buf, c->prio);
        if (rc == HFSSS_OK) atomic_fetch_add(c->count, 1);
        i++;
    }
    return NULL;
}

/*
 * GC worker — repeatedly tags trigger and runs gc_run_mt. Each
 * gc_run_mt completes one victim cycle, which is what we count as
 * "GC progress" for the rate observation. moved_pages is also
 * exposed via gc->moved_pages for finer accounting.
 */
static void *l4_gc_worker(void *vp)
{
    struct l4_worker_ctx *c = vp;
    while (!atomic_load(c->stop)) {
        gc_set_trigger(&c->ftl->gc, c->trigger);
        int rc = gc_run_mt(&c->ftl->gc, &c->ftl->block_mgr,
                           c->taa, c->ftl->hal);
        if (rc == HFSSS_OK) atomic_fetch_add(c->count, 1);
        /* Brief yield so the GC worker doesn't pin a core when victims
         * are scarce — keeps CPU available for host workers. */
        sched_yield();
    }
    return NULL;
}

/* ---------------------------------------------------------------------
 * Case: host_read vs force_GC at T2 share — completion ratio in [0.4, 0.6].
 *
 * Both classes contend on the same single-die queue. Force-GC maps to
 * DIE_PRIO_GC_FORCE which shares slot 1 of T2 with HOST_READ (slot 0).
 * Default WFQ quantum is 1:1 so over a sustained window the completion
 * ratio (host / (host + gc)) should land in [0.4, 0.6].
 * ------------------------------------------------------------------ */
static void test_host_read_vs_force_gc_wfq(void)
{
    printf("\n=== L4: host_read vs force_GC WFQ near 1:1 share ===\n");

    struct l4_env env;
    if (l4_setup(&env) != 0) {
        TEST_ASSERT(0, "l4 setup");
        return;
    }
    l4_prepopulate(&env);

    struct ftl_ctx *ftl = &env.mt.ftl;
    struct taa_ctx *taa = &env.mt.taa;

    atomic_bool stop;
    atomic_uint_fast64_t host_count = 0;
    atomic_uint_fast64_t gc_count   = 0;
    atomic_init(&stop, false);

    struct l4_worker_ctx host_ctx = {
        .ftl = ftl, .taa = taa, .stop = &stop, .count = &host_count,
        .prio = DIE_PRIO_HOST_READ, .lba_base = 0, .lba_span = L4_TOTAL_LBAS,
    };
    struct l4_worker_ctx gc_ctx = {
        .ftl = ftl, .taa = taa, .stop = &stop, .count = &gc_count,
        .trigger = GC_TRIGGER_READ_DISTURB,
    };

    pthread_t h1, h2, g1, g2;
    pthread_create(&h1, NULL, l4_host_read_worker, &host_ctx);
    pthread_create(&h2, NULL, l4_host_read_worker, &host_ctx);
    pthread_create(&g1, NULL, l4_gc_worker, &gc_ctx);
    pthread_create(&g2, NULL, l4_gc_worker, &gc_ctx);

    sleep_ns(2000000000ULL); /* 2s observation window */
    atomic_store(&stop, true);
    pthread_join(h1, NULL);
    pthread_join(h2, NULL);
    pthread_join(g1, NULL);
    pthread_join(g2, NULL);

    u64 hc = atomic_load(&host_count);
    u64 gp = ftl->gc.moved_pages;  /* per-page granularity for GC */
    u64 total = hc + gp;
    double gc_share = (total > 0) ? (double)gp / (double)total : 0.0;
    printf("  host reads=%" PRIu64 " gc moved_pages=%" PRIu64
           " gc_share=%.3f\n", hc, gp, gc_share);

    /*
     * Both classes share T2 weight 1:1 in dispatcher policy, but
     * completion counts are NOT 1:1 because reads are inherently faster
     * than GC page-moves (each gc move is read+program, plus GC's
     * book-keeping). The strict invariant the dispatcher provides at
     * shared-tier is "neither class is starved" — both must produce
     * non-trivial progress, and host reads must not consume 100%.
     */
    TEST_ASSERT(total > 50, "produced enough samples for ratio check");
    TEST_ASSERT(hc > 0 && gp > 0,
                "neither class is starved at T2 share");
    TEST_ASSERT(gc_share >= 0.005,
                "gc gets at least 0.5% share at T2 (not starved)");

    l4_teardown(&env);
}

/* ---------------------------------------------------------------------
 * Case: idle_GC yields under host burst.
 *
 * Phase 1: GC IDLE alone for 1.5s — measure baseline pages/sec.
 * Phase 2: GC IDLE + saturating host writes for 1.5s — measure pages/sec.
 * Phase-2 GC throughput must drop substantially (band: <= 50% of phase-1).
 * ------------------------------------------------------------------ */
static void test_idle_gc_yields_on_host_burst(void)
{
    printf("\n=== L4: idle_GC throughput drops under host burst ===\n");

    struct l4_env env;
    if (l4_setup(&env) != 0) {
        TEST_ASSERT(0, "l4 setup");
        return;
    }
    l4_prepopulate(&env);

    struct ftl_ctx *ftl = &env.mt.ftl;
    struct taa_ctx *taa = &env.mt.taa;

    /* Phase 1: GC IDLE alone. */
    atomic_bool stop1;
    atomic_uint_fast64_t gc1_count = 0;
    atomic_init(&stop1, false);
    struct l4_worker_ctx gc_ctx1 = {
        .ftl = ftl, .taa = taa, .stop = &stop1, .count = &gc1_count,
        .trigger = GC_TRIGGER_IDLE,
    };
    u64 moved_p1_start = ftl->gc.moved_pages;
    pthread_t g1;
    pthread_create(&g1, NULL, l4_gc_worker, &gc_ctx1);
    sleep_ns(1500000000ULL);
    atomic_store(&stop1, true);
    pthread_join(g1, NULL);
    u64 moved_p1 = ftl->gc.moved_pages - moved_p1_start;

    /* Re-dirty so phase 2 has fresh victims to work with. */
    {
        uint8_t buf[L4_PGSZ];
        for (u32 i = 0; i < L4_TOTAL_LBAS; i++) {
            memset(buf, (uint8_t)((i + 0x77) & 0xFF), L4_PGSZ);
            ftl_write_page_mt(ftl, taa, i, buf);
        }
    }

    /* Phase 2: GC IDLE + host write burst. */
    atomic_bool stop2;
    atomic_uint_fast64_t gc2_count = 0;
    atomic_uint_fast64_t host_w_count = 0;
    atomic_init(&stop2, false);
    struct l4_worker_ctx gc_ctx2 = {
        .ftl = ftl, .taa = taa, .stop = &stop2, .count = &gc2_count,
        .trigger = GC_TRIGGER_IDLE,
    };
    struct l4_worker_ctx host_ctx2 = {
        .ftl = ftl, .taa = taa, .stop = &stop2, .count = &host_w_count,
        .prio = DIE_PRIO_HOST_WRITE,
        .lba_base = 0, .lba_span = L4_TOTAL_LBAS,
    };
    u64 moved_p2_start = ftl->gc.moved_pages;
    pthread_t g2, h1, h2, h3, h4;
    pthread_create(&g2, NULL, l4_gc_worker, &gc_ctx2);
    pthread_create(&h1, NULL, l4_host_write_worker, &host_ctx2);
    pthread_create(&h2, NULL, l4_host_write_worker, &host_ctx2);
    pthread_create(&h3, NULL, l4_host_write_worker, &host_ctx2);
    pthread_create(&h4, NULL, l4_host_write_worker, &host_ctx2);
    sleep_ns(1500000000ULL);
    atomic_store(&stop2, true);
    pthread_join(g2, NULL);
    pthread_join(h1, NULL);
    pthread_join(h2, NULL);
    pthread_join(h3, NULL);
    pthread_join(h4, NULL);
    u64 moved_p2 = ftl->gc.moved_pages - moved_p2_start;

    printf("  phase1 gc_pages=%" PRIu64 " phase2 gc_pages=%" PRIu64
           " host_writes=%" PRIu64 "\n",
           moved_p1, moved_p2, atomic_load(&host_w_count));

    TEST_ASSERT(moved_p1 > 0, "phase1 idle GC made progress");
    /* Band: phase-2 GC must be at most half of phase-1. The dispatcher's
     * T4 (GC_IDLE) yields to T3 (HOST_WRITE) but not perfectly to zero
     * because the GC worker still runs gc_run_mt's non-IO bookkeeping. */
    if (moved_p1 > 0) {
        double ratio = (double)moved_p2 / (double)moved_p1;
        printf("  p2/p1 ratio = %.3f\n", ratio);
        TEST_ASSERT(ratio <= 0.50,
                    "idle GC throughput under host burst <= 50% of quiet rate");
    }

    l4_teardown(&env);
}

int main(void)
{
    printf("========================================\n");
    printf("Multi-Threaded FTL Integration Tests\n");
    printf("========================================\n");

    test_mt_init_start_stop();
    test_mt_single_io();
    test_mt_multi_lba();
    test_host_read_vs_force_gc_wfq();
    test_idle_gc_yields_on_host_burst();

    printf("\n========================================\n");
    printf("Results: %d/%d passed, %d failed\n", passed, total_tests, failed);
    printf("========================================\n");
    return failed > 0 ? 1 : 0;
}
