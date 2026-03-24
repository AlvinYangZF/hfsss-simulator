# Performance Validation Low-Level Design

## Revision History

| Version | Date       | Author | Description     |
|---------|------------|--------|-----------------|
| V1.0    | 2026-03-15 | HFSSS  | Initial release |

## Table of Contents

1. [Module Overview](#1-module-overview)
2. [Requirements Traceability](#2-requirements-traceability)
3. [Data Structure Design](#3-data-structure-design)
4. [Header File Design](#4-header-file-design)
5. [Function Interface Design](#5-function-interface-design)
6. [Validation Methods and Test Matrix](#6-validation-methods-and-test-matrix)
7. [Test Plan](#7-test-plan)

---

## 1. Module Overview

The Performance Validation module (`perf_validator`) is the simulator's self-auditing component, responsible for verifying whether the simulator meets all performance, accuracy, and stability targets specified in PRD Chapter 6. This module does not depend on external test frameworks and is fully embedded within the simulator process, executable as a standalone test suite in CI/CD pipelines or triggerable on-demand via the OOB interface.

The module consists of three subsystems:

1. **Built-in Benchmark Engine (`bench_engine`)**: A lightweight synthetic workload generator capable of producing sequential, random, mixed, and Zipfian-distributed I/O requests, with configurable queue depth, block size, read/write ratio, thread count, and complete latency histogram and throughput metric collection;
2. **NAND Timing Accuracy Validator (`timing_validator`)**: White-box validation of the simulator's NAND timing model, comparing simulated elapsed time per operation against reference datasheet values, calculating error percentages, and ensuring the 5% accuracy target is met;
3. **Scalability and Stability Validator (`scalability/stability_validator`)**: Quantifies parallel efficiency through multi-threaded scaling experiments, and detects memory leaks, data races, and data integrity issues through long-duration stress runs.

The top-level function `perf_validation_run_all` serially invokes the above subsystems in deterministic order, aggregates pass/fail results for each requirement (REQ-112 through REQ-123, REQ-131 through REQ-134), and outputs the structured report simultaneously in JSON and human-readable text formats. If any P0 requirement verification fails, the function returns a non-zero exit code, triggering CI build failure.

**Requirements Coverage**: REQ-112, REQ-113, REQ-114, REQ-115, REQ-116, REQ-117, REQ-118, REQ-119, REQ-122, REQ-131, REQ-132, REQ-133, REQ-134.

---

## 2. Requirements Traceability

| Req ID  | Description | Quantitative Target | Priority | Target Version |
|---------|-------------|---------------------|----------|----------------|
| REQ-112 | Random read IOPS (4KB, QD=32) | With ECC: 600K IOPS; without ECC peak: 1M IOPS | P0 | V1.0 |
| REQ-113 | Random write IOPS (4KB, QD=32) | FOOB: 300K; steady-state (90% fill): 150K IOPS | P0 | V1.0 |
| REQ-114 | Mixed R/W IOPS (4KB, 70R/30W, QD=32) | Steady-state: 250K IOPS | P0 | V1.5 |
| REQ-115 | Sequential R/W bandwidth | Seq read 128KB: 6.5 GB/s; Seq write 128KB: 3.5 GB/s | P0 | V1.5 |
| REQ-116 | Latency performance (QD=1) | Read P50 80us; P99 150us; P99.9 300us | P0 | V1.5 |
| REQ-117 | NAND timing simulation accuracy | tR/tPROG/tERS error all <5% | P0 | V2.0 |
| REQ-118 | Scalability | Linear IOPS growth from 64 to 256 cores | P0 | V2.0 |
| REQ-119 | Resource utilization | NAND media thread CPU 70-90%; host OS CPU <10% | P0 | V2.0 |
| REQ-122 | fio tool compatibility | io_uring+direct=1, numjobs=32, iodepth=128 runs normally | P0 | V1.0 |
| REQ-131 | Fault injection interface stability | All injection modes work correctly | P0 | V2.5 |
| REQ-132 | MTBF target | Normal load MTBF >= 720 hours; no panic under fault injection | P0 | V2.0 |
| REQ-133 | Data integrity | No fault injection: readback md5sum 100% consistent | P0 | V1.0 |
| REQ-134 | Stability requirements | 72hr stress test no crash; memory growth <1MB/hr; TSan/ASan zero reports | P0 | V2.0 |

---

## 3. Data Structure Design

### 3.1 Benchmark Configuration and Results

```c
enum bench_pattern {
    BENCH_SEQ_READ   = 0,
    BENCH_SEQ_WRITE  = 1,
    BENCH_RAND_READ  = 2,
    BENCH_RAND_WRITE = 3,
    BENCH_MIXED      = 4,
    BENCH_ZIPFIAN    = 5,
};

#define BENCH_HIST_BUCKETS   64
#define BENCH_MAX_THREADS    256

struct bench_config {
    enum bench_pattern pattern;
    uint32_t           queue_depth;
    uint32_t           block_size_bytes;
    uint32_t           rw_ratio_pct;
    uint64_t           duration_seconds;
    uint64_t           op_count;
    uint64_t           lba_range_sectors;
    uint64_t           lba_start_sector;
    uint32_t           num_threads;
    bool               verify_data;
    char               scenario_name[64];
};

struct bench_result {
    uint64_t total_ops;
    uint64_t total_read_ops;
    uint64_t total_write_ops;
    uint64_t total_bytes;
    uint64_t error_count;
    double   elapsed_seconds;
    double   iops;
    double   bw_mbps;
    double   read_iops;
    double   write_iops;
    uint64_t lat_hist[BENCH_HIST_BUCKETS];
    uint64_t lat_p50_us;
    uint64_t lat_p99_us;
    uint64_t lat_p999_us;
    uint64_t lat_min_us;
    uint64_t lat_max_us;
    bool     passed;
    char     fail_reason[256];
};
```

### 3.2 NAND Timing Accuracy Validation

```c
#define TIMING_REF_TR_LSB_US      35
#define TIMING_REF_TR_CSB_US      70
#define TIMING_REF_TR_MSB_US     100
#define TIMING_REF_TPROG_SLC_US  100
#define TIMING_REF_TPROG_TLC_US  800
#define TIMING_REF_TERS_US      3000
#define TIMING_ACCURACY_THRESHOLD_PCT  5.0

enum timing_op_type {
    TIMING_OP_READ_LSB = 0, TIMING_OP_READ_CSB, TIMING_OP_READ_MSB,
    TIMING_OP_PROG_SLC, TIMING_OP_PROG_TLC, TIMING_OP_ERASE,
    TIMING_OP_COUNT
};

struct timing_op_result {
    enum timing_op_type op_type;
    uint64_t sample_count;
    uint64_t ref_value_us;
    double   measured_avg_us;
    double   measured_stddev_us;
    double   accuracy_error_pct;
    bool     passed;
};

struct timing_validation_result {
    struct timing_op_result ops[TIMING_OP_COUNT];
    bool   all_passed;
    double worst_error_pct;
    char   worst_op_name[32];
};
```

### 3.3 Scalability and Stability Results

```c
struct scalability_result {
    struct { uint32_t thread_count; double iops; double parallel_efficiency; } levels[8];
    uint32_t level_count;
    double   iops_single_thread;
    double   amdahl_serial_fraction;
    double   efficiency_at_16t;
    bool     passed;
    char     fail_reason[256];
};

struct stability_result {
    uint64_t elapsed_seconds;
    uint64_t crash_count;
    uint64_t data_corruption_count;
    uint64_t tsan_race_count;
    uint64_t asan_heap_error_count;
    double   mem_growth_mb_per_hour;
    double   peak_nand_thread_cpu_pct;
    double   peak_host_os_cpu_pct;
    bool     passed;
    char     fail_reason[512];
};

struct integrity_result {
    uint64_t total_regions;
    uint64_t passed_regions;
    uint64_t failed_regions;
    double   integrity_pct;
    bool     all_passed;
};

struct perf_validation_report {
    char     hfsss_version[32];
    char     run_timestamp[32];
    uint32_t host_cpu_cores;
    struct pv_req_result reqs[13];
    struct bench_result iops_rand_read, iops_rand_write, iops_mixed;
    struct bench_result bw_seq_read, bw_seq_write, latency_qd1;
    struct timing_validation_result timing;
    struct scalability_result scalability;
    struct stability_result stability;
    struct integrity_result integrity;
    uint32_t total_requirements;
    uint32_t passed_requirements;
    uint32_t failed_requirements;
    bool     overall_passed;
};
```

---

## 4. Header File Design

```c
/* include/common/perf_validator.h */
#ifndef __HFSSS_PERF_VALIDATOR_H
#define __HFSSS_PERF_VALIDATOR_H

struct bench_engine_ctx *bench_engine_init(struct sssim_ctx *sssim);
void bench_engine_destroy(struct bench_engine_ctx *eng);
int bench_engine_run(struct bench_engine_ctx *eng,
                     const struct bench_config *cfg, struct bench_result *out);
int bench_run_scenario(struct bench_engine_ctx *eng,
                       const char *scenario_name, struct bench_result *out);
int timing_accuracy_validate(struct nand_ctx *nand,
                              struct timing_validation_result *out);
int scalability_test_run(struct bench_engine_ctx *eng,
                          uint32_t max_threads, struct scalability_result *out);
int stability_check_run(struct bench_engine_ctx *eng,
                         const struct stability_config *cfg, struct stability_result *out);
bool stability_check_memory_leaks(const struct bench_engine_ctx *eng);
int data_integrity_verify(struct sssim_ctx *sssim, uint64_t lba_start,
                           uint64_t lba_count, uint32_t block_size_bytes,
                           struct integrity_result *out);
int perf_validation_run_all(struct sssim_ctx *sssim, const char *report_path,
                             struct perf_validation_report *report_out);
uint64_t bench_hist_percentile(const uint64_t *hist, uint64_t total_ops, double percentile);

#endif
```

---

## 5. Function Interface Design

### 5.1 bench_engine Internal Architecture

The bench engine creates `num_threads` worker threads, each submitting I/O through the same NVMe command path as the host driver, maintaining per-thread outstanding command count for queue depth control, and collecting latency histograms via atomic operations.

### 5.2 NAND Timing Accuracy Validation

Issues N=1000 isolated NAND operations per type, measures simulated elapsed time, computes mean/stddev, and verifies error < 5% against reference spec values.

### 5.3 Scalability Test

Sweeps thread counts {1,2,4,8,16,32,64,128}, measuring IOPS at each level and computing parallel efficiency. Estimates Amdahl serial fraction from 16-thread efficiency.

### 5.4 Top-Level Orchestration

```
perf_validation_run_all(sssim, report_path, report_out):
  Step 1:  Random read IOPS (REQ-112) - pass if >= 600K
  Step 2:  Random write IOPS (REQ-113) - pass if >= 150K steady-state
  Step 3:  Mixed R/W IOPS (REQ-114) - pass if >= 250K
  Step 4:  Sequential read BW (REQ-115) - pass if >= 6656 MB/s
  Step 5:  Sequential write BW (REQ-115) - pass if >= 3584 MB/s
  Step 6:  Latency QD=1 (REQ-116) - pass if P50<=100, P99<=150, P999<=500
  Step 7:  NAND timing accuracy (REQ-117) - pass if all <5%
  Step 8:  Scalability (REQ-118) - pass if efficiency@16t >= 0.70
  Step 9:  Data integrity (REQ-133) - pass if 100% match
  Step 10: Memory leak check (REQ-134 partial)
  Write JSON + text reports
  return failed_count
```

---

## 6. Validation Methods and Test Matrix

### 6.1 Performance Target Thresholds

| Scenario | Configuration | Pass Threshold |
|----------|---------------|----------------|
| Random read IOPS | 4KB, QD=32, 32 threads, with ECC | >= 600,000 IOPS |
| Random write IOPS (steady) | 4KB, QD=32, 90% fill | >= 150,000 IOPS |
| Mixed R/W IOPS | 4KB, QD=32, 70R/30W | >= 250,000 IOPS |
| Sequential read BW | 128KB, QD=32 | >= 6,656 MB/s (6.5 GB/s) |
| Sequential write BW | 128KB, QD=32 | >= 3,584 MB/s (3.5 GB/s) |
| Read latency P50/P99/P99.9 | 4KB, QD=1 | <= 100/150/500 us |
| NAND timing accuracy | N=1000 isolated ops | < 5% error |
| Parallel efficiency @16 threads | vs single thread | > 70% |

### 6.2 fio Equivalent Commands

```bash
# REQ-112: Random read IOPS
fio --filename=/dev/nvme0n1 --rw=randread --bs=4k \
    --iodepth=32 --numjobs=32 --ioengine=io_uring \
    --direct=1 --runtime=60 --name=randread_qd32

# REQ-115: Sequential read BW
fio --filename=/dev/nvme0n1 --rw=read --bs=128k \
    --iodepth=32 --numjobs=8 --ioengine=io_uring \
    --direct=1 --runtime=30 --name=seqread_128k
```

---

## 7. Test Plan

| Test ID | Description | Verification Point |
|---------|-------------|-------------------|
| PV-001 | Random read IOPS baseline (with ECC, QD=32) | iops >= 600,000 |
| PV-002 | Random read IOPS peak (no ECC, QD=32) | iops >= 1,000,000 |
| PV-003 | Random write IOPS (FOOB, QD=32) | iops >= 300,000 |
| PV-004 | Random write IOPS (steady 90% fill, QD=32) | iops >= 150,000 |
| PV-005 | Mixed R/W IOPS (70R/30W, QD=32) | iops >= 250,000 |
| PV-006 | Sequential read BW (128KB) | bw_mbps >= 6,656 |
| PV-007 | Sequential write BW (128KB) | bw_mbps >= 3,584 |
| PV-008 | Read latency P50 (QD=1, 4KB) | <= 100 us |
| PV-009 | Read latency P99 (QD=1, 4KB) | <= 150 us |
| PV-010 | Read latency P99.9 (QD=1, 4KB) | <= 500 us |
| PV-011 | NAND tR accuracy (TLC LSB/CSB/MSB) | error < 5% |
| PV-012 | NAND tPROG accuracy (TLC) | error < 5% |
| PV-013 | NAND tERS accuracy | error < 5% |
| PV-014 | Parallel efficiency @16 threads | > 0.70 |
| PV-015 | fio io_uring compatibility | exit code 0, no I/O errors |
| PV-016 | Data integrity (4KB sequential write+read) | md5sum 100% match |
| PV-017 | Data integrity during GC | Write during GC, readback intact |
| PV-018 | ThreadSanitizer zero reports | No "DATA RACE" in 10min run |
| PV-019 | AddressSanitizer zero errors | No heap errors in 10min run |
| PV-020 | Memory leak rate (1hr load) | RSS growth < 1 MB/hr |
| PV-021 | 72-hour stress test | crash_count == 0 |
| PV-022 | Report JSON validity | jq parses successfully |

---

**Document Statistics**:
- Requirements covered: 13 (REQ-112 through REQ-119, REQ-122, REQ-131 through REQ-134)
- New header files: `include/common/perf_validator.h`, `include/common/perf_validator_types.h`
- Function interfaces: 16
- Test cases: 22+

## Appendix: Cross-References

| Reference | Document |
|-----------|----------|
| NAND timing model | LLD_03_NAND_MEDIA (not in scope) |
| OOB performance queries | LLD_07_OOB_MANAGEMENT |
| Fault injection testing | LLD_08_FAULT_INJECTION |
| QoS determinism | LLD_18_QOS_DETERMINISM |
