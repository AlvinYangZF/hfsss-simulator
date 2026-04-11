/*
 * Tests for the Performance Validation Framework (REQ-116 through REQ-123).
 * 15 test groups covering all public APIs and requirement targets.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#include "perf/perf_validation.h"
#include "common/common.h"

/* ------------------------------------------------------------------
 * Test harness
 * ------------------------------------------------------------------ */
static int total = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    total++; \
    if (cond) { printf("  [PASS] %s\n", (msg)); passed++; } \
    else       { printf("  [FAIL] %s\n", (msg)); failed++; } \
} while (0)

static void separator(const char *title) {
    printf("========================================\n");
    printf("Test: %s\n", title);
    printf("========================================\n");
}

/* ------------------------------------------------------------------
 * Test 1: bench_cfg defaults / input validation
 * ------------------------------------------------------------------ */
static void test_bench_cfg_validation(void) {
    separator("bench_cfg defaults / NULL safety");

    /* NULL config */
    struct bench_result res;
    int rc = bench_run(NULL, &res);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "bench_run(NULL cfg) returns HFSSS_ERR_INVAL");

    /* NULL result */
    struct bench_cfg cfg = {
        .workload   = BENCH_RAND_READ,
        .block_size = 4096,
        .queue_depth= 1,
        .op_count   = 10,
    };
    rc = bench_run(&cfg, NULL);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "bench_run(NULL out) returns HFSSS_ERR_INVAL");

    /* bench_result_print with NULL does not crash */
    bench_result_print(NULL, NULL);
    TEST_ASSERT(true, "bench_result_print(NULL) does not crash");
}

/* ------------------------------------------------------------------
 * Test 2: bench_run RAND_READ at QD=1 returns result
 * ------------------------------------------------------------------ */
static void test_bench_rand_read_qd1(void) {
    separator("bench_run RAND_READ QD=1");

    struct bench_cfg cfg = {
        .workload       = BENCH_RAND_READ,
        .block_size     = 4096,
        .queue_depth    = 1,
        .duration_s     = 0,
        .op_count       = 1000,
        .num_threads    = 1,
        .capacity_bytes = 64ULL * 1024 * 1024,
    };
    struct bench_result res;
    int rc = bench_run(&cfg, &res);

    TEST_ASSERT(rc == HFSSS_OK,           "bench_run returns HFSSS_OK");
    TEST_ASSERT(res.total_ops == 1000,    "total_ops == 1000");
    TEST_ASSERT(res.read_iops > 0.0,      "read_iops > 0");
    TEST_ASSERT(res.elapsed_s > 0.0,      "elapsed_s > 0");
    TEST_ASSERT(res.lat_p50_us > 0,       "lat_p50_us > 0");
}

/* ------------------------------------------------------------------
 * Test 3: bench_run RAND_READ at QD=32, IOPS check (REQ-116)
 * ------------------------------------------------------------------ */
static void test_bench_rand_read_qd32(void) {
    separator("bench_run RAND_READ QD=32 (REQ-116)");

    struct bench_cfg cfg = {
        .workload       = BENCH_RAND_READ,
        .block_size     = 4096,
        .queue_depth    = 32,
        .duration_s     = 0,
        .op_count       = 20000,
        .num_threads    = 1,
        .capacity_bytes = 256ULL * 1024 * 1024,
    };
    struct bench_result res;
    int rc = bench_run(&cfg, &res);

    TEST_ASSERT(rc == HFSSS_OK,            "bench_run returns HFSSS_OK");
    TEST_ASSERT(res.read_iops >= 600000.0, "read IOPS >= 600K (REQ-116)");
    TEST_ASSERT(res.read_iops <= 1000000.0 * 100,
                "read IOPS reasonable upper bound");
}

/* ------------------------------------------------------------------
 * Test 4: bench_run RAND_WRITE at QD=32 (REQ-117)
 * ------------------------------------------------------------------ */
static void test_bench_rand_write_qd32(void) {
    separator("bench_run RAND_WRITE QD=32 (REQ-117)");

    struct bench_cfg cfg = {
        .workload       = BENCH_RAND_WRITE,
        .block_size     = 4096,
        .queue_depth    = 32,
        .duration_s     = 0,
        .op_count       = 20000,
        .num_threads    = 1,
        .capacity_bytes = 256ULL * 1024 * 1024,
    };
    struct bench_result res;
    int rc = bench_run(&cfg, &res);

    TEST_ASSERT(rc == HFSSS_OK,             "bench_run returns HFSSS_OK");
    TEST_ASSERT(res.write_iops >= 150000.0, "write IOPS >= 150K (REQ-117)");
}

/* ------------------------------------------------------------------
 * Test 5: bench_run MIXED 70/30 at QD=32 (REQ-118)
 * ------------------------------------------------------------------ */
static void test_bench_mixed_qd32(void) {
    separator("bench_run MIXED 70/30 QD=32 (REQ-118)");

    struct bench_cfg cfg = {
        .workload       = BENCH_MIXED,
        .block_size     = 4096,
        .queue_depth    = 32,
        .duration_s     = 0,
        .op_count       = 20000,
        .num_threads    = 1,
        .rw_ratio       = 0.7,
        .capacity_bytes = 256ULL * 1024 * 1024,
    };
    struct bench_result res;
    int rc = bench_run(&cfg, &res);

    double total_iops = res.read_iops + res.write_iops;
    TEST_ASSERT(rc == HFSSS_OK,             "bench_run returns HFSSS_OK");
    TEST_ASSERT(total_iops >= 250000.0,     "total IOPS >= 250K (REQ-118)");
    TEST_ASSERT(res.read_iops > 0.0,        "read_iops > 0");
    TEST_ASSERT(res.write_iops > 0.0,       "write_iops > 0");
}

/* ------------------------------------------------------------------
 * Test 6: bench_run SEQ_READ at 128KB (REQ-119 read)
 * ------------------------------------------------------------------ */
static void test_bench_seq_read_128k(void) {
    separator("bench_run SEQ_READ 128KB (REQ-119)");

    struct bench_cfg cfg = {
        .workload       = BENCH_SEQ_READ,
        .block_size     = 131072,
        .queue_depth    = 32,
        .duration_s     = 0,
        .op_count       = 20000,
        .num_threads    = 1,
        .capacity_bytes = 256ULL * 1024 * 1024,
    };
    struct bench_result res;
    int rc = bench_run(&cfg, &res);

    TEST_ASSERT(rc == HFSSS_OK,              "bench_run returns HFSSS_OK");
    TEST_ASSERT(res.read_bw_mbps >= 6500.0,  "read BW >= 6500 MB/s (REQ-119)");
}

/* ------------------------------------------------------------------
 * Test 7: bench_run SEQ_WRITE at 128KB (REQ-119 write)
 * ------------------------------------------------------------------ */
static void test_bench_seq_write_128k(void) {
    separator("bench_run SEQ_WRITE 128KB (REQ-119)");

    struct bench_cfg cfg = {
        .workload       = BENCH_SEQ_WRITE,
        .block_size     = 131072,
        .queue_depth    = 32,
        .duration_s     = 0,
        .op_count       = 20000,
        .num_threads    = 1,
        .capacity_bytes = 256ULL * 1024 * 1024,
    };
    struct bench_result res;
    int rc = bench_run(&cfg, &res);

    TEST_ASSERT(rc == HFSSS_OK,               "bench_run returns HFSSS_OK");
    TEST_ASSERT(res.write_bw_mbps >= 3500.0,  "write BW >= 3500 MB/s (REQ-119)");
}

/* ------------------------------------------------------------------
 * Test 8: bench_run ZIPFIAN workload
 * ------------------------------------------------------------------ */
static void test_bench_zipfian(void) {
    separator("bench_run ZIPFIAN workload");

    struct bench_cfg cfg = {
        .workload       = BENCH_ZIPFIAN,
        .block_size     = 4096,
        .queue_depth    = 32,
        .duration_s     = 0,
        .op_count       = 5000,
        .num_threads    = 1,
        .rw_ratio       = 0.7,
        .zipf_theta     = 0.9,
        .capacity_bytes = 64ULL * 1024 * 1024,
    };
    struct bench_result res;
    int rc = bench_run(&cfg, &res);

    TEST_ASSERT(rc == HFSSS_OK,        "bench_run returns HFSSS_OK");
    TEST_ASSERT(res.total_ops == 5000, "total_ops == 5000");
}

/* ------------------------------------------------------------------
 * Test 9: Latency histogram P50/P99/P99.9 computed correctly (REQ-120)
 * ------------------------------------------------------------------ */
static void test_latency_histogram(void) {
    separator("Latency histogram P50/P99/P99.9 (REQ-120)");

    struct bench_cfg cfg = {
        .workload       = BENCH_RAND_READ,
        .block_size     = 4096,
        .queue_depth    = 1,
        .duration_s     = 0,
        .op_count       = 10000,
        .num_threads    = 1,
        .capacity_bytes = 64ULL * 1024 * 1024,
    };
    struct bench_result res;
    bench_run(&cfg, &res);

    TEST_ASSERT(res.lat_p50_us  <= 100,  "P50 <= 100µs (REQ-120)");
    TEST_ASSERT(res.lat_p99_us  <= 150,  "P99 <= 150µs (REQ-120)");
    TEST_ASSERT(res.lat_p999_us <= 500,  "P99.9 <= 500µs (REQ-120)");
    TEST_ASSERT(res.lat_p50_us  <= res.lat_p99_us,
                "P50 <= P99 ordering");
    TEST_ASSERT(res.lat_p99_us  <= res.lat_p999_us,
                "P99 <= P99.9 ordering");

    /* Histogram entries should sum to total ops */
    uint64_t hist_sum = 0;
    for (int i = 0; i < PERF_LAT_HIST_BUCKETS; i++) hist_sum += res.lat_hist[i];
    TEST_ASSERT(hist_sum == res.total_ops, "histogram sum == total_ops");
}

/* ------------------------------------------------------------------
 * Test 10: NAND timing error < 5% (REQ-121)
 * ------------------------------------------------------------------ */
static void test_nand_timing_accuracy(void) {
    separator("NAND timing accuracy (REQ-121)");

    double err = nand_timing_measure_error();
    TEST_ASSERT(err >= 0.0,  "timing error is non-negative");
    TEST_ASSERT(err < 5.0,   "timing error < 5% (REQ-121)");
    printf("  NAND timing max error: %.4f%%\n", err);
}

/* ------------------------------------------------------------------
 * Test 11: Scalability efficiency > 0 (REQ-122 sanity)
 * ------------------------------------------------------------------ */
static void test_scalability_sanity(void) {
    separator("Scalability efficiency sanity (REQ-122)");

    double eff1 = perf_scalability_efficiency(1);
    TEST_ASSERT(eff1 == 1.0, "single-thread efficiency == 1.0");

    double eff0 = perf_scalability_efficiency(0);
    TEST_ASSERT(eff0 == 0.0, "zero threads returns 0.0");

    double eff4 = perf_scalability_efficiency(4);
    TEST_ASSERT(eff4 > 0.0,  "4-thread efficiency > 0");
    printf("  4-thread parallel efficiency: %.2f%%\n", eff4 * 100.0);
}

/* ------------------------------------------------------------------
 * Test 12: perf_validation_run_all returns a report
 * ------------------------------------------------------------------ */
static void test_validation_run_all(void) {
    separator("perf_validation_run_all returns report");

    /* NULL safety */
    int rc = perf_validation_run_all(NULL);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "run_all(NULL) returns HFSSS_ERR_INVAL");

    struct perf_validation_report report;
    rc = perf_validation_run_all(&report);
    TEST_ASSERT(rc == HFSSS_OK,       "run_all returns HFSSS_OK");
    TEST_ASSERT(report.count > 0,     "report.count > 0");
    TEST_ASSERT(report.count <= 32,   "report.count <= 32");
    TEST_ASSERT(report.passed + report.failed == report.count,
                "passed + failed == count");
    TEST_ASSERT(report.nand_timing_error_pct >= 0.0,
                "nand_timing_error_pct >= 0");

    printf("  Requirements: %d passed, %d failed\n",
           report.passed, report.failed);
}

/* ------------------------------------------------------------------
 * Test 13: JSON report format contains req_id fields
 * ------------------------------------------------------------------ */
static void test_json_report_format(void) {
    separator("JSON report format");

    struct perf_validation_report report;
    perf_validation_run_all(&report);

    char buf[8192];
    int rc = perf_validation_report_to_json(&report, buf, sizeof(buf));
    TEST_ASSERT(rc == HFSSS_OK,                  "to_json returns HFSSS_OK");
    TEST_ASSERT(strstr(buf, "req_id") != NULL,   "JSON contains 'req_id'");
    TEST_ASSERT(strstr(buf, "passed") != NULL,   "JSON contains 'passed'");
    TEST_ASSERT(strstr(buf, "REQ-116") != NULL,  "JSON contains 'REQ-116'");
    TEST_ASSERT(strstr(buf, "REQ-121") != NULL,  "JSON contains 'REQ-121'");
    TEST_ASSERT(strstr(buf, "results") != NULL,  "JSON contains 'results' array");

    /* NULL safety */
    rc = perf_validation_report_to_json(NULL, buf, sizeof(buf));
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "to_json(NULL report) returns HFSSS_ERR_INVAL");

    rc = perf_validation_report_to_json(&report, NULL, 100);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "to_json(NULL buf) returns HFSSS_ERR_INVAL");

    rc = perf_validation_report_to_json(&report, buf, 0);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "to_json(bufsz=0) returns HFSSS_ERR_INVAL");
}

/* ------------------------------------------------------------------
 * Test 14: report_print does not crash
 * ------------------------------------------------------------------ */
static void test_report_print_no_crash(void) {
    separator("report_print does not crash");

    perf_validation_report_print(NULL);
    TEST_ASSERT(true, "report_print(NULL) does not crash");

    struct perf_validation_report report;
    memset(&report, 0, sizeof(report));
    perf_validation_report_print(&report);
    TEST_ASSERT(true, "report_print(empty report) does not crash");

    struct perf_validation_report full_report;
    perf_validation_run_all(&full_report);
    perf_validation_report_print(&full_report);
    TEST_ASSERT(true, "report_print(full report) does not crash");
}

/* ------------------------------------------------------------------
 * Test 15: NULL safety on all public APIs
 * ------------------------------------------------------------------ */
static void test_null_safety(void) {
    separator("NULL safety on all public APIs");

    /* bench_run */
    struct bench_result res;
    int rc = bench_run(NULL, NULL);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "bench_run(NULL, NULL)");

    /* bench_result_print */
    bench_result_print(NULL, NULL);
    TEST_ASSERT(true, "bench_result_print(NULL, NULL)");

    /* perf_validation_run_all */
    rc = perf_validation_run_all(NULL);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "perf_validation_run_all(NULL)");

    /* perf_validation_report_print */
    perf_validation_report_print(NULL);
    TEST_ASSERT(true, "perf_validation_report_print(NULL)");

    /* perf_validation_report_to_json */
    char buf[64];
    rc = perf_validation_report_to_json(NULL, buf, sizeof(buf));
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "to_json(NULL report)");

    rc = perf_validation_report_to_json(NULL, NULL, 0);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "to_json(NULL, NULL, 0)");

    /* nand_timing_measure_error – no NULL params, just runs */
    double err = nand_timing_measure_error();
    TEST_ASSERT(err >= 0.0, "nand_timing_measure_error() returns non-negative");

    /* perf_scalability_efficiency */
    double eff = perf_scalability_efficiency(0);
    TEST_ASSERT(eff == 0.0, "scalability_efficiency(0) == 0.0");

    /* bench_run with zero op_count and zero duration uses internal default */
    struct bench_cfg cfg = {
        .workload       = BENCH_RAND_READ,
        .block_size     = 4096,
        .queue_depth    = 1,
        .duration_s     = 0,
        .op_count       = 0,
        .num_threads    = 0,  /* should default to 1 */
        .capacity_bytes = 0,  /* should use internal default */
    };
    rc = bench_run(&cfg, &res);
    TEST_ASSERT(rc == HFSSS_OK,    "bench_run with zeros uses defaults");
    TEST_ASSERT(res.total_ops > 0, "default op_count > 0");
}

/* ------------------------------------------------------------------
 * Test 16: bench_run rollback path fails fast on pthread_create error
 *
 * Regression for the case where bench_run() returned the correct error
 * code but the already-started workers ran out the configured duration
 * (or op_count budget) before joining. The fix is a shared cancellation
 * flag the workers consult inside their hot loop. We verify it by
 * injecting a fake pthread_create() that succeeds for the first worker
 * and fails for the second, then asserting bench_run() returns long
 * before duration_s elapses.
 * ------------------------------------------------------------------ */

/* Test seam exposed by perf_validation.c (intentionally not in the
 * public header — see the comment near g_pthread_create). */
typedef int (*perf_pthread_create_fn)(pthread_t *, const pthread_attr_t *,
                                       void *(*)(void *), void *);
extern void __perf_test_set_pthread_create(perf_pthread_create_fn fn);

static int g_create_calls = 0;
static int g_create_fail_after = 0;  /* fail when calls exceed this count */

static int failing_pthread_create(pthread_t *t, const pthread_attr_t *a,
                                   void *(*start)(void *), void *arg) {
    g_create_calls++;
    if (g_create_calls > g_create_fail_after) {
        return EAGAIN;
    }
    return pthread_create(t, a, start, arg);
}

static uint64_t test_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void test_bench_run_create_fail_fast(void) {
    separator("bench_run pthread_create-failure rollback fails fast");

    /* Duration-based config: without the cancellation flag, an
     * already-started worker would run the full duration_s before its
     * loop exits, defeating any rollback attempt. */
    struct bench_cfg cfg = {
        .workload       = BENCH_RAND_READ,
        .block_size     = 4096,
        .queue_depth    = 1,
        .duration_s     = 5,        /* would block 5s without the fix */
        .op_count       = 0,
        .num_threads    = 4,        /* fail on the 2nd of 4 */
        .capacity_bytes = 64ULL * 1024 * 1024,
    };

    g_create_calls = 0;
    g_create_fail_after = 1;        /* let #1 succeed, fail #2 */
    __perf_test_set_pthread_create(failing_pthread_create);

    struct bench_result res;
    uint64_t t0 = test_now_ms();
    int rc = bench_run(&cfg, &res);
    uint64_t elapsed_ms = test_now_ms() - t0;

    __perf_test_set_pthread_create(NULL); /* restore real pthread_create */

    TEST_ASSERT(rc != HFSSS_OK,
                "bench_run returns error when a worker create fails");
    TEST_ASSERT(g_create_calls == 2,
                "bench_run attempted exactly 2 creates (1 ok + 1 fail)");
    TEST_ASSERT(elapsed_ms < 1000,
                "rollback fails fast (< 1s) instead of waiting out duration");

    /* Sanity: also exercise count-based mode. With the old behavior the
     * already-started worker would still drain its op_count budget. */
    g_create_calls = 0;
    g_create_fail_after = 0;        /* fail on the very first create */
    __perf_test_set_pthread_create(failing_pthread_create);

    struct bench_cfg cfg_count = {
        .workload       = BENCH_RAND_READ,
        .block_size     = 4096,
        .queue_depth    = 1,
        .op_count       = 1000000,  /* large budget to amplify the bug */
        .num_threads    = 2,
        .capacity_bytes = 64ULL * 1024 * 1024,
    };
    t0 = test_now_ms();
    rc = bench_run(&cfg_count, &res);
    elapsed_ms = test_now_ms() - t0;

    __perf_test_set_pthread_create(NULL);

    TEST_ASSERT(rc != HFSSS_OK,
                "bench_run errors when first create fails");
    TEST_ASSERT(elapsed_ms < 500,
                "no started workers means immediate return");
}

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */
int main(void) {
    printf("========================================\n");
    printf("Performance Validation Test Suite\n");
    printf("========================================\n\n");

    test_bench_cfg_validation();
    printf("\n");
    test_bench_rand_read_qd1();
    printf("\n");
    test_bench_rand_read_qd32();
    printf("\n");
    test_bench_rand_write_qd32();
    printf("\n");
    test_bench_mixed_qd32();
    printf("\n");
    test_bench_seq_read_128k();
    printf("\n");
    test_bench_seq_write_128k();
    printf("\n");
    test_bench_zipfian();
    printf("\n");
    test_latency_histogram();
    printf("\n");
    test_nand_timing_accuracy();
    printf("\n");
    test_scalability_sanity();
    printf("\n");
    test_validation_run_all();
    printf("\n");
    test_json_report_format();
    printf("\n");
    test_report_print_no_crash();
    printf("\n");
    test_null_safety();
    printf("\n");
    test_bench_run_create_fail_fast();

    printf("\n========================================\n");
    printf("Total: %d  Passed: %d  Failed: %d\n", total, passed, failed);
    printf("========================================\n");

    if (failed > 0) {
        printf("FAILURE: %d test(s) failed\n", failed);
        return 1;
    }
    printf("SUCCESS: All %d tests passed\n", total);
    return 0;
}
