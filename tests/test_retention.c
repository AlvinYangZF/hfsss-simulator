/*
 * REQ-048: NAND data-retention model tests.
 *
 * Two tiers:
 *
 *   Unit — exercises reliability_calculate_bit_errors() directly at
 *   retention ages that would take ~10 years of wall clock to observe
 *   naturally. Confirms:
 *     - baseline (zero retention, zero wear) stays near-floor
 *     - the RBER is monotone non-decreasing in retention time
 *     - cell-class ordering QLC > TLC > MLC > SLC holds at the same age
 *     - 10-year TLC magnitude lands in the band implied by the model
 *       constant (params->data_retention_rate * days * page_bits)
 *     - NULL model returns zero instead of dereferencing
 *
 *   Integration — drives a real media_ctx (program → backdate program_ts
 *   → read) to prove the retention path that media_inject_bit_errors
 *   runs at read time picks up the aged timestamp and surfaces an
 *   elevated bit_errors count on the nand_page. No simulation clock is
 *   introduced; the test rewinds program_ts on the public nand_page
 *   struct, which is the same effective signal the existing code reads.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdint.h>

#include "common/common.h"
#include "media/media.h"
#include "media/reliability.h"
#include "media/nand.h"

static int g_pass = 0;
static int g_fail = 0;

#define TEST_ASSERT(cond, msg) do {                           \
    if (cond) { printf("  [PASS] %s\n", msg); g_pass++; }     \
    else      { printf("  [FAIL] %s  (line %d)\n",            \
                       msg, __LINE__); g_fail++; }            \
} while (0)

/* Nanoseconds-per-day used to project multi-year retention ages into the
 * u64 retention_ns argument the model already accepts. */
#define NS_PER_DAY  (86400ULL * 1000000000ULL)

/* ---------- Unit 1: NULL / zero-retention baseline ---------- */
static void test_null_and_zero_retention(void)
{
    printf("\n=== NULL model + zero-retention baseline ===\n");

    /* NULL model must return 0, not crash. */
    u32 n = reliability_calculate_bit_errors(NULL, NAND_TYPE_TLC, 0, 0, 0);
    TEST_ASSERT(n == 0, "NULL model returns 0");

    struct reliability_model m;
    TEST_ASSERT(reliability_model_init(&m) == HFSSS_OK, "model init OK");

    /* With zero wear, zero reads, zero retention the only contribution is
     * raw_bit_error_rate. For TLC (raw=1e-6, 16KiB page -> 131072 bits)
     * the expected value is ~0.131, so the hash adder dominates; either
     * way bit_errors must sit well below 10. */
    u32 tlc0 = reliability_calculate_bit_errors(&m, NAND_TYPE_TLC, 0, 0, 0);
    TEST_ASSERT(tlc0 <= 10, "TLC zero-retention baseline is near-floor");

    reliability_model_cleanup(&m);
}

/* ---------- Unit 2: monotone non-decreasing in retention ---------- */
static void test_monotone_in_retention(void)
{
    printf("\n=== RBER is monotone non-decreasing in retention ===\n");

    struct reliability_model m;
    reliability_model_init(&m);

    /* Retention ages span six orders of magnitude to dwarf the hash
     * adder (±1 noise). For each NAND type we check that every
     * successive point is at least as large as its predecessor. */
    const u64 ages_days[] = { 0, 30, 180, 365, 1095, 3650 };
    const size_t N = sizeof(ages_days) / sizeof(ages_days[0]);

    enum nand_type types[] = { NAND_TYPE_SLC, NAND_TYPE_MLC,
                               NAND_TYPE_TLC, NAND_TYPE_QLC };
    const char *names[]   = { "SLC", "MLC", "TLC", "QLC" };

    for (size_t t = 0; t < 4; t++) {
        u32 prev = reliability_calculate_bit_errors(&m, types[t],
                                                    0, 0,
                                                    ages_days[0] * NS_PER_DAY);
        int ok = 1;
        for (size_t i = 1; i < N; i++) {
            u32 cur = reliability_calculate_bit_errors(&m, types[t],
                                                       0, 0,
                                                       ages_days[i] * NS_PER_DAY);
            /* Allow ±1 slack for the deterministic hash adder so a single
             * bucket collision does not flip the monotone check when the
             * underlying model value is equal across two ages. */
            if (cur + 1 < prev) { ok = 0; break; }
            prev = cur;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%s monotone across 0..10y", names[t]);
        TEST_ASSERT(ok, buf);
    }

    reliability_model_cleanup(&m);
}

/* ---------- Unit 3: cell-class ordering at same age ---------- */
static void test_class_ordering(void)
{
    printf("\n=== QLC >= TLC >= MLC >= SLC at same retention age ===\n");

    struct reliability_model m;
    reliability_model_init(&m);

    /* 3-year retention is well past the point where the retention term
     * dominates the raw-BER floor for every class; the ordering should
     * follow the per-class data_retention_rate (QLC 1e-5 > TLC 1e-6 >
     * MLC 1e-7 > SLC 1e-8). */
    u64 age_ns = 1095ULL * NS_PER_DAY;

    u32 slc = reliability_calculate_bit_errors(&m, NAND_TYPE_SLC, 0, 0, age_ns);
    u32 mlc = reliability_calculate_bit_errors(&m, NAND_TYPE_MLC, 0, 0, age_ns);
    u32 tlc = reliability_calculate_bit_errors(&m, NAND_TYPE_TLC, 0, 0, age_ns);
    u32 qlc = reliability_calculate_bit_errors(&m, NAND_TYPE_QLC, 0, 0, age_ns);

    printf("  slc=%u mlc=%u tlc=%u qlc=%u (3y)\n", slc, mlc, tlc, qlc);
    TEST_ASSERT(qlc >= tlc, "QLC >= TLC at 3y");
    TEST_ASSERT(tlc >= mlc, "TLC >= MLC at 3y");
    TEST_ASSERT(mlc >= slc, "MLC >= SLC at 3y");

    reliability_model_cleanup(&m);
}

/* ---------- Unit 4: 10y TLC magnitude sanity ---------- */
static void test_tlc_10y_magnitude(void)
{
    printf("\n=== TLC 10-year retention lands in modeled band ===\n");

    struct reliability_model m;
    reliability_model_init(&m);

    /* Model: rber += 1e-6/day * 3650 days = 3.65e-3.
     * bit_errors ≈ 3.65e-3 * 131072 ≈ 478 (plus raw-BER and hash ±1).
     * The bit-error-count saturates at page_bits/10 = 13107, so the
     * realistic band is comfortably below that. A [300, 700] window
     * leaves slack for the raw-BER addend and the hash adder while
     * still catching a large regression. */
    u32 errs = reliability_calculate_bit_errors(&m, NAND_TYPE_TLC,
                                                 0, 0,
                                                 3650ULL * NS_PER_DAY);
    printf("  10y TLC errors = %u\n", errs);
    TEST_ASSERT(errs >= 300 && errs <= 700,
                "TLC 10y bit_errors in [300,700]");

    reliability_model_cleanup(&m);
}

/* ---------- Unit 5: saturation cap ---------- */
static void test_saturation_cap(void)
{
    printf("\n=== Absurd retention age saturates at page_bits/10 ===\n");

    struct reliability_model m;
    reliability_model_init(&m);

    /* Push QLC to ~1000 years. The uncapped model would compute
     * 1e-5 * 365000 * 131072 ≈ 478,413 bit errors, far above the
     * 13107 ceiling. The implementation caps bit_errors at
     * page_bits/10; this test locks that invariant. */
    u64 huge_age = 365000ULL * NS_PER_DAY;
    u32 qlc = reliability_calculate_bit_errors(&m, NAND_TYPE_QLC,
                                                0, 0, huge_age);
    u32 cap = (16384u * 8u) / 10u;   /* matches reliability.c */
    printf("  QLC 1000y errors = %u (cap=%u)\n", qlc, cap);
    TEST_ASSERT(qlc <= cap, "bit_errors never exceeds page_bits/10");

    reliability_model_cleanup(&m);
}

/* ---------- Integration: program -> backdate -> read ---------- */
static void test_integration_backdate(void)
{
    printf("\n=== Integration: program -> backdate program_ts -> read ===\n");

    struct media_ctx ctx;
    struct media_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.channel_count      = 1;
    cfg.chips_per_channel  = 1;
    cfg.dies_per_chip      = 1;
    cfg.planes_per_die     = 1;
    cfg.blocks_per_plane   = 4;
    cfg.pages_per_block    = 8;
    cfg.page_size          = 4096;
    cfg.spare_size         = 64;
    cfg.nand_type          = NAND_TYPE_TLC;

    int rc = media_init(&ctx, &cfg);
    TEST_ASSERT(rc == HFSSS_OK, "media_init OK");

    /* Program a page. program_ts = get_time_ns() inside the driver. */
    u8 buf[4096];
    u8 spare[64];
    memset(buf, 0xA5, sizeof(buf));
    memset(spare, 0x5A, sizeof(spare));
    rc = media_nand_program(&ctx, 0, 0, 0, 0, 1, 0, buf, spare);
    TEST_ASSERT(rc == HFSSS_OK, "program page (block 1, page 0)");

    /* First read — retention is essentially 0, so bit_errors should be
     * at the model floor. */
    u8 rbuf[4096];
    u8 rspare[64];
    rc = media_nand_read(&ctx, 0, 0, 0, 0, 1, 0, rbuf, rspare);
    TEST_ASSERT(rc == HFSSS_OK, "read page (fresh)");

    struct nand_page *pg = nand_get_page(ctx.nand, 0, 0, 0, 0, 1, 0);
    TEST_ASSERT(pg != NULL, "nand_get_page returns the programmed page");
    u32 fresh_errs = pg->bit_errors;
    printf("  fresh bit_errors = %u\n", fresh_errs);

    /* Backdate the program timestamp by 10 years so the next read
     * sees retention_ns = now - (now - 10y) = 10 years. The public
     * nand_page struct already exposes program_ts; no new hook
     * needed. */
    pg->program_ts -= 3650ULL * NS_PER_DAY;

    rc = media_nand_read(&ctx, 0, 0, 0, 0, 1, 0, rbuf, rspare);
    TEST_ASSERT(rc == HFSSS_OK, "read page (10y retention)");

    u32 aged_errs = pg->bit_errors;
    printf("  10y bit_errors   = %u\n", aged_errs);

    /* The aged read must surface substantially more bit errors than the
     * fresh read. A 50-error gap dwarfs the ±1 hash noise and the raw-
     * BER floor, so this is a real signal that the retention term is
     * being consumed on the media read path. */
    TEST_ASSERT(aged_errs > fresh_errs + 50,
                "aged read reports materially more bit errors");

    media_cleanup(&ctx);
}

int main(void)
{
    printf("========================================\n");
    printf("REQ-048 Data Retention Model Tests\n");
    printf("========================================\n");

    test_null_and_zero_retention();
    test_monotone_in_retention();
    test_class_ordering();
    test_tlc_10y_magnitude();
    test_saturation_cap();
    test_integration_backdate();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail > 0 ? 1 : 0;
}
