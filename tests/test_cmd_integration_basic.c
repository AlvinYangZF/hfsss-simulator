/*
 * NAND command-engine integration tests (Phase 7 basic tier).
 *
 * Three scenarios combining >= 2 phases of behavior. These are the
 * lowest-complexity Phase 7 integration cases; heavier cases live in
 * test_cmd_integration_midcomplex.c and test_cmd_integration_heavy.c.
 *
 *   IS-05: init-time profile selection is observable end-to-end via
 *          read id + read parameter page. Two profiles loaded in
 *          succession must produce distinct identity output.
 *          (Phase 2 identity + Phase 6 profile)
 *
 *   IS-06: status and identity coverage across busy states. Enforces
 *          the design contract that read_status_enhanced is legal in
 *          every state while read_id/read_parameter_page are legal
 *          only in DIE_IDLE and the two suspend states (identity
 *          operations use the data bus and cannot race an active array
 *          operation — see cmd_legality.c). Verifies the rejection
 *          paths do not mutate die state.
 *          (Phase 2 identity/status + Phase 3 suspend)
 *
 *   IS-08: illegal suspend/resume submissions must be rejected
 *          deterministically without mutating the die state or the
 *          suspend_count accounting.
 *          (Phase 2 status + Phase 3 suspend legality)
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "common/common.h"
#include "media/cmd_engine.h"
#include "media/cmd_legality.h"
#include "media/cmd_state.h"
#include "media/media.h"
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
/* Shared helpers. Mirror the patterns used inside test_media.c (the file    */
/* where Phase 1-6 per-phase tests live) so the integration tier stays       */
/* idiomatic with the rest of the suite.                                     */
/* ------------------------------------------------------------------------ */

struct worker_ctx {
    struct media_ctx *ctx;
    u32 block;
    u32 page;
    int rc;
};

static void *prog_worker(void *arg)
{
    struct worker_ctx *w = arg;
    u8 buf[4096];
    memset(buf, 0x5A, sizeof(buf));
    w->rc = media_nand_program(w->ctx, 0, 0, 0, 0, w->block, w->page, buf, NULL);
    return NULL;
}

static void *erase_worker(void *arg)
{
    struct worker_ctx *w = arg;
    w->rc = media_nand_erase(w->ctx, 0, 0, 0, 0, w->block);
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

static struct media_config make_cfg(enum nand_profile_id pid, bool explicit_profile, enum nand_type type)
{
    struct media_config cfg = {
        .channel_count = 1,
        .chips_per_channel = 1,
        .dies_per_chip = 1,
        .planes_per_die = 1,
        .blocks_per_plane = 32,
        .pages_per_block = 16,
        .page_size = 4096,
        .spare_size = 64,
        .nand_type = type,
        .profile_explicit = explicit_profile,
        .profile_id = pid,
    };
    return cfg;
}

/* ------------------------------------------------------------------------ */
/* IS-05: init-time profile selection correctness                            */
/* ------------------------------------------------------------------------ */

static int test_is05_profile_selection_identity(void)
{
    printf("\n=== IS-05: init-time profile selection -> identity ===\n");

    /* First instantiation: ONFI TLC. Read id and parameter page, cache
     * the reported values. */
    struct media_config cfg_onfi = make_cfg(NAND_PROFILE_GENERIC_ONFI_TLC, true, NAND_TYPE_TLC);
    struct media_ctx ctx_onfi;
    int ret = media_init(&ctx_onfi, &cfg_onfi);
    TEST_ASSERT(ret == HFSSS_OK, "IS-05: media_init with explicit ONFI_TLC profile");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }

    const struct nand_profile *prof_onfi = ctx_onfi.profile;
    TEST_ASSERT(prof_onfi != NULL && prof_onfi->id == NAND_PROFILE_GENERIC_ONFI_TLC,
                "IS-05: ctx.profile is the explicitly requested ONFI_TLC table");

    struct nand_id id_onfi;
    ret = media_nand_read_id(&ctx_onfi, 0, 0, 0, &id_onfi);
    TEST_ASSERT(ret == HFSSS_OK, "IS-05: read_id (ONFI) OK");
    TEST_ASSERT(id_onfi.bytes[0] == prof_onfi->identity.manufacturer_id,
                "IS-05: read_id byte0 (manufacturer) matches ONFI profile");
    TEST_ASSERT(id_onfi.bytes[1] == prof_onfi->identity.device_id,
                "IS-05: read_id byte1 (device_id) matches ONFI profile");

    struct nand_parameter_page pp_onfi;
    ret = media_nand_read_parameter_page(&ctx_onfi, 0, 0, 0, &pp_onfi);
    TEST_ASSERT(ret == HFSSS_OK, "IS-05: read_parameter_page (ONFI) OK");
    TEST_ASSERT(strncmp(pp_onfi.manufacturer_name, prof_onfi->identity.manufacturer_name,
                        NAND_PARAMETER_PAGE_MFR_NAME_LEN) == 0,
                "IS-05: param_page.manufacturer_name matches ONFI profile");
    TEST_ASSERT(strncmp(pp_onfi.device_model, prof_onfi->identity.device_model, NAND_PARAMETER_PAGE_MODEL_LEN) == 0,
                "IS-05: param_page.device_model matches ONFI profile");

    media_cleanup(&ctx_onfi);

    /* Second instantiation: Toggle QLC. Same APIs, different outputs. */
    struct media_config cfg_tgl = make_cfg(NAND_PROFILE_GENERIC_TOGGLE_QLC, true, NAND_TYPE_QLC);
    struct media_ctx ctx_tgl;
    ret = media_init(&ctx_tgl, &cfg_tgl);
    TEST_ASSERT(ret == HFSSS_OK, "IS-05: media_init with explicit TOGGLE_QLC profile");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }

    const struct nand_profile *prof_tgl = ctx_tgl.profile;
    TEST_ASSERT(prof_tgl != NULL && prof_tgl->id == NAND_PROFILE_GENERIC_TOGGLE_QLC,
                "IS-05: ctx.profile is the explicitly requested TOGGLE_QLC table");

    struct nand_id id_tgl;
    ret = media_nand_read_id(&ctx_tgl, 0, 0, 0, &id_tgl);
    TEST_ASSERT(ret == HFSSS_OK, "IS-05: read_id (Toggle) OK");
    TEST_ASSERT(id_tgl.bytes[1] == prof_tgl->identity.device_id,
                "IS-05: read_id byte1 (device_id) matches Toggle profile");

    struct nand_parameter_page pp_tgl;
    ret = media_nand_read_parameter_page(&ctx_tgl, 0, 0, 0, &pp_tgl);
    TEST_ASSERT(ret == HFSSS_OK, "IS-05: read_parameter_page (Toggle) OK");

    /* Cross-profile assertions: two different profiles must produce
     * distinguishable identity. This is the whole point of Phase 6 and
     * the reason Phase 7 validates the init-time path explicitly. */
    TEST_ASSERT(id_onfi.bytes[1] != id_tgl.bytes[1], "IS-05: read_id device_id byte differs across profiles");
    TEST_ASSERT(strncmp(pp_onfi.manufacturer_name, pp_tgl.manufacturer_name, NAND_PARAMETER_PAGE_MFR_NAME_LEN) != 0,
                "IS-05: param_page.manufacturer_name differs across profiles");
    TEST_ASSERT(strncmp(pp_onfi.device_model, pp_tgl.device_model, NAND_PARAMETER_PAGE_MODEL_LEN) != 0,
                "IS-05: param_page.device_model differs across profiles");

    media_cleanup(&ctx_tgl);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------------ */
/* IS-06: read id / read status enhanced in every busy state                */
/* ------------------------------------------------------------------------ */

static int test_is06_identity_in_busy_states(void)
{
    printf("\n=== IS-06: read_id / status_enhanced in busy states ===\n");

    struct media_config cfg = make_cfg(NAND_PROFILE_GENERIC_ONFI_TLC, true, NAND_TYPE_TLC);
    struct media_ctx ctx;
    int ret = media_init(&ctx, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "IS-06: media_init succeeds");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }

    const struct nand_profile *prof = ctx.profile;
    TEST_ASSERT(prof != NULL, "IS-06: profile resolved");

    /* --- Phase A: identity + status while program is in array-busy --- */
    struct worker_ctx prog = {.ctx = &ctx, .block = 3, .page = 5, .rc = -1};
    pthread_t prog_thr;
    pthread_create(&prog_thr, NULL, prog_worker, &prog);

    bool saw_prog_busy = wait_for_state(&ctx, DIE_PROG_ARRAY_BUSY, 5000000000ULL);
    TEST_ASSERT(saw_prog_busy, "IS-06: DIE_PROG_ARRAY_BUSY observed");

    /* read_id is not legal in ARRAY_BUSY (identity needs the data bus
     * and cannot race an active array op). The engine must reject it
     * without disturbing the in-flight command's state. */
    struct nand_id id_busy;
    int rid_busy = media_nand_read_id(&ctx, 0, 0, 0, &id_busy);
    TEST_ASSERT(rid_busy != HFSSS_OK, "IS-06: read_id rejected during PROG_ARRAY_BUSY");

    /* status_enhanced, however, must be serviceable mid-flight. */
    struct nand_status_enhanced enh_busy;
    ret = media_nand_read_status_enhanced(&ctx, 0, 0, 0, &enh_busy);
    TEST_ASSERT(ret == HFSSS_OK, "IS-06: status_enhanced during PROG_ARRAY_BUSY succeeds");
    TEST_ASSERT(enh_busy.state == DIE_PROG_ARRAY_BUSY,
                "IS-06: status still reports PROG_ARRAY_BUSY after rejected read_id");
    TEST_ASSERT(!enh_busy.array_ready, "IS-06: status.array_ready == false while array busy");

    /* --- Phase B: identity + status while program is suspended --- */
    ret = media_nand_program_suspend(&ctx, 0, 0, 0);
    TEST_ASSERT(ret == HFSSS_OK, "IS-06: program_suspend accepted");

    bool saw_suspended = wait_for_state(&ctx, DIE_SUSPENDED_PROG, 5000000000ULL);
    TEST_ASSERT(saw_suspended, "IS-06: DIE_SUSPENDED_PROG observed");

    struct nand_id id_susp;
    ret = media_nand_read_id(&ctx, 0, 0, 0, &id_susp);
    TEST_ASSERT(ret == HFSSS_OK, "IS-06: read_id during SUSPENDED_PROG succeeds");
    TEST_ASSERT(id_susp.bytes[1] == prof->identity.device_id,
                "IS-06: read_id device_id byte still matches profile while suspended");

    struct nand_status_enhanced enh_susp;
    ret = media_nand_read_status_enhanced(&ctx, 0, 0, 0, &enh_susp);
    TEST_ASSERT(ret == HFSSS_OK, "IS-06: status_enhanced during SUSPENDED_PROG succeeds");
    TEST_ASSERT(enh_susp.state == DIE_SUSPENDED_PROG, "IS-06: status reports SUSPENDED_PROG");
    TEST_ASSERT(enh_susp.suspend_count == 1, "IS-06: suspend_count == 1 after first suspend");
    TEST_ASSERT(enh_susp.remaining_ns > 0, "IS-06: remaining_ns > 0 after suspend");

    /* Identity probes must not disturb the suspend bookkeeping — this is
     * the invariant the engine advertises for status/identity submission
     * paths. */
    struct nand_cmd_target t = {.ch = 0, .chip = 0, .die = 0, .plane_mask = 1};
    struct nand_die_cmd_state snap;
    ret = nand_cmd_engine_snapshot(ctx.nand, &t, &snap);
    TEST_ASSERT(ret == HFSSS_OK, "IS-06: snapshot during SUSPENDED_PROG OK");
    TEST_ASSERT(snap.state == DIE_SUSPENDED_PROG, "IS-06: snapshot still SUSPENDED_PROG after identity probe");
    TEST_ASSERT(snap.suspend_count == 1, "IS-06: suspend_count unchanged by identity probe");

    ret = media_nand_program_resume(&ctx, 0, 0, 0);
    TEST_ASSERT(ret == HFSSS_OK, "IS-06: program_resume accepted");
    pthread_join(prog_thr, NULL);
    TEST_ASSERT(prog.rc == HFSSS_OK, "IS-06: suspended program completes OK after resume");

    /* --- Phase C: identity + status while erase is in array-busy --- */
    struct worker_ctx er = {.ctx = &ctx, .block = 6, .rc = -1};
    pthread_t er_thr;
    pthread_create(&er_thr, NULL, erase_worker, &er);

    bool saw_erase_busy = wait_for_state(&ctx, DIE_ERASE_ARRAY_BUSY, 5000000000ULL);
    TEST_ASSERT(saw_erase_busy, "IS-06: DIE_ERASE_ARRAY_BUSY observed");

    struct nand_status_enhanced enh_er;
    ret = media_nand_read_status_enhanced(&ctx, 0, 0, 0, &enh_er);
    TEST_ASSERT(ret == HFSSS_OK, "IS-06: status_enhanced during ERASE_ARRAY_BUSY succeeds");
    TEST_ASSERT(enh_er.state == DIE_ERASE_ARRAY_BUSY, "IS-06: status reports ERASE_ARRAY_BUSY");

    /* Same rejection contract: read_id not legal in ERASE_ARRAY_BUSY. */
    struct nand_id id_er_busy;
    int rid_er_busy = media_nand_read_id(&ctx, 0, 0, 0, &id_er_busy);
    TEST_ASSERT(rid_er_busy != HFSSS_OK, "IS-06: read_id rejected during ERASE_ARRAY_BUSY");

    /* --- Phase D: identity + status while erase is suspended --- */
    ret = media_nand_erase_suspend(&ctx, 0, 0, 0);
    TEST_ASSERT(ret == HFSSS_OK, "IS-06: erase_suspend accepted");
    bool saw_er_susp = wait_for_state(&ctx, DIE_SUSPENDED_ERASE, 5000000000ULL);
    TEST_ASSERT(saw_er_susp, "IS-06: DIE_SUSPENDED_ERASE observed");

    struct nand_status_enhanced enh_er_susp;
    ret = media_nand_read_status_enhanced(&ctx, 0, 0, 0, &enh_er_susp);
    TEST_ASSERT(ret == HFSSS_OK, "IS-06: status_enhanced during SUSPENDED_ERASE succeeds");
    TEST_ASSERT(enh_er_susp.state == DIE_SUSPENDED_ERASE, "IS-06: status reports SUSPENDED_ERASE");
    TEST_ASSERT(enh_er_susp.suspend_count == 1, "IS-06: erase suspend_count == 1");

    struct nand_id id_er_susp;
    ret = media_nand_read_id(&ctx, 0, 0, 0, &id_er_susp);
    TEST_ASSERT(ret == HFSSS_OK, "IS-06: read_id during SUSPENDED_ERASE succeeds");
    TEST_ASSERT(id_er_susp.bytes[1] == prof->identity.device_id,
                "IS-06: read_id device_id byte matches profile in SUSPENDED_ERASE");

    ret = media_nand_erase_resume(&ctx, 0, 0, 0);
    TEST_ASSERT(ret == HFSSS_OK, "IS-06: erase_resume accepted");
    pthread_join(er_thr, NULL);
    TEST_ASSERT(er.rc == HFSSS_OK, "IS-06: suspended erase completes OK after resume");

    media_cleanup(&ctx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------------ */
/* IS-08: illegal suspend/resume rejection preserves state                   */
/* ------------------------------------------------------------------------ */

static int test_is08_illegal_suspend_resume(void)
{
    printf("\n=== IS-08: illegal suspend/resume rejection ===\n");

    struct media_config cfg = make_cfg(NAND_PROFILE_GENERIC_ONFI_TLC, true, NAND_TYPE_TLC);
    struct media_ctx ctx;
    int ret = media_init(&ctx, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "IS-08: media_init succeeds");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }

    struct nand_cmd_target t = {.ch = 0, .chip = 0, .die = 0, .plane_mask = 1};

    /* --- IDLE state: all four suspend/resume submissions illegal --- */
    struct nand_die_cmd_state snap_before;
    ret = nand_cmd_engine_snapshot(ctx.nand, &t, &snap_before);
    TEST_ASSERT(ret == HFSSS_OK && snap_before.state == DIE_IDLE, "IS-08: baseline is DIE_IDLE");
    TEST_ASSERT(snap_before.suspend_count == 0, "IS-08: baseline suspend_count == 0");

    int r1 = media_nand_program_suspend(&ctx, 0, 0, 0);
    int r2 = media_nand_erase_suspend(&ctx, 0, 0, 0);
    int r3 = media_nand_program_resume(&ctx, 0, 0, 0);
    int r4 = media_nand_erase_resume(&ctx, 0, 0, 0);
    TEST_ASSERT(r1 != HFSSS_OK, "IS-08: program_suspend rejected from IDLE");
    TEST_ASSERT(r2 != HFSSS_OK, "IS-08: erase_suspend rejected from IDLE");
    TEST_ASSERT(r3 != HFSSS_OK, "IS-08: program_resume rejected from IDLE");
    TEST_ASSERT(r4 != HFSSS_OK, "IS-08: erase_resume rejected from IDLE");

    struct nand_die_cmd_state snap_after_idle;
    ret = nand_cmd_engine_snapshot(ctx.nand, &t, &snap_after_idle);
    TEST_ASSERT(ret == HFSSS_OK, "IS-08: snapshot post-idle-rejects OK");
    TEST_ASSERT(snap_after_idle.state == DIE_IDLE, "IS-08: still DIE_IDLE after illegal submissions");
    TEST_ASSERT(snap_after_idle.suspend_count == 0, "IS-08: suspend_count still 0");
    TEST_ASSERT(snap_after_idle.in_flight == false, "IS-08: in_flight still false");

    /* --- SUSPENDED_PROG state: cross-type resume must be rejected --- */
    struct worker_ctx prog = {.ctx = &ctx, .block = 9, .page = 2, .rc = -1};
    pthread_t prog_thr;
    pthread_create(&prog_thr, NULL, prog_worker, &prog);
    TEST_ASSERT(wait_for_state(&ctx, DIE_PROG_ARRAY_BUSY, 5000000000ULL), "IS-08: entered PROG_ARRAY_BUSY");

    ret = media_nand_program_suspend(&ctx, 0, 0, 0);
    TEST_ASSERT(ret == HFSSS_OK, "IS-08: legal program_suspend accepted");
    TEST_ASSERT(wait_for_state(&ctx, DIE_SUSPENDED_PROG, 5000000000ULL), "IS-08: entered SUSPENDED_PROG");

    struct nand_die_cmd_state snap_susp;
    nand_cmd_engine_snapshot(ctx.nand, &t, &snap_susp);
    TEST_ASSERT(snap_susp.suspend_count == 1, "IS-08: suspend_count == 1 in SUSPENDED_PROG");

    /* Wrong-type resume: erase_resume on a program that is suspended. */
    int cross = media_nand_erase_resume(&ctx, 0, 0, 0);
    TEST_ASSERT(cross != HFSSS_OK, "IS-08: erase_resume rejected in SUSPENDED_PROG");

    /* Double suspend of the same type is also illegal. */
    int double_susp = media_nand_program_suspend(&ctx, 0, 0, 0);
    TEST_ASSERT(double_susp != HFSSS_OK, "IS-08: program_suspend rejected when already SUSPENDED_PROG");

    struct nand_die_cmd_state snap_after_illegal;
    nand_cmd_engine_snapshot(ctx.nand, &t, &snap_after_illegal);
    TEST_ASSERT(snap_after_illegal.state == DIE_SUSPENDED_PROG,
                "IS-08: still SUSPENDED_PROG after cross-type/double submissions");
    TEST_ASSERT(snap_after_illegal.suspend_count == 1, "IS-08: suspend_count unchanged by illegal submissions");

    /* Clean up: legitimate resume and join. */
    ret = media_nand_program_resume(&ctx, 0, 0, 0);
    TEST_ASSERT(ret == HFSSS_OK, "IS-08: legal program_resume accepted for cleanup");
    pthread_join(prog_thr, NULL);
    TEST_ASSERT(prog.rc == HFSSS_OK, "IS-08: suspended program completes after clean resume");

    /* --- SUSPENDED_ERASE state: cross-type program_resume must be rejected --- */
    struct worker_ctx er = {.ctx = &ctx, .block = 11, .rc = -1};
    pthread_t er_thr;
    pthread_create(&er_thr, NULL, erase_worker, &er);
    TEST_ASSERT(wait_for_state(&ctx, DIE_ERASE_ARRAY_BUSY, 5000000000ULL), "IS-08: entered ERASE_ARRAY_BUSY");

    ret = media_nand_erase_suspend(&ctx, 0, 0, 0);
    TEST_ASSERT(ret == HFSSS_OK, "IS-08: legal erase_suspend accepted");
    TEST_ASSERT(wait_for_state(&ctx, DIE_SUSPENDED_ERASE, 5000000000ULL), "IS-08: entered SUSPENDED_ERASE");

    int cross2 = media_nand_program_resume(&ctx, 0, 0, 0);
    TEST_ASSERT(cross2 != HFSSS_OK, "IS-08: program_resume rejected in SUSPENDED_ERASE");

    struct nand_die_cmd_state snap_er;
    nand_cmd_engine_snapshot(ctx.nand, &t, &snap_er);
    TEST_ASSERT(snap_er.state == DIE_SUSPENDED_ERASE, "IS-08: still SUSPENDED_ERASE after cross-type reject");
    TEST_ASSERT(snap_er.suspend_count == 1, "IS-08: erase suspend_count unchanged");

    ret = media_nand_erase_resume(&ctx, 0, 0, 0);
    TEST_ASSERT(ret == HFSSS_OK, "IS-08: legal erase_resume accepted for cleanup");
    pthread_join(er_thr, NULL);
    TEST_ASSERT(er.rc == HFSSS_OK, "IS-08: suspended erase completes after clean resume");

    media_cleanup(&ctx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

int main(void)
{
    printf("========================================\n");
    printf("NAND Command Engine Integration — Basic\n");
    printf("========================================\n");

    test_is05_profile_selection_identity();
    test_is06_identity_in_busy_states();
    test_is08_illegal_suspend_resume();

    printf("\n========================================\n");
    printf("Integration Basic Summary\n");
    printf("========================================\n");
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    return tests_failed == 0 ? 0 : 1;
}
