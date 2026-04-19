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

    /* Baseline equality check. The four Phase 6 generic profiles currently
     * share k_default_supported_ops. A future task will diverge ONFI vs
     * Toggle (and TLC vs QLC) bitmaps per their respective protocol
     * definitions, and the four-way equality below will then be replaced by
     * a matrix of expected per-family bitmaps. */
    if (k_case_count >= 2) {
        bool all_equal = true;
        for (size_t i = 1; i < k_case_count; i++) {
            if (observed_bitmaps[i] != observed_bitmaps[0]) {
                all_equal = false;
                break;
            }
        }
        TEST_ASSERT(all_equal,
                    "V2 baseline: all four generic profiles currently share one supported_cmd_bitmap");
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
        TEST_ASSERT(prof->mp_rules.max_planes_per_cmd == 4,
                    msg(pc->name, "V4: mp_rules.max_planes_per_cmd == 4"));
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

int main(void)
{
    printf("========================================\n");
    printf("NAND Profile Matrix Data-Layer Tests\n");
    printf("========================================\n");

    test_v1_parameter_page_timings();
    test_v2_supported_cmd_bitmap();
    test_v4_mp_rules_and_capability();

    printf("\n========================================\n");
    printf("Profile Matrix Summary\n");
    printf("========================================\n");
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    return tests_failed == 0 ? 0 : 1;
}
