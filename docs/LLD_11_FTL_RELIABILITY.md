# 高保真全栈SSD模拟器（HFSSS）详细设计文档

**文档名称**：FTL可靠性机制详细设计
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
6. [流程图](#6-流程图)
7. [集成要点](#7-集成要点)
8. [测试要点](#8-测试要点)

---

## 1. 模块概述

FTL可靠性模块（FTL Reliability）为仿真器提供完整的数据可靠性保障体系，覆盖命令全生命周期追踪、读写重试、多层流控、数据冗余保护与NVMe错误上报五个主要功能域。本模块不改变主I/O数据通路的逻辑，而是以拦截点（hook）形式嵌入，在故障注入、压力测试和异常场景下提供可观测、可验证的可靠性行为。

**REQ-110 — 命令状态机**：为每条FTL命令维护一个独立的状态上下文（`ftl_cmd_ctx`），状态序列为 `RECEIVED → PARSING → L2P_LOOKUP → NAND_QUEUED → EXECUTING → ECC_CHECK → COMPLETE`（或 `ERROR`）。每次状态迁移均记录纳秒级时间戳，供延迟分析和Trace导出使用。命令状态机是其余所有可靠性机制的调度入口，任何异常状态均通过状态机统一汇聚到 `FTL_CMD_ERROR`。

**REQ-111 — 读重试机制**：读取失败时，FTL首先尝试软判决LDPC译码（Soft-Decision LDPC），若仍失败则依次施加最多15个电压偏移量（voltage offset），对同一物理页重新读取并再次译码。偏移表在芯片特性标定时写入NOR Flash，运行时加载到 `read_retry_ctx`。若所有重试均失败，视为不可纠正错误（UCE），向NVMe错误日志页追加条目并更新SMART `media_errors` 计数器。

**REQ-112 — 写重试与写验证**：编程失败时，FTL在同一块最多重试 `WRITE_RETRY_MAX_ATTEMPTS`（3）次；首次失败后同步通过 `ftl_alloc_page()` 在备用块分配新目标页（`backup_ppn`），后续重试直接写入备用位置。若启用写验证（`verify_after_write = true`），每次编程成功后立即回读并比对数据，不一致则触发下一次重试或将块加入坏块表（BBT）。原始块在重试耗尽后标记为待退役（pending retirement）。

**REQ-113 — 多层流控**：流控分两层：（1）命名空间令牌桶（`ns_flow_ctrl`），以令牌速率限制每个NS的带宽，令牌不足时命令在FTL队列等待而不阻塞NVMe层；（2）NAND通道队列深度限制（`channel_flow_ctrl`），通道队列满时阻止新命令下发，保护Die层调度器不过载。两层流控联合作用，为多命名空间场景提供QoS隔离。

**REQ-114 — 类RAID数据保护**：L2P表维护双副本（DRAM主表 + 文件影子表），影子表每64MB主机写后同步一次，并记录序列号用于重启后一致性校验。BBT采用NOR Flash双镜像（slot_a / slot_b），写入时先更新slot_a并计算CRC32，再同步slot_b，读取时优先选择CRC有效的较新槽位。可选Die级XOR奇偶校验（`xor_parity_group`）为一组数据Die提供一个奇偶页，支持单Die故障恢复。

**REQ-115 — NVMe错误处理**：仿真器完整实现NVMe状态码体系（SCT/SC），对每种错误路径返回精确的状态字段。NVMe错误日志页（Log Page 0x03）以64项环形缓冲区实现，线程安全地追加并按 `error_count` 单调递增排序。UCE事件同步递增SMART `media_errors` 计数器和 `num_err_log_entries` 计数器，确保主机端 `nvme smart-log` 与 `nvme error-log` 两条命令的输出一致。

**覆盖需求**：REQ-110、REQ-111、REQ-112、REQ-113、REQ-114、REQ-115。

---

## 2. 功能需求详细分解

| 需求ID | 需求描述 | 优先级 | 目标版本 |
|--------|----------|--------|----------|
| REQ-110 | 命令状态机：RECEIVED→PARSING→L2P_LOOKUP→NAND_QUEUED→EXECUTING→ECC_CHECK→COMPLETE/ERROR，含纳秒级时间戳 | P0 | V2.0 |
| REQ-111 | 读重试：软判决LDPC优先；最多15次电压偏移重试；UCE写入错误日志并更新SMART计数器 | P0 | V2.0 |
| REQ-112 | 写重试：同块最多3次；首次失败分配备用块；可选写后验证；原块标记待退役 | P0 | V2.0 |
| REQ-113 | 多层流控：per-NS令牌桶 + NAND通道队列深度限制，提供多命名空间QoS隔离 | P0 | V2.0 |
| REQ-114 | 类RAID保护：DRAM+文件双L2P副本；BBT NOR双镜像；可选Die级XOR奇偶校验 | P0 | V2.0 |
| REQ-115 | NVMe错误处理：完整SCT/SC状态码；错误日志页0x03（64项环形缓冲）；SMART计数器联动更新 | P0 | V2.0 |

---

## 3. 数据结构详细设计

### 3.1 命令状态机（REQ-110）

```c
#ifndef __HFSSS_FTL_RELIABILITY_H
#define __HFSSS_FTL_RELIABILITY_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

#include "ftl/ftl_config.h"   /* MAX_NAMESPACES, MAX_CHANNELS */

/* -----------------------------------------------------------------------
 * REQ-110: Command state machine
 * Each FTL command transitions through exactly these states in order.
 * ERROR is a terminal state reachable from any non-terminal state.
 * ----------------------------------------------------------------------- */
enum ftl_cmd_state {
    FTL_CMD_RECEIVED    = 0,
    FTL_CMD_PARSING     = 1,
    FTL_CMD_L2P_LOOKUP  = 2,
    FTL_CMD_NAND_QUEUED = 3,
    FTL_CMD_EXECUTING   = 4,
    FTL_CMD_ECC_CHECK   = 5,
    FTL_CMD_COMPLETE    = 6,
    FTL_CMD_ERROR       = 7,
    FTL_CMD_STATE_COUNT = 8,
};

/*
 * Per-command execution context.
 * Allocated from a pool by ftl_cmd_alloc() and returned by ftl_cmd_free().
 * All timestamp fields use CLOCK_MONOTONIC nanoseconds.
 */
struct ftl_cmd_ctx {
    uint32_t          cmd_id;                        /* unique id within session */
    uint16_t          sq_id;                         /* NVMe submission queue id */
    enum ftl_cmd_state state;                        /* current state */
    uint64_t          lba;                           /* starting logical block address */
    uint32_t          len_sectors;                   /* transfer length in 512B sectors */
    uint64_t          submit_ns;                     /* absolute time of RECEIVED state */
    uint64_t          state_enter_ns[FTL_CMD_STATE_COUNT]; /* one entry per state */
    int               status_code;                   /* NVMe status code on completion */
    void             *priv;                          /* caller-managed private data */
};
```

### 3.2 读重试（REQ-111）

```c
/* -----------------------------------------------------------------------
 * REQ-111: Read Retry context
 * Voltage offsets are signed millivolt deltas from nominal read voltage.
 * The offset table is loaded from NOR Flash at initialisation time.
 * ----------------------------------------------------------------------- */
#define READ_RETRY_MAX_ATTEMPTS 15

struct read_retry_ctx {
    uint8_t  attempt;                                  /* current attempt index (0-based) */
    int8_t   voltage_offsets[READ_RETRY_MAX_ATTEMPTS]; /* delta from nominal, in mV */
    bool     soft_ldpc_attempted;                      /* true after soft-decision pass */
    uint64_t ppn;                                      /* physical page number being read */
    uint8_t *buf;                                      /* destination buffer */
};
```

### 3.3 写重试（REQ-112）

```c
/* -----------------------------------------------------------------------
 * REQ-112: Write Retry context
 * backup_ppn is allocated on the first program failure and reused for
 * subsequent attempts; original block is marked pending retirement.
 * ----------------------------------------------------------------------- */
#define WRITE_RETRY_MAX_ATTEMPTS 3

struct write_retry_ctx {
    uint8_t  attempt;           /* current attempt index (0-based) */
    uint64_t original_ppn;      /* target PPN before any failure */
    uint64_t backup_ppn;        /* allocated via ftl_alloc_page() on first failure */
    bool     verify_after_write; /* if true, read back and compare after each program */
};
```

### 3.4 多层流控（REQ-113）

```c
/* -----------------------------------------------------------------------
 * REQ-113: Multi-level flow control
 *
 * ns_flow_ctrl  — per-namespace token bucket; tokens are consumed on
 *                 command submission and refilled on a periodic timer.
 * channel_flow_ctrl — per-NAND-channel queue depth cap; uses an atomic
 *                     counter to avoid locking the hot path.
 * ----------------------------------------------------------------------- */
struct ns_flow_ctrl {
    uint32_t        ns_id;               /* namespace identifier (1-based) */
    uint64_t        tokens;              /* current token count (bytes) */
    uint64_t        token_rate_per_sec;  /* sustained bandwidth limit in bytes/s */
    uint64_t        token_max;           /* burst ceiling (bytes) */
    uint64_t        last_refill_ns;      /* CLOCK_MONOTONIC time of last refill */
    pthread_mutex_t lock;                /* serialises token_acquire / refill */
};

struct channel_flow_ctrl {
    uint8_t           channel_id;           /* NAND channel index */
    uint32_t          queue_depth_max;       /* configured maximum outstanding ops */
    _Atomic uint32_t  queue_depth_current;   /* live count; updated without locking */
};

struct ftl_flow_ctrl {
    struct ns_flow_ctrl      ns[MAX_NAMESPACES]; /* indexed by ns_id - 1 */
    struct channel_flow_ctrl ch[MAX_CHANNELS];   /* indexed by channel_id */
};
```

### 3.5 类RAID数据保护（REQ-114）

```c
/* -----------------------------------------------------------------------
 * REQ-114: RAID-like data protection
 *
 * l2p_redundancy  — DRAM primary + file-backed shadow copy; shadow is
 *                   synchronised every 64 MB of host writes.
 * bbt_redundancy  — dual NOR partition mirror; each slot carries a CRC32.
 * xor_parity_group — optional die-level XOR parity; one parity page
 *                    protects up to 8 data dies.
 * ----------------------------------------------------------------------- */
struct l2p_redundancy {
    uint64_t        *primary;    /* DRAM L2P array, one entry per LBA */
    uint64_t        *shadow;     /* mmap'd file-backed shadow array */
    uint64_t         shadow_seq; /* monotonic counter; detects stale shadow on boot */
    pthread_rwlock_t lock;       /* write-lock during sync; read-lock during lookup */
};

struct bbt_redundancy {
    uint8_t  *slot_a;   /* NOR partition A: primary BBT image */
    uint8_t  *slot_b;   /* NOR partition B: mirror BBT image */
    uint32_t  crc_a;    /* CRC32 of slot_a at last write */
    uint32_t  crc_b;    /* CRC32 of slot_b at last write */
};

struct xor_parity_group {
    uint8_t  die_count;        /* number of data dies in this group (1-8) */
    uint64_t parity_ppn;       /* physical page holding XOR of all data_ppns */
    uint64_t data_ppns[8];     /* per-die data page PPNs */
};
```

### 3.6 NVMe错误日志页（REQ-115）

```c
/* -----------------------------------------------------------------------
 * REQ-115: NVMe Error Log Page (Log Page ID = 0x03)
 * Entry layout follows NVMe Base Specification 2.0, Section 5.14.1.1.
 * All multi-byte integer fields are little-endian.
 * ----------------------------------------------------------------------- */
struct nvme_error_log_entry {
    uint64_t error_count;      /* monotonically increasing per-device error counter */
    uint16_t sq_id;            /* submission queue id of the failed command */
    uint16_t cmd_id;           /* command identifier of the failed command */
    uint16_t status_field;     /* bits[15:1]=Status Field, bit[0]=Phase Tag */
    uint16_t param_error_loc;  /* byte and bit location of detected parameter error */
    uint64_t lba;              /* first LBA experiencing the error */
    uint32_t ns;               /* namespace identifier */
    uint8_t  vs_info;          /* vendor specific information */
    uint8_t  trtype;           /* transport type */
    uint8_t  reserved[2];
    uint64_t cs;               /* command specific information */
    uint16_t trtype_spec_info; /* transport type specific information */
    uint8_t  reserved2[22];
} __attribute__((packed));

#define NVME_ERROR_LOG_ENTRIES 64

/*
 * Ring buffer for error log entries.
 * head always points to the slot of the next write; wrap-around is handled
 * by (head % NVME_ERROR_LOG_ENTRIES).  count is unbounded for SMART reporting.
 */
struct nvme_error_log {
    struct nvme_error_log_entry entries[NVME_ERROR_LOG_ENTRIES];
    uint32_t        head;   /* next write position (mod NVME_ERROR_LOG_ENTRIES) */
    uint32_t        count;  /* total entries ever written (for SMART num_err_log_entries) */
    pthread_mutex_t lock;
};

#endif /* __HFSSS_FTL_RELIABILITY_H */
```

---

## 4. 头文件设计

完整头文件路径：`include/ftl/ftl_reliability.h`

```c
/**
 * ftl_reliability.h — FTL Reliability public API
 *
 * Covers REQ-110 (command state machine), REQ-111 (read retry),
 * REQ-112 (write retry), REQ-113 (flow control),
 * REQ-114 (L2P/BBT redundancy), REQ-115 (NVMe error log).
 */
#ifndef HFSSS_FTL_RELIABILITY_H
#define HFSSS_FTL_RELIABILITY_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

#include "ftl/ftl_config.h"

/* ---- data structures (see LLD section 3) ---- */

enum ftl_cmd_state;
struct ftl_cmd_ctx;
struct read_retry_ctx;
struct write_retry_ctx;
struct ns_flow_ctrl;
struct channel_flow_ctrl;
struct ftl_flow_ctrl;
struct l2p_redundancy;
struct bbt_redundancy;
struct xor_parity_group;
struct nvme_error_log_entry;
struct nvme_error_log;

/* -----------------------------------------------------------------------
 * REQ-110: Command state machine
 * ----------------------------------------------------------------------- */

/** Allocate a command context from the global pool; returns NULL on exhaustion. */
struct ftl_cmd_ctx *ftl_cmd_alloc(uint32_t cmd_id, uint16_t sq_id);

/** Return a command context to the pool; ctx must be in COMPLETE or ERROR state. */
void ftl_cmd_free(struct ftl_cmd_ctx *ctx);

/**
 * Transition ctx to next_state; records CLOCK_MONOTONIC timestamp.
 * Returns 0 on success, -EINVAL if the transition is illegal.
 */
int ftl_cmd_transition(struct ftl_cmd_ctx *ctx, enum ftl_cmd_state next_state);

/* -----------------------------------------------------------------------
 * REQ-111: Read Retry
 * ----------------------------------------------------------------------- */

/**
 * Execute a read with retry.
 * Tries soft-decision LDPC first, then iterates voltage_offsets up to
 * READ_RETRY_MAX_ATTEMPTS times.  On UCE appends to nvme_error_log and
 * returns -EIO; on success returns 0.
 */
int read_retry_execute(struct read_retry_ctx *ctx, uint64_t ppn,
                       uint8_t *buf, uint32_t len);

/**
 * Initialise a read_retry_ctx; loads voltage offset table from NOR.
 * Must be called once per context before read_retry_execute().
 */
void read_retry_ctx_init(struct read_retry_ctx *ctx, uint64_t ppn, uint8_t *buf);

/* -----------------------------------------------------------------------
 * REQ-112: Write Retry and Write Verify
 * ----------------------------------------------------------------------- */

/**
 * Execute a write with retry.
 * Tries original_ppn up to WRITE_RETRY_MAX_ATTEMPTS times; allocates
 * backup_ppn via ftl_alloc_page() on the first failure.  If
 * verify_after_write is set, reads back and compares after each successful
 * program.  Returns 0 on success, -EIO if all attempts fail.
 */
int write_retry_execute(struct write_retry_ctx *ctx, uint64_t ppn,
                        const uint8_t *data, uint32_t len);

/**
 * Initialise a write_retry_ctx.
 */
void write_retry_ctx_init(struct write_retry_ctx *ctx, uint64_t ppn,
                          bool verify);

/* -----------------------------------------------------------------------
 * REQ-113: Multi-level flow control
 * ----------------------------------------------------------------------- */

/**
 * Acquire tokens for ns_id covering bytes bytes of I/O.
 * Blocks (condition-wait) if the token bucket is depleted.
 * Returns 0 when tokens are granted, -EINVAL on bad ns_id.
 */
int ns_flow_ctrl_acquire(struct ftl_flow_ctrl *fc, uint32_t ns_id,
                         uint64_t bytes);

/**
 * Refill the token bucket for ns_id based on elapsed wall time.
 * Should be called periodically (e.g. every 1 ms) from the flow control
 * timer thread.
 */
void ns_flow_ctrl_refill(struct ftl_flow_ctrl *fc, uint32_t ns_id);

/**
 * Release tokens back to ns_id (used when a command is cancelled).
 */
void ns_flow_ctrl_release(struct ftl_flow_ctrl *fc, uint32_t ns_id,
                          uint64_t bytes);

/**
 * Check whether channel ch_id can accept a new command.
 * Returns true if queue_depth_current < queue_depth_max, false otherwise.
 * This is a non-blocking check; no state is mutated.
 */
bool channel_flow_ctrl_check(const struct ftl_flow_ctrl *fc, uint8_t ch_id);

/**
 * Increment the in-flight count for ch_id when a command is issued.
 */
void channel_flow_ctrl_inc(struct ftl_flow_ctrl *fc, uint8_t ch_id);

/**
 * Decrement the in-flight count for ch_id when a command completes.
 */
void channel_flow_ctrl_dec(struct ftl_flow_ctrl *fc, uint8_t ch_id);

/**
 * Initialise the entire ftl_flow_ctrl structure from config.
 */
int ftl_flow_ctrl_init(struct ftl_flow_ctrl *fc);

/**
 * Tear down all flow control resources.
 */
void ftl_flow_ctrl_destroy(struct ftl_flow_ctrl *fc);

/* -----------------------------------------------------------------------
 * REQ-114: L2P and BBT redundancy
 * ----------------------------------------------------------------------- */

/**
 * Copy primary L2P array to shadow; update shadow_seq; compute and store
 * CRC32 of the shadow file.  Acquires write-lock for the duration.
 * Returns 0 on success, -EIO on file write failure.
 */
int l2p_shadow_sync(struct l2p_redundancy *red);

/**
 * Compare primary L2P entries against shadow.
 * Logs a WARNING and emits an alert on any mismatch; does not auto-repair.
 * Returns the number of mismatched entries (0 = consistent).
 */
uint32_t l2p_redundancy_check(const struct l2p_redundancy *red);

/**
 * Restore primary L2P from shadow after detecting corruption.
 * Returns 0 on success, -EIO if shadow is also unreadable.
 */
int l2p_restore_from_shadow(struct l2p_redundancy *red);

/**
 * Write bbt of len bytes to both NOR slots (slot_a then slot_b).
 * Computes CRC32 for each slot after writing.
 * Returns 0 on success; -EIO if either write fails.
 */
int bbt_write_both_slots(struct bbt_redundancy *red,
                         const uint8_t *bbt, uint32_t len);

/**
 * Verify CRC of both BBT slots; select the valid (or newer) slot as the
 * authoritative copy; repair the corrupted slot from the valid one.
 * Returns 0 on success, -EIO if both slots are corrupt.
 */
int bbt_verify_and_repair(struct bbt_redundancy *red, uint32_t len);

/**
 * Compute the XOR parity page for a parity group and write it to
 * parity_ppn.  Returns 0 on success.
 */
int xor_parity_compute_and_write(struct xor_parity_group *grp);

/**
 * Recover a failed die's data using XOR parity; reads all surviving
 * data_ppns and the parity_ppn, XORs them, writes result to dst_ppn.
 * Returns 0 on success, -EIO if any source read fails.
 */
int xor_parity_recover(struct xor_parity_group *grp, uint8_t failed_die_idx,
                       uint64_t dst_ppn);

/* -----------------------------------------------------------------------
 * REQ-115: NVMe Error Log
 * ----------------------------------------------------------------------- */

/**
 * Append entry to the error log ring buffer.  Assigns error_count,
 * advances head, increments count.  Thread-safe.
 */
void nvme_error_log_append(struct nvme_error_log *log,
                           const struct nvme_error_log_entry *entry);

/**
 * Copy the most recent num_entries entries into buf (NVMe Get Log Page
 * order: most recent first).  Returns the actual number of entries copied.
 */
uint32_t nvme_error_log_get_page(const struct nvme_error_log *log,
                                 struct nvme_error_log_entry *buf,
                                 uint32_t num_entries);

/**
 * Initialise nvme_error_log; zeroes all entries, initialises mutex.
 */
int nvme_error_log_init(struct nvme_error_log *log);

/**
 * Destroy nvme_error_log resources.
 */
void nvme_error_log_destroy(struct nvme_error_log *log);

/**
 * Build a populated nvme_error_log_entry from cmd_ctx and NVMe status.
 * Convenience helper for callers that have a command context.
 */
void nvme_error_entry_from_cmd(struct nvme_error_log_entry *entry,
                               const struct ftl_cmd_ctx *ctx,
                               uint16_t status_field);

#endif /* HFSSS_FTL_RELIABILITY_H */
```

---

## 5. 函数接口详细设计

### 5.1 命令状态机：`ftl_cmd_alloc` / `ftl_cmd_free` / `ftl_cmd_transition`

```
ftl_cmd_alloc(cmd_id, sq_id):
  1. 从全局命令上下文池获取一个空闲槽位（pool_acquire()）
  2. 若池已满，返回 NULL
  3. 初始化 ctx->cmd_id = cmd_id, ctx->sq_id = sq_id
  4. ctx->state = FTL_CMD_RECEIVED
  5. ctx->submit_ns = clock_gettime(CLOCK_MONOTONIC)
  6. ctx->state_enter_ns[FTL_CMD_RECEIVED] = ctx->submit_ns
  7. ctx->status_code = 0，ctx->priv = NULL
  8. 返回 ctx

ftl_cmd_free(ctx):
  1. 断言 ctx->state 为 FTL_CMD_COMPLETE 或 FTL_CMD_ERROR
  2. 清零 ctx 所有字段
  3. 将槽位归还命令池（pool_release()）

ftl_cmd_transition(ctx, next_state):
  1. 验证迁移合法性：
       合法路径表 valid_transitions[current] → {next, ERROR}
       若 next_state 不在合法集合中，返回 -EINVAL
  2. ctx->state = next_state
  3. ctx->state_enter_ns[next_state] = clock_gettime(CLOCK_MONOTONIC)
  4. 若 next_state 为 FTL_CMD_ERROR 且 ctx->status_code == 0：
       ctx->status_code = NVME_SC_INTERNAL_PATH_ERROR（默认）
  5. 返回 0
```

### 5.2 读重试：`read_retry_execute`

```
read_retry_execute(ctx, ppn, buf, len):
  1. 调用 hal_nand_read(ppn, buf, len) 执行原始读
  2. 若成功（无 ECC 错误）→ 返回 0

  /* 软判决 LDPC */
  3. ctx->soft_ldpc_attempted = false
  4. 调用 ldpc_soft_decode(buf, len, soft_reliability_data)
  5. ctx->soft_ldpc_attempted = true
  6. 若软解码成功 → 返回 0

  /* 电压偏移重试循环 */
  7. for ctx->attempt = 0; ctx->attempt < READ_RETRY_MAX_ATTEMPTS; ctx->attempt++:
       a. offset = ctx->voltage_offsets[ctx->attempt]
       b. hal_nand_set_read_voltage(ppn, nominal_voltage + offset)
       c. hal_nand_read(ppn, buf, len)
       d. ecc_result = ecc_check_and_correct(buf, len)
       e. 若 ecc_result == ECC_OK → 恢复默认电压; 返回 0
       f. 尝试 ldpc_hard_decode(buf, len)
       g. 若解码成功 → 恢复默认电压; 返回 0
  8. 恢复默认电压 hal_nand_set_read_voltage(ppn, nominal_voltage)

  /* 不可纠正错误 (UCE) */
  9. 构造 nvme_error_log_entry，填写 lba、ns、status_field
     （SCT=2 Media/Data Integrity Error，SC=0x81 Unrecovered Read Error）
  10. nvme_error_log_append(global_error_log, &entry)
  11. smart_increment_media_errors()
  12. 返回 -EIO
```

### 5.3 写重试：`write_retry_execute`

```
write_retry_execute(ctx, ppn, data, len):
  1. ctx->attempt = 0
  2. target_ppn = ctx->original_ppn

  3. while ctx->attempt < WRITE_RETRY_MAX_ATTEMPTS:
       a. rc = hal_nand_program(target_ppn, data, len)
       b. 若 rc == 0 且 ctx->verify_after_write:
            readback_buf = temp_buf_alloc(len)
            hal_nand_read(target_ppn, readback_buf, len)
            若 memcmp(data, readback_buf, len) != 0:
                rc = -EIO   /* 验证失败，视为编程失败 */
            temp_buf_free(readback_buf)
       c. 若 rc == 0 → 更新 L2P：lba → target_ppn; 返回 0

       /* 失败处理 */
       d. 若 ctx->attempt == 0:
            ctx->backup_ppn = ftl_alloc_page()
            若 backup_ppn == PPN_INVALID → 返回 -ENOSPC
            target_ppn = ctx->backup_ppn
       e. ctx->attempt++

  4. /* 所有重试耗尽 */
  5. bbt_mark_block_bad(ppn_to_block(ctx->original_ppn), BBT_REASON_PROG_FAIL)
  6. 返回 -EIO
```

### 5.4 命名空间令牌桶：`ns_flow_ctrl_acquire`

```
ns_flow_ctrl_acquire(fc, ns_id, bytes):
  1. 若 ns_id < 1 或 ns_id > MAX_NAMESPACES → 返回 -EINVAL
  2. nfc = &fc->ns[ns_id - 1]
  3. pthread_mutex_lock(&nfc->lock)
  4. loop:
       a. ns_flow_ctrl_refill_locked(nfc)   /* 按经过时间补充令牌 */
       b. 若 nfc->tokens >= bytes:
            nfc->tokens -= bytes
            pthread_mutex_unlock(&nfc->lock)
            返回 0
       c. 否则: pthread_cond_wait(&nfc->cond, &nfc->lock)
                /* 由定时器线程 ns_flow_ctrl_refill() 触发 broadcast */
  5. （不可达）

ns_flow_ctrl_refill_locked(nfc):   /* 内部，调用前已持有锁 */
  now_ns = clock_gettime(CLOCK_MONOTONIC)
  elapsed_ns = now_ns - nfc->last_refill_ns
  new_tokens = (elapsed_ns * nfc->token_rate_per_sec) / 1e9
  nfc->tokens = min(nfc->tokens + new_tokens, nfc->token_max)
  nfc->last_refill_ns = now_ns
```

### 5.5 通道队列深度检查：`channel_flow_ctrl_check`

```
channel_flow_ctrl_check(fc, ch_id):
  1. 若 ch_id >= MAX_CHANNELS → 返回 false
  2. cfc = &fc->ch[ch_id]
  3. current = atomic_load_explicit(&cfc->queue_depth_current,
                                    memory_order_acquire)
  4. 返回 (current < cfc->queue_depth_max)
     /* 纯查询，不修改任何计数；调用方在获准后调用 channel_flow_ctrl_inc() */
```

### 5.6 L2P影子同步：`l2p_shadow_sync`

```
l2p_shadow_sync(red):
  1. pthread_rwlock_wrlock(&red->lock)
  2. memcpy(red->shadow, red->primary, L2P_TABLE_SIZE * sizeof(uint64_t))
  3. red->shadow_seq++
  4. crc = crc32(red->shadow, L2P_TABLE_SIZE * sizeof(uint64_t))
  5. 将 shadow_seq 和 crc 写入影子文件头部元数据
  6. msync(red->shadow, ..., MS_SYNC)   /* 强制落盘 */
  7. 若 msync 失败:
       pthread_rwlock_unlock(&red->lock)
       返回 -EIO
  8. pthread_rwlock_unlock(&red->lock)
  9. 返回 0
```

### 5.7 L2P冗余检查：`l2p_redundancy_check`

```
l2p_redundancy_check(red):
  1. pthread_rwlock_rdlock(&red->lock)
  2. mismatches = 0
  3. for i = 0; i < L2P_TABLE_SIZE; i++:
       若 red->primary[i] != red->shadow[i]:
           mismatches++
           若 mismatches == 1: log_warn("L2P mismatch at LBA %lu", i)
  4. pthread_rwlock_unlock(&red->lock)
  5. 若 mismatches > 0:
       reliability_alert(ALERT_L2P_MISMATCH, mismatches)
  6. 返回 mismatches
```

### 5.8 BBT双槽写入：`bbt_write_both_slots`

```
bbt_write_both_slots(red, bbt, len):
  1. memcpy(red->slot_a, bbt, len)
  2. red->crc_a = crc32(red->slot_a, len)
  3. rc = nor_flash_write(NOR_BBT_SLOT_A_OFFSET, red->slot_a, len)
  4. nor_flash_write(NOR_BBT_SLOT_A_CRC_OFFSET, &red->crc_a, 4)
  5. 若 rc != 0 → 返回 -EIO

  6. memcpy(red->slot_b, bbt, len)
  7. red->crc_b = crc32(red->slot_b, len)
  8. rc = nor_flash_write(NOR_BBT_SLOT_B_OFFSET, red->slot_b, len)
  9. nor_flash_write(NOR_BBT_SLOT_B_CRC_OFFSET, &red->crc_b, 4)
  10. 若 rc != 0 → 返回 -EIO

  11. 返回 0
```

### 5.9 BBT验证与修复：`bbt_verify_and_repair`

```
bbt_verify_and_repair(red, len):
  1. a_ok = (crc32(red->slot_a, len) == red->crc_a)
  2. b_ok = (crc32(red->slot_b, len) == red->crc_b)

  3. 若 a_ok && b_ok:
       /* 两个槽位均有效；选择 shadow_seq 较新的（若可用），否则默认 slot_a */
       返回 0

  4. 若 a_ok && !b_ok:
       log_warn("BBT slot_b CRC invalid; repairing from slot_a")
       memcpy(red->slot_b, red->slot_a, len)
       red->crc_b = red->crc_a
       nor_flash_write(NOR_BBT_SLOT_B_OFFSET, red->slot_b, len)
       nor_flash_write(NOR_BBT_SLOT_B_CRC_OFFSET, &red->crc_b, 4)
       返回 0

  5. 若 !a_ok && b_ok:
       log_warn("BBT slot_a CRC invalid; repairing from slot_b")
       memcpy(red->slot_a, red->slot_b, len)
       red->crc_a = red->crc_b
       nor_flash_write(NOR_BBT_SLOT_A_OFFSET, red->slot_a, len)
       nor_flash_write(NOR_BBT_SLOT_A_CRC_OFFSET, &red->crc_a, 4)
       返回 0

  6. /* 两个槽位均损坏 */
  7. log_error("BBT both slots corrupt; device may be unrecoverable")
  8. 返回 -EIO
```

### 5.10 NVMe错误日志追加：`nvme_error_log_append`

```
nvme_error_log_append(log, entry):
  1. pthread_mutex_lock(&log->lock)
  2. slot = log->head % NVME_ERROR_LOG_ENTRIES
  3. log->entries[slot] = *entry
  4. log->entries[slot].error_count = (uint64_t)(log->count + 1)
  5. log->head = (log->head + 1) % NVME_ERROR_LOG_ENTRIES
  6. log->count++
  7. pthread_mutex_unlock(&log->lock)
  /* no return value; never fails */
```

### 5.11 NVMe错误日志读取：`nvme_error_log_get_page`

```
nvme_error_log_get_page(log, buf, num_entries):
  1. pthread_mutex_lock(&log->lock)
  2. available = min(log->count, NVME_ERROR_LOG_ENTRIES)
  3. to_copy = min(num_entries, available)
  4. 按 NVMe Get Log Page 规定，从最新条目向最旧条目方向填充 buf：
       for i = 0; i < to_copy; i++:
           src_slot = (log->head - 1 - i + NVME_ERROR_LOG_ENTRIES)
                      % NVME_ERROR_LOG_ENTRIES
           buf[i] = log->entries[src_slot]
  5. pthread_mutex_unlock(&log->lock)
  6. 返回 to_copy
```

---

## 6. 流程图

### 6.1 读重试决策树

```
                      hal_nand_read(ppn)
                            │
                    ┌───────┴───────┐
                    │               │
                 成功（无ECC错误）   失败
                    │               │
                  返回0        软判决LDPC译码
                               │
                     ┌─────────┴─────────┐
                     │                   │
                  译码成功            译码失败
                     │                   │
                   返回0         电压偏移重试循环
                                    attempt=0
                                       │
                              ┌────────▼────────┐
                              │ 施加voltage_     │
                              │ offsets[attempt] │
                              │ hal_nand_read    │
                              │ ecc_check        │
                              └────────┬─────────┘
                                       │
                             ┌─────────┴──────────┐
                             │                    │
                          ECC成功              ECC失败
                             │                    │
                          恢复默认电压       attempt < 15?
                             │               ┌────┴────┐
                           返回0            是         否
                                             │          │
                                       attempt++   UCE处理:
                                        重试        ├─ nvme_error_log_append
                                                    ├─ smart_increment_media_errors
                                                    └─ 返回 -EIO
```

### 6.2 写重试与备用块分配

```
                    write_retry_execute(ppn, data)
                              │
                      attempt=0, target=original_ppn
                              │
                    ┌─────────▼──────────┐
                    │ hal_nand_program    │
                    │ (target_ppn, data)  │
                    └─────────┬──────────┘
                              │
                 ┌────────────┴────────────┐
                 │                         │
             成功 (rc==0)              失败 (rc!=0)
                 │                         │
        verify_after_write?          attempt==0?
            ┌───┴───┐                 ┌────┴────┐
           是       否               是          否
            │       │                │          │
          回读    更新L2P          分配backup_ppn  attempt++
          比对    返回0            target=backup   继续循环
            │                         │
     ┌──────┴──────┐            attempt++, 继续循环
     │             │
  一致           不一致
     │             │
  更新L2P       视为失败
  返回0         (进入失败分支)
                              │（attempt >= 3）
                    bbt_mark_block_bad(original_ppn)
                              │
                           返回 -EIO
```

### 6.3 L2P影子同步与不一致检测

```
    [正常写路径，每64MB触发]          [启动时执行]
              │                             │
    l2p_shadow_sync(red)        l2p_redundancy_check(red)
              │                             │
    wrlock(red->lock)             rdlock(red->lock)
              │                             │
    memcpy(shadow←primary)        逐条比对 primary vs shadow
              │                             │
    shadow_seq++                  ┌─────────┴─────────┐
              │                   │                   │
    crc=crc32(shadow)          全部一致           存在不一致
              │                   │                   │
    写入文件头部元数据           unlock            unlock
    (shadow_seq + crc)             │         reliability_alert
              │                   │                   │
    msync(MS_SYNC)             返回 0          l2p_restore_from_shadow
              │                                       │
    ┌─────────┴──────────┐                   wrlock(red->lock)
    │                    │                   memcpy(primary←shadow)
  成功                 失败                   unlock
    │                    │                   返回 mismatches
  unlock              unlock
    │                    │
  返回 0            返回 -EIO
```

### 6.4 NVMe错误上报路径

```
        UCE发生（read_retry_execute 所有重试耗尽）
                          │
            ┌─────────────▼──────────────┐
            │ nvme_error_entry_from_cmd  │
            │ 填充：lba, ns, sq_id,      │
            │ cmd_id, status_field       │
            │ SCT=2 / SC=0x81           │
            └─────────────┬──────────────┘
                          │
              ┌───────────▼────────────┐
              │ nvme_error_log_append  │  ← 线程安全环形缓冲
              │ mutex_lock             │
              │ entries[head] = entry  │
              │ head = (head+1) % 64   │
              │ count++                │
              │ mutex_unlock           │
              └───────────┬────────────┘
                          │
              ┌───────────▼───────────────┐
              │ smart_increment_media_    │
              │ errors()                  │
              │ smart_log.media_errors[0]++│
              │ smart_log.num_err_log_    │
              │ entries[0]++              │
              └───────────┬───────────────┘
                          │
              ┌───────────▼────────────────────────────┐
              │ 主机发出 NVMe Get Log Page (LID=0x03)   │
              │ nvme_error_log_get_page()               │
              │ 从 head 向后复制请求条目数               │
              │ 返回 CQE + log page data                │
              └────────────────────────────────────────┘
```

---

## 7. 集成要点

**`nand.c` → 读重试**

`nand.c` 的底层读取函数在 ECC 引擎报告可纠正或不可纠正错误时，不再直接向上层返回错误码，而是构造 `read_retry_ctx` 并调用 `read_retry_execute()`。读重试模块透明地完成电压偏移序列，只有在所有重试耗尽后才向调用方返回 `-EIO`，同时已向错误日志追加条目。

**`ftl.c` → 写重试**

`ftl.c` 在调用 `hal_nand_program()` 后检测返回值；若非零，则构造 `write_retry_ctx` 并调用 `write_retry_execute()`，传入原始 PPN 和写数据。备用块分配、写验证和坏块标记全部由 `write_retry_execute()` 内部处理，`ftl.c` 仅需检查最终返回码。

**`ftl.c` → 多层流控**

`ftl.c` 在将命令下发至 NAND 队列之前串行执行两个门控：
1. 调用 `ns_flow_ctrl_acquire(fc, ns_id, transfer_bytes)`：若令牌不足则在此阻塞，直至定时器补充令牌；
2. 调用 `channel_flow_ctrl_check(fc, ch_id)`：若返回 `false` 则将命令暂存于 per-channel 等待队列，待通道有空位时重新检查。通道命令完成时调用 `channel_flow_ctrl_dec()` 触发等待命令的重新调度。

**`sssim.c` → NVMe错误日志**

`sssim.c` 维护全局 `nvme_error_log` 实例，在仿真器启动时调用 `nvme_error_log_init()`。NVMe Get Log Page 命令处理函数（LID=0x03 分支）调用 `nvme_error_log_get_page()` 填充响应数据缓冲区，随后填写 CQE 返回给主机。SMART log（LID=0x02）中的 `media_errors` 和 `num_err_log_entries` 字段由 `smart_increment_media_errors()` 保持同步，确保两条 log 查询结果一致。

**启动流程 → L2P/BBT 冗余**

`sssim.c` 或 `ftl.c` 的初始化路径在挂载 L2P 后立即调用 `l2p_redundancy_check()`，若检测到不一致则调用 `l2p_restore_from_shadow()` 修复主表。BBT 初始化时调用 `bbt_verify_and_repair()` 选择有效槽位；若两个槽位均损坏则返回错误，中止启动。

**写路径 → L2P影子同步**

`ftl.c` 维护一个 64 MB 写计数器（`host_write_bytes_since_sync`）；达到阈值时将 `l2p_shadow_sync()` 投递到低优先级后台工作线程执行，避免阻塞主 I/O 路径。

---

## 8. 测试要点

| 测试ID | 测试描述 | 验证点 |
|--------|----------|--------|
| FR-001 | 注入ECC错误，触发读重试 | `read_retry_ctx.attempt` 依次递增至15；最终返回 `-EIO` |
| FR-002 | 软判决LDPC在第一次重试时成功 | `soft_ldpc_attempted==true`；返回0；`attempt==0` |
| FR-003 | 读重试第7次电压偏移成功 | 返回0；`attempt==6`；电压恢复默认值 |
| FR-004 | 所有15次读重试均失败（UCE） | `nvme_error_log.count` 增加1；`media_errors` SMART字段递增 |
| FR-005 | UCE错误日志条目字段完整性 | `status_field` SCT=2、SC=0x81；`lba`、`ns`、`sq_id` 正确 |
| FR-006 | 写重试：首次失败分配备用块 | `write_retry_ctx.backup_ppn != PPN_INVALID`；原始PPN未被更新到L2P |
| FR-007 | 写重试：备用块写入成功 | L2P更新为backup_ppn；返回0 |
| FR-008 | 写重试3次全部失败 | `bbt_mark_block_bad()` 被调用；原块在BBT中标记为坏块；返回 `-EIO` |
| FR-009 | 写验证：回读数据不一致触发重试 | 注入回读翻转，验证下一次 `attempt` 递增并重写 |
| FR-010 | 令牌桶：超速率命令被阻塞 | 注入高速写流量；`ns_flow_ctrl_acquire` 阻塞时间 > 0 |
| FR-011 | 令牌桶：定时器补充后命令继续 | 阻塞后等待1个补充周期；命令在1ms内恢复执行 |
| FR-012 | 令牌桶：多命名空间隔离 | NS-1超限不影响NS-2的令牌余量 |
| FR-013 | 通道队列满时新命令被阻止 | `channel_flow_ctrl_check` 返回 `false`；命令进入等待队列 |
| FR-014 | 通道命令完成后等待命令调度 | `channel_flow_ctrl_dec()` 后等待队列中首命令被下发 |
| FR-015 | L2P影子同步后校验通过 | `l2p_shadow_sync()` 返回0；`l2p_redundancy_check()` 返回0 |
| FR-016 | 人工破坏primary L2P某条目 | `l2p_redundancy_check()` 返回 mismatches > 0；告警被触发 |
| FR-017 | `l2p_restore_from_shadow` 修复后一致 | `l2p_redundancy_check()` 再次返回0 |
| FR-018 | BBT slot_a CRC损坏，从slot_b修复 | `bbt_verify_and_repair()` 返回0；slot_a内容与slot_b一致 |
| FR-019 | BBT slot_b CRC损坏，从slot_a修复 | `bbt_verify_and_repair()` 返回0；slot_b内容与slot_a一致 |
| FR-020 | BBT两个槽位均损坏 | `bbt_verify_and_repair()` 返回 `-EIO`；仿真器启动中止 |
| FR-021 | 错误日志第65条写入环形缓冲 | `head` 回绕到0；旧条目被覆盖；`count==65`；最新条目在 `entries[0]` |
| FR-022 | 错误日志 Get Page 顺序 | `nvme_error_log_get_page(log, buf, 4)` 返回最近4条，按最新优先排列 |
| FR-023 | 命令状态机非法迁移 | `ftl_cmd_transition(COMPLETE → PARSING)` 返回 `-EINVAL`；状态不变 |
| FR-024 | 命令状态机时间戳单调递增 | 遍历 `state_enter_ns[]`；每个已到达的状态时间戳严格大于前一个 |

---

**文档统计**：
- 覆盖需求：6个（REQ-110至REQ-115）
- 新增头文件：`include/ftl/ftl_reliability.h`
- 函数接口数：35+
- 测试用例：24个（FR-001至FR-024）
