# 高保真全栈SSD模拟器（HFSSS）详细设计文档

**文档名称**：故障注入框架详细设计
**文档版本**：V1.0
**编制日期**：2026-03-15
**设计阶段**：V2.5 (Enterprise)
**密级**：内部资料

---

## 目录

1. [模块概述](#1-模块概述)
2. [功能需求详细分解](#2-功能需求详细分解)
3. [数据结构详细设计](#3-数据结构详细设计)
4. [头文件设计](#4-头文件设计)
5. [函数接口详细设计](#5-函数接口详细设计)
6. [流程图](#6-流程图)
7. [测试要点](#7-测试要点)

---

## 1. 模块概述

故障注入框架（Fault Injection Framework）是HFSSS仿真器的可测试性核心组件，提供对NAND介质层、电源子系统、控制器内部状态的受控破坏能力，使上层测试套件能够系统性地验证仿真器在各类异常条件下的行为正确性。

框架分为四个子域：

1. **NAND介质故障**（REQ-128）：在页/块/Die粒度注入坏块、读错误、写错误、擦除错误、Bit翻转、读干扰风暴和数据保持退化；
2. **电源故障**（REQ-129）：模拟不同时机的掉电场景，配合WAL恢复机制验证数据持久化路径；
3. **控制器故障**（REQ-130）：注入固件Panic、内存池耗尽、命令超时风暴、L2P映射表损坏；
4. **故障控制接口**（REQ-131）：通过OOB JSON-RPC统一管理故障的注册、调度、持久化和清除。

**设计约束**：

- `fault_check_*` 系列函数处于NAND操作的**热路径**，必须保证 O(log N) 或更优的查询复杂度；单次检查开销不超过200 ns（N ≤ 1024个活跃故障）。
- 故障注册和清除为低频操作（管理路径），允许使用写锁，不限制复杂度。
- 模块与OOB模块（LLD_07）解耦：OOB层负责JSON-RPC的序列化/反序列化，并将已解析的参数传递给本模块；本模块对传输协议不感知。
- 所有公共函数必须线程安全。

**覆盖需求**：REQ-128、REQ-129、REQ-130、REQ-131。

---

## 2. 功能需求详细分解

| 需求ID | 需求描述 | 优先级 | 目标版本 |
|--------|----------|--------|----------|
| REQ-128 | NAND介质故障注入：坏块、读/写/擦错误、Bit Flip、读干扰风暴、数据保持退化 | P0 | V2.5 |
| REQ-129 | 电源故障注入：立即掉电、写途中掉电、GC途中掉电、Checkpoint途中掉电 | P0 | V2.5 |
| REQ-130 | 控制器故障注入：固件Panic、内存池耗尽、命令超时风暴、L2P表损坏 | P0 | V2.5 |
| REQ-131 | 故障注入统一接口：OOB JSON-RPC `fault.inject`、立即/延迟调度、一次性/持久化模式 | P0 | V2.5 |

### 2.1 REQ-128 NAND介质故障细分

| 子故障类型 | 注入粒度 | 触发机制 | 效果 |
|------------|----------|----------|------|
| `bad_block` | ch/chip/die/plane/block | media层erase调用时 | BBT标记为坏块，FTL退役该Block |
| `read_error` | ch/chip/die/plane/block/page | media层read调用时 | 返回不可纠正ECC错误（UECC） |
| `program_error` | ch/chip/die/plane/block/page | media层program调用时 | 返回写入失败状态 |
| `erase_error` | ch/chip/die/plane/block | media层erase调用时 | 返回擦除失败状态 |
| `bit_flip` | ch/chip/die/plane/block/page/bit_pos | media层read调用返回前 | 对页数据缓冲区指定bit执行XOR |
| `read_disturb_storm` | ch/chip/die/block | 后台计数器 | 将该Block的read_count强制累加至阈值，使概率性读错误概率骤升 |
| `data_retention` | ch/chip/die/plane/block/aging_factor | 后台时钟推进 | 以aging_factor倍数加速Vt漂移，提前产生软错误 |

### 2.2 REQ-129 电源故障场景

| 场景 | 触发时机 | 模拟方式 |
|------|----------|----------|
| `power_loss_idle` | 空闲状态立即掉电 | 写crash_marker后调用`_exit(1)` |
| `power_loss_during_write` | Write Buffer正在下刷NAND期间 | 等待下一次nand_program调用时触发 |
| `power_loss_during_gc` | GC正在搬移数据期间 | 等待下一次gc_move_page调用时触发 |
| `power_loss_during_checkpoint` | Checkpoint正在写盘期间 | 等待下一次checkpoint_write调用时触发 |

### 2.3 REQ-130 控制器故障细分

| 子故障类型 | 触发机制 | 效果 |
|------------|----------|------|
| `firmware_panic` | 立即执行 | 调用内部`HFSSS_PANIC()`宏，触发断言失败处理路径 |
| `mem_pool_exhaust` | 下次从指定pool分配时 | 强制分配函数返回NULL |
| `cmd_timeout_storm` | 立即生效，持续N ms | 所有命令完成回调被延迟至`now + storm_duration_ms` |
| `l2p_corrupt` | 立即执行 | 将指定LBA范围的映射表项写入随机PPN值 |

---

## 3. 数据结构详细设计

### 3.1 故障类型枚举

```c
/*
 * enum fault_type – all injectable fault categories.
 *
 * NAND media faults (FAULT_NAND_*) are checked on the hot path by the
 * media layer.  Controller faults (FAULT_CTRL_*) and power faults
 * (FAULT_POWER_*) are handled by their respective subsystems.
 */
enum fault_type {
    /* ---- NAND media faults ---------------------------------------- */
    FAULT_NAND_BAD_BLOCK         = 0,  /* mark block permanently bad     */
    FAULT_NAND_READ_ERROR        = 1,  /* uncorrectable ECC on read      */
    FAULT_NAND_PROGRAM_ERROR     = 2,  /* program (write) failure        */
    FAULT_NAND_ERASE_ERROR       = 3,  /* erase failure                  */
    FAULT_NAND_BIT_FLIP          = 4,  /* flip bit(s) in page buffer     */
    FAULT_NAND_READ_DISTURB      = 5,  /* accelerate read disturb count  */
    FAULT_NAND_DATA_RETENTION    = 6,  /* accelerate Vt drift / aging    */

    /* ---- Power faults --------------------------------------------- */
    FAULT_POWER_LOSS_IDLE        = 10, /* crash during idle              */
    FAULT_POWER_LOSS_WRITE       = 11, /* crash mid-NAND-program         */
    FAULT_POWER_LOSS_GC          = 12, /* crash mid-GC-erase             */
    FAULT_POWER_LOSS_CHECKPOINT  = 13, /* crash during checkpoint write  */

    /* ---- Controller faults ---------------------------------------- */
    FAULT_CTRL_FIRMWARE_PANIC    = 20, /* trigger ASSERT / panic path    */
    FAULT_CTRL_MEM_POOL_EXHAUST  = 21, /* force NULL from mem_pool_alloc */
    FAULT_CTRL_CMD_TIMEOUT_STORM = 22, /* delay all cmd completions      */
    FAULT_CTRL_L2P_CORRUPT       = 23, /* corrupt a range of L2P entries */

    FAULT_TYPE_MAX               = 32,
};
```

### 3.2 故障目标地址

```c
/*
 * struct fault_target – physical address of the fault injection point.
 *
 * Fields are set to FAULT_TARGET_ANY (0xFF / 0xFFFF…) to indicate
 * "match any value at this level."  This allows, for example, injecting
 * a read error on ALL pages of a specific block by setting page =
 * FAULT_TARGET_ANY.
 *
 * For non-NAND faults (power, controller) only the fields relevant to
 * that fault type are used; unused fields must be set to FAULT_TARGET_ANY.
 */
#define FAULT_TARGET_ANY_U8    0xFFu
#define FAULT_TARGET_ANY_U16   0xFFFFu
#define FAULT_TARGET_ANY_U32   0xFFFFFFFFu

struct fault_target {
    uint8_t  channel;     /* NAND channel index (0-based)                */
    uint8_t  chip;        /* Chip-enable within channel                   */
    uint8_t  die;         /* Die within chip                              */
    uint8_t  plane;       /* Plane within die                             */
    uint32_t block;       /* Block index within plane                     */
    uint32_t page;        /* Page index within block                      */
    uint32_t bit_pos;     /* Bit position within page (FAULT_NAND_BIT_FLIP
                           * only; zero for all other types)              */

    /* Controller-fault specific fields */
    uint32_t pool_id;     /* for FAULT_CTRL_MEM_POOL_EXHAUST              */
    uint64_t lba_start;   /* for FAULT_CTRL_L2P_CORRUPT: range start LBA  */
    uint64_t lba_count;   /* for FAULT_CTRL_L2P_CORRUPT: number of LBAs   */
    uint32_t storm_duration_ms; /* for FAULT_CTRL_CMD_TIMEOUT_STORM       */
    double   aging_factor;      /* for FAULT_NAND_DATA_RETENTION          */
};
```

### 3.3 触发模式与持久化模式

```c
/*
 * enum fault_trigger_mode – when the fault becomes active.
 *
 * IMMEDIATE: the fault takes effect on the very next I/O operation that
 *            touches the target address.
 * DEFERRED:  the fault activates only after trigger_count operations have
 *            touched the target address (used to reproduce specific timing
 *            windows, e.g. "fail the 3rd write to this page").
 */
enum fault_trigger_mode {
    FAULT_TRIGGER_IMMEDIATE = 0,
    FAULT_TRIGGER_DEFERRED  = 1,
};

/*
 * enum fault_persist_mode – how long the fault remains active after firing.
 *
 * ONE_SHOT: the fault entry is automatically deactivated (active = false)
 *           after it fires once.  The entry remains in the registry so its
 *           hit_count can be inspected; it must be explicitly removed with
 *           fault_clear() to reclaim the slot.
 * STICKY:   the fault remains active indefinitely after firing; it will
 *           fire on every matching operation until fault_clear() is called.
 */
enum fault_persist_mode {
    FAULT_PERSIST_ONE_SHOT = 0,
    FAULT_PERSIST_STICKY   = 1,
};
```

### 3.4 故障条目

```c
/*
 * struct fault_entry – one registered fault in the active registry.
 *
 * Layout is padded to 128 bytes so that a registry array of N entries
 * occupies N cache lines (each 64 bytes * 2).  This reduces false-sharing
 * between the hot-path readers and the management-path writers.
 */
struct fault_entry {
    uint32_t              id;           /* unique fault ID, assigned by
                                         * fault_register(); never reused
                                         * within one simulator session     */
    enum fault_type       type;         /* which fault to inject            */
    struct fault_target   target;       /* where to inject                  */
    enum fault_trigger_mode trigger_mode;
    enum fault_persist_mode persist_mode;

    /*
     * trigger_count: for DEFERRED mode, the fault fires only after this
     * many matching operations have passed.  Decremented on each match
     * until it reaches zero, at which point the fault fires.
     * For IMMEDIATE mode, this field is unused (set to 0).
     */
    uint32_t              trigger_count;

    /*
     * hit_count: total number of times this fault has fired.
     * Incremented atomically on each firing; useful for test assertions.
     */
    uint64_t              hit_count;

    bool                  active;       /* false = entry is logically removed
                                         * but slot not yet reclaimed        */
    uint8_t               _pad[7];

} __attribute__((aligned(64)));
```

### 3.5 故障注册表

```c
#define FAULT_REGISTRY_MAX_ENTRIES  1024

/*
 * struct fault_registry – the central in-memory store of all active faults.
 *
 * The entries array is logically a flat list; lookups on the hot path
 * use a sorted index (sorted_ids[] + binary search) to achieve O(log N).
 * Insertions and deletions rebuild the sorted index under the write lock.
 *
 * Hot-path readers hold a read lock for the duration of fault_check_*;
 * management-path writers (fault_register, fault_clear) hold a write lock.
 *
 * A per-type bitmask (type_present) allows the hot path to skip the
 * binary search entirely when no fault of the relevant type is registered
 * (the common case during normal operation).
 */
struct fault_registry {
    struct fault_entry   entries[FAULT_REGISTRY_MAX_ENTRIES];
    uint32_t             count;          /* number of non-freed entries      */
    uint32_t             next_id;        /* monotonically increasing ID      */

    /*
     * sorted_ids[]: indices into entries[], sorted by (type, channel, chip,
     * die, plane, block, page) for binary search in fault_check_*.
     * Rebuilt on every insert/delete.
     */
    uint16_t             sorted_idx[FAULT_REGISTRY_MAX_ENTRIES];
    uint32_t             sorted_count;   /* number of active entries in index */

    /*
     * type_present: one bit per fault_type.  Set when any active entry of
     * that type exists.  Checked atomically on the hot path; if the bit for
     * the relevant type is zero, fault_check_* returns immediately without
     * acquiring the read lock.
     */
    uint32_t             type_present;   /* bitmask over enum fault_type     */

    pthread_rwlock_t     lock;

    /* crash marker file path for power-loss scenarios */
    char                 crash_marker_path[256];
};
```

### 3.6 掉电场景状态

```c
/*
 * enum power_loss_scenario – mirrors FAULT_POWER_LOSS_* fault types but
 * is used in the atexit handler to record which scenario triggered.
 */
enum power_loss_scenario {
    PLS_IDLE        = 0,
    PLS_WRITE       = 1,
    PLS_GC          = 2,
    PLS_CHECKPOINT  = 3,
};

/*
 * struct crash_marker – written atomically to crash_marker_path before
 * calling _exit().  On the next simulator startup, fault_inject_init()
 * detects this file and sets sssim_ctx->recovery_needed = true, causing
 * the WAL recovery path to run before the main loop starts.
 *
 * All fields are fixed-width integers in little-endian byte order.
 */
struct crash_marker {
    uint32_t magic;          /* CRASH_MARKER_MAGIC = 0x43524153 ("CRAS")  */
    uint32_t version;        /* currently 1                                */
    uint32_t scenario;       /* enum power_loss_scenario                   */
    uint32_t reserved;
    uint64_t crash_time_ns;  /* simulator monotonic timestamp at crash     */
    uint64_t last_wal_seq;   /* last WAL sequence number before crash      */
    uint8_t  padding[96];    /* pad to 128 bytes                           */
} __attribute__((packed));

#define CRASH_MARKER_MAGIC  0x43524153u
#define CRASH_MARKER_PATH_DEFAULT  "/var/lib/hfsss/wal/crash_marker"
```

---

## 4. 头文件设计

```c
/* include/common/fault_inject.h */
#ifndef __HFSSS_FAULT_INJECT_H
#define __HFSSS_FAULT_INJECT_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

/* Forward declarations */
struct sssim_ctx;

/* Pull in type definitions from this module */
/* (enum fault_type, fault_target, fault_entry, fault_registry defined above
 * in the implementation header; repeated here in abbreviated form for
 * consumers that only need the function prototypes.) */

/* ------------------------------------------------------------------ */
/* Registry lifecycle                                                   */
/* ------------------------------------------------------------------ */

/*
 * fault_inject_init – initialise the fault registry and check for a
 * crash marker left by a previous power-loss injection.
 *
 * crash_marker_path: path where the crash marker file is read/written.
 *   Pass NULL to use CRASH_MARKER_PATH_DEFAULT.
 *
 * If a crash marker is found, it is read into *marker_out (if non-NULL)
 * and the file is removed; the caller should initiate WAL recovery.
 *
 * Returns 0 on success, negative errno on failure.
 */
int fault_inject_init(struct fault_registry *reg,
                      const char *crash_marker_path,
                      struct crash_marker *marker_out);

/*
 * fault_inject_cleanup – destroy the registry and release all resources.
 * Safe to call even if fault_inject_init() was never called.
 */
void fault_inject_cleanup(struct fault_registry *reg);

/* ------------------------------------------------------------------ */
/* Fault registration and removal                                       */
/* ------------------------------------------------------------------ */

/*
 * fault_register – add a new fault to the registry.
 *
 * trigger_count: for FAULT_TRIGGER_DEFERRED, the fault fires after this
 *   many matching operations.  Pass 0 for FAULT_TRIGGER_IMMEDIATE.
 *
 * Returns the assigned fault_id (> 0) on success, or negative errno:
 *   -ENOMEM  registry is full (FAULT_REGISTRY_MAX_ENTRIES reached)
 *   -EINVAL  type or target fields are out of range
 */
int fault_register(struct fault_registry *reg,
                   enum fault_type type,
                   const struct fault_target *target,
                   enum fault_trigger_mode trigger_mode,
                   enum fault_persist_mode persist_mode,
                   uint32_t trigger_count);

/*
 * fault_clear – deactivate and remove a single fault entry by ID.
 *
 * Returns 0 on success, -ENOENT if fault_id is not found.
 */
int fault_clear(struct fault_registry *reg, uint32_t fault_id);

/*
 * fault_clear_all – remove all active fault entries from the registry.
 * After this call, all fault_check_* functions return false immediately
 * (type_present == 0).
 */
void fault_clear_all(struct fault_registry *reg);

/* ------------------------------------------------------------------ */
/* Hot-path fault checks (called from the media layer)                 */
/* ------------------------------------------------------------------ */

/*
 * fault_check_nand_read – query whether a read to (ch, chip, die, plane,
 * block, page) should return an uncorrectable ECC error.
 *
 * Hot path: acquires read lock only when type_present bit is set.
 * Returns true if the operation should fail.
 */
bool fault_check_nand_read(struct fault_registry *reg,
                           uint8_t ch, uint8_t chip, uint8_t die,
                           uint8_t plane, uint32_t block, uint32_t page);

/*
 * fault_check_nand_program – query whether a program (write) to the
 * specified page should fail.
 *
 * Returns true if the operation should fail.
 */
bool fault_check_nand_program(struct fault_registry *reg,
                              uint8_t ch, uint8_t chip, uint8_t die,
                              uint8_t plane, uint32_t block, uint32_t page);

/*
 * fault_check_nand_erase – query whether an erase of the specified block
 * should fail (covers both FAULT_NAND_ERASE_ERROR and FAULT_NAND_BAD_BLOCK).
 *
 * Returns true if the operation should fail.
 */
bool fault_check_nand_erase(struct fault_registry *reg,
                            uint8_t ch, uint8_t chip, uint8_t die,
                            uint8_t plane, uint32_t block);

/*
 * fault_get_bit_flip_mask – retrieve the XOR mask for bit-flip injection
 * on the specified page.
 *
 * Returns a 64-bit mask where each set bit corresponds to a bit position
 * that should be flipped in the page buffer.  Returns 0 if no bit-flip
 * fault is registered for this page.
 *
 * The caller must XOR this mask into each 64-bit word of the page buffer
 * at the appropriate byte offset (bit_pos / 64).
 */
uint64_t fault_get_bit_flip_mask(struct fault_registry *reg,
                                 uint8_t ch, uint8_t chip, uint8_t die,
                                 uint8_t plane, uint32_t block, uint32_t page);

/* ------------------------------------------------------------------ */
/* Power-loss injection                                                 */
/* ------------------------------------------------------------------ */

/*
 * fault_inject_power_loss – write the crash marker file and terminate
 * the process immediately via _exit(1).
 *
 * This function does NOT return.  The crash marker contains the scenario,
 * the current simulator timestamp, and the last committed WAL sequence
 * number so that recovery can determine the exact crash point.
 *
 * scenario: one of PLS_IDLE, PLS_WRITE, PLS_GC, PLS_CHECKPOINT.
 * last_wal_seq: the last WAL sequence number written before the crash.
 */
void fault_inject_power_loss(struct fault_registry *reg,
                             enum power_loss_scenario scenario,
                             uint64_t last_wal_seq) __attribute__((noreturn));

/* ------------------------------------------------------------------ */
/* Controller fault helpers                                             */
/* ------------------------------------------------------------------ */

/*
 * fault_check_cmd_timeout – returns true if a command-timeout storm is
 * currently active.  Called by the controller layer before completing
 * any command; if true, the completion is deferred by storm_duration_ms.
 */
bool fault_check_cmd_timeout(struct fault_registry *reg,
                             uint32_t *storm_duration_ms_out);

/*
 * fault_check_mem_pool_exhaust – returns true if the specified pool
 * should return NULL on the next allocation attempt.
 *
 * pool_id: implementation-defined pool identifier.
 */
bool fault_check_mem_pool_exhaust(struct fault_registry *reg,
                                  uint32_t pool_id);

/*
 * fault_trigger_firmware_panic – if a FAULT_CTRL_FIRMWARE_PANIC entry
 * is active, invoke HFSSS_PANIC() immediately.  Called at the start of
 * the controller main loop.
 */
void fault_trigger_firmware_panic(struct fault_registry *reg);

/* ------------------------------------------------------------------ */
/* Management interface                                                 */
/* ------------------------------------------------------------------ */

/*
 * fault_list_active – serialise all active fault entries to a
 * JSON array string in out_buf.
 *
 * The output is a JSON array of objects, one per active entry:
 *   [{"id":1,"type":"read_error","ch":0,"chip":0,...,"hit_count":3}, ...]
 *
 * Returns the number of bytes written (excluding NUL), or -ENOSPC if
 * out_buf is too small.
 */
int fault_list_active(const struct fault_registry *reg,
                      char *out_buf, size_t buf_len);

/*
 * fault_type_to_str – return a human-readable name for a fault type.
 * Returns "unknown" for values outside the defined range.
 */
const char *fault_type_to_str(enum fault_type type);

/*
 * fault_type_from_str – parse a fault type name string.
 * Returns the enum value, or -EINVAL if the name is not recognised.
 */
int fault_type_from_str(const char *name);

#endif /* __HFSSS_FAULT_INJECT_H */
```

---

## 5. 函数接口详细设计

### 5.1 fault_inject_init

```
fault_inject_init(reg, crash_marker_path, marker_out):
  1. memset(reg, 0, sizeof(*reg))
  2. pthread_rwlock_init(&reg->lock)
  3. reg->next_id = 1
  4. 设置 reg->crash_marker_path（默认 CRASH_MARKER_PATH_DEFAULT）
  5. 尝试打开 crash_marker_path：
       ├─ 文件不存在 → 正常启动，return 0
       └─ 文件存在：
             a. 读取 128 字节到 crash_marker 结构体
             b. 校验 magic == CRASH_MARKER_MAGIC
             c. 若 magic 匹配 → 将内容复制到 *marker_out（若非NULL）
             d. 删除 crash_marker_path 文件
             e. return 0（调用者从 *marker_out 得知需要WAL恢复）
             f. 若 magic 不匹配 → 记录 WARN 日志（文件损坏），删除文件，return 0
```

### 5.2 fault_register

```
fault_register(reg, type, target, trigger_mode, persist_mode, trigger_count):
  1. 参数校验：
       ├─ type >= FAULT_TYPE_MAX → return -EINVAL
       ├─ target->channel 超出 sssim_cfg.channels → return -EINVAL
       └─ reg->count >= FAULT_REGISTRY_MAX_ENTRIES → return -ENOMEM
  2. 获取写锁 pthread_rwlock_wrlock(&reg->lock)
  3. 线性扫描 entries[] 找到 active==false 的空槽（最多扫描 MAX_ENTRIES）
  4. 填充 entries[slot]:
       .id            = reg->next_id++
       .type          = type
       .target        = *target
       .trigger_mode  = trigger_mode
       .persist_mode  = persist_mode
       .trigger_count = trigger_count
       .hit_count     = 0
       .active        = true
  5. reg->count++
  6. 重建 sorted_idx[]（按 type 为主键排序，次键为 channel/chip/die/plane/block/page）
  7. 原子置位 reg->type_present |= (1u << type)
  8. 释放写锁
  9. return entries[slot].id
```

### 5.3 热路径查询——fault_check_nand_read

热路径查询的关键优化策略：

**第一级快速过滤（无锁）**：`type_present` 是 `uint32_t`，以原子读取操作（`atomic_load`）检查对应type的比特位。若比特位为零，直接返回 `false`，不进入锁。

**第二级索引查询（读锁）**：`sorted_idx[]` 按 `(type, channel, chip, die, plane, block, page)` 七元组排序，使用二分查找定位候选范围，复杂度 O(log N)。

```
fault_check_nand_read(reg, ch, chip, die, plane, block, page):
  1. if !(atomic_load(&reg->type_present) & (1u << FAULT_NAND_READ_ERROR)):
         return false                         // 无锁快速退出（最常见路径）
  2. pthread_rwlock_rdlock(&reg->lock)
  3. 二分查找 sorted_idx[]：
       找到所有 type == FAULT_NAND_READ_ERROR 的条目区间 [lo, hi)
  4. 对区间内每个候选条目 e = &entries[sorted_idx[i]]：
       if !e->active: continue
       if !fault_target_match(&e->target, ch, chip, die, plane, block, page):
           continue
       // 找到匹配条目
       if e->trigger_mode == FAULT_TRIGGER_DEFERRED && e->trigger_count > 0:
           e->trigger_count--
           continue                            // 未到触发时刻
       // 触发
       atomic_fetch_add(&e->hit_count, 1)
       if e->persist_mode == FAULT_PERSIST_ONE_SHOT:
           e->active = false
           reg->count--
           需要重建索引标记（延迟到释放写锁后）
           atomic_and(&reg->type_present, ~(1u << type))  // 若该type无其他活跃条目
       pthread_rwlock_unlock(&reg->lock)
       return true
  5. pthread_rwlock_unlock(&reg->lock)
  6. return false
```

`fault_check_nand_program` 和 `fault_check_nand_erase` 遵循完全相同的模式，仅第1步中的 type 比特位和第3步中的搜索 type 不同。`fault_check_nand_erase` 额外还要检查 `FAULT_NAND_BAD_BLOCK` 类型。

### 5.4 fault_get_bit_flip_mask

```
fault_get_bit_flip_mask(reg, ch, chip, die, plane, block, page):
  1. if !(atomic_load(&reg->type_present) & (1u << FAULT_NAND_BIT_FLIP)):
         return 0ULL
  2. pthread_rwlock_rdlock(&reg->lock)
  3. 二分查找 FAULT_NAND_BIT_FLIP 区间
  4. 对每个匹配的活跃条目：
       mask |= (1ULL << (e->target.bit_pos % 64))
       触发逻辑（同 fault_check_nand_read 步骤4）
  5. pthread_rwlock_unlock(&reg->lock)
  6. return mask
  // 调用者在页缓冲区 offset = bit_pos / 64 处执行 buf[offset] ^= mask
```

### 5.5 fault_inject_power_loss

```
fault_inject_power_loss(reg, scenario, last_wal_seq):
  // 此函数不返回
  1. 构造 crash_marker 结构体：
       .magic         = CRASH_MARKER_MAGIC
       .version       = 1
       .scenario      = (uint32_t)scenario
       .crash_time_ns = hfsss_get_mono_ns()
       .last_wal_seq  = last_wal_seq
  2. open(reg->crash_marker_path, O_WRONLY|O_CREAT|O_TRUNC, 0644)
  3. write(fd, &marker, sizeof(marker))
  4. fsync(fd); close(fd)
  5. _exit(1)                // 绕过C库atexit，模拟硬掉电
```

**与WAL恢复的交互**：下次 `fault_inject_init()` 发现 crash_marker 文件时，将场景信息返回给调用者（`sssim_init()`），后者根据场景调用相应的WAL replay函数，然后才进入正常运行状态。

### 5.6 fault_target_match（内部辅助函数）

```c
/*
 * fault_target_match – check if a registered fault target matches
 * the given physical address.  A field set to FAULT_TARGET_ANY_* in
 * the entry matches any value in the query.
 */
static inline bool fault_target_match(const struct fault_target *t,
                                      uint8_t ch, uint8_t chip, uint8_t die,
                                      uint8_t plane, uint32_t block,
                                      uint32_t page)
{
    if (t->channel != FAULT_TARGET_ANY_U8   && t->channel != ch)   return false;
    if (t->chip    != FAULT_TARGET_ANY_U8   && t->chip    != chip)  return false;
    if (t->die     != FAULT_TARGET_ANY_U8   && t->die     != die)   return false;
    if (t->plane   != FAULT_TARGET_ANY_U8   && t->plane   != plane) return false;
    if (t->block   != FAULT_TARGET_ANY_U32  && t->block   != block) return false;
    if (t->page    != FAULT_TARGET_ANY_U32  && t->page    != page)  return false;
    return true;
}
```

### 5.7 OOB JSON-RPC 集成（handle_fault_inject）

OOB模块（LLD_07）中 `fault.inject` 方法的处理函数将JSON参数解析后调用本模块接口：

```
handle_fault_inject(ctx, params, result_out, errmsg):
  1. 从 params 中读取必填字段 "type" → fault_type_from_str()
  2. 读取目标地址字段（channel/chip/die/plane/block/page/bit_pos/…）
       未提供的字段默认设置为 FAULT_TARGET_ANY_*
  3. 读取调度参数：
       "immediate": true/false → trigger_mode
       "trigger_count": N     → deferred trigger count
  4. 读取持久化参数：
       "persistent": true/false → persist_mode
  5. 调用 fault_register(reg, type, &target, trigger_mode, persist_mode, count)
  6. 成功：result_out["fault_id"] = returned id
          result_out["status"] = "registered"
  7. 失败：填充 errmsg，返回负数

handle_fault_clear(ctx, params, result_out, errmsg):
  1. 读取 "fault_id"
  2. fault_clear(reg, fault_id)
  3. result_out["status"] = "cleared"

handle_fault_list(ctx, params, result_out, errmsg):
  1. fault_list_active(reg, tmp_buf, sizeof(tmp_buf))
  2. result_out["faults"] = parsed JSON array
```

**JSON-RPC 请求示例**（注入读错误，延迟3次触发，一次性）：

```json
{
  "jsonrpc": "2.0",
  "method": "fault.inject",
  "params": {
    "type": "read_error",
    "channel": 2,
    "chip": 1,
    "die": 0,
    "plane": 0,
    "block": 512,
    "page": 100,
    "immediate": false,
    "trigger_count": 3,
    "persistent": false
  },
  "id": 7
}
```

**成功响应**：

```json
{
  "jsonrpc": "2.0",
  "result": {
    "fault_id": 42,
    "status": "registered"
  },
  "id": 7
}
```

**fault.list 响应示例**：

```json
{
  "jsonrpc": "2.0",
  "result": {
    "faults": [
      {
        "id": 42,
        "type": "read_error",
        "channel": 2, "chip": 1, "die": 0, "plane": 0,
        "block": 512, "page": 100,
        "trigger_mode": "deferred",
        "persist_mode": "one_shot",
        "trigger_count": 1,
        "hit_count": 0,
        "active": true
      },
      {
        "id": 37,
        "type": "bad_block",
        "channel": 0, "chip": 0, "die": 0, "plane": 0,
        "block": 2048, "page": 255,
        "trigger_mode": "immediate",
        "persist_mode": "sticky",
        "trigger_count": 0,
        "hit_count": 5,
        "active": true
      }
    ]
  },
  "id": 8
}
```

### 5.8 媒体层集成点（nand.c 调用规范）

媒体层在执行NAND操作前后，按以下顺序调用故障检查：

```c
/* nand_read_page() 中的故障检查流程（伪代码） */
int nand_read_page(uint8_t ch, uint8_t chip, uint8_t die, uint8_t plane,
                  uint32_t block, uint32_t page, uint8_t *buf)
{
    /* 1. 检查读错误注入 */
    if (fault_check_nand_read(g_fault_reg, ch, chip, die, plane, block, page))
        return NAND_ERR_UECC;

    /* 2. 执行实际DRAM读取（仿真介质数据） */
    memcpy(buf, nand_get_page_ptr(ch, chip, die, plane, block, page),
           g_nand_cfg.page_size);

    /* 3. 应用Bit Flip掩码 */
    uint64_t flip_mask = fault_get_bit_flip_mask(
        g_fault_reg, ch, chip, die, plane, block, page);
    if (flip_mask) {
        uint64_t *w = (uint64_t *)buf;
        /* bit_pos 定位到页内的64位字偏移 */
        uint32_t word_off = g_active_bit_pos / 64;
        w[word_off] ^= flip_mask;
    }

    return NAND_OK;
}
```

```c
/* nand_program_page() 中的故障检查 */
int nand_program_page(uint8_t ch, uint8_t chip, uint8_t die, uint8_t plane,
                      uint32_t block, uint32_t page, const uint8_t *buf)
{
    if (fault_check_nand_program(g_fault_reg, ch, chip, die, plane, block, page))
        return NAND_ERR_PROGRAM;

    /* 检查掉电注入（写途中掉电场景） */
    if (fault_check_power_loss_write(g_fault_reg))
        fault_inject_power_loss(g_fault_reg, PLS_WRITE, wal_get_last_seq());

    /* 执行仿真写入 */
    memcpy(nand_get_page_ptr(ch, chip, die, plane, block, page), buf,
           g_nand_cfg.page_size);
    return NAND_OK;
}
```

---

## 6. 流程图

### 6.1 故障注册流程

```
fault_register(reg, type, target, trigger_mode, persist_mode, count)
    │
    ▼
参数校验
    ├─ type 越界 ──────────────────────────────► return -EINVAL
    ├─ channel/chip/die 超出硬件配置范围 ──────► return -EINVAL
    └─ reg->count >= MAX_ENTRIES ──────────────► return -ENOMEM
    │
    ▼
获取写锁 pthread_rwlock_wrlock()
    │
    ▼
扫描 entries[] 找到空槽（active == false）
    │
    ▼
填充 fault_entry：id / type / target / trigger_mode /
persist_mode / trigger_count / hit_count=0 / active=true
    │
    ▼
reg->count++  /  reg->next_id++
    │
    ▼
重建 sorted_idx[]（按 type+地址七元组排序）
    │
    ▼
原子置位 reg->type_present |= (1u << type)
    │
    ▼
释放写锁
    │
    ▼
return fault_id
```

### 6.2 热路径故障检查流程（以 fault_check_nand_read 为例）

```
fault_check_nand_read(ch, chip, die, plane, block, page)
    │
    ▼
[无锁] atomic_load(type_present) & READ_ERROR_BIT == 0?
    ├─ 是 ─────────────────────────────────────► return false  ← 最常见路径，ns级返回
    └─ 否
         │
         ▼
    获取读锁 pthread_rwlock_rdlock()
         │
         ▼
    二分查找 sorted_idx[]（READ_ERROR 区间）
         │
         ▼
    对区间内每条候选 entry e：
         │
         ├─ e->active == false → 跳过
         │
         ├─ fault_target_match(e, ch,chip,die,plane,block,page) == false → 跳过
         │
         └─ 匹配成功
                │
                ├─ trigger_mode == DEFERRED && trigger_count > 0
                │      └─ trigger_count--  →  跳过（未到触发时刻）
                │
                └─ 触发！
                       │
                       ▼
                  atomic_fetch_add(&e->hit_count, 1)
                       │
                       ├─ persist_mode == ONE_SHOT
                       │      └─ e->active = false
                       │         reg->count--
                       │         若该 type 无其他活跃条目：
                       │           atomic_and(type_present, ~BIT)
                       │
                       ▼
                  释放读锁
                       │
                       ▼
                  return true  ← NAND层返回 UECC
    │
    ▼
释放读锁
    │
    ▼
return false
```

### 6.3 电源故障注入与WAL恢复流程

```
                  测试脚本调用 fault.inject (type=power_loss_during_gc)
                        │
                        ▼
             fault_register(reg, FAULT_POWER_LOSS_GC, ...)
                        │
                        ▼
              GC线程调用 gc_move_page()
                        │
                        ▼
         fault_check_power_loss_gc(reg) == true?
                        │ 是
                        ▼
         fault_inject_power_loss(reg, PLS_GC, last_wal_seq)
                        │
                        ▼
         写入 crash_marker 文件（magic + scenario + last_wal_seq）
                        │
                        ▼
                  fsync + _exit(1)
                        │
                   [进程终止]
                        │
                  [重新启动仿真器]
                        │
                        ▼
         fault_inject_init() 发现 crash_marker 文件
                        │
                        ▼
         读取并校验 crash_marker（magic == CRASH_MARKER_MAGIC?）
                        │
              ┌─────────┴─────────┐
              │ 是                │ 否（文件损坏）
              ▼                   ▼
    *marker_out 已填充         LOG WARN + 删除文件
    删除 crash_marker 文件      继续正常启动
              │
              ▼
    sssim_init() 检测到 marker_out->scenario == PLS_GC
              │
              ▼
    调用 wal_replay(last_wal_seq)
              │
              ▼
    恢复一致性状态后进入正常运行
```

### 6.4 Bit Flip 注入与媒体层应用流程

```
测试脚本：fault.inject type=bit_flip ch=0 chip=0 die=0 plane=0 block=100 page=5 bit_pos=42
    │
    ▼
fault_register(..., FAULT_NAND_BIT_FLIP, target={.bit_pos=42}, ONE_SHOT)
    │
    ▼
主机发起读命令 LBA X → FTL映射 → PPN (ch=0,chip=0,die=0,plane=0,block=100,page=5)
    │
    ▼
nand_read_page(0,0,0,0,100,5,buf)
    │
    ├─ fault_check_nand_read() → false（无读错误）
    │
    ├─ 执行仿真数据读取：memcpy(buf, ...)
    │
    ├─ fault_get_bit_flip_mask(0,0,0,0,100,5) → mask = (1ULL << (42%64)) = 0x400000000000ULL
    │
    ├─ word_offset = 42 / 64 = 0
    │
    └─ buf_as_u64[0] ^= mask   ← 第42位被翻转
    │
    ▼
ECC层检测到1位软错误（可纠正）→ 纠正后返回给FTL
    │
    ▼
主机收到正确数据（ECC已恢复），但 SMART media_errors 计数+1
```

---

## 7. 测试要点

| 测试ID | 测试描述 | 前置条件 | 验证点 |
|--------|----------|----------|--------|
| FI-001 | 注入坏块后BBT更新 | 仿真器运行中，指定Block处于正常状态 | `fault.inject type=bad_block` 后，`bbt_query(ch,chip,die,block)` 返回坏块状态；FTL不再分配该Block |
| FI-002 | 坏块注入后GC跳过 | FI-001已执行 | GC搬移时跳过该坏块；`gc.stats` 中 `bad_block_skipped` 计数增加 |
| FI-003 | 读错误立即触发 | 指定页处于正常状态 | `fault.inject type=read_error immediate=true` 后第一次读该页返回UECC；`smart.get` 中 `media_errors` +1 |
| FI-004 | 读错误一次性触发后自动清除 | FI-003已执行 | 第二次读同一页返回成功（故障已自动清除）；`fault.list` 中该条目 `active=false` |
| FI-005 | 读错误Sticky模式持续触发 | 注入 `persistent=true` 读错误 | 连续10次读该页均返回UECC；`hit_count` == 10；调用 `fault.clear` 后第11次读成功 |
| FI-006 | 延迟触发模式（trigger_count=3） | 指定页正常 | 前3次读成功，第4次读返回UECC；`fault.list` 中 `trigger_count` 每次递减可观测 |
| FI-007 | 写错误注入 | 指定页可写 | `fault.inject type=program_error` 后FTL收到Program Fail，触发Block退役逻辑；`smart.media_errors` +1 |
| FI-008 | 擦除错误注入 | 指定Block可擦 | `fault.inject type=erase_error` 后GC擦除该Block失败；FTL将其标记为坏块并加入BBT |
| FI-009 | Bit Flip精确位翻转 | 指定页已写入已知数据（全0） | 注入 `bit_pos=42`；读回后第42位为1，其余位为0；ECC层报告1-bit软错误 |
| FI-010 | Bit Flip掩码多bit | 对同一页注入两个不同 `bit_pos` 的Bit Flip条目 | 读回数据中两个bit均被翻转；`fault_get_bit_flip_mask` 返回值包含两个set bit |
| FI-011 | FAULT_TARGET_ANY 通配符 | 某Channel下所有页 | `channel=0, chip=ANY, die=ANY, plane=ANY, block=ANY, page=ANY, type=read_error` 注入；该Channel所有读操作均返回UECC |
| FI-012 | 读干扰风暴加速老化 | 指定Block read_count 初始为0 | `fault.inject type=read_disturb_storm`；执行1次读操作后，`nand_block_stats.read_count` 直接达到读干扰阈值；后续读该Block概率性报错 |
| FI-013 | 数据保持退化注入 | 指定Block aging_factor=100 | 注入后时钟推进1秒；等效于100秒的Vt漂移；弱cell出现软错误率上升（ECC可纠正错误计数增加） |
| FI-014 | 立即掉电（idle） | 仿真器空闲（无I/O） | `fault.inject type=power_loss_idle`；进程立即退出（exit code=1）；crash_marker 文件存在，内容合法，scenario=PLS_IDLE |
| FI-015 | 写途中掉电与WAL恢复 | Write Buffer中有脏数据 | 注入 `power_loss_during_write`；进程在下一次 `nand_program` 时退出；重启后 `fault_inject_init` 检测到 marker；WAL replay完成；已提交数据可读；未提交数据丢失 |
| FI-016 | GC途中掉电与WAL恢复 | GC后台运行中 | 注入 `power_loss_during_gc`；进程在 `gc_move_page` 时退出；重启后WAL replay，文件系统一致性验证通过（md5sum）|
| FI-017 | Checkpoint途中掉电恢复 | Checkpoint周期触发中 | 注入 `power_loss_during_checkpoint`；重启后从上一个完整Checkpoint+WAL恢复；所有FUA写数据可读 |
| FI-018 | 固件Panic路径 | 仿真器正常运行 | `fault.inject type=firmware_panic`；下一次控制器主循环调用 `fault_trigger_firmware_panic`；HFSSS_PANIC宏触发，进程以非零码退出，panic日志记录到文件 |
| FI-019 | 内存池耗尽注入 | 指定pool_id有空闲块 | `fault.inject type=mem_pool_exhaust pool_id=2`；下一次从该pool分配返回NULL；控制器进入DEGRADED模式；不崩溃；错误日志中含"pool exhausted" |
| FI-020 | 命令超时风暴 | 仿真器处理I/O | `fault.inject type=cmd_timeout_storm storm_duration_ms=500`；期间所有命令完成延迟>=500ms；`perf.get latency_write.p50` 显著升高；风暴结束后延迟恢复正常 |
| FI-021 | L2P映射表损坏注入 | 仿真器有已映射LBA | `fault.inject type=l2p_corrupt lba_start=0 lba_count=1024`；读该LBA范围返回乱序数据（PPN指向错误页）；`data_integrity_check` 可检测到数据不一致 |
| FI-022 | fault_clear 精确清除 | 注册多个故障（Sticky模式） | 通过 `fault_id` 清除其中一个；`fault.list` 显示该条目消失；其他故障依然有效 |
| FI-023 | fault_clear_all 全清 | 注册5个不同类型故障 | `fault.clear_all` 后：`type_present == 0`；`fault.list` 返回空数组；所有后续NAND操作正常 |
| FI-024 | 注册表容量上限 | 注册1024个故障条目 | 第1025次 `fault_register` 返回 `-ENOMEM`；服务器继续正常运行；OOB返回 JSON-RPC error -32000 |
| FI-025 | 热路径性能（type_present == 0） | 无任何活跃故障 | 100万次 `fault_check_nand_read` 调用的平均耗时 < 10 ns（无锁原子读分支） |
| FI-026 | 热路径性能（1024个活跃故障） | 注册1024个 read_error 故障 | `fault_check_nand_read` 平均耗时 < 200 ns（读锁 + O(log N) 二分查找） |
| FI-027 | 并发注入与查询线程安全 | 4线程并发注册故障，16线程并发 fault_check | 运行10秒后，`hit_count` 总和与实际触发次数一致；无崩溃；Thread Sanitizer 无数据竞争报告 |
| FI-028 | crash_marker 校验失败容错 | 手动写入内容不合法（magic错误）的 crash_marker 文件 | `fault_inject_init` 记录 WARN 日志，删除文件，返回0（正常启动），不触发WAL恢复 |

---

**文档统计**：
- 覆盖需求：4个（REQ-128、REQ-129、REQ-130、REQ-131）
- 新增头文件：`include/common/fault_inject.h`
- 新增源文件：`src/common/fault_inject.c`（注册表与热路径检查）、`src/common/fault_power.c`（掉电注入与crash marker）
- 函数接口数：18个（公共接口）+ 4个内部辅助函数
- 测试用例：28个（FI-001 至 FI-028）
