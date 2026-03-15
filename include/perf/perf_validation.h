#ifndef HFSSS_PERF_VALIDATION_H
#define HFSSS_PERF_VALIDATION_H

/*
 * Performance Validation Framework (REQ-116 through REQ-123)
 *
 * Provides:
 *  - Built-in benchmark engine with multiple workload types
 *  - Latency histogram (64 exponential buckets)
 *  - Requirement validation against SSD spec targets
 *  - NAND timing accuracy measurement
 *  - Scalability efficiency measurement
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Latency histogram bucket count (matches oob.h LATENCY_HIST_BUCKETS) */
#define PERF_LAT_HIST_BUCKETS  64

/* Workload types */
enum bench_workload {
    BENCH_SEQ_READ,
    BENCH_SEQ_WRITE,
    BENCH_RAND_READ,
    BENCH_RAND_WRITE,
    BENCH_MIXED,    /* configurable rw_ratio */
    BENCH_ZIPFIAN
};

/* Benchmark configuration */
struct bench_cfg {
    enum bench_workload workload;
    uint32_t block_size;      /* bytes: 4096, 131072, etc. */
    uint32_t queue_depth;     /* 1..128 */
    uint32_t duration_s;      /* run time; 0 = use op_count */
    uint64_t op_count;        /* total ops if duration_s == 0 */
    uint32_t num_threads;     /* parallelism */
    double   rw_ratio;        /* 0.0..1.0 fraction of reads for MIXED */
    double   zipf_theta;      /* Zipfian skew; 0.9 typical */
    uint64_t capacity_bytes;  /* simulated device capacity */
};

/* Benchmark result */
struct bench_result {
    double   read_iops;
    double   write_iops;
    double   read_bw_mbps;
    double   write_bw_mbps;
    double   waf;
    uint64_t lat_p50_us;
    uint64_t lat_p99_us;
    uint64_t lat_p999_us;
    uint64_t total_ops;
    double   elapsed_s;
    double   cpu_util_pct;         /* 0 if not measured */
    double   parallel_efficiency;  /* 0 if num_threads == 1 */

    /* Internal histogram (exposed for testing) */
    uint64_t lat_hist[PERF_LAT_HIST_BUCKETS];
};

/* Requirement validation result */
struct perf_req_result {
    const char *req_id;
    const char *description;
    bool        passed;
    double      measured;
    double      target;
    const char *unit;
};

/* Full validation report */
struct perf_validation_report {
    struct perf_req_result results[32];
    int    count;
    int    passed;
    int    failed;
    double nand_timing_error_pct; /* REQ-121 */
};

/* Run a benchmark with the given configuration */
int  bench_run(const struct bench_cfg *cfg, struct bench_result *out);

/* Print benchmark result to stdout */
void bench_result_print(const struct bench_result *r, const struct bench_cfg *cfg);

/* Run all requirement validations and populate report */
int  perf_validation_run_all(struct perf_validation_report *report);

/* Print validation report to stdout */
void perf_validation_report_print(const struct perf_validation_report *report);

/* Serialize validation report to JSON */
int  perf_validation_report_to_json(const struct perf_validation_report *report,
                                    char *buf, size_t bufsz);

/* NAND timing accuracy measurement (REQ-121): returns max error % over N=1000 samples */
double nand_timing_measure_error(void);

/* Scalability efficiency at num_threads (REQ-122): returns throughput_N / (N * throughput_1) */
double perf_scalability_efficiency(uint32_t num_threads);

#ifdef __cplusplus
}
#endif

#endif /* HFSSS_PERF_VALIDATION_H */
