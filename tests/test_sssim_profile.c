/*
 * sssim-level profile selection tests.
 *
 * Validates that sssim_config carries the Phase 6 profile selection
 * fields end-to-end and that sssim_init resolves the active profile
 * the same way media_init does when consumed directly. Also covers
 * the short-name CLI lookup used by hfsss-nbd-server.
 *
 * Back-compat contract under test: zero-initialized sssim_config
 * (profile_explicit=false) falls back to nand_profile_get_default_for_type,
 * so existing callers observe no behavior change.
 */

#include <stdio.h>
#include <string.h>

#include "common/common.h"
#include "ftl/ftl.h"
#include "media/nand_profile.h"
#include "sssim.h"

#define TEST_PASS 0
#define TEST_FAIL 1

#define CHECK(cond, msg)                                                       \
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

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

/*
 * Minimal geometry that keeps sssim_init cheap while still exercising
 * the full media + HAL + FTL stack. Picks a capacity small enough that
 * FTL setup does not allocate huge bookkeeping tables.
 */
static void small_config(struct sssim_config *cfg, enum nand_type type)
{
    sssim_config_default(cfg);
    cfg->nand_type = type;
    cfg->page_size = 4096;
    cfg->spare_size = 64;
    cfg->pages_per_block = 8;
    cfg->blocks_per_plane = 16;
    cfg->planes_per_die = 2;
    cfg->dies_per_chip = 1;
    cfg->chips_per_channel = 1;
    cfg->channel_count = 1;
    cfg->lba_size = 4096;
    cfg->total_lbas = 64;
    cfg->op_ratio = 20;
}

static int run_case(const char *label, enum nand_type type, bool explicit_profile, enum nand_profile_id pid,
                    enum nand_profile_id expected)
{
    struct sssim_ctx ctx;
    struct sssim_config cfg;

    small_config(&cfg, type);
    cfg.profile_explicit = explicit_profile;
    cfg.profile_id = pid;

    int ret = sssim_init(&ctx, &cfg);
    if (ret != HFSSS_OK) {
        printf("  [FAIL] %s: sssim_init returned %d\n", label, ret);
        tests_run++;
        tests_failed++;
        return TEST_FAIL;
    }

    const struct nand_profile *got = ftl_get_profile(&ctx.ftl);
    const struct nand_profile *want = nand_profile_get(expected);
    CHECK(got != NULL, label);
    CHECK(got == want, label);

    sssim_cleanup(&ctx);
    return TEST_PASS;
}

static int test_default_tlc_back_compat(void)
{
    printf("\n--- default sssim_config (TLC, no explicit profile) ---\n");
    return run_case("default TLC -> ONFI TLC", NAND_TYPE_TLC, false, NAND_PROFILE_GENERIC_ONFI_TLC,
                    NAND_PROFILE_GENERIC_ONFI_TLC);
}

static int test_default_qlc_back_compat(void)
{
    printf("\n--- nand_type=QLC alone (no explicit profile) ---\n");
    return run_case("QLC type -> ONFI QLC", NAND_TYPE_QLC, false, 0, NAND_PROFILE_GENERIC_ONFI_QLC);
}

static int test_explicit_toggle_tlc(void)
{
    printf("\n--- explicit TOGGLE_TLC overrides nand_type ---\n");
    return run_case("explicit TOGGLE_TLC", NAND_TYPE_TLC, true, NAND_PROFILE_GENERIC_TOGGLE_TLC,
                    NAND_PROFILE_GENERIC_TOGGLE_TLC);
}

static int test_explicit_toggle_qlc(void)
{
    printf("\n--- explicit TOGGLE_QLC ---\n");
    return run_case("explicit TOGGLE_QLC", NAND_TYPE_QLC, true, NAND_PROFILE_GENERIC_TOGGLE_QLC,
                    NAND_PROFILE_GENERIC_TOGGLE_QLC);
}

static int test_name_lookup(void)
{
    printf("\n--- nand_profile_id_from_name short-name lookup ---\n");
    CHECK(nand_profile_id_from_name("onfi-tlc") == NAND_PROFILE_GENERIC_ONFI_TLC, "onfi-tlc");
    CHECK(nand_profile_id_from_name("onfi-qlc") == NAND_PROFILE_GENERIC_ONFI_QLC, "onfi-qlc");
    CHECK(nand_profile_id_from_name("toggle-tlc") == NAND_PROFILE_GENERIC_TOGGLE_TLC, "toggle-tlc");
    CHECK(nand_profile_id_from_name("toggle-qlc") == NAND_PROFILE_GENERIC_TOGGLE_QLC, "toggle-qlc");
    CHECK(nand_profile_id_from_name("bogus") == NAND_PROFILE_COUNT, "unknown -> COUNT sentinel");
    CHECK(nand_profile_id_from_name(NULL) == NAND_PROFILE_COUNT, "NULL -> COUNT sentinel");
    return TEST_PASS;
}

/*
 * nand_profile_name is the inverse used for round-trippable log output:
 * banners, nvme vendor-log synthesis, and diagnostics all want the CLI
 * short alias rather than the internal id_string.
 */
static int test_name_roundtrip(void)
{
    printf("\n--- nand_profile_name reverse lookup ---\n");
    const char *tlc = nand_profile_name(NAND_PROFILE_GENERIC_ONFI_TLC);
    const char *qlc = nand_profile_name(NAND_PROFILE_GENERIC_ONFI_QLC);
    const char *ttlc = nand_profile_name(NAND_PROFILE_GENERIC_TOGGLE_TLC);
    const char *tqlc = nand_profile_name(NAND_PROFILE_GENERIC_TOGGLE_QLC);

    CHECK(tlc && strcmp(tlc, "onfi-tlc") == 0, "ONFI TLC -> onfi-tlc");
    CHECK(qlc && strcmp(qlc, "onfi-qlc") == 0, "ONFI QLC -> onfi-qlc");
    CHECK(ttlc && strcmp(ttlc, "toggle-tlc") == 0, "TOGGLE TLC -> toggle-tlc");
    CHECK(tqlc && strcmp(tqlc, "toggle-qlc") == 0, "TOGGLE QLC -> toggle-qlc");
    CHECK(nand_profile_name((enum nand_profile_id)NAND_PROFILE_COUNT) == NULL, "invalid id -> NULL");

    /* Round-trip through both directions to catch table drift. */
    CHECK(nand_profile_id_from_name(tlc) == NAND_PROFILE_GENERIC_ONFI_TLC, "round-trip onfi-tlc");
    CHECK(nand_profile_id_from_name(ttlc) == NAND_PROFILE_GENERIC_TOGGLE_TLC, "round-trip toggle-tlc");
    return TEST_PASS;
}

int main(void)
{
    printf("=== sssim profile selection tests ===\n");

    test_default_tlc_back_compat();
    test_default_qlc_back_compat();
    test_explicit_toggle_tlc();
    test_explicit_toggle_qlc();
    test_name_lookup();
    test_name_roundtrip();

    printf("\n=== Summary: run=%d pass=%d fail=%d ===\n", tests_run, tests_passed, tests_failed);
    return (tests_failed == 0) ? 0 : 1;
}
