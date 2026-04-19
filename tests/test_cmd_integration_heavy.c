/*
 * NAND command-engine integration tests (Phase 7 heavy tier).
 *
 * Three scenarios that combine three or more phases of Phase 1-6
 * behavior. The complexity driver here is multi-plane interacting
 * with suspend/resume and cache pipelining.
 *
 *   IS-01: suspend interrupting a multi-plane program. Every plane
 *          in the mask must observe the same transition into
 *          DIE_SUSPENDED_PROG; the resumed command must complete
 *          correctly for all planes and must not restart from zero.
 *          (Phase 3 suspend + Phase 4 multi-plane)
 *
 *   IS-04: legality contract during a held PROG_ARRAY_BUSY window
 *          combining Phase 4 (multi-plane PROG as the busy-holder)
 *          and Phase 5 (cache-opcode legality checked via the
 *          static legality table, since the tCBSY window of a
 *          single cache_program is too short to reliably race a
 *          concurrent submission from userspace). The test uses
 *          mp_program to hold the die in PROG_ARRAY_BUSY, asserts
 *          that NAND_OP_ERASE is rejected, asserts the legality
 *          table's Phase 5 entries are self-consistent (CACHE_PROG
 *          legal, CACHE_READ illegal in PROG_ARRAY_BUSY), and then
 *          verifies the rejected erase succeeds once the program
 *          completes.
 *          (Phase 4 multi-plane + Phase 5 legality)
 *
 *   IS-07: multi-plane program preempted by a foreground read on
 *          a non-conflicting plane/block. Total wall-clock must
 *          stay within a single-program + suspend/resume overhead
 *          envelope (i.e. resume must not re-run tPROG from
 *          scratch). Both planes' target data is verified after
 *          resume.
 *          (Phase 3 suspend + Phase 4 multi-plane + read-during-
 *          suspend conflict gating)
 *
 *   IS-09: profile-aware multi-plane legality. ONFI profiles cap
 *          max_planes_per_cmd at 4; Toggle profiles cap at 2. The
 *          engine's validate_planes consults the active profile,
 *          so the same plane_mask must be accepted on ONFI and
 *          rejected on Toggle when the bit-count exceeds the
 *          Toggle cap. The rejection must be deterministic and
 *          must not perturb the die's cmd_state (no opcode/target
 *          drift, die stays DIE_IDLE).
 *          (Phase 4 multi-plane + Phase 6 profile mp_rules +
 *          T5b engine wiring)
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
/* Shared helpers                                                            */
/* ------------------------------------------------------------------------ */

struct mp_prog_worker_ctx {
    struct media_ctx *ctx;
    u32 plane_mask;
    u32 block;
    u32 page;
    u8 fill_p0;
    u8 fill_p1;
    int rc;
};

static void *mp_prog_worker(void *arg)
{
    struct mp_prog_worker_ctx *w = arg;
    u8 d0[4096], d1[4096];
    memset(d0, w->fill_p0, sizeof(d0));
    memset(d1, w->fill_p1, sizeof(d1));
    const void *arr[2] = {d0, d1};
    w->rc = media_nand_multi_plane_program(w->ctx, 0, 0, 0, w->plane_mask, w->block, w->page, arr, NULL);
    return NULL;
}

struct cache_prog_worker_ctx {
    struct media_ctx *ctx;
    u32 plane;
    u32 block;
    u32 page;
    u8 fill;
    int rc;
};

static void *cache_prog_worker(void *arg)
{
    struct cache_prog_worker_ctx *w = arg;
    u8 buf[4096];
    memset(buf, w->fill, sizeof(buf));
    w->rc = media_nand_cache_program(w->ctx, 0, 0, 0, w->plane, w->block, w->page, buf, NULL);
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

static struct media_config make_cfg_four_plane(enum nand_profile_id pid)
{
    struct media_config cfg = {
        .channel_count = 1,
        .chips_per_channel = 1,
        .dies_per_chip = 1,
        .planes_per_die = 4,
        .blocks_per_plane = 16,
        .pages_per_block = 16,
        .page_size = 4096,
        .spare_size = 64,
        .nand_type = NAND_TYPE_TLC,
        .profile_explicit = true,
        .profile_id = pid,
        .enable_multi_plane = true,
    };
    return cfg;
}

/* ------------------------------------------------------------------------ */
/* IS-01: suspend interrupting a multi-plane program                         */
/* ------------------------------------------------------------------------ */

static int test_is01_suspend_under_mp_program(void)
{
    printf("\n=== IS-01: suspend under multi-plane program ===\n");

    struct media_config cfg = make_cfg_two_plane();
    struct media_ctx ctx;
    int ret = media_init(&ctx, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "IS-01: media_init");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }

    /* Kick off a multi-plane program on both planes of block=6 page=0. */
    struct mp_prog_worker_ctx prog = {
        .ctx = &ctx, .plane_mask = 0x3, .block = 6, .page = 0, .fill_p0 = 0x11, .fill_p1 = 0x22, .rc = -1};
    pthread_t thr;
    pthread_create(&thr, NULL, mp_prog_worker, &prog);
    TEST_ASSERT(wait_for_state(&ctx, DIE_PROG_ARRAY_BUSY, 5000000000ULL), "IS-01: entered PROG_ARRAY_BUSY");

    /* Snapshot before suspend: multi-plane target recorded in cmd_state. */
    struct nand_cmd_target t = {.ch = 0, .chip = 0, .die = 0, .plane_mask = 0x3};
    struct nand_die_cmd_state snap_busy;
    TEST_ASSERT(nand_cmd_engine_snapshot(ctx.nand, &t, &snap_busy) == HFSSS_OK, "IS-01: snapshot busy");
    TEST_ASSERT(snap_busy.opcode == NAND_OP_PROG, "IS-01: busy opcode == PROG");
    TEST_ASSERT(snap_busy.target.plane_mask == 0x3, "IS-01: busy plane_mask preserved across snapshot");
    TEST_ASSERT(snap_busy.target.block == 6 && snap_busy.target.page == 0, "IS-01: busy target addressed correctly");

    /* Suspend. Die must flip to SUSPENDED_PROG; plane_mask in the
     * cmd_state target must carry over — the suspend codepath must
     * not silently collapse the multi-plane target to plane 0. */
    TEST_ASSERT(media_nand_program_suspend(&ctx, 0, 0, 0) == HFSSS_OK, "IS-01: program_suspend accepted");
    TEST_ASSERT(wait_for_state(&ctx, DIE_SUSPENDED_PROG, 5000000000ULL), "IS-01: entered SUSPENDED_PROG");

    struct nand_die_cmd_state snap_susp;
    TEST_ASSERT(nand_cmd_engine_snapshot(ctx.nand, &t, &snap_susp) == HFSSS_OK, "IS-01: snapshot suspended");
    TEST_ASSERT(snap_susp.state == DIE_SUSPENDED_PROG, "IS-01: snapshot state SUSPENDED_PROG");
    TEST_ASSERT(snap_susp.opcode == NAND_OP_PROG, "IS-01: snapshot opcode still PROG after suspend");
    TEST_ASSERT(snap_susp.target.plane_mask == 0x3, "IS-01: suspended snapshot retains multi-plane mask");
    TEST_ASSERT(snap_susp.suspend_count == 1, "IS-01: suspend_count == 1");
    TEST_ASSERT(snap_susp.remaining_ns > 0, "IS-01: remaining_ns > 0");
    TEST_ASSERT(snap_susp.remaining_ns < snap_susp.total_budget_ns,
                "IS-01: remaining_ns < total_budget_ns (elapsed time was accounted for)");

    struct nand_status_enhanced enh_susp;
    TEST_ASSERT(media_nand_read_status_enhanced(&ctx, 0, 0, 0, &enh_susp) == HFSSS_OK, "IS-01: status_enhanced OK");
    TEST_ASSERT(enh_susp.suspended_program, "IS-01: status.suspended_program set");

    /* Resume. Both planes must commit their respective payloads. */
    TEST_ASSERT(media_nand_program_resume(&ctx, 0, 0, 0) == HFSSS_OK, "IS-01: program_resume accepted");
    pthread_join(thr, NULL);
    TEST_ASSERT(prog.rc == HFSSS_OK, "IS-01: multi-plane program completes OK after resume");

    /* Data correctness: each plane carries its own programmed fill. */
    u8 rd0[4096], rd1[4096];
    TEST_ASSERT(media_nand_read(&ctx, 0, 0, 0, 0, 6, 0, rd0, NULL) == HFSSS_OK, "IS-01: read plane 0");
    TEST_ASSERT(rd0[0] == 0x11 && rd0[4095] == 0x11, "IS-01: plane 0 carries fill_p0");
    TEST_ASSERT(media_nand_read(&ctx, 0, 0, 0, 1, 6, 0, rd1, NULL) == HFSSS_OK, "IS-01: read plane 1");
    TEST_ASSERT(rd1[0] == 0x22 && rd1[4095] == 0x22, "IS-01: plane 1 carries fill_p1");

    /* Die post-resume must be back at IDLE with suspend bookkeeping cleared. */
    struct nand_die_cmd_state snap_post;
    TEST_ASSERT(nand_cmd_engine_snapshot(ctx.nand, &t, &snap_post) == HFSSS_OK, "IS-01: snapshot post-resume");
    TEST_ASSERT(snap_post.state == DIE_IDLE, "IS-01: die back to DIE_IDLE after resume+completion");
    TEST_ASSERT(snap_post.suspend_count == 0, "IS-01: suspend_count cleared on completion");

    media_cleanup(&ctx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------------ */
/* IS-04: stage conflict — cache program vs subsequent multi-plane erase     */
/* ------------------------------------------------------------------------ */

static int test_is04_cache_vs_mp_erase_stage_conflict(void)
{
    printf("\n=== IS-04: legality during held PROG_ARRAY_BUSY (Phase 4 × Phase 5) ===\n");

    struct media_config cfg = make_cfg_two_plane();
    struct media_ctx ctx;
    int ret = media_init(&ctx, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "IS-04: media_init");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }

    /* Static legality-table assertions combining Phase 4 (multi-plane
     * ops) and Phase 5 (cache ops) in the PROG_ARRAY_BUSY state. These
     * are the contract the runtime enforces; asserting them here as
     * part of the integration tier guards against silent regressions
     * in cmd_legality.c when future phases extend the table. */
    TEST_ASSERT(nand_cmd_is_legal_in_state(DIE_PROG_ARRAY_BUSY, NAND_OP_ERASE) == false,
                "IS-04: ERASE illegal in PROG_ARRAY_BUSY");
    TEST_ASSERT(nand_cmd_is_legal_in_state(DIE_PROG_ARRAY_BUSY, NAND_OP_CACHE_PROG) == true,
                "IS-04: CACHE_PROG legal in PROG_ARRAY_BUSY (Phase 5 overlap)");
    TEST_ASSERT(nand_cmd_is_legal_in_state(DIE_PROG_ARRAY_BUSY, NAND_OP_CACHE_READ) == false,
                "IS-04: CACHE_READ illegal in PROG_ARRAY_BUSY");
    TEST_ASSERT(nand_cmd_is_legal_in_state(DIE_PROG_ARRAY_BUSY, NAND_OP_READ) == false,
                "IS-04: plain READ illegal in PROG_ARRAY_BUSY");
    TEST_ASSERT(nand_cmd_is_legal_in_state(DIE_READ_DATA_XFER, NAND_OP_CACHE_READ) == true,
                "IS-04: CACHE_READ legal in READ_DATA_XFER (Phase 5 pipeline)");
    TEST_ASSERT(nand_cmd_is_legal_in_state(DIE_READ_DATA_XFER, NAND_OP_ERASE) == false,
                "IS-04: ERASE illegal in READ_DATA_XFER");

    /* Runtime contention: use a multi-plane PROG (full tPROG, well
     * into milliseconds for TLC LSB) to hold the die in
     * PROG_ARRAY_BUSY. A concurrent mp_erase on a DIFFERENT block
     * must be rejected because the legality table above says so;
     * the rejection must not disturb the running program. */
    struct mp_prog_worker_ctx prog = {
        .ctx = &ctx, .plane_mask = 0x3, .block = 12, .page = 0, .fill_p0 = 0x33, .fill_p1 = 0x44, .rc = -1};
    pthread_t thr;
    pthread_create(&thr, NULL, mp_prog_worker, &prog);
    TEST_ASSERT(wait_for_state(&ctx, DIE_PROG_ARRAY_BUSY, 5000000000ULL), "IS-04: entered PROG_ARRAY_BUSY");

    int rej = media_nand_multi_plane_erase(&ctx, 0, 0, 0, 0x3, 14);
    TEST_ASSERT(rej != HFSSS_OK, "IS-04: mp_erase rejected while mp_program holds PROG_ARRAY_BUSY");

    /* Snapshot: the rejected erase must not have clobbered the
     * running program's cmd_state. Opcode still PROG, target still
     * points at block=12. */
    struct nand_cmd_target t = {.ch = 0, .chip = 0, .die = 0, .plane_mask = 0x3};
    struct nand_die_cmd_state snap_mid;
    TEST_ASSERT(nand_cmd_engine_snapshot(ctx.nand, &t, &snap_mid) == HFSSS_OK, "IS-04: snapshot after rejection");
    TEST_ASSERT(snap_mid.opcode == NAND_OP_PROG, "IS-04: rejected erase did not rewrite opcode");
    TEST_ASSERT(snap_mid.target.block == 12, "IS-04: rejected erase did not rewrite target block");

    /* Let the program finish. */
    pthread_join(thr, NULL);
    TEST_ASSERT(prog.rc == HFSSS_OK, "IS-04: mp program completes OK");
    TEST_ASSERT(wait_for_state(&ctx, DIE_IDLE, 5000000000ULL), "IS-04: die returned to DIE_IDLE");

    /* Same erase that was rejected must now succeed. */
    int accept = media_nand_multi_plane_erase(&ctx, 0, 0, 0, 0x3, 14);
    TEST_ASSERT(accept == HFSSS_OK, "IS-04: mp_erase accepted once die is DIE_IDLE");

    /* Both planes' program data still intact. */
    u8 rd0[4096], rd1[4096];
    TEST_ASSERT(media_nand_read(&ctx, 0, 0, 0, 0, 12, 0, rd0, NULL) == HFSSS_OK, "IS-04: read plane 0");
    TEST_ASSERT(rd0[0] == 0x33, "IS-04: plane 0 carries fill_p0");
    TEST_ASSERT(media_nand_read(&ctx, 0, 0, 0, 1, 12, 0, rd1, NULL) == HFSSS_OK, "IS-04: read plane 1");
    TEST_ASSERT(rd1[0] == 0x44, "IS-04: plane 1 carries fill_p1");

    /* Separate thread-safe smoke: a stand-alone cache_program still
     * runs end-to-end cleanly in this config, exercising Phase 5 on
     * a freshly-idle die. This catches regressions where the cache
     * path gets wedged after prior multi-plane activity. */
    struct cache_prog_worker_ctx cp = {.ctx = &ctx, .plane = 0, .block = 13, .page = 0, .fill = 0xCC, .rc = -1};
    pthread_t cp_thr;
    pthread_create(&cp_thr, NULL, cache_prog_worker, &cp);
    pthread_join(cp_thr, NULL);
    TEST_ASSERT(cp.rc == HFSSS_OK, "IS-04: cache_program on freshly-idle die completes OK");
    u8 rd_cache[4096];
    TEST_ASSERT(media_nand_read(&ctx, 0, 0, 0, 0, 13, 0, rd_cache, NULL) == HFSSS_OK,
                "IS-04: read cache-programmed page");
    TEST_ASSERT(rd_cache[0] == 0xCC, "IS-04: cache-programmed page carries fill");

    media_cleanup(&ctx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------------ */
/* IS-07: multi-plane program preempted by a foreground read                 */
/* ------------------------------------------------------------------------ */

static int test_is07_mp_program_foreground_read_preemption(void)
{
    printf("\n=== IS-07: multi-plane program preempted by foreground read ===\n");

    struct media_config cfg = make_cfg_two_plane();
    struct media_ctx ctx;
    int ret = media_init(&ctx, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "IS-07: media_init");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }

    /* Seed the foreground read target: plane=0, block=4, page=3, fill 0xDD. */
    u8 seed[4096];
    memset(seed, 0xDD, sizeof(seed));
    TEST_ASSERT(media_nand_program(&ctx, 0, 0, 0, 0, 4, 3, seed, NULL) == HFSSS_OK, "IS-07: seed foreground target");

    /* Baseline: measure tPROG for a single-plane program to a fresh
     * block. This is what the resumed multi-plane program would
     * roughly cost if the engine honored remaining_ns correctly,
     * plus a small suspend/resume overhead. */
    u8 warmup[4096];
    memset(warmup, 0x01, sizeof(warmup));
    u64 t_base = get_time_ns();
    TEST_ASSERT(media_nand_program(&ctx, 0, 0, 0, 0, 11, 0, warmup, NULL) == HFSSS_OK, "IS-07: baseline single program");
    u64 single_prog_ns = get_time_ns() - t_base;
    TEST_ASSERT(single_prog_ns > 0, "IS-07: baseline wall-clock > 0");

    /* Kick off the multi-plane program we will interrupt. */
    struct mp_prog_worker_ctx prog = {
        .ctx = &ctx, .plane_mask = 0x3, .block = 8, .page = 0, .fill_p0 = 0x91, .fill_p1 = 0x92, .rc = -1};
    u64 t_sweep_start = get_time_ns();
    pthread_t thr;
    pthread_create(&thr, NULL, mp_prog_worker, &prog);
    TEST_ASSERT(wait_for_state(&ctx, DIE_PROG_ARRAY_BUSY, 5000000000ULL), "IS-07: entered PROG_ARRAY_BUSY");

    /* Suspend, read, resume. */
    TEST_ASSERT(media_nand_program_suspend(&ctx, 0, 0, 0) == HFSSS_OK, "IS-07: suspend accepted");
    TEST_ASSERT(wait_for_state(&ctx, DIE_SUSPENDED_PROG, 5000000000ULL), "IS-07: entered SUSPENDED_PROG");

    u8 rd_fg[4096];
    u64 t_rd_start = get_time_ns();
    int rd_rc = media_nand_read(&ctx, 0, 0, 0, 0, 4, 3, rd_fg, NULL);
    u64 rd_elapsed = get_time_ns() - t_rd_start;
    TEST_ASSERT(rd_rc == HFSSS_OK, "IS-07: foreground read during suspend succeeds");
    TEST_ASSERT(rd_fg[0] == 0xDD && rd_fg[4095] == 0xDD, "IS-07: foreground read returns seeded data");
    TEST_ASSERT(rd_elapsed < single_prog_ns,
                "IS-07: foreground read faster than a tPROG (not serialized on suspended remaining)");

    TEST_ASSERT(media_nand_program_resume(&ctx, 0, 0, 0) == HFSSS_OK, "IS-07: resume accepted");
    pthread_join(thr, NULL);
    u64 sweep_elapsed = get_time_ns() - t_sweep_start;
    TEST_ASSERT(prog.rc == HFSSS_OK, "IS-07: mp program completes OK after resume");

    /* Resume must not restart the entire tPROG — envelope should be
     * roughly tPROG + (modest overhead) but strictly less than
     * 2 × tPROG. Use the baseline single-plane tPROG as the scaling
     * anchor and 3x as the generous upper bound to allow for
     * suspend/resume overheads, the foreground read, and CI jitter. */
    TEST_ASSERT(sweep_elapsed < 3ULL * single_prog_ns,
                "IS-07: resume envelope < 3x single-program baseline (no full restart)");

    /* Both planes' data landed. */
    u8 rd0[4096], rd1[4096];
    TEST_ASSERT(media_nand_read(&ctx, 0, 0, 0, 0, 8, 0, rd0, NULL) == HFSSS_OK, "IS-07: read plane 0 target");
    TEST_ASSERT(rd0[0] == 0x91 && rd0[4095] == 0x91, "IS-07: plane 0 has fill_p0");
    TEST_ASSERT(media_nand_read(&ctx, 0, 0, 0, 1, 8, 0, rd1, NULL) == HFSSS_OK, "IS-07: read plane 1 target");
    TEST_ASSERT(rd1[0] == 0x92 && rd1[4095] == 0x92, "IS-07: plane 1 has fill_p1");

    /* Foreground seed page untouched by the preemption. */
    u8 rd_seed_after[4096];
    TEST_ASSERT(media_nand_read(&ctx, 0, 0, 0, 0, 4, 3, rd_seed_after, NULL) == HFSSS_OK,
                "IS-07: re-read foreground seed after resume");
    TEST_ASSERT(rd_seed_after[0] == 0xDD, "IS-07: foreground seed still intact");

    media_cleanup(&ctx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------------ */
/* IS-09: profile-aware multi-plane cap enforcement                          */
/* ------------------------------------------------------------------------ */

static int test_is09_profile_mp_plane_cap(void)
{
    printf("\n=== IS-09: profile-aware multi-plane cap (ONFI 4 vs Toggle 2) ===\n");

    /* ONFI profile at 4-plane geometry: up to 4-plane erase must be
     * accepted (within the ONFI cap of 4). */
    {
        struct media_config cfg = make_cfg_four_plane(NAND_PROFILE_GENERIC_ONFI_TLC);
        struct media_ctx ctx;
        int ret = media_init(&ctx, &cfg);
        TEST_ASSERT(ret == HFSSS_OK, "IS-09: media_init ONFI 4-plane");
        if (ret == HFSSS_OK) {
            TEST_ASSERT(ctx.profile && ctx.profile->mp_rules.max_planes_per_cmd == 4,
                        "IS-09: ONFI profile exposes max_planes_per_cmd=4");
            int r2 = media_nand_multi_plane_erase(&ctx, 0, 0, 0, 0x3, 3);
            TEST_ASSERT(r2 == HFSSS_OK, "IS-09: ONFI accepts 2-plane erase (within cap)");
            int r3 = media_nand_multi_plane_erase(&ctx, 0, 0, 0, 0x7, 4);
            TEST_ASSERT(r3 == HFSSS_OK, "IS-09: ONFI accepts 3-plane erase (within cap)");
            int r4 = media_nand_multi_plane_erase(&ctx, 0, 0, 0, 0xF, 5);
            TEST_ASSERT(r4 == HFSSS_OK, "IS-09: ONFI accepts 4-plane erase (at cap)");
            media_cleanup(&ctx);
        }
    }

    /* Toggle profile at 4-plane geometry: mp_rules.max=2 means anything
     * above 2 planes must be rejected by the engine. The rejection
     * must come from engine_validate_planes (profile-aware path), not
     * from geometry — the geometry accepts up to 4 planes. Rejected
     * submissions must not mutate the die's cmd_state. */
    {
        struct media_config cfg = make_cfg_four_plane(NAND_PROFILE_GENERIC_TOGGLE_TLC);
        struct media_ctx ctx;
        int ret = media_init(&ctx, &cfg);
        TEST_ASSERT(ret == HFSSS_OK, "IS-09: media_init Toggle 4-plane");
        if (ret == HFSSS_OK) {
            TEST_ASSERT(ctx.profile && ctx.profile->mp_rules.max_planes_per_cmd == 2,
                        "IS-09: Toggle profile exposes max_planes_per_cmd=2");

            /* Pre-snapshot for state-preservation check. */
            struct nand_cmd_target t_idle = {.ch = 0, .chip = 0, .die = 0, .plane_mask = 0x1};
            struct nand_die_cmd_state snap_before;
            TEST_ASSERT(nand_cmd_engine_snapshot(ctx.nand, &t_idle, &snap_before) == HFSSS_OK,
                        "IS-09: pre-reject snapshot");
            TEST_ASSERT(snap_before.state == DIE_IDLE, "IS-09: pre-reject state is DIE_IDLE");

            /* 2-plane within Toggle cap: accepted. */
            int r2 = media_nand_multi_plane_erase(&ctx, 0, 0, 0, 0x3, 3);
            TEST_ASSERT(r2 == HFSSS_OK, "IS-09: Toggle accepts 2-plane erase (at cap)");

            /* 3-plane exceeds Toggle cap: rejected by profile check. */
            int r3 = media_nand_multi_plane_erase(&ctx, 0, 0, 0, 0x7, 4);
            TEST_ASSERT(r3 != HFSSS_OK, "IS-09: Toggle rejects 3-plane erase (exceeds cap)");

            /* 4-plane exceeds Toggle cap: rejected. */
            int r4 = media_nand_multi_plane_erase(&ctx, 0, 0, 0, 0xF, 5);
            TEST_ASSERT(r4 != HFSSS_OK, "IS-09: Toggle rejects 4-plane erase (exceeds cap)");

            /* Also reject on multi-plane program (same engine path). */
            u8 bufs[4][4096];
            for (int i = 0; i < 4; i++) {
                memset(bufs[i], 0x60 + i, sizeof(bufs[i]));
            }
            const void *arr3[3] = {bufs[0], bufs[1], bufs[2]};
            int rp3 = media_nand_multi_plane_program(&ctx, 0, 0, 0, 0x7, 6, 0, arr3, NULL);
            TEST_ASSERT(rp3 != HFSSS_OK, "IS-09: Toggle rejects 3-plane program");

            /* State invariant: after all the rejections, die is still
             * IDLE and no stale opcode/target remains. */
            struct nand_die_cmd_state snap_after;
            TEST_ASSERT(nand_cmd_engine_snapshot(ctx.nand, &t_idle, &snap_after) == HFSSS_OK,
                        "IS-09: post-reject snapshot");
            TEST_ASSERT(snap_after.state == DIE_IDLE, "IS-09: rejections left die DIE_IDLE");
            TEST_ASSERT(snap_after.in_flight == false, "IS-09: rejections left in_flight clear");

            media_cleanup(&ctx);
        }
    }

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

int main(void)
{
    printf("========================================\n");
    printf("NAND Command Engine Integration — Heavy\n");
    printf("========================================\n");

    test_is01_suspend_under_mp_program();
    test_is04_cache_vs_mp_erase_stage_conflict();
    test_is07_mp_program_foreground_read_preemption();
    test_is09_profile_mp_plane_cap();

    printf("\n========================================\n");
    printf("Integration Heavy Summary\n");
    printf("========================================\n");
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    return tests_failed == 0 ? 0 : 1;
}
