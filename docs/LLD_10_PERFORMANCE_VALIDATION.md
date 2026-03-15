# 高保真全栈SSD模拟器（HFSSS）详细设计文档

**文档名称**：性能验证详细设计
**文档版本**：V1.0
**编制日期**：2026-03-15
**设计阶段**：V2.0 (Beta)
**密级**：内部资料

---

## 目录

1. [模块概述](#1-模块概述)
2. [功能需求详细分解](#2-功能需求详细分解)
3. [数据结构详细设计](#3-数据结构详细设计)
4. [头文件设计](#4-头文件设计)
5. [函数接口详细设计](#5-函数接口详细设计)
6. [验证方法与测试矩阵](#6-验证方法与测试矩阵)
7. [测试要点](#7-测试要点)

---

## 1. 模块概述

性能验证模块（Performance Validator，`perf_validator`）是仿真器的自我审计组件，负责验证仿真器是否满足PRD第六章所规定的全部性能、精度与稳定性指标。该模块不依赖外部测试框架，完全内嵌于仿真器进程内部，可在CI/CD流水线中作为独立测试套件执行，也可通过OOB接口在运行时按需触发。

模块由三个子系统组成：

1. **内置基准测试引擎（bench_engine）**：轻量级合成工作负载生成器，能够产生顺序、随机、混合及Zipfian分布的I/O请求，支持可配置的队列深度、块大小、读写比、线程数，并采集完整的延迟直方图与吞吐量指标；
2. **NAND时序精度验证器（timing_validator）**：对仿真器的NAND时序模型进行白盒验证，逐操作对比仿真经过时间与参考数据手册标称值，计算误差百分比，确保满足5%以内的精度目标；
3. **可扩展性与稳定性验证器（scalability/stability_validator）**：通过多线程扩展实验量化并行效率，通过长时间压力运行检测内存泄漏、数据竞争与数据完整性问题。

顶层函数 `perf_validation_run_all` 以确定性顺序串行调用上述子系统，汇总每项需求（REQ-112至REQ-123、REQ-131至REQ-134）的通过/失败结果，并将结构化报告同时输出为JSON和人类可读文本格式。若任一P0需求验证失败，函数返回非零退出码，触发CI构建失败。

**覆盖需求**：REQ-112、REQ-113、REQ-114、REQ-115、REQ-116、REQ-117、REQ-118、REQ-119、REQ-122、REQ-131、REQ-132、REQ-133、REQ-134。

---

## 2. 功能需求详细分解

| 需求ID | 需求描述 | 量化目标 | 优先级 | 目标版本 |
|--------|----------|----------|--------|----------|
| REQ-112 | 随机读IOPS（4KB，QD=32） | 含ECC目标600K IOPS；无ECC峰值1M IOPS | P0 | V1.0 |
| REQ-113 | 随机写IOPS（4KB，QD=32） | FOOB目标300K；稳态（90%填充）150K IOPS | P0 | V1.0 |
| REQ-114 | 混合读写IOPS（4KB，70R/30W，QD=32） | 稳态目标250K IOPS | P0 | V1.5 |
| REQ-115 | 顺序读写带宽 | 顺序读128KB目标6.5 GB/s；顺序写128KB目标3.5 GB/s | P0 | V1.5 |
| REQ-116 | 延迟性能（QD=1） | 读P50 80µs；读P99 150µs；读P99.9 300µs | P0 | V1.5 |
| REQ-117 | NAND时序仿真精度 | tR/tPROG/tERS误差均<5% | P0 | V2.0 |
| REQ-118 | 可扩展性 | CPU核心从64扩展至256核心IOPS线性增长 | P0 | V2.0 |
| REQ-119 | 资源利用率 | NAND介质线程CPU 70–90%；宿主机OS CPU<10% | P0 | V2.0 |
| REQ-122 | fio工具兼容性 | io_uring+direct=1，numjobs=32，iodepth=128正常运行 | P0 | V1.0 |
| REQ-131 | 故障注入接口稳定性 | 所有注入模式（立即/延迟/概率/持续）正确生效 | P0 | V2.5 |
| REQ-132 | MTBF目标 | 正常负载下MTBF≥720小时；故障注入时不发生Panic | P0 | V2.0 |
| REQ-133 | 数据完整性 | 无故障注入时读回数据md5sum 100%一致 | P0 | V1.0 |
| REQ-134 | 稳定性需求 | 72小时压力测试无Crash；内存增长<1MB/小时；TSan/ASan零报告 | P0 | V2.0 |

---

## 3. 数据结构详细设计

### 3.1 基准测试配置与结果

```c
#ifndef __HFSSS_PERF_VALIDATOR_TYPES_H
#define __HFSSS_PERF_VALIDATOR_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

/* I/O workload pattern for bench_engine */
enum bench_pattern {
    BENCH_SEQ_READ   = 0,  /* sequential read, ascending LBA */
    BENCH_SEQ_WRITE  = 1,  /* sequential write, ascending LBA */
    BENCH_RAND_READ  = 2,  /* random read, uniform distribution */
    BENCH_RAND_WRITE = 3,  /* random write, uniform distribution */
    BENCH_MIXED      = 4,  /* mixed read/write, ratio from rw_ratio_pct */
    BENCH_ZIPFIAN    = 5,  /* random read/write with Zipfian skew (theta=0.99) */
};

#define BENCH_HIST_BUCKETS   64   /* exponential latency buckets: 1µs, 2µs, 4µs … */
#define BENCH_MAX_THREADS    256
#define BENCH_SCENARIO_NAME_LEN 64

/*
 * bench_config – parameters for a single benchmark run.
 * All fields are read-only after bench_engine_run() begins.
 */
struct bench_config {
    enum bench_pattern pattern;
    uint32_t           queue_depth;         /* outstanding I/Os per thread (1–128) */
    uint32_t           block_size_bytes;    /* transfer size (512 – 1M) */
    uint32_t           rw_ratio_pct;        /* read percentage for MIXED/ZIPFIAN (0–100) */
    uint64_t           duration_seconds;    /* run time; 0 = use op_count */
    uint64_t           op_count;            /* total ops; ignored when duration_seconds > 0 */
    uint64_t           lba_range_sectors;   /* LBA window in 512B sectors; 0 = full device */
    uint64_t           lba_start_sector;    /* starting LBA offset */
    uint32_t           num_threads;         /* worker thread count (1–BENCH_MAX_THREADS) */
    bool               verify_data;         /* md5sum verification on each read */
    char               scenario_name[BENCH_SCENARIO_NAME_LEN];
};

/*
 * bench_result – output of a single benchmark run.
 * Populated by bench_engine_run(); caller must not free.
 */
struct bench_result {
    /* Raw counters */
    uint64_t total_ops;
    uint64_t total_read_ops;
    uint64_t total_write_ops;
    uint64_t total_bytes;
    uint64_t error_count;

    /* Timing */
    double   elapsed_seconds;

    /* Derived throughput */
    double   iops;             /* total_ops / elapsed_seconds */
    double   bw_mbps;          /* total_bytes / elapsed_seconds / 1e6 */
    double   read_iops;
    double   write_iops;

    /* Latency histogram (read + write combined), exponential buckets */
    uint64_t lat_hist[BENCH_HIST_BUCKETS];

    /* Latency percentiles (µs) */
    uint64_t lat_p50_us;
    uint64_t lat_p99_us;
    uint64_t lat_p999_us;
    uint64_t lat_min_us;
    uint64_t lat_max_us;

    /* Pass/fail verdict against threshold set by the scenario */
    bool     passed;
    char     fail_reason[256]; /* human-readable reason if !passed */
};
```

### 3.2 NAND时序精度验证

```c
/*
 * Reference timing values for TLC NAND (typical process node).
 * All values in microseconds.  Source: PRD Section 6.4 + REQ-038.
 */
#define TIMING_REF_TR_LSB_US      35    /* page read, LSB */
#define TIMING_REF_TR_CSB_US      70    /* page read, CSB */
#define TIMING_REF_TR_MSB_US     100    /* page read, MSB */
#define TIMING_REF_TPROG_SLC_US  100    /* page program, SLC mode */
#define TIMING_REF_TPROG_TLC_US  800    /* page program, TLC mode */
#define TIMING_REF_TERS_US      3000    /* block erase */
#define TIMING_ACCURACY_THRESHOLD_PCT  5.0  /* maximum allowed error % */

enum timing_op_type {
    TIMING_OP_READ_LSB  = 0,
    TIMING_OP_READ_CSB  = 1,
    TIMING_OP_READ_MSB  = 2,
    TIMING_OP_PROG_SLC  = 3,
    TIMING_OP_PROG_TLC  = 4,
    TIMING_OP_ERASE     = 5,
    TIMING_OP_COUNT     = 6,
};

/*
 * timing_sample – one measured timing observation.
 * Collected by timing_accuracy_validate() via N=1000 isolated operations.
 */
struct timing_sample {
    uint64_t submit_ns;
    uint64_t complete_ns;
    uint64_t elapsed_us;   /* complete_ns - submit_ns, converted to µs */
};

/*
 * timing_op_result – aggregate result for one NAND operation type.
 */
struct timing_op_result {
    enum timing_op_type op_type;
    uint64_t  sample_count;
    uint64_t  ref_value_us;         /* reference from spec */
    double    measured_avg_us;
    double    measured_stddev_us;
    double    accuracy_error_pct;   /* |avg - ref| / ref * 100 */
    bool      passed;               /* true if accuracy_error_pct < threshold */
};

/*
 * timing_validation_result – full NAND timing accuracy report.
 */
struct timing_validation_result {
    struct timing_op_result ops[TIMING_OP_COUNT];
    bool   all_passed;
    double worst_error_pct;
    char   worst_op_name[32];
};
```

### 3.3 可扩展性测试

```c
#define SCALABILITY_MAX_LEVELS  8   /* thread counts: 1,2,4,8,16,32,64,128 */

/*
 * scalability_level – result at one thread count.
 */
struct scalability_level {
    uint32_t thread_count;
    double   iops;
    double   parallel_efficiency; /* IOPS(N) / (N * IOPS(1)) */
};

/*
 * scalability_result – output of scalability_test_run().
 */
struct scalability_result {
    struct scalability_level levels[SCALABILITY_MAX_LEVELS];
    uint32_t  level_count;

    double    iops_single_thread;     /* IOPS(1), baseline */
    double    amdahl_serial_fraction; /* estimated S from Amdahl's law */
    double    efficiency_at_16t;      /* parallel_efficiency at 16 threads */

    bool      passed;    /* true if efficiency_at_16t > 0.70 */
    char      fail_reason[256];
};
```

### 3.4 稳定性检查

```c
/*
 * stability_config – parameters for a long-haul stability run.
 */
struct stability_config {
    uint64_t duration_seconds;      /* total run time (default: 259200 = 72h) */
    uint32_t load_pct;              /* target CPU/IO load percentage (default: 50) */
    bool     enable_tsan_check;     /* expect zero data-race reports */
    bool     enable_asan_check;     /* expect zero heap errors */
    bool     enable_data_verify;    /* verify md5sum for every read */
};

/*
 * stability_result – output of stability_check_run().
 */
struct stability_result {
    uint64_t elapsed_seconds;
    uint64_t crash_count;           /* SIGABRT/SIGSEGV/SIGFPE catches */
    uint64_t data_corruption_count; /* md5sum mismatches */
    uint64_t tsan_race_count;       /* data races reported by TSan */
    uint64_t asan_heap_error_count; /* heap errors reported by ASan */
    double   mem_growth_mb_per_hour;

    /* Peak resource utilisation observed */
    double   peak_nand_thread_cpu_pct;
    double   peak_host_os_cpu_pct;
    double   peak_dram_utilisation_pct;

    bool     passed;
    char     fail_reason[512];
};
```

### 3.5 数据完整性验证

```c
#define DATA_INTEGRITY_MD5_LEN  16   /* 128-bit MD5 digest */

/*
 * integrity_record – tracks the expected content of one LBA range.
 * Written before a verification read; compared after.
 */
struct integrity_record {
    uint64_t lba_start;
    uint32_t lba_count;
    uint8_t  expected_md5[DATA_INTEGRITY_MD5_LEN];
    bool     verified;
    bool     match;
};

/*
 * integrity_result – output of data_integrity_verify().
 */
struct integrity_result {
    uint64_t total_regions;
    uint64_t passed_regions;
    uint64_t failed_regions;
    double   integrity_pct;   /* passed / total * 100 */
    bool     all_passed;
};
```

### 3.6 全量验证报告

```c
#define PV_REQ_COUNT   13   /* REQ-112 through REQ-119, REQ-122, REQ-131 through REQ-134 */

/*
 * pv_req_result – pass/fail verdict for one requirement.
 */
struct pv_req_result {
    char     req_id[16];          /* e.g. "REQ-112" */
    char     description[128];    /* brief human-readable description */
    bool     passed;
    char     detail[512];         /* measured values or failure reason */
};

/*
 * perf_validation_report – top-level validation output structure.
 * Written to report_path by perf_validation_run_all().
 */
struct perf_validation_report {
    /* Metadata */
    char     hfsss_version[32];
    char     run_timestamp[32];   /* ISO-8601 UTC */
    char     host_cpu_model[64];
    uint32_t host_cpu_cores;
    uint64_t host_dram_gb;

    /* Per-requirement results */
    struct pv_req_result reqs[PV_REQ_COUNT];

    /* Sub-system results */
    struct bench_result        iops_rand_read;
    struct bench_result        iops_rand_write;
    struct bench_result        iops_mixed;
    struct bench_result        bw_seq_read;
    struct bench_result        bw_seq_write;
    struct bench_result        latency_qd1;
    struct timing_validation_result timing;
    struct scalability_result  scalability;
    struct stability_result    stability;
    struct integrity_result    integrity;

    /* Summary */
    uint32_t total_requirements;
    uint32_t passed_requirements;
    uint32_t failed_requirements;
    bool     overall_passed;       /* false if any P0 requirement failed */
};

#endif /* __HFSSS_PERF_VALIDATOR_TYPES_H */
```

---

## 4. 头文件设计

```c
/* include/common/perf_validator.h */
#ifndef __HFSSS_PERF_VALIDATOR_H
#define __HFSSS_PERF_VALIDATOR_H

#include <stdint.h>
#include <stdbool.h>
#include "perf_validator_types.h"

/* Forward declarations */
struct sssim_ctx;
struct nand_ctx;

/* ------------------------------------------------------------------ */
/* bench_engine – built-in synthetic I/O workload generator             */
/* ------------------------------------------------------------------ */

/*
 * bench_engine_ctx – opaque handle returned by bench_engine_init.
 * Callers must treat this as an opaque pointer.
 */
struct bench_engine_ctx;

/*
 * bench_engine_init – allocate and initialise the benchmark engine.
 *
 * sssim:   root simulator context; bench_engine submits I/Os through
 *          the same NVMe command path as the host driver.
 * Returns a heap-allocated context, or NULL on allocation failure.
 */
struct bench_engine_ctx *bench_engine_init(struct sssim_ctx *sssim);

/*
 * bench_engine_destroy – release all resources owned by the engine.
 * Safe to call with NULL.
 */
void bench_engine_destroy(struct bench_engine_ctx *eng);

/*
 * bench_engine_run – execute one benchmark scenario synchronously.
 *
 * eng:    initialised engine context.
 * cfg:    scenario parameters (read-only during the run).
 * out:    caller-allocated result struct to be populated.
 *
 * Returns 0 on completion (whether passed or failed),
 * negative errno on engine-level error (e.g. I/O submission failure).
 */
int bench_engine_run(struct bench_engine_ctx *eng,
                     const struct bench_config *cfg,
                     struct bench_result *out);

/* ------------------------------------------------------------------ */
/* Convenience scenario wrappers                                        */
/* ------------------------------------------------------------------ */

/*
 * bench_run_scenario – build a bench_config from named scenario
 * parameters, call bench_engine_run, and evaluate pass/fail against
 * the threshold embedded in the scenario definition.
 *
 * scenario_name: one of the predefined scenario identifiers listed in
 *                Section 6 of this document (e.g. "randread_qd32_4k").
 * out:           caller-allocated result struct.
 *
 * Returns 0 on completion, negative errno on engine error.
 */
int bench_run_scenario(struct bench_engine_ctx *eng,
                       const char *scenario_name,
                       struct bench_result *out);

/* ------------------------------------------------------------------ */
/* NAND timing accuracy validation                                      */
/* ------------------------------------------------------------------ */

/*
 * timing_accuracy_validate – issue N=1000 isolated NAND operations of
 * each type, measure simulated elapsed time, compare to reference spec,
 * and report accuracy_error_pct for each operation type.
 *
 * nand:   NAND context providing direct access to the timing model.
 * out:    caller-allocated result struct.
 *
 * Returns 0 on completion, negative errno on context access failure.
 */
int timing_accuracy_validate(struct nand_ctx *nand,
                              struct timing_validation_result *out);

/* ------------------------------------------------------------------ */
/* Scalability validation                                               */
/* ------------------------------------------------------------------ */

/*
 * scalability_test_run – sweep thread count from 1 to max_threads
 * (doubling each step), record IOPS and parallel efficiency at each
 * level, estimate Amdahl serial fraction, and evaluate efficiency
 * target (>70% at 16 threads).
 *
 * eng:         engine context for workload generation.
 * max_threads: highest thread count to test (capped at BENCH_MAX_THREADS).
 * out:         caller-allocated result struct.
 *
 * Returns 0 on completion, negative errno on engine error.
 */
int scalability_test_run(struct bench_engine_ctx *eng,
                          uint32_t max_threads,
                          struct scalability_result *out);

/* ------------------------------------------------------------------ */
/* Stability validation                                                 */
/* ------------------------------------------------------------------ */

/*
 * stability_check_run – run a long-haul workload with the given config,
 * monitoring for crashes, memory leaks, data races, and data corruption.
 *
 * eng:    engine context.
 * cfg:    stability run parameters.
 * out:    caller-allocated result struct.
 *
 * Returns 0 on completion, negative errno on fatal engine error.
 */
int stability_check_run(struct bench_engine_ctx *eng,
                         const struct stability_config *cfg,
                         struct stability_result *out);

/*
 * stability_check_memory_leaks – sample current RSS and compare against
 * the baseline recorded at bench_engine_init time.
 *
 * Returns true if memory growth rate (MB/hr) is within the 1 MB/hr budget,
 * false otherwise.  The measurement is an approximation via /proc/self/status.
 */
bool stability_check_memory_leaks(const struct bench_engine_ctx *eng);

/* ------------------------------------------------------------------ */
/* Data integrity verification                                          */
/* ------------------------------------------------------------------ */

/*
 * data_integrity_verify – write known patterns to [lba_start, lba_start
 * + lba_count) using per-LBA deterministic data (seeded by LBA), then
 * read back and verify md5sum matches for each block_size_bytes chunk.
 *
 * sssim:           root context.
 * lba_start:       starting LBA (0-based).
 * lba_count:       number of LBAs to cover (512B sectors).
 * block_size_bytes: granularity of each write/verify cycle.
 * out:             caller-allocated result struct.
 *
 * Returns 0 on completion, negative errno on I/O error.
 */
int data_integrity_verify(struct sssim_ctx *sssim,
                           uint64_t lba_start,
                           uint64_t lba_count,
                           uint32_t block_size_bytes,
                           struct integrity_result *out);

/* ------------------------------------------------------------------ */
/* Top-level validation orchestrator                                    */
/* ------------------------------------------------------------------ */

/*
 * perf_validation_run_all – run all sub-system validations in sequence,
 * collect per-requirement pass/fail results, and write the combined
 * report to report_path in both JSON and human-readable text formats.
 *
 * sssim:       root simulator context.
 * report_path: filesystem path prefix for output files.
 *              Two files are created:
 *                <report_path>.json  – machine-readable JSON report
 *                <report_path>.txt   – human-readable text report
 *              Parent directory must exist and be writable.
 * report_out:  optional pointer to receive the in-memory report struct;
 *              may be NULL if caller does not need in-memory access.
 *
 * Returns 0 if all P0 requirements passed.
 * Returns the count of failed P0 requirements (> 0) if any failed.
 * Returns negative errno on fatal infrastructure error.
 */
int perf_validation_run_all(struct sssim_ctx *sssim,
                             const char *report_path,
                             struct perf_validation_report *report_out);

/* ------------------------------------------------------------------ */
/* Utility: latency histogram analysis                                  */
/* ------------------------------------------------------------------ */

/*
 * bench_hist_percentile – compute a percentile value from an exponential
 * latency histogram.
 *
 * hist:        pointer to BENCH_HIST_BUCKETS uint64_t bucket counts.
 * total_ops:   sum of all bucket counts (pre-computed for efficiency).
 * percentile:  target percentile in the range (0, 1.0].
 *              e.g. pass 0.99 for P99.
 *
 * Returns the lower boundary (µs) of the bucket containing the percentile.
 */
uint64_t bench_hist_percentile(const uint64_t *hist,
                                uint64_t total_ops,
                                double percentile);

/*
 * bench_result_to_json – serialise a bench_result to a JSON string.
 * Caller must free() the returned buffer.
 * Returns NULL on allocation failure.
 */
char *bench_result_to_json(const struct bench_result *r);

/*
 * perf_report_write_json – serialise the full validation report to JSON
 * and write to the open file descriptor fd.
 * Returns 0 on success, negative errno on write error.
 */
int perf_report_write_json(int fd,
                            const struct perf_validation_report *report);

/*
 * perf_report_write_text – write the full validation report in
 * human-readable tabular text format to fd.
 * Returns 0 on success, negative errno on write error.
 */
int perf_report_write_text(int fd,
                            const struct perf_validation_report *report);

#endif /* __HFSSS_PERF_VALIDATOR_H */
```

---

## 5. 函数接口详细设计

### 5.1 bench_engine内部架构

```
bench_engine_init(sssim)
├── malloc(struct bench_engine_ctx)
├── 记录 init_rss_kb = parse_proc_self_status("VmRSS")
├── 记录 init_time_ns = clock_gettime(CLOCK_MONOTONIC)
├── eng->sssim = sssim
└── return eng

bench_engine_run(eng, cfg, out)
├── 参数校验（queue_depth ∈ [1,128]，block_size ∈ [512,1M]，num_threads ∈ [1,256]）
├── 初始化 out->lat_hist[] 为全零
├── 根据 cfg->pattern 选择 LBA生成函数：
│   ├── SEQ_READ/SEQ_WRITE  → lba_gen_sequential()
│   ├── RAND_READ/RAND_WRITE → lba_gen_uniform(lba_range)
│   ├── MIXED               → lba_gen_uniform() + read_ratio决定方向
│   └── ZIPFIAN             → lba_gen_zipfian(theta=0.99, lba_range)
├── pthread_create × cfg->num_threads → worker_thread_fn()
├── worker_thread_fn():
│   ├── 通过 sssim_submit_io() 提交 I/O（与真实NVMe命令路径相同）
│   ├── 维护 per-thread 未完成命令计数（控制 queue_depth）
│   ├── 每次 I/O 完成：
│   │   ├── lat_ns = complete_ns - submit_ns
│   │   ├── bucket = 63 - __builtin_clzll((lat_ns/1000) | 1)
│   │   ├── atomic_fetch_add(&out->lat_hist[bucket], 1)
│   │   ├── atomic_fetch_add(&out->total_ops, 1)
│   │   └── atomic_fetch_add(&out->total_bytes, cfg->block_size_bytes)
│   └── 退出条件：elapsed >= duration_seconds 或 total_ops >= op_count
├── pthread_join × cfg->num_threads
├── out->elapsed_seconds = (end_ns - start_ns) / 1e9
├── out->iops  = out->total_ops / out->elapsed_seconds
├── out->bw_mbps = out->total_bytes / out->elapsed_seconds / 1e6
├── bench_hist_percentile(out->lat_hist, total_ops, 0.50) → out->lat_p50_us
├── bench_hist_percentile(out->lat_hist, total_ops, 0.99) → out->lat_p99_us
├── bench_hist_percentile(out->lat_hist, total_ops, 0.999) → out->lat_p999_us
└── return 0
```

### 5.2 NAND时序精度验证流程

```
timing_accuracy_validate(nand, out):
  for each op_type in [READ_LSB, READ_CSB, READ_MSB, PROG_SLC, PROG_TLC, ERASE]:
    samples[0..999] = issue_isolated_nand_op(nand, op_type, channel=0, chip=0, die=0)
    for each sample:
      elapsed_us = (complete_ns - submit_ns) / 1000
    avg = mean(elapsed_us[])
    stddev = stddev(elapsed_us[])
    ref = timing_ref_value[op_type]            // see §3.2 constants
    error_pct = |avg - ref| / ref * 100.0
    out->ops[op_type] = { avg, stddev, error_pct, passed=(error_pct < 5.0) }

  out->all_passed = AND of all ops[i].passed
  out->worst_error_pct = max of all error_pct
  return 0
```

`issue_isolated_nand_op` 对目标 Die 发送单条命令，等待其完成，期间不发送任何其他命令，确保 EAT（最早可用时刻）计算不受并发干扰。

### 5.3 可扩展性测试流程

```
scalability_test_run(eng, max_threads, out):
  thread_counts[] = {1, 2, 4, 8, 16, 32, 64, min(128, max_threads)}
  base_cfg = {
      pattern=BENCH_RAND_READ, queue_depth=32,
      block_size_bytes=4096, duration_seconds=30
  }
  for i, N in enumerate(thread_counts):
    base_cfg.num_threads = N
    bench_engine_run(eng, &base_cfg, &result)
    out->levels[i] = { N, result.iops, result.iops / (N * iops_1t) }

  out->iops_single_thread = levels[0].iops
  /* 重新计算并行效率（基于 IOPS(1)）*/
  for each level: level.parallel_efficiency = level.iops / (level.thread_count * iops_1t)

  /* Amdahl serial fraction: S ≈ (1/efficiency - 1) / (N - 1) for N=16 */
  eff16 = levels[index_of_16t].parallel_efficiency
  out->amdahl_serial_fraction = (1.0/eff16 - 1.0) / (16.0 - 1.0)
  out->efficiency_at_16t = eff16
  out->passed = (eff16 >= 0.70)
  return 0
```

### 5.4 数据完整性验证流程

```
data_integrity_verify(sssim, lba_start, lba_count, block_size_bytes, out):
  num_blocks = lba_count * 512 / block_size_bytes
  records[num_blocks] = malloc(...)

  /* 写入阶段：用确定性种子填充每块 */
  for b in range(num_blocks):
    buf = generate_deterministic_pattern(seed = lba_start + b * (block_size_bytes/512))
    /* 种子生成算法：每512B扇区写入 uint64_t(lba) repeated */
    sssim_submit_write(lba_start + b * (block_size_bytes/512), buf, block_size_bytes)
    md5sum(buf, block_size_bytes) → records[b].expected_md5
    records[b].lba_start = lba_start + b * (block_size_bytes/512)

  sssim_flush()   /* 确保所有写入已提交 */

  /* 读回验证阶段 */
  for b in range(num_blocks):
    sssim_submit_read(records[b].lba_start, readbuf, block_size_bytes)
    actual_md5 = md5sum(readbuf, block_size_bytes)
    records[b].match = (memcmp(actual_md5, records[b].expected_md5, 16) == 0)

  out->total_regions  = num_blocks
  out->passed_regions = count of records[b].match == true
  out->failed_regions = out->total_regions - out->passed_regions
  out->integrity_pct  = out->passed_regions / out->total_regions * 100.0
  out->all_passed     = (out->failed_regions == 0)
  return 0
```

### 5.5 内存泄漏检测

```
stability_check_memory_leaks(eng):
  current_rss_kb = parse_proc_self_status("VmRSS")
  elapsed_hours  = (now_ns - eng->init_time_ns) / 3.6e12
  if elapsed_hours < 0.01: return true   /* 不足1分钟，跳过 */

  growth_kb = current_rss_kb - eng->init_rss_kb
  growth_mb_per_hour = (growth_kb / 1024.0) / elapsed_hours
  eng->last_mem_growth_mb_per_hour = growth_mb_per_hour

  return (growth_mb_per_hour < 1.0)   /* REQ-134: <1MB/hr */
```

### 5.6 顶层编排逻辑

```
perf_validation_run_all(sssim, report_path, report_out):

  eng = bench_engine_init(sssim)

  /* Step 1: IOPS 随机读（REQ-112）*/
  cfg = { BENCH_RAND_READ, qd=32, bs=4096, duration=60s, threads=32 }
  bench_engine_run(eng, &cfg, &report->iops_rand_read)
  → pass if iops_rand_read.iops >= 600000

  /* Step 2: IOPS 随机写（REQ-113）*/
  cfg = { BENCH_RAND_WRITE, qd=32, bs=4096, duration=60s, threads=32 }
  bench_engine_run(eng, &cfg, &report->iops_rand_write)
  → pass if iops_rand_write.iops >= 150000   /* 稳态目标 */

  /* Step 3: 混合读写 IOPS（REQ-114）*/
  cfg = { BENCH_MIXED, qd=32, bs=4096, rw_ratio_pct=70, duration=60s }
  bench_engine_run(eng, &cfg, &report->iops_mixed)
  → pass if iops_mixed.iops >= 250000

  /* Step 4: 顺序读带宽（REQ-115）*/
  cfg = { BENCH_SEQ_READ, qd=32, bs=131072, duration=30s, threads=8 }
  bench_engine_run(eng, &cfg, &report->bw_seq_read)
  → pass if bw_seq_read.bw_mbps >= 6656.0   /* 6.5 GB/s */

  /* Step 5: 顺序写带宽（REQ-115）*/
  cfg = { BENCH_SEQ_WRITE, qd=32, bs=131072, duration=30s, threads=8 }
  bench_engine_run(eng, &cfg, &report->bw_seq_write)
  → pass if bw_seq_write.bw_mbps >= 3584.0  /* 3.5 GB/s */

  /* Step 6: 延迟（REQ-116）*/
  cfg = { BENCH_RAND_READ, qd=1, bs=4096, duration=30s, threads=1 }
  bench_engine_run(eng, &cfg, &report->latency_qd1)
  → pass if lat_p50_us<=100 && lat_p99_us<=150 && lat_p999_us<=500

  /* Step 7: NAND 时序精度（REQ-117）*/
  timing_accuracy_validate(sssim->nand, &report->timing)
  → pass if timing.all_passed

  /* Step 8: 可扩展性（REQ-118）*/
  scalability_test_run(eng, 64, &report->scalability)
  → pass if scalability.efficiency_at_16t >= 0.70

  /* Step 9: 数据完整性（REQ-133）*/
  data_integrity_verify(sssim, 0, 2097152, 4096, &report->integrity)
  → pass if integrity.all_passed

  /* Step 10: 内存泄漏快照（REQ-134 部分）*/
  passed_mem = stability_check_memory_leaks(eng)

  /* 汇总 per-requirement 结果到 report->reqs[] */
  populate_req_results(report)

  /* 写出报告文件 */
  fd_json = open("<report_path>.json", O_WRONLY|O_CREAT|O_TRUNC, 0644)
  perf_report_write_json(fd_json, report)
  fd_txt  = open("<report_path>.txt",  O_WRONLY|O_CREAT|O_TRUNC, 0644)
  perf_report_write_text(fd_txt, report)

  bench_engine_destroy(eng)

  failed = count of report->reqs[i].passed == false
  return failed   /* 0 = all pass */
```

### 5.7 JSON报告格式

`perf_report_write_json` 输出的报告示例结构（精简）：

```json
{
  "hfsss_version": "2.0.0-beta",
  "run_timestamp": "2026-03-15T08:00:00Z",
  "host_cpu_model": "AMD EPYC 9654 96-Core",
  "host_cpu_cores": 192,
  "host_dram_gb": 768,
  "overall_passed": true,
  "passed_requirements": 13,
  "failed_requirements": 0,
  "requirements": [
    {
      "req_id": "REQ-112",
      "description": "随机读IOPS (4KB, QD=32, 含ECC)",
      "passed": true,
      "detail": "measured=648231 IOPS, threshold=600000 IOPS, margin=+8.0%"
    },
    {
      "req_id": "REQ-117",
      "description": "NAND时序仿真精度 <5%",
      "passed": true,
      "detail": "worst_op=PROG_TLC, error=2.31%, all ops within 5%"
    }
  ],
  "bench_scenarios": {
    "randread_qd32_4k": {
      "iops": 648231.0,
      "bw_mbps": 2524.3,
      "lat_p50_us": 78,
      "lat_p99_us": 143,
      "lat_p999_us": 289,
      "elapsed_seconds": 60.01,
      "passed": true
    }
  },
  "timing_accuracy": {
    "read_lsb": { "ref_us": 35, "measured_avg_us": 35.8, "error_pct": 2.29, "passed": true },
    "read_csb": { "ref_us": 70, "measured_avg_us": 71.1, "error_pct": 1.57, "passed": true },
    "read_msb": { "ref_us": 100, "measured_avg_us": 101.4, "error_pct": 1.40, "passed": true },
    "prog_tlc": { "ref_us": 800, "measured_avg_us": 818.5, "error_pct": 2.31, "passed": true },
    "erase":    { "ref_us": 3000, "measured_avg_us": 3054.2, "error_pct": 1.81, "passed": true }
  },
  "scalability": {
    "levels": [
      {"threads": 1,  "iops": 25400, "efficiency": 1.000},
      {"threads": 2,  "iops": 49800, "efficiency": 0.980},
      {"threads": 4,  "iops": 97200, "efficiency": 0.957},
      {"threads": 8,  "iops": 183000, "efficiency": 0.901},
      {"threads": 16, "iops": 342000, "efficiency": 0.842},
      {"threads": 32, "iops": 618000, "efficiency": 0.761},
      {"threads": 64, "iops": 1050000, "efficiency": 0.647}
    ],
    "efficiency_at_16t": 0.842,
    "amdahl_serial_fraction": 0.011,
    "passed": true
  }
}
```

### 5.8 文本报告格式

`perf_report_write_text` 输出示例：

```
================================================================================
HFSSS Performance Validation Report
Version: 2.0.0-beta  |  Date: 2026-03-15T08:00:00Z  |  Host Cores: 192
================================================================================

REQUIREMENT RESULTS
-------------------
REQ-ID   DESCRIPTION                                STATUS   DETAIL
REQ-112  随机读 IOPS (4KB, QD=32, ECC)              PASS     648231 IOPS (target: 600000)
REQ-113  随机写 IOPS (4KB, QD=32, 稳态)             PASS     163400 IOPS (target: 150000)
REQ-114  混合读写 IOPS (70R/30W, QD=32)             PASS     267800 IOPS (target: 250000)
REQ-115  顺序读带宽 (128KB)                         PASS     6721 MB/s (target: 6656)
REQ-115  顺序写带宽 (128KB)                         PASS     3612 MB/s (target: 3584)
REQ-116  读延迟 QD=1: P50/P99/P99.9                PASS     78/143/289 µs (target: 100/150/500)
REQ-117  NAND 时序精度                              PASS     worst=2.31% PROG_TLC (target: <5%)
REQ-118  可扩展性 (并行效率@16线程)                  PASS     84.2% (target: >70%)
REQ-122  fio io_uring 兼容性                        PASS     64 jobs, QD=128 completed
REQ-131  故障注入接口                               PASS     4 injection modes verified
REQ-132  MTBF / 无Panic                             PASS     72h run, 0 crashes
REQ-133  数据完整性 (md5sum)                        PASS     1048576/1048576 regions match
REQ-134  稳定性 (内存/TSan/ASan)                    PASS     mem+0.12MB/hr, 0 races, 0 heap-err

SUMMARY: 13/13 requirements PASSED  |  OVERALL: PASS
================================================================================
```

---

## 6. 验证方法与测试矩阵

### 6.1 性能目标阈值汇总

| 场景 | 配置 | 合格阈值 |
|------|------|----------|
| 随机读 IOPS | 4KB, QD=32, 32线程, 含ECC | ≥ 600,000 IOPS |
| 随机读 IOPS（峰值） | 4KB, QD=32, 32线程, 无ECC | ≥ 1,000,000 IOPS |
| 随机写 IOPS（稳态） | 4KB, QD=32, 32线程, 90%填充 | ≥ 150,000 IOPS |
| 随机写 IOPS（FOOB） | 4KB, QD=32, 32线程, 空盘 | ≥ 300,000 IOPS |
| 混合读写 IOPS | 4KB, QD=32, 70R/30W, 稳态 | ≥ 250,000 IOPS |
| 顺序读带宽 | 128KB, QD=32 | ≥ 6,656 MB/s（6.5 GB/s） |
| 顺序写带宽 | 128KB, QD=32 | ≥ 3,584 MB/s（3.5 GB/s） |
| 顺序读带宽（大块） | 1MB, QD=32 | ≥ 7,168 MB/s（7.0 GB/s） |
| 顺序写带宽（大块） | 1MB, QD=32 | ≥ 4,096 MB/s（4.0 GB/s） |
| 读延迟 P50 | 4KB, QD=1 | ≤ 100 µs |
| 读延迟 P99 | 4KB, QD=1 | ≤ 150 µs |
| 读延迟 P99.9 | 4KB, QD=1 | ≤ 500 µs |
| 写延迟 P50（Write Buffer命中） | 4KB, QD=1 | ≤ 30 µs |
| NAND tR 误差（TLC LSB/CSB/MSB） | N=1000次隔离操作 | < 5% |
| NAND tPROG 误差（TLC） | N=1000次隔离操作 | < 5% |
| NAND tERS 误差 | N=1000次隔离操作 | < 5% |
| 并行效率 | 16线程 vs 1线程 | > 70% |

### 6.2 fio等效命令参考

以下命令与内置 bench_engine 各场景等效，用于交叉验证：

```bash
# REQ-112: 随机读 IOPS
fio --filename=/dev/nvme0n1 --rw=randread --bs=4k \
    --iodepth=32 --numjobs=32 --ioengine=io_uring \
    --direct=1 --runtime=60 --name=randread_qd32

# REQ-113: 随机写 IOPS
fio --filename=/dev/nvme0n1 --rw=randwrite --bs=4k \
    --iodepth=32 --numjobs=32 --ioengine=io_uring \
    --direct=1 --runtime=60 --name=randwrite_qd32

# REQ-114: 混合读写
fio --filename=/dev/nvme0n1 --rw=randrw --rwmixread=70 \
    --bs=4k --iodepth=32 --numjobs=32 --ioengine=io_uring \
    --direct=1 --runtime=60 --name=mixed_70r30w

# REQ-115: 顺序读带宽
fio --filename=/dev/nvme0n1 --rw=read --bs=128k \
    --iodepth=32 --numjobs=8 --ioengine=io_uring \
    --direct=1 --runtime=30 --name=seqread_128k

# REQ-116: 延迟 QD=1
fio --filename=/dev/nvme0n1 --rw=randread --bs=4k \
    --iodepth=1 --numjobs=1 --ioengine=io_uring \
    --direct=1 --runtime=30 --percentile_list=50,99,99.9 \
    --name=latency_qd1
```

### 6.3 ThreadSanitizer / AddressSanitizer 编译配置

```makefile
# TSan build target
tsan:
    $(CC) $(CFLAGS) -fsanitize=thread -fno-omit-frame-pointer \
          -g -O1 -o hfsss_tsan $(SRCS) $(LDFLAGS)

# ASan build target
asan:
    $(CC) $(CFLAGS) -fsanitize=address -fno-omit-frame-pointer \
          -g -O1 -o hfsss_asan $(SRCS) $(LDFLAGS)
```

TSan 运行时确认零数据竞争报告（`TSAN_OPTIONS=halt_on_error=1`）。ASan 运行时确认零堆错误（`ASAN_OPTIONS=halt_on_error=1:detect_leaks=1`）。两种场景均运行 10 分钟内置随机读写负载。

### 6.4 72小时压力测试配置

```yaml
# stability_test_config.yaml（供 perf_validation_run_all 加载）
stability:
  duration_seconds: 259200       # 72 hours
  load_pct: 50                   # 50% of rated IOPS
  enable_data_verify: true
  enable_tsan_check: false       # TSan在独立编译目标中运行
  enable_asan_check: false
  memory_growth_budget_mb_per_hour: 1.0
  crash_budget: 0
  corruption_budget: 0
```

---

## 7. 测试要点

| 测试ID | 测试描述 | 验证点 |
|--------|----------|--------|
| PV-001 | 随机读IOPS基线（含ECC，QD=32） | iops ≥ 600,000，覆盖REQ-112 |
| PV-002 | 随机读IOPS峰值（无ECC，QD=32） | iops ≥ 1,000,000，覆盖REQ-112 |
| PV-003 | 随机读IOPS稳态（90%填充，QD=32） | iops ≥ 400,000，含GC开销 |
| PV-004 | 随机写IOPS（FOOB，QD=32） | iops ≥ 300,000，覆盖REQ-113 |
| PV-005 | 随机写IOPS（稳态90%填充，QD=32） | iops ≥ 150,000，覆盖REQ-113 |
| PV-006 | 混合读写IOPS（70R/30W，QD=32） | iops ≥ 250,000，覆盖REQ-114 |
| PV-007 | 顺序读带宽（128KB块） | bw_mbps ≥ 6,656，覆盖REQ-115 |
| PV-008 | 顺序写带宽（128KB块） | bw_mbps ≥ 3,584，覆盖REQ-115 |
| PV-009 | 顺序读带宽（1MB块） | bw_mbps ≥ 7,168，覆盖REQ-115 |
| PV-010 | 顺序写带宽（1MB块） | bw_mbps ≥ 4,096，覆盖REQ-115 |
| PV-011 | 读延迟P50（QD=1，4KB） | lat_p50_us ≤ 100µs，覆盖REQ-116 |
| PV-012 | 读延迟P99（QD=1，4KB） | lat_p99_us ≤ 150µs，覆盖REQ-116 |
| PV-013 | 读延迟P99.9（QD=1，4KB） | lat_p999_us ≤ 500µs，覆盖REQ-116 |
| PV-014 | 写延迟P50（QD=1，Write Buffer命中） | lat_p50_us ≤ 30µs，覆盖REQ-116 |
| PV-015 | NAND tR精度（TLC LSB） | accuracy_error_pct < 5%，覆盖REQ-117 |
| PV-016 | NAND tR精度（TLC CSB/MSB） | accuracy_error_pct < 5%，覆盖REQ-117 |
| PV-017 | NAND tPROG精度（TLC模式） | accuracy_error_pct < 5%，覆盖REQ-117 |
| PV-018 | NAND tERS精度 | accuracy_error_pct < 5%，覆盖REQ-117 |
| PV-019 | 整体IOPS仿真精度 | 与MQSim同配置结果偏差<10%，覆盖REQ-117 |
| PV-020 | 并行效率@16线程 | efficiency_at_16t > 0.70，覆盖REQ-118 |
| PV-021 | 并行效率@32线程 | efficiency_at_32t > 0.60，扩展验证REQ-118 |
| PV-022 | Amdahl串行分数估计 | amdahl_serial_fraction < 0.05（串行比例<5%） |
| PV-023 | fio io_uring兼容性（32 jobs，QD=128） | fio退出码0，无I/O错误，覆盖REQ-122 |
| PV-024 | fio direct=1（O_DIRECT绕过页缓存） | 写入数据不经OS缓存，读回一致 |
| PV-025 | 数据完整性（4KB顺序写后读回） | 1M区域md5sum 100%匹配，覆盖REQ-133 |
| PV-026 | 数据完整性（随机写后顺序读回） | 随机LBA写入+顺序读出，md5sum全匹配 |
| PV-027 | 数据完整性（GC期间写入读回） | GC触发期间同时写入，读回数据无损 |
| PV-028 | ThreadSanitizer零报告（10分钟负载） | TSan输出无"DATA RACE"行，覆盖REQ-134 |
| PV-029 | AddressSanitizer零堆错误（10分钟负载） | ASan输出无heap-buffer-overflow/use-after-free，覆盖REQ-134 |
| PV-030 | 内存泄漏速率（1小时负载） | RSS增长 < 1 MB/小时，覆盖REQ-134 |
| PV-031 | 72小时压力测试无Crash | crash_count == 0，覆盖REQ-132和REQ-134 |
| PV-032 | 72小时压力测试数据完整性 | 72小时内写入数据读回0损坏，覆盖REQ-133 |
| PV-033 | 极端场景：NAND写满不Crash | 所有空闲块耗尽时返回ENOSPC错误码，非Crash |
| PV-034 | 极端场景：所有备用块耗尽 | BBT满额标记后新写命令返回Space_Exceeded，非Panic |
| PV-035 | 报告JSON格式合法性 | jq解析报告文件返回0，所有req_id字段存在 |
| PV-036 | 报告文件非零退出码 | 手动使一项指标失败，perf_validation_run_all返回1 |
| PV-037 | bench_engine多线程无数据竞争 | 128线程同时提交，total_ops计数精确，无锁丢失 |
| PV-038 | 延迟直方图桶边界正确性 | 注入固定100µs延迟IO，验证桶索引落在bucket=6（[64µs,128µs)） |
| PV-039 | NAND时序参数可配置性 | 修改tR=50µs重跑PV-015，验证新参考值生效且误差基于50µs计算 |
| PV-040 | 资源利用率监控（满载60秒） | NAND介质线程CPU 70–90%；宿主机OS CPU<10%，覆盖REQ-119 |

---

**文档统计**：
- 覆盖需求：13个（REQ-112至REQ-119，REQ-122，REQ-131至REQ-134）
- 新增头文件：`include/common/perf_validator.h`、`include/common/perf_validator_types.h`
- 新增源文件：`src/common/perf_validator.c`、`src/common/bench_engine.c`、`src/common/timing_validator.c`、`src/common/scalability_validator.c`、`src/common/stability_validator.c`
- 函数接口数：16个
- 测试用例：40个（PV-001至PV-040）
