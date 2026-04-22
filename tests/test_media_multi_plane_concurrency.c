#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/common.h"
#include "media/cmd_engine.h"
#include "media/eat.h"
#include "media/media.h"
#include "media/nand_profile.h"
#include "media/timing.h"

#define TEST_PASS 0
#define TEST_FAIL 1

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                                                                                         \
    do {                                                                                                               \
        tests_run++;                                                                                                   \
        if (cond) {                                                                                                    \
            printf("  [PASS] %s\n", msg);                                                                              \
            tests_passed++;                                                                                            \
        } else {                                                                                                       \
            printf("  [FAIL] %s\n", msg);                                                                              \
            tests_failed++;                                                                                            \
        }                                                                                                              \
    } while (0)

static u64 wall_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static struct media_config make_cfg(enum nand_profile_id prof)
{
    struct media_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.channel_count = 1;
    cfg.chips_per_channel = 1;
    cfg.dies_per_chip = 1;
    cfg.planes_per_die = 4;
    cfg.blocks_per_plane = 4;
    cfg.pages_per_block = 16;
    cfg.page_size = 4096;
    cfg.spare_size = 128;
    cfg.nand_type = NAND_TYPE_SLC;
    cfg.enable_multi_plane = true;
    cfg.enable_die_interleaving = false;
    cfg.profile_id = prof;
    cfg.profile_explicit = true;
    return cfg;
}

/*
 * Helper: fill a caller-owned buffer with a deterministic pattern keyed to
 * the plane/page index. The multi-plane commit hooks need distinct payloads
 * per plane so we can distinguish them in the read-back path.
 */
static void fill_pattern(void *buf, u32 page_size, u32 seed)
{
    u8 *p = (u8 *)buf;
    for (u32 i = 0; i < page_size; i++) {
        p[i] = (u8)((seed * 31 + i) & 0xFF);
    }
}

/*
 * Core REQ-042 assertion: a multi-plane PROG across planes on the same die
 * completes in wall-clock time close to a single-plane PROG, NOT N times
 * slower. The hardware runs the N planes in parallel within the array-busy
 * window, so tPROG is charged once, not N times.
 */
static int test_multi_plane_prog_parallel_wallclock(void)
{
    printf("\n=== REQ-042: multi-plane PROG parallel wall-clock ===\n");

    struct media_config cfg = make_cfg(NAND_PROFILE_GENERIC_ONFI_TLC);
    struct media_ctx ctx;
    int rc = media_init(&ctx, &cfg);
    TEST_ASSERT(rc == HFSSS_OK, "media_init for 4-plane ONFI TLC profile");

    /* Erase two blocks so plane 0 and plane 1 are program-ready. Multi-plane
     * PROG targets block index B on every plane in the mask; we use block 0
     * on every plane. */
    rc = media_nand_erase(&ctx, 0, 0, 0, 0, 0);
    TEST_ASSERT(rc == HFSSS_OK, "pre-erase plane 0 block 0");
    rc = media_nand_erase(&ctx, 0, 0, 0, 1, 0);
    TEST_ASSERT(rc == HFSSS_OK, "pre-erase plane 1 block 0");
    rc = media_nand_erase(&ctx, 0, 0, 0, 2, 0);
    TEST_ASSERT(rc == HFSSS_OK, "pre-erase plane 2 block 0");
    rc = media_nand_erase(&ctx, 0, 0, 0, 3, 0);
    TEST_ASSERT(rc == HFSSS_OK, "pre-erase plane 3 block 0");

    u8 *buf_p0 = (u8 *)malloc(cfg.page_size);
    u8 *buf_p1 = (u8 *)malloc(cfg.page_size);
    u8 *buf_p2 = (u8 *)malloc(cfg.page_size);
    u8 *buf_p3 = (u8 *)malloc(cfg.page_size);
    TEST_ASSERT(buf_p0 && buf_p1 && buf_p2 && buf_p3, "alloc 4 per-plane buffers");
    fill_pattern(buf_p0, cfg.page_size, 1);
    fill_pattern(buf_p1, cfg.page_size, 2);
    fill_pattern(buf_p2, cfg.page_size, 3);
    fill_pattern(buf_p3, cfg.page_size, 4);

    /* Baseline: single-plane PROG on plane 0, page 0. */
    u64 t0 = wall_ns();
    rc = media_nand_program(&ctx, 0, 0, 0, 0, 0, 0, buf_p0, NULL);
    u64 t1 = wall_ns();
    TEST_ASSERT(rc == HFSSS_OK, "single-plane PROG plane 0");
    u64 single_plane_ns = t1 - t0;

    /* Sequential two single-plane PROGs on different planes (page 1, since
     * plane 0 page 0 is now written). This is the anti-baseline: must take
     * ≈ 2x single-plane. */
    u64 t2 = wall_ns();
    rc = media_nand_program(&ctx, 0, 0, 0, 0, 0, 1, buf_p0, NULL);
    TEST_ASSERT(rc == HFSSS_OK, "sequential plane 0 page 1 PROG");
    rc = media_nand_program(&ctx, 0, 0, 0, 1, 0, 1, buf_p1, NULL);
    TEST_ASSERT(rc == HFSSS_OK, "sequential plane 1 page 1 PROG");
    u64 t3 = wall_ns();
    u64 sequential_two_ns = t3 - t2;

    /* Multi-plane two-plane PROG, page 2 this time. This is the primary
     * assertion: ≈ 1x single-plane, NOT 2x. */
    const void *data_mp2[2] = {buf_p0, buf_p1};
    u64 t4 = wall_ns();
    rc = media_nand_multi_plane_program(&ctx, 0, 0, 0, 0x3, 0, 2, data_mp2, NULL);
    u64 t5 = wall_ns();
    TEST_ASSERT(rc == HFSSS_OK, "2-plane multi-plane PROG page 2");
    u64 mp2_ns = t5 - t4;

    /* Multi-plane four-plane PROG on page 3. ONFI profile caps at 4 planes,
     * so this is the widest legal mask. Must remain ≈ 1x single-plane. */
    const void *data_mp4[4] = {buf_p0, buf_p1, buf_p2, buf_p3};
    u64 t6 = wall_ns();
    rc = media_nand_multi_plane_program(&ctx, 0, 0, 0, 0xF, 0, 3, data_mp4, NULL);
    u64 t7 = wall_ns();
    TEST_ASSERT(rc == HFSSS_OK, "4-plane multi-plane PROG page 3");
    u64 mp4_ns = t7 - t6;

    printf("  timing: single=%llu ns, 2x-seq=%llu ns, mp2=%llu ns, mp4=%llu ns\n",
           (unsigned long long)single_plane_ns, (unsigned long long)sequential_two_ns,
           (unsigned long long)mp2_ns, (unsigned long long)mp4_ns);

    /* Sequential two ops must be close to 2x a single op. Tolerate 30%
     * wander — the NAND timing engine busy-waits, so wall-clock closely
     * tracks the modeled budget, but test machines add jitter. */
    TEST_ASSERT(sequential_two_ns >= (single_plane_ns * 18 / 10),
                "sequential 2 single-plane PROGs ≈ 2x single baseline (≥1.8x)");

    /* Multi-plane 2-plane PROG must be much closer to 1x than 2x. Set the
     * cap at 1.5x single to catch the case where the engine charges tPROG
     * once per plane. */
    TEST_ASSERT(mp2_ns <= (single_plane_ns * 15 / 10),
                "2-plane multi-plane PROG ≤ 1.5x single baseline (parallel timing)");

    /* Multi-plane 4-plane PROG same guarantee. Profile caps at 4 planes so
     * this is the full-width case. */
    TEST_ASSERT(mp4_ns <= (single_plane_ns * 15 / 10),
                "4-plane multi-plane PROG ≤ 1.5x single baseline (parallel timing)");

    /* Strongest assertion: mp2 and sequential-two must not be within 20% of
     * each other. If they are, the engine is really running planes in series. */
    TEST_ASSERT(mp2_ns * 12 / 10 < sequential_two_ns,
                "2-plane multi-plane strictly faster than sequential 2x single");

    free(buf_p0);
    free(buf_p1);
    free(buf_p2);
    free(buf_p3);
    media_cleanup(&ctx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/*
 * After a 2-plane PROG, both targeted planes' EAT advance to the same
 * future time, and non-targeted planes on the same die stay unmoved.
 * Confirms per-plane EAT independence (the data-level half of REQ-042).
 */
static int test_multi_plane_eat_independence(void)
{
    printf("\n=== REQ-042: per-plane EAT independence ===\n");

    struct media_config cfg = make_cfg(NAND_PROFILE_GENERIC_ONFI_TLC);
    struct media_ctx ctx;
    int rc = media_init(&ctx, &cfg);
    TEST_ASSERT(rc == HFSSS_OK, "media_init OK");

    /* Pre-erase plane 0 and plane 1 on block 0 so PROG can land. */
    rc = media_nand_erase(&ctx, 0, 0, 0, 0, 0);
    TEST_ASSERT(rc == HFSSS_OK, "erase plane 0 block 0");
    rc = media_nand_erase(&ctx, 0, 0, 0, 1, 0);
    TEST_ASSERT(rc == HFSSS_OK, "erase plane 1 block 0");

    /* Snapshot baseline EAT for each plane. Erase advanced plane 0 and 1,
     * but 2 and 3 should remain untouched. */
    u64 plane2_before = eat_get_for_plane(ctx.eat, 0, 0, 0, 2);
    u64 plane3_before = eat_get_for_plane(ctx.eat, 0, 0, 0, 3);

    u8 *buf = (u8 *)malloc(cfg.page_size);
    fill_pattern(buf, cfg.page_size, 7);
    const void *data_mp[2] = {buf, buf};
    rc = media_nand_multi_plane_program(&ctx, 0, 0, 0, 0x3, 0, 0, data_mp, NULL);
    TEST_ASSERT(rc == HFSSS_OK, "2-plane PROG page 0");

    u64 plane0_eat = eat_get_for_plane(ctx.eat, 0, 0, 0, 0);
    u64 plane1_eat = eat_get_for_plane(ctx.eat, 0, 0, 0, 1);
    u64 plane2_after = eat_get_for_plane(ctx.eat, 0, 0, 0, 2);
    u64 plane3_after = eat_get_for_plane(ctx.eat, 0, 0, 0, 3);

    TEST_ASSERT(plane0_eat > 0 && plane1_eat > 0, "both targeted planes have non-zero EAT");
    TEST_ASSERT(plane0_eat == plane1_eat, "both targeted planes share the same EAT timestamp");
    TEST_ASSERT(plane2_after == plane2_before, "untargeted plane 2 EAT unchanged");
    TEST_ASSERT(plane3_after == plane3_before, "untargeted plane 3 EAT unchanged");

    free(buf);
    media_cleanup(&ctx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/*
 * Profile-level plane cap enforcement. ONFI profile caps at 4 planes; any
 * mask with more bits must be rejected. Toggle profile caps at 2 planes;
 * a 3+ bit mask must be rejected. Uses NOTSUPP when enable_multi_plane is
 * off or INVAL when the mask exceeds the profile cap.
 */
static int test_profile_plane_cap_enforcement(void)
{
    printf("\n=== REQ-042: profile-level plane cap enforcement ===\n");

    /* ONFI profile caps at 4 planes. We can't test >4 because geometry
     * tops out at 4 in this config, so instead confirm 4-plane is
     * accepted. Toggle profile caps at 2 — 3+ bits must be rejected. */
    struct media_config cfg_onfi = make_cfg(NAND_PROFILE_GENERIC_ONFI_TLC);
    struct media_ctx ctx_onfi;
    int rc = media_init(&ctx_onfi, &cfg_onfi);
    TEST_ASSERT(rc == HFSSS_OK, "media_init ONFI TLC OK");

    /* Pre-erase four planes on block 0. */
    for (u32 p = 0; p < 4; p++) {
        TEST_ASSERT(media_nand_erase(&ctx_onfi, 0, 0, 0, p, 0) == HFSSS_OK, "erase ONFI plane block 0");
    }

    u8 *b = (u8 *)malloc(cfg_onfi.page_size);
    fill_pattern(b, cfg_onfi.page_size, 11);
    const void *data4[4] = {b, b, b, b};
    rc = media_nand_multi_plane_program(&ctx_onfi, 0, 0, 0, 0xF, 0, 0, data4, NULL);
    TEST_ASSERT(rc == HFSSS_OK, "ONFI: 4-plane PROG accepted (within cap)");
    free(b);
    media_cleanup(&ctx_onfi);

    /* Toggle profile caps at 2 planes per cmd. A 3-plane mask must be
     * rejected with INVAL. */
    struct media_config cfg_toggle = make_cfg(NAND_PROFILE_GENERIC_TOGGLE_TLC);
    struct media_ctx ctx_toggle;
    rc = media_init(&ctx_toggle, &cfg_toggle);
    TEST_ASSERT(rc == HFSSS_OK, "media_init Toggle TLC OK");

    for (u32 p = 0; p < 4; p++) {
        TEST_ASSERT(media_nand_erase(&ctx_toggle, 0, 0, 0, p, 0) == HFSSS_OK, "erase Toggle plane block 0");
    }

    u8 *b2 = (u8 *)malloc(cfg_toggle.page_size);
    fill_pattern(b2, cfg_toggle.page_size, 13);
    const void *data3[3] = {b2, b2, b2};
    rc = media_nand_multi_plane_program(&ctx_toggle, 0, 0, 0, 0x7, 0, 0, data3, NULL);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "Toggle: 3-plane PROG rejected (exceeds cap=2)");

    const void *data2[2] = {b2, b2};
    rc = media_nand_multi_plane_program(&ctx_toggle, 0, 0, 0, 0x3, 0, 0, data2, NULL);
    TEST_ASSERT(rc == HFSSS_OK, "Toggle: 2-plane PROG accepted (at cap)");
    free(b2);
    media_cleanup(&ctx_toggle);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/*
 * Multi-plane READ path. The engine does not drain wall-clock for READ
 * (array-busy spin runs only for PROG/ERASE), so READ parallelism shows
 * up in per-plane EAT deltas rather than wall-clock. To keep the delta
 * comparison honest, both planes' pre-read EATs are first aligned by
 * doing the pre-program via a single multi-plane PROG op — that way the
 * two targeted planes start the READ from the same EAT floor and any
 * skew in their deltas is attributable to the READ charge itself.
 */
static int test_multi_plane_read_eat_and_payload(void)
{
    printf("\n=== REQ-042: multi-plane READ per-plane EAT + payload ===\n");

    struct media_config cfg = make_cfg(NAND_PROFILE_GENERIC_ONFI_TLC);
    struct media_ctx ctx;
    int rc = media_init(&ctx, &cfg);
    TEST_ASSERT(rc == HFSSS_OK, "media_init OK");

    /* Erase planes 0 and 1 block 0. Multi-plane PROG into the same
     * block aligns both planes' EATs exactly — they are charged the
     * same stage_ns from the same current_time inside the engine. */
    u8 *src = (u8 *)malloc(cfg.page_size);
    fill_pattern(src, cfg.page_size, 17);
    TEST_ASSERT(media_nand_erase(&ctx, 0, 0, 0, 0, 0) == HFSSS_OK, "erase plane 0 block 0");
    TEST_ASSERT(media_nand_erase(&ctx, 0, 0, 0, 1, 0) == HFSSS_OK, "erase plane 1 block 0");

    const void *src_mp[2] = {src, src};
    TEST_ASSERT(media_nand_multi_plane_program(&ctx, 0, 0, 0, 0x3, 0, 0, src_mp, NULL) == HFSSS_OK,
                "multi-plane PROG aligns both plane EATs");

    /* Confirm the alignment landed. If it didn't, the delta comparison
     * below loses meaning — fail loudly instead of silently passing. */
    u64 plane0_before = eat_get_for_plane(ctx.eat, 0, 0, 0, 0);
    u64 plane1_before = eat_get_for_plane(ctx.eat, 0, 0, 0, 1);
    TEST_ASSERT(plane0_before == plane1_before, "pre-read EATs aligned via multi-plane PROG");

    /* Multi-plane 2-plane READ. */
    u8 *r1 = (u8 *)malloc(cfg.page_size);
    u8 *r2 = (u8 *)malloc(cfg.page_size);
    void *data_mp2[2] = {r1, r2};
    rc = media_nand_multi_plane_read(&ctx, 0, 0, 0, 0x3, 0, 0, data_mp2, NULL);
    TEST_ASSERT(rc == HFSSS_OK, "2-plane multi-plane READ");

    u64 plane0_after = eat_get_for_plane(ctx.eat, 0, 0, 0, 0);
    u64 plane1_after = eat_get_for_plane(ctx.eat, 0, 0, 0, 1);
    u64 plane0_delta = plane0_after - plane0_before;
    u64 plane1_delta = plane1_after - plane1_before;

    printf("  read EAT deltas: plane0=%llu ns, plane1=%llu ns\n", (unsigned long long)plane0_delta,
           (unsigned long long)plane1_delta);

    TEST_ASSERT(plane0_after > plane0_before, "2-plane READ advances plane 0 EAT");
    TEST_ASSERT(plane1_after > plane1_before, "2-plane READ advances plane 1 EAT");
    TEST_ASSERT(plane0_after == plane1_after,
                "both targeted planes share the same post-read EAT (parallel charge)");
    TEST_ASSERT(plane0_delta == plane1_delta,
                "both planes charged the exact same READ delta (proves parallel, not serial)");

    /* Verify payload fidelity: each plane got the seed-17 pattern back. */
    TEST_ASSERT(memcmp(r1, src, cfg.page_size) == 0, "multi-plane read plane 0 payload matches");
    TEST_ASSERT(memcmp(r2, src, cfg.page_size) == 0, "multi-plane read plane 1 payload matches");

    free(src);
    free(r1);
    free(r2);
    media_cleanup(&ctx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

int main(void)
{
    printf("========================================\n");
    printf("   Multi-Plane Concurrency (REQ-042)    \n");
    printf("========================================\n");

    test_multi_plane_prog_parallel_wallclock();
    test_multi_plane_eat_independence();
    test_profile_plane_cap_enforcement();
    test_multi_plane_read_eat_and_payload();

    printf("\n========================================\n");
    printf("Tests run:    %d\n", tests_run);
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);
    printf("========================================\n");

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}
