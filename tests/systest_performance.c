/*
 * systest_performance.c -- System-level performance tests for HFSSS.
 *
 * Covers throughput, WAF, latency percentiles, IOPS scaling,
 * SLO validation, NAND timing accuracy, and Zipfian workload
 * behavior through the built-in benchmark engine.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "perf/perf_validation.h"
#include "common/common.h"

/* ---------------------------------------------------------------
 * Test harness
 * ------------------------------------------------------------- */
static int total_tests = 0;
static int passed_tests = 0;
static int failed_tests = 0;

#define TEST_ASSERT(cond, msg) do { \
    total_tests++; \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        passed_tests++; \
    } else { \
        printf("  [FAIL] %s\n", msg); \
        failed_tests++; \
    } \
} while (0)

/* ---------------------------------------------------------------
 * PS-001: Large geometry throughput
 * ------------------------------------------------------------- */
static void test_ps_001(void) {
    printf("\n--- PS-001: Large geometry throughput ---\n");

    /* 8ch x 4chip x 2die => ~64GB capacity equivalent */
    const uint64_t capacity = 64ULL * 1024 * 1024 * 1024;

    /* Sequential write: 50K ops */
    struct bench_cfg cfg_w = {
        .workload       = BENCH_SEQ_WRITE,
        .block_size     = 4096,
        .queue_depth    = 32,
        .duration_s     = 0,
        .op_count       = 50000,
        .num_threads    = 1,
        .rw_ratio       = 0.0,
        .zipf_theta     = 0.0,
        .capacity_bytes = capacity,
    };
    struct bench_result res_w;
    int rc_w = bench_run(&cfg_w, &res_w);

    TEST_ASSERT(rc_w == 0, "PS-001: seq write completes (rc == 0)");
    TEST_ASSERT(res_w.write_iops > 0.0,
                "PS-001: seq write IOPS > 0");
    printf("  seq_write: %.0f IOPS, %.2f MB/s\n",
           res_w.write_iops, res_w.write_bw_mbps);

    /* Sequential read: 50K ops */
    struct bench_cfg cfg_r = {
        .workload       = BENCH_SEQ_READ,
        .block_size     = 4096,
        .queue_depth    = 32,
        .duration_s     = 0,
        .op_count       = 50000,
        .num_threads    = 1,
        .rw_ratio       = 0.0,
        .zipf_theta     = 0.0,
        .capacity_bytes = capacity,
    };
    struct bench_result res_r;
    int rc_r = bench_run(&cfg_r, &res_r);

    TEST_ASSERT(rc_r == 0, "PS-001: seq read completes (rc == 0)");
    TEST_ASSERT(res_r.read_iops > 0.0,
                "PS-001: seq read IOPS > 0");
    printf("  seq_read:  %.0f IOPS, %.2f MB/s\n",
           res_r.read_iops, res_r.read_bw_mbps);
}

/* ---------------------------------------------------------------
 * PS-002: Sustained write WAF measurement
 * ------------------------------------------------------------- */
static void test_ps_002(void) {
    printf("\n--- PS-002: Sustained write WAF measurement ---\n");

    struct bench_cfg cfg = {
        .workload       = BENCH_RAND_WRITE,
        .block_size     = 4096,
        .queue_depth    = 32,
        .duration_s     = 0,
        .op_count       = 100000,
        .num_threads    = 1,
        .rw_ratio       = 0.0,
        .zipf_theta     = 0.0,
        .capacity_bytes = 2ULL * 1024 * 1024 * 1024,
    };
    struct bench_result res;
    int rc = bench_run(&cfg, &res);

    TEST_ASSERT(rc == 0, "PS-002: rand write completes (rc == 0)");
    TEST_ASSERT(res.waf >= 1.0,
                "PS-002: WAF >= 1.0 (physical writes >= logical)");
    printf("  WAF = %.3f\n", res.waf);
}

/* ---------------------------------------------------------------
 * PS-003: WAF sequential vs random comparison
 * ------------------------------------------------------------- */
static void test_ps_003(void) {
    printf("\n--- PS-003: WAF sequential vs random comparison ---\n");

    const uint64_t capacity = 2ULL * 1024 * 1024 * 1024;

    /* Sequential write */
    struct bench_cfg cfg_seq = {
        .workload       = BENCH_SEQ_WRITE,
        .block_size     = 4096,
        .queue_depth    = 1,
        .duration_s     = 0,
        .op_count       = 50000,
        .num_threads    = 1,
        .rw_ratio       = 0.0,
        .zipf_theta     = 0.0,
        .capacity_bytes = capacity,
    };
    struct bench_result res_seq;
    int rc_seq = bench_run(&cfg_seq, &res_seq);

    TEST_ASSERT(rc_seq == 0, "PS-003: seq write completes");

    /* Random write */
    struct bench_cfg cfg_rand = {
        .workload       = BENCH_RAND_WRITE,
        .block_size     = 4096,
        .queue_depth    = 1,
        .duration_s     = 0,
        .op_count       = 50000,
        .num_threads    = 1,
        .rw_ratio       = 0.0,
        .zipf_theta     = 0.0,
        .capacity_bytes = capacity,
    };
    struct bench_result res_rand;
    int rc_rand = bench_run(&cfg_rand, &res_rand);

    TEST_ASSERT(rc_rand == 0, "PS-003: rand write completes");

    double waf_seq  = res_seq.waf;
    double waf_rand = res_rand.waf;

    TEST_ASSERT(waf_seq <= waf_rand,
                "PS-003: seq WAF <= rand WAF");
    printf("  waf_seq = %.3f, waf_rand = %.3f\n", waf_seq, waf_rand);
}

/* ---------------------------------------------------------------
 * PS-004: Latency percentiles
 * ------------------------------------------------------------- */
static void test_ps_004(void) {
    printf("\n--- PS-004: Latency percentiles ---\n");

    struct bench_cfg cfg = {
        .workload       = BENCH_RAND_READ,
        .block_size     = 4096,
        .queue_depth    = 1,
        .duration_s     = 0,
        .op_count       = 50000,
        .num_threads    = 1,
        .rw_ratio       = 0.0,
        .zipf_theta     = 0.0,
        .capacity_bytes = 2ULL * 1024 * 1024 * 1024,
    };
    struct bench_result res;
    int rc = bench_run(&cfg, &res);

    TEST_ASSERT(rc == 0, "PS-004: rand read completes");
    TEST_ASSERT(res.lat_p50_us > 0,
                "PS-004: p50 latency > 0");
    TEST_ASSERT(res.lat_p50_us <= res.lat_p99_us,
                "PS-004: p50 <= p99 (monotonic)");
    TEST_ASSERT(res.lat_p99_us <= res.lat_p999_us,
                "PS-004: p99 <= p999 (monotonic)");
    printf("  p50=%lu us, p99=%lu us, p999=%lu us\n",
           (unsigned long)res.lat_p50_us,
           (unsigned long)res.lat_p99_us,
           (unsigned long)res.lat_p999_us);
}

/* ---------------------------------------------------------------
 * PS-005: IOPS scaling with parallelism
 * ------------------------------------------------------------- */
static void test_ps_005(void) {
    printf("\n--- PS-005: IOPS scaling with parallelism ---\n");

    const uint64_t capacity = 2ULL * 1024 * 1024 * 1024;

    /* Single-threaded baseline */
    struct bench_cfg cfg_1 = {
        .workload       = BENCH_RAND_READ,
        .block_size     = 4096,
        .queue_depth    = 32,
        .duration_s     = 0,
        .op_count       = 10000,
        .num_threads    = 1,
        .rw_ratio       = 0.0,
        .zipf_theta     = 0.0,
        .capacity_bytes = capacity,
    };
    struct bench_result res_1;
    int rc_1 = bench_run(&cfg_1, &res_1);

    TEST_ASSERT(rc_1 == 0, "PS-005: single-thread completes");

    /* 4-thread run */
    struct bench_cfg cfg_4 = {
        .workload       = BENCH_RAND_READ,
        .block_size     = 4096,
        .queue_depth    = 32,
        .duration_s     = 0,
        .op_count       = 10000,
        .num_threads    = 4,
        .rw_ratio       = 0.0,
        .zipf_theta     = 0.0,
        .capacity_bytes = capacity,
    };
    struct bench_result res_4;
    int rc_4 = bench_run(&cfg_4, &res_4);

    TEST_ASSERT(rc_4 == 0, "PS-005: 4-thread completes");

    double iops_1 = res_1.read_iops;
    double iops_4 = res_4.read_iops;

    TEST_ASSERT(iops_4 > iops_1,
                "PS-005: 4-thread IOPS > single-thread IOPS");
    printf("  iops_1t = %.0f, iops_4t = %.0f\n", iops_1, iops_4);

    /* Also check via the dedicated efficiency API */
    double eff = perf_scalability_efficiency(4);
    TEST_ASSERT(eff > 0.0,
                "PS-005: scalability efficiency(4) > 0.0");
    printf("  scalability_efficiency(4) = %.3f\n", eff);
}

/* ---------------------------------------------------------------
 * PS-006: Performance SLO validation
 * ------------------------------------------------------------- */
static void test_ps_006(void) {
    printf("\n--- PS-006: Performance SLO validation ---\n");

    struct perf_validation_report report;
    int rc = perf_validation_run_all(&report);

    TEST_ASSERT(rc == 0, "PS-006: perf_validation_run_all succeeds");
    TEST_ASSERT(report.count > 0,
                "PS-006: framework reported > 0 results");

    printf("  SLO results: %d total, %d passed, %d failed\n",
           report.count, report.passed, report.failed);

    for (int i = 0; i < report.count; i++) {
        const struct perf_req_result *r = &report.results[i];
        printf("    [%s] %s: measured=%.2f target=%.2f %s\n",
               r->passed ? "PASS" : "FAIL",
               r->req_id ? r->req_id : "???",
               r->measured, r->target,
               r->unit ? r->unit : "");
    }
}

/* ---------------------------------------------------------------
 * PS-007: NAND timing accuracy
 * ------------------------------------------------------------- */
static void test_ps_007(void) {
    printf("\n--- PS-007: NAND timing accuracy ---\n");

    double error_pct = nand_timing_measure_error();

    TEST_ASSERT(error_pct < 20.0,
                "PS-007: NAND timing error < 20%%");
    printf("  max timing error = %.2f%%\n", error_pct);
}

/* ---------------------------------------------------------------
 * PS-008: Zipfian workload WAF and latency
 * ------------------------------------------------------------- */
static void test_ps_008(void) {
    printf("\n--- PS-008: Zipfian workload WAF and latency ---\n");

    struct bench_cfg cfg = {
        .workload       = BENCH_ZIPFIAN,
        .block_size     = 4096,
        .queue_depth    = 1,
        .duration_s     = 0,
        .op_count       = 50000,
        .num_threads    = 1,
        .rw_ratio       = 0.0,
        .zipf_theta     = 0.9,
        .capacity_bytes = 2ULL * 1024 * 1024 * 1024,
    };
    struct bench_result res;
    int rc = bench_run(&cfg, &res);

    TEST_ASSERT(rc == 0, "PS-008: Zipfian workload completes");
    TEST_ASSERT(res.waf >= 1.0,
                "PS-008: Zipfian WAF >= 1.0");
    TEST_ASSERT(res.lat_p99_us > 0,
                "PS-008: Zipfian p99 latency > 0");
    printf("  WAF = %.3f, p99 = %lu us\n",
           res.waf, (unsigned long)res.lat_p99_us);
}

/* ---------------------------------------------------------------
 * main
 * ------------------------------------------------------------- */
int main(void) {
    printf("========================================\n");
    printf("HFSSS System-Level Performance Tests\n");
    printf("========================================\n");

    uint64_t t0 = get_time_ns();

    test_ps_001();
    test_ps_002();
    test_ps_003();
    test_ps_004();
    test_ps_005();
    test_ps_006();
    test_ps_007();
    test_ps_008();

    uint64_t elapsed_ms = (get_time_ns() - t0) / 1000000ULL;

    printf("\n========================================\n");
    printf("Summary: %d total, %d passed, %d failed  (%.1f s)\n",
           total_tests, passed_tests, failed_tests,
           (double)elapsed_ms / 1000.0);
    printf("========================================\n");

    return (failed_tests == 0) ? 0 : 1;
}
