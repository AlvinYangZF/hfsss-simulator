/*
 * Profile matrix parameterized data-layer tests.
 *
 * Enumerates the four generic profiles (ONFI/Toggle x TLC/QLC) and for each
 * profile exercises three data-layer validation points. Unlike the
 * integration-tier tests, nothing here drives a command through the engine:
 * every assertion reads profile tables or parameter-page state that should
 * be derivable purely from the active profile plus media_config.
 *
 *   V1: parameter page advertises the same tR / tPROG / tBERS that the
 *       timing lane selected by the profile's nand_class would produce.
 *       For TLC, timing_get_read_latency / timing_get_prog_latency pick
 *       the LSB lane at page_idx=0. timing_get_erase_latency for TLC
 *       derives tBERS from slc.tERS * 2 because the tlc_timing struct
 *       carries no dedicated tERS field. For QLC, all three come from
 *       qlc_params verbatim. The test hardcodes these relationships so
 *       any future drift in nand_identity.c or timing.c is caught.
 *
 *   V2: the supported_cmd_bitmap reported by read_parameter_page must
 *       agree bit-for-bit with nand_profile_supports_op over the full
 *       enum nand_cmd_opcode domain. The four generic profiles currently
 *       share one default supported-ops mask; V2 asserts that baseline
 *       explicitly. Divergence between ONFI and Toggle (e.g. Toggle-only
 *       opcodes, cache-prog gating) is a future phase-7 task and will
 *       replace the cross-profile-equality assertions below.
 *
 *   V4: mp_rules and capability fields are exposed at the data layer
 *       with the values the Phase 6 profile tables publish. The engine
 *       is not yet wired to enforce max_planes_per_cmd, so this test
 *       deliberately reads the fields directly from the profile struct
 *       rather than probing engine_validate_planes.
 */

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
/* Table-driven enumeration of the four generic profiles.                    */
/* ------------------------------------------------------------------------ */

struct profile_case {
    enum nand_profile_id id;
    const char *name;
    enum nand_type init_type;
};

static const struct profile_case k_cases[] = {
    {NAND_PROFILE_GENERIC_ONFI_TLC, "onfi_tlc", NAND_TYPE_TLC},
    {NAND_PROFILE_GENERIC_ONFI_QLC, "onfi_qlc", NAND_TYPE_QLC},
    {NAND_PROFILE_GENERIC_TOGGLE_TLC, "toggle_tlc", NAND_TYPE_TLC},
    {NAND_PROFILE_GENERIC_TOGGLE_QLC, "toggle_qlc", NAND_TYPE_QLC},
};

static const size_t k_case_count = sizeof(k_cases) / sizeof(k_cases[0]);

static struct media_config make_cfg(const struct profile_case *pc)
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
        .nand_type = pc->init_type,
        .profile_explicit = true,
        .profile_id = pc->id,
    };
    return cfg;
}

/*
 * Compose a per-profile assertion message into a small static buffer. The
 * buffer is reused across the single-threaded test loop; message lifetime is
 * bounded by the immediately following TEST_ASSERT call.
 */
static char s_msgbuf[192];
static const char *msg(const char *profile_name, const char *detail)
{
    snprintf(s_msgbuf, sizeof(s_msgbuf), "[%s] %s", profile_name, detail);
    return s_msgbuf;
}

/* ------------------------------------------------------------------------ */
/* V1: parameter-page timing consistency with profile-derived lanes.         */
/* ------------------------------------------------------------------------ */

static void expected_timings_for_profile(const struct nand_profile *prof, u64 *exp_tR, u64 *exp_tPROG, u64 *exp_tBERS)
{
    if (prof->nand_class == NAND_CLASS_ENTERPRISE_QLC) {
        *exp_tR = prof->timing.qlc_params.tR;
        *exp_tPROG = prof->timing.qlc_params.tPROG;
        *exp_tBERS = prof->timing.qlc_params.tERS;
    } else {
        /* TLC lane: page_idx=0 selects the LSB sub-page latencies.
         * timing_get_erase_latency falls back to slc.tERS * 2 for the
         * TLC lane because tlc_timing carries no tERS field of its own. */
        *exp_tR = prof->timing.tlc_timing.tR_LSB;
        *exp_tPROG = prof->timing.tlc_timing.tPROG_LSB;
        *exp_tBERS = prof->timing.slc_params.tERS * 2ULL;
    }
}

static int test_v1_parameter_page_timings(void)
{
    printf("\n=== V1: parameter page timing == profile-lane expected ===\n");

    for (size_t i = 0; i < k_case_count; i++) {
        const struct profile_case *pc = &k_cases[i];
        const struct nand_profile *prof = nand_profile_get(pc->id);
        TEST_ASSERT(prof != NULL, msg(pc->name, "V1: nand_profile_get resolves"));
        if (!prof) {
            continue;
        }

        struct media_config cfg = make_cfg(pc);
        struct media_ctx ctx;
        int ret = media_init(&ctx, &cfg);
        TEST_ASSERT(ret == HFSSS_OK, msg(pc->name, "V1: media_init OK"));
        if (ret != HFSSS_OK) {
            continue;
        }

        struct nand_parameter_page pp;
        ret = media_nand_read_parameter_page(&ctx, 0, 0, 0, &pp);
        TEST_ASSERT(ret == HFSSS_OK, msg(pc->name, "V1: read_parameter_page OK"));

        u64 exp_tR = 0;
        u64 exp_tPROG = 0;
        u64 exp_tBERS = 0;
        expected_timings_for_profile(prof, &exp_tR, &exp_tPROG, &exp_tBERS);

        TEST_ASSERT(pp.tR_ns == exp_tR, msg(pc->name, "V1: pp.tR_ns matches profile lane"));
        TEST_ASSERT(pp.tPROG_ns == exp_tPROG, msg(pc->name, "V1: pp.tPROG_ns matches profile lane"));
        TEST_ASSERT(pp.tBERS_ns == exp_tBERS, msg(pc->name, "V1: pp.tBERS_ns matches profile lane"));

        /* Sanity: timings must be positive. A zero here would mean the
         * profile table was never populated. */
        TEST_ASSERT(pp.tR_ns > 0, msg(pc->name, "V1: pp.tR_ns is positive"));
        TEST_ASSERT(pp.tPROG_ns > 0, msg(pc->name, "V1: pp.tPROG_ns is positive"));
        TEST_ASSERT(pp.tBERS_ns > 0, msg(pc->name, "V1: pp.tBERS_ns is positive"));

        media_cleanup(&ctx);
    }

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------------ */
/* V2: supported_cmd_bitmap vs nand_profile_supports_op consistency.         */
/* ------------------------------------------------------------------------ */

static int test_v2_supported_cmd_bitmap(void)
{
    printf("\n=== V2: supported_cmd_bitmap == nand_profile_supports_op ===\n");

    u32 observed_bitmaps[sizeof(k_cases) / sizeof(k_cases[0])];
    memset(observed_bitmaps, 0, sizeof(observed_bitmaps));

    for (size_t i = 0; i < k_case_count; i++) {
        const struct profile_case *pc = &k_cases[i];
        const struct nand_profile *prof = nand_profile_get(pc->id);
        TEST_ASSERT(prof != NULL, msg(pc->name, "V2: nand_profile_get resolves"));
        if (!prof) {
            continue;
        }

        struct media_config cfg = make_cfg(pc);
        struct media_ctx ctx;
        int ret = media_init(&ctx, &cfg);
        TEST_ASSERT(ret == HFSSS_OK, msg(pc->name, "V2: media_init OK"));
        if (ret != HFSSS_OK) {
            continue;
        }

        struct nand_parameter_page pp;
        ret = media_nand_read_parameter_page(&ctx, 0, 0, 0, &pp);
        TEST_ASSERT(ret == HFSSS_OK, msg(pc->name, "V2: read_parameter_page OK"));

        /* parameter page and profile must agree on the bitmap content. */
        TEST_ASSERT(pp.supported_cmd_bitmap == prof->capability.supported_ops_bitmap,
                    msg(pc->name, "V2: pp.supported_cmd_bitmap == profile.capability.supported_ops_bitmap"));

        /* For every opcode, the profile helper and the bitmap bit must
         * agree. This also covers opcodes the profile does not support. */
        bool all_consistent = true;
        for (u32 op = 0; op < (u32)NAND_OP_COUNT; op++) {
            bool helper = nand_profile_supports_op(prof, (enum nand_cmd_opcode)op);
            bool bitmap = (pp.supported_cmd_bitmap & (1u << op)) != 0;
            if (helper != bitmap) {
                printf("    opcode %u: helper=%d bitmap=%d\n", op, (int)helper, (int)bitmap);
                all_consistent = false;
            }
        }
        TEST_ASSERT(all_consistent, msg(pc->name, "V2: per-opcode helper vs bitmap bit are consistent"));

        /* Sanity: the mandatory core opcodes must be advertised. */
        TEST_ASSERT((pp.supported_cmd_bitmap & (1u << NAND_OP_READ)) != 0,
                    msg(pc->name, "V2: READ is advertised"));
        TEST_ASSERT((pp.supported_cmd_bitmap & (1u << NAND_OP_PROG)) != 0,
                    msg(pc->name, "V2: PROG is advertised"));
        TEST_ASSERT((pp.supported_cmd_bitmap & (1u << NAND_OP_ERASE)) != 0,
                    msg(pc->name, "V2: ERASE is advertised"));
        TEST_ASSERT((pp.supported_cmd_bitmap & (1u << NAND_OP_RESET)) != 0,
                    msg(pc->name, "V2: RESET is advertised"));
        TEST_ASSERT((pp.supported_cmd_bitmap & (1u << NAND_OP_READ_ID)) != 0,
                    msg(pc->name, "V2: READ_ID is advertised"));
        TEST_ASSERT((pp.supported_cmd_bitmap & (1u << NAND_OP_READ_PARAM_PAGE)) != 0,
                    msg(pc->name, "V2: READ_PARAM_PAGE is advertised"));

        observed_bitmaps[i] = pp.supported_cmd_bitmap;

        media_cleanup(&ctx);
    }

    /*
     * Phase 7 divergence: ONFI 3.5 adds Read Status Enhanced (78h) over
     * the core opcode set; JEDEC Toggle does not standardize it and uses
     * classic Read Status (70h) for the same purpose. The expected
     * cross-family relationship is:
     *   - within a family (ONFI or Toggle), TLC and QLC share the bitmap
     *   - across families, ONFI advertises READ_STATUS_ENHANCED, Toggle
     *     does not, so the bitmaps must differ exactly on that bit
     */
    u32 onfi_bm = 0;
    u32 toggle_bm = 0;
    bool have_onfi = false;
    bool have_toggle = false;
    for (size_t i = 0; i < k_case_count; i++) {
        const struct nand_profile *p = nand_profile_get(k_cases[i].id);
        if (!p) {
            continue;
        }
        if (p->interface_family == NAND_IF_ONFI) {
            if (!have_onfi) {
                onfi_bm = observed_bitmaps[i];
                have_onfi = true;
            } else {
                TEST_ASSERT(observed_bitmaps[i] == onfi_bm,
                            "V2: ONFI family members (TLC vs QLC) share one bitmap");
            }
        } else {
            if (!have_toggle) {
                toggle_bm = observed_bitmaps[i];
                have_toggle = true;
            } else {
                TEST_ASSERT(observed_bitmaps[i] == toggle_bm,
                            "V2: Toggle family members (TLC vs QLC) share one bitmap");
            }
        }
    }
    TEST_ASSERT(have_onfi && have_toggle, "V2: both families represented in the matrix");
    TEST_ASSERT(onfi_bm != toggle_bm, "V2: ONFI vs Toggle bitmaps differ (Phase 7 divergence landed)");

    /* Enhanced-status is the concrete divergence point. */
    TEST_ASSERT((onfi_bm & (1u << NAND_OP_READ_STATUS_ENHANCED)) != 0,
                "V2: ONFI advertises READ_STATUS_ENHANCED (ONFI 3.5 section 5.17)");
    TEST_ASSERT((toggle_bm & (1u << NAND_OP_READ_STATUS_ENHANCED)) == 0,
                "V2: Toggle does not advertise READ_STATUS_ENHANCED");

    /* The two bitmaps should differ on that single bit — nothing else. */
    TEST_ASSERT((onfi_bm ^ toggle_bm) == (1u << NAND_OP_READ_STATUS_ENHANCED),
                "V2: ONFI vs Toggle differ exactly on READ_STATUS_ENHANCED bit");

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------------ */
/* V2b: runtime rejection of unsupported opcodes.                            */
/*                                                                           */
/* nand_cmd_is_legal_for_profile_state already consults                     */
/* nand_profile_supports_op, so a Toggle-profile media_ctx must reject      */
/* media_nand_read_status_enhanced even in DIE_IDLE where the state-only    */
/* legality table would accept it. Flip side: ONFI accepts it.              */
/* ------------------------------------------------------------------------ */

static int test_v2b_runtime_rejection_on_toggle(void)
{
    printf("\n=== V2b: runtime rejection of READ_STATUS_ENHANCED on Toggle ===\n");

    struct media_config cfg_tgl = {
        .channel_count = 1,
        .chips_per_channel = 1,
        .dies_per_chip = 1,
        .planes_per_die = 1,
        .blocks_per_plane = 8,
        .pages_per_block = 8,
        .page_size = 4096,
        .spare_size = 64,
        .nand_type = NAND_TYPE_TLC,
        .profile_explicit = true,
        .profile_id = NAND_PROFILE_GENERIC_TOGGLE_TLC,
    };
    struct media_ctx ctx_tgl;
    int ret = media_init(&ctx_tgl, &cfg_tgl);
    TEST_ASSERT(ret == HFSSS_OK, "V2b: media_init with Toggle profile");
    if (ret == HFSSS_OK) {
        struct nand_status_enhanced enh;
        int r = media_nand_read_status_enhanced(&ctx_tgl, 0, 0, 0, &enh);
        TEST_ASSERT(r != HFSSS_OK,
                    "V2b: Toggle profile rejects media_nand_read_status_enhanced");

        /* Classic status read remains serviceable — it is in the core
         * bitmap both families advertise. */
        u8 st = 0;
        r = media_nand_read_status_byte(&ctx_tgl, 0, 0, 0, &st);
        TEST_ASSERT(r == HFSSS_OK, "V2b: Toggle profile accepts classic read_status");

        media_cleanup(&ctx_tgl);
    }

    /* Mirror with ONFI: enhanced status must be accepted. */
    struct media_config cfg_onfi = cfg_tgl;
    cfg_onfi.profile_id = NAND_PROFILE_GENERIC_ONFI_TLC;
    struct media_ctx ctx_onfi;
    ret = media_init(&ctx_onfi, &cfg_onfi);
    TEST_ASSERT(ret == HFSSS_OK, "V2b: media_init with ONFI profile");
    if (ret == HFSSS_OK) {
        struct nand_status_enhanced enh;
        int r = media_nand_read_status_enhanced(&ctx_onfi, 0, 0, 0, &enh);
        TEST_ASSERT(r == HFSSS_OK,
                    "V2b: ONFI profile accepts media_nand_read_status_enhanced");
        media_cleanup(&ctx_onfi);
    }

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------------ */
/* V4: mp_rules and capability data-layer exposure.                          */
/* ------------------------------------------------------------------------ */

static int test_v4_mp_rules_and_capability(void)
{
    printf("\n=== V4: mp_rules + capability data-layer exposure ===\n");

    for (size_t i = 0; i < k_case_count; i++) {
        const struct profile_case *pc = &k_cases[i];
        const struct nand_profile *prof = nand_profile_get(pc->id);
        TEST_ASSERT(prof != NULL, msg(pc->name, "V4: nand_profile_get resolves"));
        if (!prof) {
            continue;
        }

        /* mp_rules: all four Phase 6 profiles publish the same trio. */
        /* Phase 7 T5b divergence: ONFI profiles cap at 4 planes/cmd
         * (matches ONFI 3.5 enterprise-class parts with 4 planes per
         * die); Toggle profiles cap at 2 planes/cmd (matches the
         * dual-plane baseline common in Toggle 2.x parts). The engine
         * side of this wiring lives in cmd_engine.c::engine_validate_planes. */
        u8 expected_max_planes = (prof->interface_family == NAND_IF_ONFI) ? 4 : 2;
        TEST_ASSERT(prof->mp_rules.max_planes_per_cmd == expected_max_planes,
                    msg(pc->name, "V4: mp_rules.max_planes_per_cmd matches family cap"));
        TEST_ASSERT(prof->mp_rules.allow_cross_block == false,
                    msg(pc->name, "V4: mp_rules.allow_cross_block == false"));
        TEST_ASSERT(prof->mp_rules.plane_addr_mask == 0x01,
                    msg(pc->name, "V4: mp_rules.plane_addr_mask == 0x01"));

        /* capability.bits_per_cell is keyed by the profile's nand_class. */
        u8 expected_bpc = (prof->nand_class == NAND_CLASS_ENTERPRISE_QLC) ? 4 : 3;
        TEST_ASSERT(prof->capability.bits_per_cell == expected_bpc,
                    msg(pc->name, "V4: capability.bits_per_cell matches nand_class"));

        /* ecc_bits_required must be strictly positive; 0 would mean the
         * profile has no ECC contract, which upper layers rely on. */
        TEST_ASSERT(prof->capability.ecc_bits_required > 0,
                    msg(pc->name, "V4: capability.ecc_bits_required > 0"));
        TEST_ASSERT(prof->capability.ecc_codeword_size > 0,
                    msg(pc->name, "V4: capability.ecc_codeword_size > 0"));

        /* Parameter page must republish the same bits_per_cell and ECC
         * contract: profile -> parameter page is the only advertisement
         * path exposed to upper layers. */
        struct media_config cfg = make_cfg(pc);
        struct media_ctx ctx;
        int ret = media_init(&ctx, &cfg);
        TEST_ASSERT(ret == HFSSS_OK, msg(pc->name, "V4: media_init OK"));
        if (ret != HFSSS_OK) {
            continue;
        }

        struct nand_parameter_page pp;
        ret = media_nand_read_parameter_page(&ctx, 0, 0, 0, &pp);
        TEST_ASSERT(ret == HFSSS_OK, msg(pc->name, "V4: read_parameter_page OK"));
        TEST_ASSERT(pp.bits_per_cell == prof->capability.bits_per_cell,
                    msg(pc->name, "V4: pp.bits_per_cell == profile bits_per_cell"));
        TEST_ASSERT(pp.ecc_bits_required == prof->capability.ecc_bits_required,
                    msg(pc->name, "V4: pp.ecc_bits_required == profile ecc_bits_required"));
        TEST_ASSERT(pp.ecc_codeword_size == prof->capability.ecc_codeword_size,
                    msg(pc->name, "V4: pp.ecc_codeword_size == profile ecc_codeword_size"));

        media_cleanup(&ctx);
    }

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------------ */
/* V3: status byte semantics across profiles.                                */
/*                                                                           */
/* The classic status byte is computed from nand_die_cmd_state (ONFI 4.2     */
/* Status Register encoding) and does not vary by profile. V3 asserts this  */
/* invariant: across all four profiles, an idle die and a post-erase die    */
/* produce the same status byte bits. Separately asserts the specific bit   */
/* pattern for an idle, write-permitted die (RDY | ARDY | WP_N).            */
/* ------------------------------------------------------------------------ */

static int test_v3_status_byte_semantics(void)
{
    printf("\n=== V3: classic status byte semantics parity across profiles ===\n");

    u8 idle_status_per_profile[sizeof(k_cases) / sizeof(k_cases[0])];
    u8 post_erase_per_profile[sizeof(k_cases) / sizeof(k_cases[0])];
    memset(idle_status_per_profile, 0, sizeof(idle_status_per_profile));
    memset(post_erase_per_profile, 0, sizeof(post_erase_per_profile));

    for (size_t i = 0; i < k_case_count; i++) {
        const struct profile_case *pc = &k_cases[i];
        struct media_config cfg = make_cfg(pc);
        struct media_ctx ctx;
        int ret = media_init(&ctx, &cfg);
        TEST_ASSERT(ret == HFSSS_OK, msg(pc->name, "V3: media_init OK"));
        if (ret != HFSSS_OK) {
            continue;
        }

        u8 sb = 0;
        ret = media_nand_read_status_byte(&ctx, 0, 0, 0, &sb);
        TEST_ASSERT(ret == HFSSS_OK, msg(pc->name, "V3: read_status_byte on idle die OK"));
        idle_status_per_profile[i] = sb;

        /* ONFI 4.2 Status Register idle contract: ARDY|RDY|WP_N set,
         * FAIL/FAILC clear. Assert both bit groups explicitly so a
         * regression to either side is caught. */
        u8 want = NAND_STATUS_RDY | NAND_STATUS_ARDY | NAND_STATUS_WP_N;
        TEST_ASSERT((sb & want) == want,
                    msg(pc->name, "V3: idle status has RDY|ARDY|WP_N set"));
        TEST_ASSERT((sb & (NAND_STATUS_FAIL | NAND_STATUS_FAILC)) == 0,
                    msg(pc->name, "V3: idle status has FAIL and FAILC clear"));

        /* Drive a clean erase on a fresh block and re-read. A successful
         * erase must not set FAIL; RDY/ARDY must reassert when the die
         * returns to idle. */
        ret = media_nand_erase(&ctx, 0, 0, 0, 0, 1);
        TEST_ASSERT(ret == HFSSS_OK, msg(pc->name, "V3: clean erase OK"));
        ret = media_nand_read_status_byte(&ctx, 0, 0, 0, &sb);
        TEST_ASSERT(ret == HFSSS_OK, msg(pc->name, "V3: read_status_byte post-erase OK"));
        post_erase_per_profile[i] = sb;
        TEST_ASSERT((sb & want) == want, msg(pc->name, "V3: post-erase has RDY|ARDY|WP_N set"));
        TEST_ASSERT((sb & NAND_STATUS_FAIL) == 0, msg(pc->name, "V3: post-erase FAIL bit clear"));

        media_cleanup(&ctx);
    }

    /* Cross-profile parity: the observed status byte must be identical
     * across all profiles for both the idle and post-erase checkpoints.
     * The status byte is a projection of cmd_state, not a function of
     * profile, so divergence here would indicate a silent regression in
     * nand_status_byte_from_cmd_state. */
    for (size_t i = 1; i < k_case_count; i++) {
        TEST_ASSERT(idle_status_per_profile[i] == idle_status_per_profile[0],
                    "V3 parity: idle status byte identical across profiles");
        TEST_ASSERT(post_erase_per_profile[i] == post_erase_per_profile[0],
                    "V3 parity: post-erase status byte identical across profiles");
    }

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------------ */
/* V6: reset_policy exposure and behavior per profile.                       */
/*                                                                           */
/* Data-layer: every generic profile currently sets                         */
/* abort_inflight_on_reset=true and preserve_partial_program=false. V6      */
/* asserts this contract so a future profile addition cannot silently       */
/* invert the default.                                                       */
/*                                                                           */
/* Behavior: confirm that reset-in-IDLE succeeds on every profile and       */
/* leaves the die state in IDLE with cmd_state cleared. The heavier         */
/* reset-abort coverage (stamping target pages with the DEAD pattern) is    */
/* in test_cmd_integration_midcomplex::IS-03; V6 only verifies the policy   */
/* surface and the simplest reset path.                                      */
/* ------------------------------------------------------------------------ */

static int test_v6_reset_policy(void)
{
    printf("\n=== V6: reset_policy data-layer + in-idle behavior per profile ===\n");

    for (size_t i = 0; i < k_case_count; i++) {
        const struct profile_case *pc = &k_cases[i];
        const struct nand_profile *prof = nand_profile_get(pc->id);
        TEST_ASSERT(prof != NULL, msg(pc->name, "V6: nand_profile_get resolves"));
        if (!prof) {
            continue;
        }

        TEST_ASSERT(prof->reset_policy.abort_inflight_on_reset == true,
                    msg(pc->name, "V6: reset_policy.abort_inflight_on_reset == true"));
        TEST_ASSERT(prof->reset_policy.preserve_partial_program == false,
                    msg(pc->name, "V6: reset_policy.preserve_partial_program == false"));

        struct media_config cfg = make_cfg(pc);
        struct media_ctx ctx;
        int ret = media_init(&ctx, &cfg);
        TEST_ASSERT(ret == HFSSS_OK, msg(pc->name, "V6: media_init OK"));
        if (ret != HFSSS_OK) {
            continue;
        }

        struct nand_cmd_target t = {.ch = 0, .chip = 0, .die = 0, .plane_mask = 0x1};
        ret = nand_cmd_engine_submit_reset(ctx.nand, &t);
        TEST_ASSERT(ret == HFSSS_OK, msg(pc->name, "V6: reset in IDLE accepted"));

        struct nand_die_cmd_state snap;
        ret = nand_cmd_engine_snapshot(ctx.nand, &t, &snap);
        TEST_ASSERT(ret == HFSSS_OK, msg(pc->name, "V6: post-reset snapshot OK"));
        TEST_ASSERT(snap.state == DIE_IDLE, msg(pc->name, "V6: post-reset state == DIE_IDLE"));
        TEST_ASSERT(snap.suspend_count == 0, msg(pc->name, "V6: post-reset suspend_count == 0"));

        media_cleanup(&ctx);
    }

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

int main(void)
{
    printf("========================================\n");
    printf("NAND Profile Matrix Data-Layer Tests\n");
    printf("========================================\n");

    test_v1_parameter_page_timings();
    test_v2_supported_cmd_bitmap();
    test_v2b_runtime_rejection_on_toggle();
    test_v3_status_byte_semantics();
    test_v4_mp_rules_and_capability();
    test_v6_reset_policy();

    printf("\n========================================\n");
    printf("Profile Matrix Summary\n");
    printf("========================================\n");
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    return tests_failed == 0 ? 0 : 1;
}
