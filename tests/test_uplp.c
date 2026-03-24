#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "common/common.h"
#include "common/uplp.h"

static int total = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    total++; \
    if (cond) { printf("  [PASS] %s\n", msg); passed++; } \
    else       { printf("  [FAIL] %s\n", msg); failed++; } \
} while (0)

static void separator(void) {
    printf("========================================\n");
}

/* ------------------------------------------------------------------
 * Supercapacitor energy calculation: E = 0.5*C*(V^2 - Vcut^2)
 * ------------------------------------------------------------------ */
static void test_supercap_energy(void) {
    separator();
    printf("Test: supercap energy calculation\n");
    separator();

    struct supercap_model cap;
    double cap_f = 2.0;
    double v_charged = 5.0;
    double v_cutoff = 2.5;

    supercap_init(&cap, cap_f, 0.05, v_charged, v_cutoff, 10.0);

    /* E = 0.5 * 2.0 * (25.0 - 6.25) = 0.5 * 2.0 * 18.75 = 18.75 J */
    double expected = 0.5 * cap_f * (v_charged * v_charged -
                                      v_cutoff * v_cutoff);
    TEST_ASSERT(fabs(cap.energy_j - expected) < 1e-9,
                "Energy matches E = 0.5*C*(V^2 - Vcut^2)");
    TEST_ASSERT(fabs(cap.energy_j - 18.75) < 1e-9,
                "Energy = 18.75 J for 2F, 5V/2.5V");
    TEST_ASSERT(cap.voltage_v == v_charged,
                "Initial voltage equals charged voltage");
    TEST_ASSERT(!cap.discharging,
                "Capacitor not discharging initially");
}

/* ------------------------------------------------------------------
 * Discharge curve: V(t) follows exponential decay
 * ------------------------------------------------------------------ */
static void test_supercap_discharge_curve(void) {
    separator();
    printf("Test: supercap discharge curve\n");
    separator();

    struct supercap_model cap;
    double cap_f = 1.0;
    double v_charged = 5.0;
    double v_cutoff = 1.0;
    double r_load = 10.0;

    supercap_init(&cap, cap_f, 0.05, v_charged, v_cutoff, r_load);

    u64 start = 1000000000ULL;  /* 1 second mark */
    supercap_start_discharge(&cap, start);
    TEST_ASSERT(cap.discharging, "Capacitor is discharging after start");

    /* At t=0 (start), voltage should be v_charged */
    supercap_update(&cap, start);
    TEST_ASSERT(fabs(cap.voltage_v - v_charged) < 0.01,
                "Voltage at t=0 equals charged voltage");

    /* At t=tau (RC = 10*1 = 10 sec), V = V0 * e^-1 ~= 1.839V */
    double tau = r_load * cap_f;
    u64 t_tau = start + (u64)(tau * 1e9);
    supercap_update(&cap, t_tau);
    double expected_v = v_charged * exp(-1.0);
    TEST_ASSERT(fabs(cap.voltage_v - expected_v) < 0.01,
                "Voltage at t=tau matches V0*e^-1");

    /* At t=2*tau, V = V0 * e^-2 ~= 0.677V, but clamped to cutoff */
    u64 t_2tau = start + (u64)(2.0 * tau * 1e9);
    supercap_update(&cap, t_2tau);
    double raw_v = v_charged * exp(-2.0);
    if (raw_v < v_cutoff) {
        TEST_ASSERT(fabs(cap.voltage_v - v_cutoff) < 0.01,
                    "Voltage clamped to cutoff at t=2*tau");
    } else {
        TEST_ASSERT(fabs(cap.voltage_v - raw_v) < 0.01,
                    "Voltage at t=2*tau matches V0*e^-2");
    }
}

/* ------------------------------------------------------------------
 * Drain time calculation
 * ------------------------------------------------------------------ */
static void test_supercap_drain_time(void) {
    separator();
    printf("Test: supercap drain time calculation\n");
    separator();

    struct supercap_model cap;
    double cap_f = 1.0;
    double v_charged = 5.0;
    double v_cutoff = 2.0;
    double r_load = 10.0;

    supercap_init(&cap, cap_f, 0.05, v_charged, v_cutoff, r_load);

    double drain_ms = supercap_drain_time_ms(&cap);
    double tau = r_load * cap_f;
    double expected_ms = -tau * log(v_cutoff / v_charged) * 1000.0;

    TEST_ASSERT(fabs(drain_ms - expected_ms) < 0.01,
                "Drain time matches -tau*ln(Vcut/Vch)*1000");
    TEST_ASSERT(drain_ms > 0.0, "Drain time is positive");
}

/* ------------------------------------------------------------------
 * Supercapacitor energy depletion
 * ------------------------------------------------------------------ */
static void test_supercap_energy_depletion(void) {
    separator();
    printf("Test: supercap energy depletion\n");
    separator();

    struct supercap_model cap;
    supercap_init(&cap, 1.0, 0.05, 5.0, 2.0, 10.0);

    u64 start = 1000000000ULL;
    supercap_start_discharge(&cap, start);

    /* Update well past drain time */
    double drain_ms = supercap_drain_time_ms(&cap);
    u64 past_drain = start + (u64)((drain_ms * 2.0) * 1e6);
    supercap_update(&cap, past_drain);

    TEST_ASSERT(cap.voltage_v <= cap.v_cutoff + 0.001,
                "Voltage at or below cutoff after depletion");
    TEST_ASSERT(cap.energy_j < 0.001,
                "Energy near zero after depletion");
    TEST_ASSERT(!supercap_has_energy(&cap),
                "supercap_has_energy returns false after depletion");
}

/* ------------------------------------------------------------------
 * UPLP init and state check
 * ------------------------------------------------------------------ */
static void test_uplp_init(void) {
    separator();
    printf("Test: UPLP init and state\n");
    separator();

    struct uplp_ctx ctx;
    int ret = uplp_init(&ctx, 2.0, 0.05, 5.0, 2.5, 10.0);

    TEST_ASSERT(ret == HFSSS_OK, "uplp_init returns OK");
    TEST_ASSERT(ctx.initialized, "UPLP context is initialized");
    TEST_ASSERT(uplp_get_state(&ctx) == UPLP_NORMAL,
                "Initial state is NORMAL");
    TEST_ASSERT(ctx.unsafe_shutdown_count == 0,
                "Unsafe shutdown count starts at 0");
    TEST_ASSERT(ctx.progress.completed_steps == 0,
                "Flush progress starts empty");

    uplp_cleanup(&ctx);
    TEST_ASSERT(!ctx.initialized, "Cleaned up UPLP context");
}

/* ------------------------------------------------------------------
 * UPLP power fail signal: state transitions
 * ------------------------------------------------------------------ */
static void test_uplp_power_fail_transitions(void) {
    separator();
    printf("Test: UPLP power fail state transitions\n");
    separator();

    struct uplp_ctx ctx;
    uplp_init(&ctx, 2.0, 0.05, 5.0, 2.5, 10.0);

    TEST_ASSERT(uplp_get_state(&ctx) == UPLP_NORMAL,
                "Starts in NORMAL state");

    int ret = uplp_power_fail_signal(&ctx);
    TEST_ASSERT(ret == HFSSS_OK, "Power fail signal returns OK");
    TEST_ASSERT(uplp_get_state(&ctx) == UPLP_SAFE_STATE,
                "Ends in SAFE_STATE after power fail");
    TEST_ASSERT(ctx.unsafe_shutdown_count == 1,
                "Unsafe shutdown count incremented to 1");
    TEST_ASSERT(ctx.power_fail_ns > 0,
                "Power fail timestamp recorded");

    /* Second power fail should fail (not in NORMAL state) */
    ret = uplp_power_fail_signal(&ctx);
    TEST_ASSERT(ret == HFSSS_ERR_BUSY,
                "Second power fail rejected with BUSY");

    uplp_cleanup(&ctx);
}

/* ------------------------------------------------------------------
 * Emergency flush with sufficient energy: all steps complete
 * ------------------------------------------------------------------ */
static void test_emergency_flush_full(void) {
    separator();
    printf("Test: emergency flush with sufficient energy\n");
    separator();

    struct uplp_ctx ctx;
    /* Large capacitor: plenty of energy for all steps */
    uplp_init(&ctx, 10.0, 0.05, 5.0, 2.5, 10.0);

    int ret = uplp_power_fail_signal(&ctx);
    TEST_ASSERT(ret == HFSSS_OK,
                "Power fail with full flush returns OK");
    TEST_ASSERT(ctx.progress.completed_steps == FLUSH_ALL_STEPS,
                "All flush steps completed (0x3F)");
    TEST_ASSERT((ctx.progress.completed_steps & FLUSH_STEP_INFLIGHT_NAND) != 0,
                "INFLIGHT_NAND step completed");
    TEST_ASSERT((ctx.progress.completed_steps & FLUSH_STEP_L2P_JOURNAL) != 0,
                "L2P_JOURNAL step completed");
    TEST_ASSERT((ctx.progress.completed_steps & FLUSH_STEP_BBT) != 0,
                "BBT step completed");
    TEST_ASSERT((ctx.progress.completed_steps & FLUSH_STEP_SMART) != 0,
                "SMART step completed");
    TEST_ASSERT((ctx.progress.completed_steps & FLUSH_STEP_WAL_COMMIT) != 0,
                "WAL_COMMIT step completed");
    TEST_ASSERT((ctx.progress.completed_steps & FLUSH_STEP_SYSINFO) != 0,
                "SYSINFO step completed");

    uplp_cleanup(&ctx);
}

/* ------------------------------------------------------------------
 * Emergency flush with limited energy: partial completion
 * ------------------------------------------------------------------ */
static void test_emergency_flush_partial(void) {
    separator();
    printf("Test: emergency flush with limited energy\n");
    separator();

    struct uplp_ctx ctx;
    /*
     * Tiny capacitor: very small energy.
     * Use values that give just enough for a few steps.
     * E = 0.5 * 0.01 * (5^2 - 4.9^2) = 0.5 * 0.01 * (25 - 24.01) = 0.00495 J
     * This is not enough for step 0 (0.010 J).
     */
    uplp_init(&ctx, 0.01, 0.05, 5.0, 4.9, 10.0);

    int ret = uplp_power_fail_signal(&ctx);
    TEST_ASSERT(ret == HFSSS_ERR_POWER_LOSS,
                "Partial flush returns POWER_LOSS error");
    TEST_ASSERT(ctx.progress.completed_steps != FLUSH_ALL_STEPS,
                "Not all steps completed with limited energy");
    TEST_ASSERT(ctx.progress.completed_steps == 0,
                "No steps completed with very limited energy");

    uplp_cleanup(&ctx);
}

/* ------------------------------------------------------------------
 * Torn write detection
 * ------------------------------------------------------------------ */
static void test_torn_write_detection(void) {
    separator();
    printf("Test: torn write detection\n");
    separator();

    u8 data[64];
    memset(data, 0xAA, sizeof(data));

    /* Valid header + matching CRC */
    struct write_unit_header hdr;
    hdr.magic = WRITE_UNIT_MAGIC;
    hdr.sequence = 1;
    hdr.data_crc32 = hfsss_crc32(data, sizeof(data));
    hdr.flags = 0;

    bool torn = uplp_check_torn_write(&hdr, data, sizeof(data));
    TEST_ASSERT(!torn, "Valid write not detected as torn");

    /* Corrupt the data: CRC mismatch */
    data[0] = 0xBB;
    torn = uplp_check_torn_write(&hdr, data, sizeof(data));
    TEST_ASSERT(torn, "Corrupted data detected as torn write");

    /* Invalid magic */
    struct write_unit_header bad_hdr;
    memset(&bad_hdr, 0, sizeof(bad_hdr));
    bad_hdr.magic = 0xDEADDEAD;
    torn = uplp_check_torn_write(&bad_hdr, data, sizeof(data));
    TEST_ASSERT(torn, "Invalid magic detected as torn write");

    /* NULL data */
    torn = uplp_check_torn_write(&hdr, NULL, 0);
    TEST_ASSERT(torn, "NULL data detected as torn write");
}

/* ------------------------------------------------------------------
 * Flush progress persistence: verify bitmask and CRC
 * ------------------------------------------------------------------ */
static void test_flush_progress_persistence(void) {
    separator();
    printf("Test: flush progress persistence\n");
    separator();

    struct uplp_ctx ctx;
    uplp_init(&ctx, 10.0, 0.05, 5.0, 2.5, 10.0);

    uplp_power_fail_signal(&ctx);

    const struct flush_progress *prog = uplp_get_flush_progress(&ctx);
    TEST_ASSERT(prog != NULL, "Flush progress pointer is valid");
    TEST_ASSERT(prog->completed_steps == FLUSH_ALL_STEPS,
                "Flush progress has correct bitmask");

    /* Verify CRC of progress record */
    u32 expected_crc = hfsss_crc32(prog,
                                    offsetof(struct flush_progress, crc32));
    TEST_ASSERT(prog->crc32 == expected_crc,
                "Flush progress CRC is valid");

    uplp_cleanup(&ctx);
}

/* ------------------------------------------------------------------
 * Test hook: uplp_inject_power_fail
 * ------------------------------------------------------------------ */
static void test_inject_power_fail(void) {
    separator();
    printf("Test: uplp_inject_power_fail hook\n");
    separator();

    struct uplp_ctx ctx;
    uplp_init(&ctx, 2.0, 0.05, 5.0, 2.5, 10.0);

    uplp_inject_power_fail(&ctx, 5000000ULL);  /* 5ms delay */
    TEST_ASSERT(ctx.inject_power_fail,
                "inject_power_fail flag set");
    TEST_ASSERT(ctx.inject_delay_ns == 5000000ULL,
                "inject delay set to 5ms");

    uplp_cleanup(&ctx);
}

/* ------------------------------------------------------------------
 * Test hook: uplp_set_cap_drain_time
 * ------------------------------------------------------------------ */
static void test_set_cap_drain_time(void) {
    separator();
    printf("Test: uplp_set_cap_drain_time hook\n");
    separator();

    struct uplp_ctx ctx;
    uplp_init(&ctx, 1.0, 0.05, 5.0, 2.0, 10.0);

    double original_drain = supercap_drain_time_ms(&ctx.cap);
    TEST_ASSERT(original_drain > 0.0,
                "Original drain time is positive");

    /* Override drain time to 100ms */
    uplp_set_cap_drain_time(&ctx, 100.0);

    double new_drain = supercap_drain_time_ms(&ctx.cap);
    TEST_ASSERT(fabs(new_drain - 100.0) < 0.1,
                "Drain time changed to ~100ms after override");

    /* Override to 50ms */
    uplp_set_cap_drain_time(&ctx, 50.0);
    new_drain = supercap_drain_time_ms(&ctx.cap);
    TEST_ASSERT(fabs(new_drain - 50.0) < 0.1,
                "Drain time changed to ~50ms after second override");

    uplp_cleanup(&ctx);
}

/* ------------------------------------------------------------------
 * Test hook: uplp_inject_at_phase
 * ------------------------------------------------------------------ */
static void test_inject_at_phase(void) {
    separator();
    printf("Test: uplp_inject_at_phase hook\n");
    separator();

    struct uplp_ctx ctx;
    uplp_init(&ctx, 2.0, 0.05, 5.0, 2.5, 10.0);

    TEST_ASSERT(ctx.inject_at_phase == -1,
                "inject_at_phase starts disabled (-1)");

    uplp_inject_at_phase(&ctx, 3);
    TEST_ASSERT(ctx.inject_at_phase == 3,
                "inject_at_phase set to phase 3");

    uplp_cleanup(&ctx);
}

/* ------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------ */
int main(void) {
    printf("\n");
    separator();
    printf("HFSSS UPLP Module Test Suite\n");
    separator();
    printf("\n");

    test_supercap_energy();
    printf("\n");
    test_supercap_discharge_curve();
    printf("\n");
    test_supercap_drain_time();
    printf("\n");
    test_supercap_energy_depletion();
    printf("\n");
    test_uplp_init();
    printf("\n");
    test_uplp_power_fail_transitions();
    printf("\n");
    test_emergency_flush_full();
    printf("\n");
    test_emergency_flush_partial();
    printf("\n");
    test_torn_write_detection();
    printf("\n");
    test_flush_progress_persistence();
    printf("\n");
    test_inject_power_fail();
    printf("\n");
    test_set_cap_drain_time();
    printf("\n");
    test_inject_at_phase();

    printf("\n");
    separator();
    printf("UPLP Tests: %d total, %d passed, %d failed\n",
           total, passed, failed);
    separator();

    return (failed > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
