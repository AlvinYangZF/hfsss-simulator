/*
 * FTL profile consumer tests (Phase 7 consumer-layer PR).
 *
 * Validates that the FTL layer picks up the NAND profile attached to
 * the media at ftl_init, exposes it through the new query API, and
 * continues to service read/write/erase correctly under both ONFI
 * and Toggle profiles.
 *
 * Scope is deliberately small: thread + query + regression. FTL does
 * not yet use profile information to change its dispatch strategy
 * (that would be a behavioral PR on top of this thread PR).
 */

#include <stdio.h>
#include <string.h>

#include "common/common.h"
#include "ftl/ftl.h"
#include "hal/hal.h"
#include "media/cmd_state.h"
#include "media/media.h"
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

/*
 * Stand up the full media + HAL + FTL stack for a single profile. Caller
 * owns every struct so cleanup is straightforward. Returns HFSSS_OK only
 * when every layer initialized cleanly; partial setups are rolled back
 * by walking backwards through the visible initialized flags.
 */
struct stack {
    struct media_ctx media;
    struct media_config mc;
    struct hal_nand_dev nand;
    struct hal_ctx hal;
    struct ftl_ctx ftl;
    struct ftl_config fc;
};

static void stack_cfg(struct stack *s, enum nand_profile_id pid, enum nand_type type)
{
    memset(s, 0, sizeof(*s));
    s->mc.channel_count = 1;
    s->mc.chips_per_channel = 1;
    s->mc.dies_per_chip = 1;
    s->mc.planes_per_die = 2;
    s->mc.blocks_per_plane = 16;
    s->mc.pages_per_block = 8;
    s->mc.page_size = 4096;
    s->mc.spare_size = 64;
    s->mc.nand_type = type;
    s->mc.profile_explicit = true;
    s->mc.profile_id = pid;

    s->fc.total_lbas = 64;
    s->fc.page_size = s->mc.page_size;
    s->fc.pages_per_block = s->mc.pages_per_block;
    s->fc.blocks_per_plane = s->mc.blocks_per_plane;
    s->fc.planes_per_die = s->mc.planes_per_die;
    s->fc.dies_per_chip = s->mc.dies_per_chip;
    s->fc.chips_per_channel = s->mc.chips_per_channel;
    s->fc.channel_count = s->mc.channel_count;
    s->fc.op_ratio = 10;
    s->fc.gc_policy = GC_POLICY_GREEDY;
    s->fc.gc_threshold = 5;
    s->fc.gc_hiwater = 10;
    s->fc.gc_lowater = 2;
}

static int stack_bring_up(struct stack *s)
{
    int ret = media_init(&s->media, &s->mc);
    if (ret != HFSSS_OK) {
        return ret;
    }
    ret = hal_nand_dev_init(&s->nand, s->mc.channel_count, s->mc.chips_per_channel, s->mc.dies_per_chip,
                            s->mc.planes_per_die, s->mc.blocks_per_plane, s->mc.pages_per_block, s->mc.page_size,
                            s->mc.spare_size, &s->media);
    if (ret != HFSSS_OK) {
        media_cleanup(&s->media);
        return ret;
    }
    ret = hal_init(&s->hal, &s->nand);
    if (ret != HFSSS_OK) {
        hal_nand_dev_cleanup(&s->nand);
        media_cleanup(&s->media);
        return ret;
    }
    ret = ftl_init(&s->ftl, &s->fc, &s->hal);
    if (ret != HFSSS_OK) {
        hal_cleanup(&s->hal);
        hal_nand_dev_cleanup(&s->nand);
        media_cleanup(&s->media);
        return ret;
    }
    return HFSSS_OK;
}

static void stack_tear_down(struct stack *s)
{
    if (s->ftl.initialized) {
        ftl_cleanup(&s->ftl);
    }
    if (s->hal.initialized) {
        hal_cleanup(&s->hal);
    }
    hal_nand_dev_cleanup(&s->nand);
    if (s->media.initialized) {
        media_cleanup(&s->media);
    }
}

/* ------------------------------------------------------------------------ */
/* Test 1: profile threads through ftl_init for ONFI and Toggle media       */
/* ------------------------------------------------------------------------ */

static int test_profile_thread(void)
{
    printf("\n=== FTL profile thread ===\n");

    static const struct {
        enum nand_profile_id pid;
        enum nand_type type;
        const char *name;
        enum nand_interface_family fam;
    } cases[] = {
        {NAND_PROFILE_GENERIC_ONFI_TLC, NAND_TYPE_TLC, "onfi_tlc", NAND_IF_ONFI},
        {NAND_PROFILE_GENERIC_ONFI_QLC, NAND_TYPE_QLC, "onfi_qlc", NAND_IF_ONFI},
        {NAND_PROFILE_GENERIC_TOGGLE_TLC, NAND_TYPE_TLC, "toggle_tlc", NAND_IF_TOGGLE_EQ},
        {NAND_PROFILE_GENERIC_TOGGLE_QLC, NAND_TYPE_QLC, "toggle_qlc", NAND_IF_TOGGLE_EQ},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct stack s;
        stack_cfg(&s, cases[i].pid, cases[i].type);
        int ret = stack_bring_up(&s);
        char tag[128];
        snprintf(tag, sizeof(tag), "[%s] stack bring-up", cases[i].name);
        TEST_ASSERT(ret == HFSSS_OK, tag);
        if (ret != HFSSS_OK) {
            continue;
        }

        /* HAL bridge returns the media profile. */
        const struct nand_profile *hal_p = hal_get_profile(&s.hal);
        snprintf(tag, sizeof(tag), "[%s] hal_get_profile != NULL", cases[i].name);
        TEST_ASSERT(hal_p != NULL, tag);
        snprintf(tag, sizeof(tag), "[%s] hal_get_profile.id matches config", cases[i].name);
        TEST_ASSERT(hal_p && hal_p->id == cases[i].pid, tag);

        /* FTL stored the same pointer at init. */
        const struct nand_profile *ftl_p = ftl_get_profile(&s.ftl);
        snprintf(tag, sizeof(tag), "[%s] ftl_get_profile == hal_get_profile", cases[i].name);
        TEST_ASSERT(ftl_p == hal_p, tag);

        /* Interface family is the observable divergence axis. */
        snprintf(tag, sizeof(tag), "[%s] interface_family matches expectation", cases[i].name);
        TEST_ASSERT(ftl_p && ftl_p->interface_family == cases[i].fam, tag);

        stack_tear_down(&s);
    }

    /* Defensive: NULL ctx returns NULL without crashing. */
    TEST_ASSERT(ftl_get_profile(NULL) == NULL, "ftl_get_profile(NULL) == NULL");
    TEST_ASSERT(hal_get_profile(NULL) == NULL, "hal_get_profile(NULL) == NULL");

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------------ */
/* Test 2: ftl_preferred_plane_batch honors per-family cap                  */
/* ------------------------------------------------------------------------ */

static int test_preferred_plane_batch(void)
{
    printf("\n=== FTL preferred_plane_batch ===\n");

    /* ONFI profile: cap 4. */
    {
        struct stack s;
        stack_cfg(&s, NAND_PROFILE_GENERIC_ONFI_TLC, NAND_TYPE_TLC);
        int ret = stack_bring_up(&s);
        TEST_ASSERT(ret == HFSSS_OK, "onfi: bring-up");
        if (ret == HFSSS_OK) {
            TEST_ASSERT(ftl_preferred_plane_batch(&s.ftl) == 4,
                        "onfi: ftl_preferred_plane_batch == 4");
            stack_tear_down(&s);
        }
    }

    /* Toggle profile: cap 2. */
    {
        struct stack s;
        stack_cfg(&s, NAND_PROFILE_GENERIC_TOGGLE_TLC, NAND_TYPE_TLC);
        int ret = stack_bring_up(&s);
        TEST_ASSERT(ret == HFSSS_OK, "toggle: bring-up");
        if (ret == HFSSS_OK) {
            TEST_ASSERT(ftl_preferred_plane_batch(&s.ftl) == 2,
                        "toggle: ftl_preferred_plane_batch == 2");
            stack_tear_down(&s);
        }
    }

    /* NULL / no-profile fallback: returns 1 (sequential). */
    TEST_ASSERT(ftl_preferred_plane_batch(NULL) == 1,
                "null ctx: preferred_plane_batch falls back to 1");

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------------ */
/* Test 3: ftl_supports_op matches the per-family bitmap                    */
/* ------------------------------------------------------------------------ */

static int test_supports_op(void)
{
    printf("\n=== FTL supports_op ===\n");

    {
        struct stack s;
        stack_cfg(&s, NAND_PROFILE_GENERIC_ONFI_TLC, NAND_TYPE_TLC);
        if (stack_bring_up(&s) == HFSSS_OK) {
            TEST_ASSERT(ftl_supports_op(&s.ftl, NAND_OP_READ), "onfi: READ supported");
            TEST_ASSERT(ftl_supports_op(&s.ftl, NAND_OP_PROG), "onfi: PROG supported");
            TEST_ASSERT(ftl_supports_op(&s.ftl, NAND_OP_ERASE), "onfi: ERASE supported");
            TEST_ASSERT(ftl_supports_op(&s.ftl, NAND_OP_RESET), "onfi: RESET supported");
            TEST_ASSERT(ftl_supports_op(&s.ftl, NAND_OP_READ_STATUS), "onfi: READ_STATUS supported");
            TEST_ASSERT(ftl_supports_op(&s.ftl, NAND_OP_READ_STATUS_ENHANCED),
                        "onfi: READ_STATUS_ENHANCED supported (ONFI 3.5 §5.17)");
            stack_tear_down(&s);
        }
    }

    {
        struct stack s;
        stack_cfg(&s, NAND_PROFILE_GENERIC_TOGGLE_TLC, NAND_TYPE_TLC);
        if (stack_bring_up(&s) == HFSSS_OK) {
            TEST_ASSERT(ftl_supports_op(&s.ftl, NAND_OP_READ), "toggle: READ supported");
            TEST_ASSERT(ftl_supports_op(&s.ftl, NAND_OP_PROG), "toggle: PROG supported");
            TEST_ASSERT(ftl_supports_op(&s.ftl, NAND_OP_READ_STATUS), "toggle: classic READ_STATUS supported");
            TEST_ASSERT(ftl_supports_op(&s.ftl, NAND_OP_READ_STATUS_ENHANCED) == false,
                        "toggle: READ_STATUS_ENHANCED not supported");
            stack_tear_down(&s);
        }
    }

    /* NULL / no-profile fallback: assume supported for backward compat. */
    TEST_ASSERT(ftl_supports_op(NULL, NAND_OP_READ), "null ctx: supports_op falls back to true");

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------------ */
/* Test 4: write / read round-trip is profile-agnostic                      */
/*                                                                           */
/* FTL currently issues only READ/PROG/ERASE, all in the core bitmap, so    */
/* neither family should regress. Asserts parity: the same LBA/pattern      */
/* round-trips through ONFI and Toggle identically.                         */
/* ------------------------------------------------------------------------ */

static int test_write_read_parity(void)
{
    printf("\n=== FTL write/read parity across profiles ===\n");

    static const struct {
        enum nand_profile_id pid;
        enum nand_type type;
        const char *name;
        u8 fill;
    } cases[] = {
        {NAND_PROFILE_GENERIC_ONFI_TLC, NAND_TYPE_TLC, "onfi_tlc", 0xAA},
        {NAND_PROFILE_GENERIC_TOGGLE_TLC, NAND_TYPE_TLC, "toggle_tlc", 0xAA},
        {NAND_PROFILE_GENERIC_ONFI_QLC, NAND_TYPE_QLC, "onfi_qlc", 0x55},
        {NAND_PROFILE_GENERIC_TOGGLE_QLC, NAND_TYPE_QLC, "toggle_qlc", 0x55},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct stack s;
        stack_cfg(&s, cases[i].pid, cases[i].type);
        int ret = stack_bring_up(&s);
        char tag[128];
        snprintf(tag, sizeof(tag), "[%s] bring-up", cases[i].name);
        TEST_ASSERT(ret == HFSSS_OK, tag);
        if (ret != HFSSS_OK) {
            continue;
        }

        u8 wr[4096];
        u8 rd[4096];
        memset(wr, cases[i].fill, sizeof(wr));
        ret = ftl_write(&s.ftl, 7, 4096, wr);
        snprintf(tag, sizeof(tag), "[%s] ftl_write", cases[i].name);
        TEST_ASSERT(ret == HFSSS_OK, tag);

        memset(rd, 0, sizeof(rd));
        ret = ftl_read(&s.ftl, 7, 4096, rd);
        snprintf(tag, sizeof(tag), "[%s] ftl_read", cases[i].name);
        TEST_ASSERT(ret == HFSSS_OK, tag);

        snprintf(tag, sizeof(tag), "[%s] round-trip first byte matches", cases[i].name);
        TEST_ASSERT(rd[0] == cases[i].fill, tag);
        snprintf(tag, sizeof(tag), "[%s] round-trip last byte matches", cases[i].name);
        TEST_ASSERT(rd[4095] == cases[i].fill, tag);

        stack_tear_down(&s);
    }

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

int main(void)
{
    printf("========================================\n");
    printf("FTL Profile Consumer Tests\n");
    printf("========================================\n");

    test_profile_thread();
    test_preferred_plane_batch();
    test_supports_op();
    test_write_read_parity();

    printf("\n========================================\n");
    printf("FTL Profile Tests Summary\n");
    printf("========================================\n");
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    return tests_failed == 0 ? 0 : 1;
}
