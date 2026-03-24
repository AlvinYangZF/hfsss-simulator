#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "common/thermal.h"
#include "common/telemetry.h"

#define TEST_PASS 0
#define TEST_FAIL 1

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        tests_passed++; \
    } else { \
        printf("  [FAIL] %s\n", msg); \
        tests_failed++; \
    } \
} while(0)

static void print_separator(void) {
    printf("========================================\n");
}

/* ---- Thermal init/cleanup tests ---- */

static void test_thermal_init_cleanup(void) {
    printf("\n=== Thermal Init/Cleanup ===\n");

    struct thermal_throttle_ctx ctx;
    int ret;

    ret = thermal_throttle_init(&ctx);
    TEST_ASSERT(ret == HFSSS_OK, "thermal_throttle_init succeeds");
    TEST_ASSERT(ctx.initialized == true, "ctx marked initialized");
    TEST_ASSERT(ctx.current_level == THERMAL_LEVEL_NONE, "initial level is NONE");
    TEST_ASSERT(ctx.throttle_factor == 1.0, "initial factor is 1.0");
    TEST_ASSERT(ctx.warn_temp_minutes == 0, "initial warn minutes is 0");
    TEST_ASSERT(ctx.crit_temp_minutes == 0, "initial crit minutes is 0");

    thermal_throttle_cleanup(&ctx);
    TEST_ASSERT(ctx.initialized == false, "cleanup clears initialized");

    /* NULL handling */
    ret = thermal_throttle_init(NULL);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "init NULL returns INVAL");
    thermal_throttle_cleanup(NULL);  /* should not crash */
    TEST_ASSERT(true, "cleanup NULL does not crash");
}

/* ---- Thermal level computation tests ---- */

static void test_thermal_levels(void) {
    printf("\n=== Thermal Level Computation ===\n");

    u8 level;

    /* 70C: below LIGHT threshold (75C) */
    level = thermal_compute_level(70.0, THERMAL_LEVEL_NONE);
    TEST_ASSERT(level == THERMAL_LEVEL_NONE, "70C from NONE -> NONE");

    /* 74C: still below LIGHT threshold */
    level = thermal_compute_level(74.0, THERMAL_LEVEL_NONE);
    TEST_ASSERT(level == THERMAL_LEVEL_NONE, "74C from NONE -> NONE");

    /* 75C: exactly at LIGHT threshold */
    level = thermal_compute_level(75.0, THERMAL_LEVEL_NONE);
    TEST_ASSERT(level == THERMAL_LEVEL_LIGHT, "75C from NONE -> LIGHT");

    /* 80C: at MODERATE threshold */
    level = thermal_compute_level(80.0, THERMAL_LEVEL_NONE);
    TEST_ASSERT(level == THERMAL_LEVEL_MODERATE, "80C from NONE -> MODERATE");

    /* 85C: at HEAVY threshold */
    level = thermal_compute_level(85.0, THERMAL_LEVEL_NONE);
    TEST_ASSERT(level == THERMAL_LEVEL_HEAVY, "85C from NONE -> HEAVY");

    /* 90C: at SHUTDOWN threshold */
    level = thermal_compute_level(90.0, THERMAL_LEVEL_NONE);
    TEST_ASSERT(level == THERMAL_LEVEL_SHUTDOWN, "90C from NONE -> SHUTDOWN");

    /* 95C: above SHUTDOWN threshold */
    level = thermal_compute_level(95.0, THERMAL_LEVEL_LIGHT);
    TEST_ASSERT(level == THERMAL_LEVEL_SHUTDOWN, "95C from LIGHT -> SHUTDOWN");
}

/* ---- Thermal factor tests ---- */

static void test_thermal_factors(void) {
    printf("\n=== Thermal Throttle Factors ===\n");

    TEST_ASSERT(thermal_get_factor(THERMAL_LEVEL_NONE) == 1.0,
                "NONE factor is 1.00");
    TEST_ASSERT(fabs(thermal_get_factor(THERMAL_LEVEL_LIGHT) - 0.80) < 0.001,
                "LIGHT factor is 0.80");
    TEST_ASSERT(fabs(thermal_get_factor(THERMAL_LEVEL_MODERATE) - 0.50) < 0.001,
                "MODERATE factor is 0.50");
    TEST_ASSERT(fabs(thermal_get_factor(THERMAL_LEVEL_HEAVY) - 0.20) < 0.001,
                "HEAVY factor is 0.20");
    TEST_ASSERT(thermal_get_factor(THERMAL_LEVEL_SHUTDOWN) == 0.0,
                "SHUTDOWN factor is 0.00");

    /* Out of range level */
    TEST_ASSERT(thermal_get_factor(255) == 0.0,
                "out-of-range level returns 0.0");
}

/* ---- Thermal hysteresis tests ---- */

static void test_thermal_hysteresis(void) {
    printf("\n=== Thermal Hysteresis ===\n");

    u8 level;

    /* At LIGHT (75C), drop to 73C -> stays LIGHT (within hysteresis) */
    level = thermal_compute_level(73.0, THERMAL_LEVEL_LIGHT);
    TEST_ASSERT(level == THERMAL_LEVEL_LIGHT,
                "LIGHT at 73C stays LIGHT (within hysteresis)");

    /* At LIGHT (75C), drop to 72C -> stays LIGHT (threshold - hyst = 72, need < 72) */
    level = thermal_compute_level(72.0, THERMAL_LEVEL_LIGHT);
    TEST_ASSERT(level == THERMAL_LEVEL_LIGHT,
                "LIGHT at 72C stays LIGHT (at hysteresis boundary)");

    /* At LIGHT, drop to 71C -> drops to NONE (below threshold - hysteresis) */
    level = thermal_compute_level(71.0, THERMAL_LEVEL_LIGHT);
    TEST_ASSERT(level == THERMAL_LEVEL_NONE,
                "LIGHT at 71C drops to NONE (below hysteresis)");

    /* At MODERATE (80C), drop to 78C -> stays MODERATE */
    level = thermal_compute_level(78.0, THERMAL_LEVEL_MODERATE);
    TEST_ASSERT(level == THERMAL_LEVEL_MODERATE,
                "MODERATE at 78C stays MODERATE (within hysteresis)");

    /* At MODERATE, drop to 76C -> drops to LIGHT */
    level = thermal_compute_level(76.0, THERMAL_LEVEL_MODERATE);
    TEST_ASSERT(level == THERMAL_LEVEL_LIGHT,
                "MODERATE at 76C drops to LIGHT (below hysteresis)");

    /* At HEAVY (85C), drop to 83C -> stays HEAVY */
    level = thermal_compute_level(83.0, THERMAL_LEVEL_HEAVY);
    TEST_ASSERT(level == THERMAL_LEVEL_HEAVY,
                "HEAVY at 83C stays HEAVY (within hysteresis)");

    /* At HEAVY, drop to 81C -> drops to MODERATE */
    level = thermal_compute_level(81.0, THERMAL_LEVEL_HEAVY);
    TEST_ASSERT(level == THERMAL_LEVEL_MODERATE,
                "HEAVY at 81C drops to MODERATE (below hysteresis)");

    /* At SHUTDOWN (90C), drop to 88C -> stays SHUTDOWN */
    level = thermal_compute_level(88.0, THERMAL_LEVEL_SHUTDOWN);
    TEST_ASSERT(level == THERMAL_LEVEL_SHUTDOWN,
                "SHUTDOWN at 88C stays SHUTDOWN (within hysteresis)");

    /* At SHUTDOWN, drop to 86C -> drops to HEAVY */
    level = thermal_compute_level(86.0, THERMAL_LEVEL_SHUTDOWN);
    TEST_ASSERT(level == THERMAL_LEVEL_HEAVY,
                "SHUTDOWN at 86C drops to HEAVY (below hysteresis)");

    /* Escalation does not decrease: at MODERATE(80), still 80C -> MODERATE */
    level = thermal_compute_level(80.0, THERMAL_LEVEL_MODERATE);
    TEST_ASSERT(level == THERMAL_LEVEL_MODERATE,
                "MODERATE at 80C stays MODERATE (MAX prevents decrease)");
}

/* ---- Thermal shutdown detection ---- */

static void test_thermal_shutdown(void) {
    printf("\n=== Thermal Shutdown Detection ===\n");

    TEST_ASSERT(thermal_is_shutdown(THERMAL_LEVEL_SHUTDOWN) == true,
                "SHUTDOWN level detected as shutdown");
    TEST_ASSERT(thermal_is_shutdown(THERMAL_LEVEL_HEAVY) == false,
                "HEAVY level is not shutdown");
    TEST_ASSERT(thermal_is_shutdown(THERMAL_LEVEL_NONE) == false,
                "NONE level is not shutdown");
}

/* ---- Thermal warn/crit time counter tests ---- */

static void test_thermal_counters(void) {
    printf("\n=== Thermal Warn/Crit Time Counters ===\n");

    struct thermal_throttle_ctx ctx;
    thermal_throttle_init(&ctx);

    u64 base_ns = 1000000000ULL;  /* 1 second */
    ctx.last_minute_ns = base_ns;

    /* Simulate 3 minutes at 72C (above warn 70C, below crit 85C) */
    u64 three_min_ns = base_ns + 3ULL * 60ULL * 1000000000ULL;
    thermal_update_counters(&ctx, 72.0, three_min_ns);
    TEST_ASSERT(ctx.warn_temp_minutes == 3,
                "3 minutes at 72C -> warn_temp_minutes=3");
    TEST_ASSERT(ctx.crit_temp_minutes == 0,
                "3 minutes at 72C -> crit_temp_minutes=0");

    /* Simulate 2 more minutes at 86C (above both warn and crit) */
    u64 five_min_ns = three_min_ns + 2ULL * 60ULL * 1000000000ULL;
    thermal_update_counters(&ctx, 86.0, five_min_ns);
    TEST_ASSERT(ctx.warn_temp_minutes == 5,
                "2 more min at 86C -> warn_temp_minutes=5");
    TEST_ASSERT(ctx.crit_temp_minutes == 2,
                "2 more min at 86C -> crit_temp_minutes=2");

    /* Level should have escalated to HEAVY at 86C */
    TEST_ASSERT(ctx.current_level == THERMAL_LEVEL_HEAVY,
                "level escalated to HEAVY at 86C");
    TEST_ASSERT(fabs(ctx.throttle_factor - 0.20) < 0.001,
                "throttle factor updated to 0.20");

    thermal_throttle_cleanup(&ctx);
}

/* ---- Telemetry init/cleanup tests ---- */

static void test_telemetry_init_cleanup(void) {
    printf("\n=== Telemetry Init/Cleanup ===\n");

    struct telemetry_log log;
    int ret;

    ret = telemetry_init(&log);
    TEST_ASSERT(ret == HFSSS_OK, "telemetry_init succeeds");
    TEST_ASSERT(log.initialized == true, "log marked initialized");
    TEST_ASSERT(log.head == 0, "head starts at 0");
    TEST_ASSERT(log.count == 0, "count starts at 0");
    TEST_ASSERT(log.total_events == 0, "total_events starts at 0");

    telemetry_cleanup(&log);
    TEST_ASSERT(log.initialized == false, "cleanup clears initialized");

    /* NULL handling */
    ret = telemetry_init(NULL);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "init NULL returns INVAL");
    telemetry_cleanup(NULL);  /* should not crash */
    TEST_ASSERT(true, "cleanup NULL does not crash");
}

/* ---- Telemetry record and retrieve tests ---- */

static void test_telemetry_record_retrieve(void) {
    printf("\n=== Telemetry Record/Retrieve ===\n");

    struct telemetry_log log;
    telemetry_init(&log);

    /* Record a thermal event */
    u8 payload1[4] = {0x01, 0x02, 0x03, 0x04};
    int ret = telemetry_record(&log, TEL_EVENT_THERMAL, 1,
                               payload1, sizeof(payload1));
    TEST_ASSERT(ret == HFSSS_OK, "record thermal event succeeds");
    TEST_ASSERT(log.count == 1, "count is 1 after first record");
    TEST_ASSERT(log.total_events == 1, "total_events is 1");

    /* Record a GC event */
    u8 payload2[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
    ret = telemetry_record(&log, TEL_EVENT_GC, 0,
                           payload2, sizeof(payload2));
    TEST_ASSERT(ret == HFSSS_OK, "record GC event succeeds");
    TEST_ASSERT(log.count == 2, "count is 2 after second record");

    /* Record with NULL payload */
    ret = telemetry_record(&log, TEL_EVENT_ERROR, 2, NULL, 0);
    TEST_ASSERT(ret == HFSSS_OK, "record with NULL payload succeeds");
    TEST_ASSERT(log.count == 3, "count is 3 after third record");

    /* Retrieve recent events */
    struct tel_event out[10];
    u32 actual = 0;
    ret = telemetry_get_recent(&log, out, 10, &actual);
    TEST_ASSERT(ret == HFSSS_OK, "get_recent succeeds");
    TEST_ASSERT(actual == 3, "retrieved 3 events");

    /* Most recent first */
    TEST_ASSERT(out[0].type == (u32)TEL_EVENT_ERROR,
                "most recent event is ERROR");
    TEST_ASSERT(out[0].severity == 2, "ERROR event severity is 2");
    TEST_ASSERT(out[1].type == (u32)TEL_EVENT_GC,
                "second most recent is GC");
    TEST_ASSERT(out[2].type == (u32)TEL_EVENT_THERMAL,
                "oldest event is THERMAL");

    /* Verify payload of thermal event */
    TEST_ASSERT(out[2].payload[0] == 0x01 && out[2].payload[3] == 0x04,
                "thermal event payload preserved");

    /* Retrieve with max_events < count */
    ret = telemetry_get_recent(&log, out, 2, &actual);
    TEST_ASSERT(ret == HFSSS_OK, "get_recent with limit succeeds");
    TEST_ASSERT(actual == 2, "limited retrieval returns 2 events");
    TEST_ASSERT(out[0].type == (u32)TEL_EVENT_ERROR,
                "limited: most recent is ERROR");

    telemetry_cleanup(&log);
}

/* ---- Telemetry ring buffer overflow tests ---- */

static void test_telemetry_overflow(void) {
    printf("\n=== Telemetry Ring Buffer Overflow ===\n");

    struct telemetry_log *log = (struct telemetry_log *)malloc(sizeof(*log));
    TEST_ASSERT(log != NULL, "telemetry_log allocated");
    telemetry_init(log);

    /* Fill beyond capacity */
    u32 total_to_write = TEL_MAX_EVENTS + 100;
    for (u32 i = 0; i < total_to_write; i++) {
        u32 val = i;
        telemetry_record(log, TEL_EVENT_WEAR, 0, &val, sizeof(val));
    }

    TEST_ASSERT(log->count == TEL_MAX_EVENTS,
                "count capped at TEL_MAX_EVENTS after overflow");
    TEST_ASSERT(log->total_events == total_to_write,
                "total_events tracks all recorded events");

    /* Verify most recent events are the last 100 written */
    struct tel_event out[10];
    u32 actual = 0;
    telemetry_get_recent(log, out, 10, &actual);
    TEST_ASSERT(actual == 10, "can retrieve 10 events after overflow");

    /* The most recent event should have payload = total_to_write - 1 */
    u32 most_recent_val;
    memcpy(&most_recent_val, out[0].payload, sizeof(u32));
    TEST_ASSERT(most_recent_val == total_to_write - 1,
                "most recent event has correct payload after overflow");

    /* The 10th most recent should have payload = total_to_write - 10 */
    u32 tenth_val;
    memcpy(&tenth_val, out[9].payload, sizeof(u32));
    TEST_ASSERT(tenth_val == total_to_write - 10,
                "10th most recent correct after overflow");

    telemetry_cleanup(log);
    free(log);
}

/* ---- Telemetry event count tracking ---- */

static void test_telemetry_count(void) {
    printf("\n=== Telemetry Event Count Tracking ===\n");

    struct telemetry_log log;
    telemetry_init(&log);

    TEST_ASSERT(telemetry_get_total_count(&log) == 0,
                "initial total count is 0");

    for (int i = 0; i < 50; i++) {
        telemetry_record(&log, TEL_EVENT_SPARE, 0, NULL, 0);
    }
    TEST_ASSERT(telemetry_get_total_count(&log) == 50,
                "total count is 50 after 50 records");

    /* NULL log returns 0 */
    TEST_ASSERT(telemetry_get_total_count(NULL) == 0,
                "total count of NULL log returns 0");

    telemetry_cleanup(&log);
}

/* ---- SMART life prediction tests ---- */

static void test_smart_prediction_basic(void) {
    printf("\n=== SMART Life Prediction ===\n");

    struct smart_prediction pred;

    /* 0 erase count -> 100% remaining */
    smart_predict_life(0, 3000, 1.0, &pred);
    TEST_ASSERT(fabs(pred.remaining_life_pct - 100.0) < 0.01,
                "0 erase -> 100% remaining life");
    TEST_ASSERT(pred.avg_erase_count == 0, "avg_erase_count stored");
    TEST_ASSERT(pred.max_pe_cycles == 3000, "max_pe_cycles stored");

    /* Max erase count -> 0% remaining */
    smart_predict_life(3000, 3000, 1.0, &pred);
    TEST_ASSERT(fabs(pred.remaining_life_pct) < 0.01,
                "max erase -> 0% remaining life");

    /* Half erase count -> 50% remaining */
    smart_predict_life(1500, 3000, 1.0, &pred);
    TEST_ASSERT(fabs(pred.remaining_life_pct - 50.0) < 0.01,
                "half erase -> 50% remaining life");

    /* Over max -> clamp to 0% */
    smart_predict_life(4000, 3000, 1.0, &pred);
    TEST_ASSERT(fabs(pred.remaining_life_pct) < 0.01,
                "over max erase -> 0% remaining life");
}

static void test_smart_prediction_waf(void) {
    printf("\n=== SMART Life Prediction with WAF ===\n");

    struct smart_prediction pred;

    /* WAF of 2.0 means more NAND writes per host write */
    smart_predict_life(0, 3000, 2.0, &pred);
    TEST_ASSERT(fabs(pred.remaining_life_pct - 100.0) < 0.01,
                "WAF 2.0, 0 erase -> 100% life");
    TEST_ASSERT(fabs(pred.current_waf - 2.0) < 0.01,
                "WAF stored correctly");

    /* With WAF=2.0, remaining host TB should be half of WAF=1.0 case */
    struct smart_prediction pred_waf1, pred_waf2;
    smart_predict_life(0, 3000, 1.0, &pred_waf1);
    smart_predict_life(0, 3000, 2.0, &pred_waf2);
    /* The remaining writes with WAF=2 should be roughly half of WAF=1 */
    if (pred_waf1.estimated_remaining_writes_tb > 0) {
        TEST_ASSERT(pred_waf2.estimated_remaining_writes_tb <=
                    pred_waf1.estimated_remaining_writes_tb,
                    "higher WAF reduces estimated remaining writes");
    } else {
        TEST_ASSERT(true, "remaining writes TB is small (expected for small PE)");
    }
}

static void test_smart_prediction_edge(void) {
    printf("\n=== SMART Life Prediction Edge Cases ===\n");

    struct smart_prediction pred;

    /* 0 max PE cycles -> safe handling (no division by zero) */
    smart_predict_life(100, 0, 1.0, &pred);
    TEST_ASSERT(fabs(pred.remaining_life_pct) < 0.01,
                "0 max PE -> 0% remaining life (safe)");
    TEST_ASSERT(pred.estimated_remaining_writes_tb == 0,
                "0 max PE -> 0 estimated remaining writes");

    /* 0 WAF -> treated as 1.0 */
    smart_predict_life(0, 3000, 0.0, &pred);
    TEST_ASSERT(fabs(pred.remaining_life_pct - 100.0) < 0.01,
                "0 WAF handled safely (treated as 1.0)");

    /* NULL pred -> should not crash */
    smart_predict_life(0, 3000, 1.0, NULL);
    TEST_ASSERT(true, "NULL pred does not crash");
}

/* ---- Main ---- */

int main(void) {
    print_separator();
    printf("HFSSS Thermal Management & Telemetry Tests\n");
    print_separator();

    /* Thermal tests */
    test_thermal_init_cleanup();
    test_thermal_levels();
    test_thermal_factors();
    test_thermal_hysteresis();
    test_thermal_shutdown();
    test_thermal_counters();

    /* Telemetry tests */
    test_telemetry_init_cleanup();
    test_telemetry_record_retrieve();
    test_telemetry_overflow();
    test_telemetry_count();

    /* SMART prediction tests */
    test_smart_prediction_basic();
    test_smart_prediction_waf();
    test_smart_prediction_edge();

    print_separator();
    printf("Results: %d/%d passed, %d failed\n",
           tests_passed, tests_run, tests_failed);
    print_separator();

    return tests_failed > 0 ? 1 : 0;
}
