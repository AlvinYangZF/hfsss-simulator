/*
 * NAND command-engine long-form integration sweep.
 *
 * Scope (read before assuming what this suite proves):
 *   Single-plane path coverage of identity, status, program/read,
 *   erase+reprogram, cache read/program, suspend/resume, and
 *   reset-abort under each generic profile. Each profile gets a
 *   fresh media_ctx; the sequence runs end-to-end; the ctx is torn
 *   down before the next profile is instantiated.
 *
 *   Multi-plane paths are NOT exercised here. The sweep geometry
 *   uses planes_per_die=1 for fast iteration, so multi-plane legal
 *   and illegal-mask behavior, multi-plane suspend, and multi-plane
 *   reset-abort are explicitly skipped (see the annotated no-op in
 *   run_profile_sequence). Multi-plane end-to-end coverage lives in
 *   tests/test_cmd_integration_heavy.c (IS-01, IS-04, IS-07, IS-09)
 *   and in the profile-matrix mp_rules assertions in
 *   tests/test_profile_matrix.c. Do not read the pass output of this
 *   binary as evidence that multi-plane is green.
 *
 * Profiles covered:
 *   NAND_PROFILE_GENERIC_ONFI_TLC
 *   NAND_PROFILE_GENERIC_ONFI_QLC
 *   NAND_PROFILE_GENERIC_TOGGLE_TLC
 *   NAND_PROFILE_GENERIC_TOGGLE_QLC
 *
 * Geometry is intentionally small (8 blocks, 4 pages/block) because the
 * suite's purpose is command-path coverage, not data volume. Blocks are
 * partitioned by role so each sub-step writes into a dedicated block and
 * step N does not rely on step N-1 leaving the block in a particular
 * state.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "common/common.h"
#include "media/cmd_engine.h"
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
/* Harness helpers. Mirror the pattern used by the basic integration driver  */
/* so the long-form sweep stays idiomatic with the rest of the suite.        */
/* ------------------------------------------------------------------------ */

struct prog_worker_ctx {
    struct media_ctx *ctx;
    u32 block;
    u32 page;
    u8 pattern;
    int rc;
};

static void *prog_worker(void *arg)
{
    struct prog_worker_ctx *w = arg;
    u8 buf[4096];
    memset(buf, w->pattern, sizeof(buf));
    w->rc = media_nand_program(w->ctx, 0, 0, 0, 0, w->block, w->page, buf, NULL);
    return NULL;
}

/*
 * Profile-agnostic state poll. Uses the engine snapshot directly so this
 * works on Toggle profiles too (those reject READ_STATUS_ENHANCED per
 * the Phase 7 bitmap divergence). Snapshot is lock-free and safe to call
 * concurrently with an in-flight worker.
 */
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

static int snapshot_die(struct media_ctx *ctx, struct nand_die_cmd_state *out)
{
    struct nand_cmd_target t = {.ch = 0, .chip = 0, .die = 0, .plane_mask = 0x1};
    return nand_cmd_engine_snapshot(ctx->nand, &t, out);
}

static struct media_config make_cfg(enum nand_profile_id pid, enum nand_type type)
{
    struct media_config cfg = {
        .channel_count = 1,
        .chips_per_channel = 1,
        .dies_per_chip = 1,
        .planes_per_die = 1,
        .blocks_per_plane = 8,
        .pages_per_block = 4,
        .page_size = 4096,
        .spare_size = 64,
        .nand_type = type,
        .profile_explicit = true,
        .profile_id = pid,
    };
    return cfg;
}

/* Per-profile block layout. Each sub-step owns a dedicated block so steps
 * do not implicitly depend on each other's final state. */
enum {
    BLK_PROG_READ = 1,
    BLK_ERASE_REPROG = 2,
    BLK_CACHE_READ = 3,
    BLK_CACHE_PROG = 4,
    BLK_SUSPEND = 5,
    BLK_RESET_ABORT = 6,
    BLK_POST_RESET = 7,
};

struct profile_entry {
    enum nand_profile_id id;
    const char *name;
    enum nand_type type;
    u8 pattern;
};

static const struct profile_entry k_profiles[] = {
    {NAND_PROFILE_GENERIC_ONFI_TLC, "onfi_tlc", NAND_TYPE_TLC, 0xA0},
    {NAND_PROFILE_GENERIC_ONFI_QLC, "onfi_qlc", NAND_TYPE_QLC, 0xA1},
    {NAND_PROFILE_GENERIC_TOGGLE_TLC, "toggle_tlc", NAND_TYPE_TLC, 0xA2},
    {NAND_PROFILE_GENERIC_TOGGLE_QLC, "toggle_qlc", NAND_TYPE_QLC, 0xA3},
};

/* ------------------------------------------------------------------------ */
/* Per-profile full sequence                                                  */
/* ------------------------------------------------------------------------ */

static int run_profile_sequence(const struct profile_entry *pe)
{
    printf("\n=== profile sweep: %s ===\n", pe->name);
    char tag[96];

    /* --- init --- */
    struct media_config cfg = make_cfg(pe->id, pe->type);
    struct media_ctx ctx;
    int ret = media_init(&ctx, &cfg);
    snprintf(tag, sizeof(tag), "%s: media_init", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }
    const struct nand_profile *prof = ctx.profile;
    snprintf(tag, sizeof(tag), "%s: ctx.profile bound to requested id", pe->name);
    TEST_ASSERT(prof != NULL && prof->id == pe->id, tag);

    /* --- identity --- */
    struct nand_id id;
    ret = media_nand_read_id(&ctx, 0, 0, 0, &id);
    snprintf(tag, sizeof(tag), "%s: read_id OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    snprintf(tag, sizeof(tag), "%s: read_id.manufacturer matches profile", pe->name);
    TEST_ASSERT(id.bytes[0] == prof->identity.manufacturer_id, tag);
    snprintf(tag, sizeof(tag), "%s: read_id.device_id matches profile", pe->name);
    TEST_ASSERT(id.bytes[1] == prof->identity.device_id, tag);

    struct nand_parameter_page pp;
    ret = media_nand_read_parameter_page(&ctx, 0, 0, 0, &pp);
    snprintf(tag, sizeof(tag), "%s: read_parameter_page OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    snprintf(tag, sizeof(tag), "%s: param_page.manufacturer_name matches profile", pe->name);
    TEST_ASSERT(strncmp(pp.manufacturer_name, prof->identity.manufacturer_name,
                        NAND_PARAMETER_PAGE_MFR_NAME_LEN) == 0,
                tag);
    snprintf(tag, sizeof(tag), "%s: param_page.device_model matches profile", pe->name);
    TEST_ASSERT(strncmp(pp.device_model, prof->identity.device_model, NAND_PARAMETER_PAGE_MODEL_LEN) == 0, tag);

    /* --- status on idle die --- */
    u8 sb = 0;
    ret = media_nand_read_status_byte(&ctx, 0, 0, 0, &sb);
    snprintf(tag, sizeof(tag), "%s: read_status_byte OK on idle die", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    snprintf(tag, sizeof(tag), "%s: status byte RDY|ARDY set on idle die", pe->name);
    TEST_ASSERT((sb & (NAND_STATUS_RDY | NAND_STATUS_ARDY)) == (NAND_STATUS_RDY | NAND_STATUS_ARDY), tag);

    /*
     * Enhanced-status behavior is profile-dependent after the Phase 7
     * divergence: ONFI profiles accept it, Toggle profiles reject it
     * (the legality path consults profile->capability.supported_ops_bitmap).
     * Either way the baseline die state must still be observable — we
     * assert the correct outcome per profile family and then use the
     * engine snapshot as the profile-agnostic source of truth for the
     * state check.
     */
    bool enh_supported = nand_profile_supports_op(ctx.profile, NAND_OP_READ_STATUS_ENHANCED);
    struct nand_status_enhanced enh;
    ret = media_nand_read_status_enhanced(&ctx, 0, 0, 0, &enh);
    if (enh_supported) {
        snprintf(tag, sizeof(tag), "%s: read_status_enhanced OK on idle die (ONFI)", pe->name);
        TEST_ASSERT(ret == HFSSS_OK, tag);
        snprintf(tag, sizeof(tag), "%s: enhanced.ready && array_ready on idle die", pe->name);
        TEST_ASSERT(enh.ready && enh.array_ready, tag);
        snprintf(tag, sizeof(tag), "%s: enhanced.state == DIE_IDLE", pe->name);
        TEST_ASSERT(enh.state == DIE_IDLE, tag);
    } else {
        snprintf(tag, sizeof(tag), "%s: read_status_enhanced rejected (Toggle)", pe->name);
        TEST_ASSERT(ret != HFSSS_OK, tag);
    }
    struct nand_die_cmd_state snap_idle;
    ret = snapshot_die(&ctx, &snap_idle);
    snprintf(tag, sizeof(tag), "%s: snapshot on idle die OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK && snap_idle.state == DIE_IDLE, tag);

    /* --- program + read round-trip --- */
    u8 wr[4096];
    u8 rd[4096];
    memset(wr, pe->pattern, sizeof(wr));
    ret = media_nand_program(&ctx, 0, 0, 0, 0, BLK_PROG_READ, 0, wr, NULL);
    snprintf(tag, sizeof(tag), "%s: program page OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    memset(rd, 0, sizeof(rd));
    ret = media_nand_read(&ctx, 0, 0, 0, 0, BLK_PROG_READ, 0, rd, NULL);
    snprintf(tag, sizeof(tag), "%s: read-back OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    snprintf(tag, sizeof(tag), "%s: read-back byte matches programmed pattern", pe->name);
    TEST_ASSERT(rd[0] == pe->pattern && rd[4095] == pe->pattern, tag);

    /* --- erase + reprogram with a different pattern --- */
    ret = media_nand_erase(&ctx, 0, 0, 0, 0, BLK_ERASE_REPROG);
    snprintf(tag, sizeof(tag), "%s: erase block OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    u8 alt = (u8)(pe->pattern ^ 0x0F);
    memset(wr, alt, sizeof(wr));
    ret = media_nand_program(&ctx, 0, 0, 0, 0, BLK_ERASE_REPROG, 0, wr, NULL);
    snprintf(tag, sizeof(tag), "%s: reprogram after erase OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    memset(rd, 0, sizeof(rd));
    ret = media_nand_read(&ctx, 0, 0, 0, 0, BLK_ERASE_REPROG, 0, rd, NULL);
    snprintf(tag, sizeof(tag), "%s: read after erase+reprogram OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    snprintf(tag, sizeof(tag), "%s: read matches alternate pattern", pe->name);
    TEST_ASSERT(rd[0] == alt && rd[2048] == alt, tag);

    /* --- multi-plane program: skipped in this config --- */
    /* planes_per_die is 1 in the sweep geometry; multi-plane paths are
     * covered by per-feature unit tests where plane count is configurable.
     * Retained as an explicit no-op so the sweep reads the same list of
     * steps that the spec enumerates. */

    /* --- cache read sequence --- */
    memset(wr, (u8)(pe->pattern + 0x10), sizeof(wr));
    ret = media_nand_program(&ctx, 0, 0, 0, 0, BLK_CACHE_READ, 0, wr, NULL);
    snprintf(tag, sizeof(tag), "%s: prep cache_read page0 OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    memset(wr, (u8)(pe->pattern + 0x11), sizeof(wr));
    ret = media_nand_program(&ctx, 0, 0, 0, 0, BLK_CACHE_READ, 1, wr, NULL);
    snprintf(tag, sizeof(tag), "%s: prep cache_read page1 OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);

    u8 cr0[4096];
    u8 cr1[4096];
    ret = media_nand_cache_read(&ctx, 0, 0, 0, 0, BLK_CACHE_READ, 0, cr0, NULL);
    snprintf(tag, sizeof(tag), "%s: cache_read page0 OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    ret = media_nand_cache_read_end(&ctx, 0, 0, 0, 0, BLK_CACHE_READ, 1, cr1, NULL);
    snprintf(tag, sizeof(tag), "%s: cache_read_end page1 OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    snprintf(tag, sizeof(tag), "%s: cache_read data correct", pe->name);
    TEST_ASSERT(cr0[0] == (u8)(pe->pattern + 0x10) && cr1[0] == (u8)(pe->pattern + 0x11), tag);

    /* --- cache program sequence --- */
    ret = media_nand_erase(&ctx, 0, 0, 0, 0, BLK_CACHE_PROG);
    snprintf(tag, sizeof(tag), "%s: erase for cache_program OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    u8 cpw0[4096];
    u8 cpw1[4096];
    memset(cpw0, (u8)(pe->pattern + 0x20), sizeof(cpw0));
    memset(cpw1, (u8)(pe->pattern + 0x21), sizeof(cpw1));
    ret = media_nand_cache_program(&ctx, 0, 0, 0, 0, BLK_CACHE_PROG, 0, cpw0, NULL);
    snprintf(tag, sizeof(tag), "%s: cache_program page0 OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    ret = media_nand_cache_program(&ctx, 0, 0, 0, 0, BLK_CACHE_PROG, 1, cpw1, NULL);
    snprintf(tag, sizeof(tag), "%s: cache_program page1 OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    memset(rd, 0, sizeof(rd));
    ret = media_nand_read(&ctx, 0, 0, 0, 0, BLK_CACHE_PROG, 0, rd, NULL);
    snprintf(tag, sizeof(tag), "%s: read-back cache_program page0 OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK && rd[0] == (u8)(pe->pattern + 0x20), tag);
    memset(rd, 0, sizeof(rd));
    ret = media_nand_read(&ctx, 0, 0, 0, 0, BLK_CACHE_PROG, 1, rd, NULL);
    snprintf(tag, sizeof(tag), "%s: read-back cache_program page1 OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK && rd[0] == (u8)(pe->pattern + 0x21), tag);

    /* --- suspend + resume against an in-flight program --- */
    u8 susp_pattern = (u8)(pe->pattern + 0x30);
    struct prog_worker_ctx pw = {.ctx = &ctx, .block = BLK_SUSPEND, .page = 0, .pattern = susp_pattern, .rc = -1};
    pthread_t pthr;
    pthread_create(&pthr, NULL, prog_worker, &pw);

    bool saw_busy = wait_for_state(&ctx, DIE_PROG_ARRAY_BUSY, 5000000000ULL);
    snprintf(tag, sizeof(tag), "%s: entered PROG_ARRAY_BUSY", pe->name);
    TEST_ASSERT(saw_busy, tag);

    ret = media_nand_program_suspend(&ctx, 0, 0, 0);
    snprintf(tag, sizeof(tag), "%s: program_suspend accepted", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);

    bool saw_susp = wait_for_state(&ctx, DIE_SUSPENDED_PROG, 5000000000ULL);
    snprintf(tag, sizeof(tag), "%s: entered SUSPENDED_PROG", pe->name);
    TEST_ASSERT(saw_susp, tag);

    /* Snapshot works on every profile; the enhanced-status wrapper
     * only works on profiles that advertise it, but the book-keeping
     * we care about (suspend_count, remaining_ns) comes from the
     * cmd_state itself, which the snapshot reads directly. */
    struct nand_die_cmd_state snap_susp;
    ret = snapshot_die(&ctx, &snap_susp);
    snprintf(tag, sizeof(tag), "%s: snapshot during SUSPENDED_PROG OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    snprintf(tag, sizeof(tag), "%s: suspend_count == 1 after first suspend", pe->name);
    TEST_ASSERT(snap_susp.suspend_count == 1, tag);
    snprintf(tag, sizeof(tag), "%s: remaining_ns > 0 while suspended", pe->name);
    TEST_ASSERT(snap_susp.remaining_ns > 0, tag);

    ret = media_nand_program_resume(&ctx, 0, 0, 0);
    snprintf(tag, sizeof(tag), "%s: program_resume accepted", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    pthread_join(pthr, NULL);
    snprintf(tag, sizeof(tag), "%s: suspended program completes OK after resume", pe->name);
    TEST_ASSERT(pw.rc == HFSSS_OK, tag);

    memset(rd, 0, sizeof(rd));
    ret = media_nand_read(&ctx, 0, 0, 0, 0, BLK_SUSPEND, 0, rd, NULL);
    snprintf(tag, sizeof(tag), "%s: post-resume readback OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    snprintf(tag, sizeof(tag), "%s: post-resume data matches pattern", pe->name);
    TEST_ASSERT(rd[0] == susp_pattern && rd[4095] == susp_pattern, tag);

    /* --- reset path (direct engine call during an in-flight program) --- */
    struct nand_die_cmd_state snap_post_resume;
    ret = snapshot_die(&ctx, &snap_post_resume);
    snprintf(tag, sizeof(tag), "%s: die idle after suspend/resume cycle", pe->name);
    TEST_ASSERT(ret == HFSSS_OK && snap_post_resume.state == DIE_IDLE, tag);

    struct prog_worker_ctx rw = {
        .ctx = &ctx, .block = BLK_RESET_ABORT, .page = 0, .pattern = (u8)(pe->pattern ^ 0xFF), .rc = 0};
    pthread_t rthr;
    pthread_create(&rthr, NULL, prog_worker, &rw);

    bool saw_rb = wait_for_state(&ctx, DIE_PROG_ARRAY_BUSY, 5000000000ULL);
    snprintf(tag, sizeof(tag), "%s: reset-abort program entered PROG_ARRAY_BUSY", pe->name);
    TEST_ASSERT(saw_rb, tag);

    struct nand_cmd_target rt = {.ch = 0, .chip = 0, .die = 0, .plane_mask = 1, .block = BLK_RESET_ABORT, .page = 0};
    ret = nand_cmd_engine_submit_reset(ctx.nand, &rt);
    snprintf(tag, sizeof(tag), "%s: engine reset accepted during ARRAY_BUSY", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);

    pthread_join(rthr, NULL);
    snprintf(tag, sizeof(tag), "%s: in-flight program returns non-OK after reset abort", pe->name);
    TEST_ASSERT(rw.rc != HFSSS_OK, tag);

    struct nand_die_cmd_state snap_post_reset;
    ret = snapshot_die(&ctx, &snap_post_reset);
    snprintf(tag, sizeof(tag), "%s: post-reset snapshot OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    snprintf(tag, sizeof(tag), "%s: die returns to DIE_IDLE after reset", pe->name);
    TEST_ASSERT(snap_post_reset.state == DIE_IDLE, tag);
    snprintf(tag, sizeof(tag), "%s: suspend_count cleared by reset", pe->name);
    TEST_ASSERT(snap_post_reset.suspend_count == 0, tag);

    /* Post-reset: a fresh program against a clean block must succeed. */
    u8 post_pattern = (u8)(pe->pattern ^ 0x55);
    memset(wr, post_pattern, sizeof(wr));
    ret = media_nand_program(&ctx, 0, 0, 0, 0, BLK_POST_RESET, 0, wr, NULL);
    snprintf(tag, sizeof(tag), "%s: post-reset program OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK, tag);
    memset(rd, 0, sizeof(rd));
    ret = media_nand_read(&ctx, 0, 0, 0, 0, BLK_POST_RESET, 0, rd, NULL);
    snprintf(tag, sizeof(tag), "%s: post-reset readback OK", pe->name);
    TEST_ASSERT(ret == HFSSS_OK && rd[0] == post_pattern, tag);

    /* --- cleanup --- */
    media_cleanup(&ctx);
    return TEST_PASS;
}

int main(void)
{
    printf("========================================\n");
    printf("NAND Command Engine Integration — Sweep\n");
    printf("========================================\n");

    u64 start_ns = get_time_ns();
    for (size_t i = 0; i < sizeof(k_profiles) / sizeof(k_profiles[0]); i++) {
        run_profile_sequence(&k_profiles[i]);
    }
    u64 elapsed_ns = get_time_ns() - start_ns;

    printf("\n========================================\n");
    printf("Integration Sweep Summary\n");
    printf("========================================\n");
    printf("  Total:   %d\n", tests_run);
    printf("  Passed:  %d\n", tests_passed);
    printf("  Failed:  %d\n", tests_failed);
    printf("  Wall:    %.3f s\n", (double)elapsed_ns / 1e9);

    return tests_failed == 0 ? 0 : 1;
}
