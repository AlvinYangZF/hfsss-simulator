/*
 * REQ-121: NAND latency error (jitter) tests.
 *
 * Covers the timing_model_enable_jitter API:
 *   - Default state is disabled (deterministic baseline).
 *   - Same seed produces the same sequence across two model instances.
 *   - Every sample sits in [base - bp_frac*base, base + bp_frac*base].
 *   - Mean of many samples approaches the baseline within a tight band.
 *   - disable_jitter zeroes both the factor and the state, restoring
 *     the deterministic return.
 *   - Ceiling clamp: basis_points > TIMING_JITTER_MAX_BP is clamped,
 *     not silently expanded.
 *
 * Notes:
 * - All tests drive a fresh single-threaded timing_model so the
 *   sequence is reproducible from a given seed. The underlying impl
 *   uses atomic CAS for multi-thread safety; cross-thread
 *   reproducibility is not a contract and is not tested.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdint.h>

#include "media/timing.h"
#include "common/common.h"

static int g_pass = 0;
static int g_fail = 0;

#define TEST_ASSERT(cond, msg) do {                           \
    if (cond) { printf("  [PASS] %s\n", msg); g_pass++; }     \
    else      { printf("  [FAIL] %s  (line %d)\n",            \
                       msg, __LINE__); g_fail++; }            \
} while (0)

/* ---------- Test 1: default is disabled ---------- */
static void test_jitter_disabled_by_default(void)
{
    printf("\n=== Jitter disabled by default ===\n");
    struct timing_model m;
    int rc = timing_model_init(&m, NAND_TYPE_TLC);
    TEST_ASSERT(rc == HFSSS_OK, "init OK");

    u64 base = timing_get_read_latency(&m, 0);
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT(timing_get_read_latency(&m, 0) == base,
                    "read latency is deterministic with jitter off");
        if (g_fail) break;
    }

    timing_model_cleanup(&m);
}

/* ---------- Test 2: bounded within ±basis_points ---------- */
static void test_jitter_bounded(void)
{
    printf("\n=== Jitter stays within ±basis_points ===\n");
    struct timing_model m;
    timing_model_init(&m, NAND_TYPE_TLC);

    /* Capture deterministic baselines BEFORE arming jitter. */
    u64 base_r0 = timing_get_read_latency(&m, 0);
    u64 base_r1 = timing_get_read_latency(&m, 1);
    u64 base_r2 = timing_get_read_latency(&m, 2);
    u64 base_p0 = timing_get_prog_latency(&m, 0);
    u64 base_e  = timing_get_erase_latency(&m);

    u32 bp   = 500;  /* ±5% */
    u64 lo_r0 = base_r0 - (base_r0 * bp) / 10000;
    u64 hi_r0 = base_r0 + (base_r0 * bp) / 10000;
    u64 lo_p0 = base_p0 - (base_p0 * bp) / 10000;
    u64 hi_p0 = base_p0 + (base_p0 * bp) / 10000;
    u64 lo_e  = base_e  - (base_e  * bp) / 10000;
    u64 hi_e  = base_e  + (base_e  * bp) / 10000;
    (void)base_r1; (void)base_r2;

    timing_model_enable_jitter(&m, bp, 0xDEADBEEFULL);

    int r_out = 0, p_out = 0, e_out = 0;
    for (int i = 0; i < 2000; i++) {
        u64 r = timing_get_read_latency(&m, (u32)i);
        u64 p = timing_get_prog_latency(&m, 0);
        u64 e = timing_get_erase_latency(&m);
        /* Allow ±1 ns for integer-division rounding in the clamp arith. */
        if (!(r >= lo_r0 - 1 && r <= hi_r0 + 1) &&
            !(r >= base_r1 - (base_r1*bp)/10000 - 1 &&
              r <= base_r1 + (base_r1*bp)/10000 + 1) &&
            !(r >= base_r2 - (base_r2*bp)/10000 - 1 &&
              r <= base_r2 + (base_r2*bp)/10000 + 1)) {
            r_out++;
        }
        if (p < lo_p0 - 1 || p > hi_p0 + 1) p_out++;
        if (e < lo_e  - 1 || e > hi_e  + 1) e_out++;
    }

    TEST_ASSERT(r_out == 0, "read latency samples all within ±5%");
    TEST_ASSERT(p_out == 0, "program latency samples all within ±5%");
    TEST_ASSERT(e_out == 0, "erase latency samples all within ±5%");

    timing_model_cleanup(&m);
}

/* ---------- Test 3: reproducible across identical models ---------- */
static void test_jitter_reproducible(void)
{
    printf("\n=== Same seed → same sequence ===\n");
    struct timing_model a, b;
    timing_model_init(&a, NAND_TYPE_TLC);
    timing_model_init(&b, NAND_TYPE_TLC);

    timing_model_enable_jitter(&a, 800, 0x12345678ULL);
    timing_model_enable_jitter(&b, 800, 0x12345678ULL);

    int mismatches = 0;
    for (int i = 0; i < 500; i++) {
        if (timing_get_read_latency(&a, (u32)i) !=
            timing_get_read_latency(&b, (u32)i)) mismatches++;
        if (timing_get_prog_latency(&a, (u32)i) !=
            timing_get_prog_latency(&b, (u32)i)) mismatches++;
        if (timing_get_erase_latency(&a) !=
            timing_get_erase_latency(&b)) mismatches++;
    }
    TEST_ASSERT(mismatches == 0,
                "same seed produces byte-identical latency stream");

    /* Different seed should not produce the same stream
     * (probabilistic: with 500 * 3 draws it's astronomically
     * unlikely to collide across the whole sequence). */
    struct timing_model c;
    timing_model_init(&c, NAND_TYPE_TLC);
    timing_model_enable_jitter(&c, 800, 0x87654321ULL);
    int all_equal = 1;
    for (int i = 0; i < 500 && all_equal; i++) {
        if (timing_get_read_latency(&a, (u32)i) !=
            timing_get_read_latency(&c, (u32)i)) all_equal = 0;
    }
    TEST_ASSERT(!all_equal, "different seed diverges from reference stream");

    timing_model_cleanup(&a);
    timing_model_cleanup(&b);
    timing_model_cleanup(&c);
}

/* ---------- Test 4: mean converges to baseline ---------- */
static void test_jitter_mean_unbiased(void)
{
    printf("\n=== Large-sample mean ≈ baseline ===\n");
    struct timing_model m;
    timing_model_init(&m, NAND_TYPE_SLC);   /* single-lane: no page_idx variance */

    u64 base = timing_get_read_latency(&m, 0);
    timing_model_enable_jitter(&m, 1000, 0xC0FFEEULL);  /* ±10% */

    const int N = 20000;
    u64 sum = 0;
    for (int i = 0; i < N; i++) {
        sum += timing_get_read_latency(&m, 0);
    }
    u64 mean = sum / N;

    /* With N=20k draws from a uniform [-10%, +10%] window, the
     * standard error of the mean on the multiplier is ~5.8% / sqrt(N)
     * ≈ 0.04%, so the observed mean should be within ±0.5% of base. */
    u64 tol  = base / 200;  /* 0.5% */
    u64 diff = (mean > base) ? (mean - base) : (base - mean);
    printf("  base=%" PRIu64 " mean=%" PRIu64 " diff=%" PRIu64
           " tol=%" PRIu64 "\n", base, mean, diff, tol);
    TEST_ASSERT(diff <= tol, "mean converges to base within 0.5%");

    timing_model_cleanup(&m);
}

/* ---------- Test 5: disable restores baseline ---------- */
static void test_jitter_disable_restores(void)
{
    printf("\n=== disable_jitter restores deterministic baseline ===\n");
    struct timing_model m;
    timing_model_init(&m, NAND_TYPE_TLC);

    u64 base = timing_get_prog_latency(&m, 0);
    timing_model_enable_jitter(&m, 500, 42);
    (void)timing_get_prog_latency(&m, 0);   /* advance state */
    timing_model_disable_jitter(&m);

    int ok = 1;
    for (int i = 0; i < 50; i++) {
        if (timing_get_prog_latency(&m, 0) != base) { ok = 0; break; }
    }
    TEST_ASSERT(ok, "after disable, prog latency equals baseline");

    timing_model_cleanup(&m);
}

/* ---------- Test 6: basis_points ceiling clamp ---------- */
static void test_jitter_clamped_ceiling(void)
{
    printf("\n=== basis_points ceiling clamped at TIMING_JITTER_MAX_BP ===\n");
    struct timing_model m;
    timing_model_init(&m, NAND_TYPE_TLC);

    u64 base = timing_get_read_latency(&m, 0);

    /* Pass 9999 (99.99%). Impl must clamp to TIMING_JITTER_MAX_BP. */
    timing_model_enable_jitter(&m, 9999, 0xABCDULL);

    u64 max_bp    = TIMING_JITTER_MAX_BP;   /* ±20% */
    u64 hard_hi   = base + (base * max_bp) / 10000 + 1;
    u64 hard_lo   = base - (base * max_bp) / 10000 - 1;
    /* After the base/2 floor clamp the effective lower bound is
     * max(base/2, hard_lo). */
    if (hard_lo < base / 2) hard_lo = base / 2;

    int out = 0;
    for (int i = 0; i < 1000; i++) {
        u64 s = timing_get_read_latency(&m, 0);
        if (s < hard_lo || s > hard_hi) out++;
    }
    TEST_ASSERT(out == 0,
                "no samples exceed ±20% envelope even when bp=9999 requested");

    timing_model_cleanup(&m);
}

/* ---------- Test 7: NULL / zero basis_points safe ---------- */
static void test_jitter_null_and_zero_bp(void)
{
    printf("\n=== NULL model and basis_points=0 are safe ===\n");
    timing_model_enable_jitter(NULL, 500, 1);   /* no crash */
    timing_model_disable_jitter(NULL);          /* no crash */
    TEST_ASSERT(1, "NULL model calls do not crash");

    struct timing_model m;
    timing_model_init(&m, NAND_TYPE_TLC);
    u64 base = timing_get_read_latency(&m, 0);
    timing_model_enable_jitter(&m, 0, 12345ULL);   /* 0 == disable */
    TEST_ASSERT(timing_get_read_latency(&m, 0) == base,
                "basis_points=0 leaves jitter disabled");
    timing_model_cleanup(&m);
}

int main(void)
{
    printf("========================================\n");
    printf("REQ-121 NAND Latency Jitter Tests\n");
    printf("========================================\n");

    test_jitter_disabled_by_default();
    test_jitter_bounded();
    test_jitter_reproducible();
    test_jitter_mean_unbiased();
    test_jitter_disable_restores();
    test_jitter_clamped_ceiling();
    test_jitter_null_and_zero_bp();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail > 0 ? 1 : 0;
}
