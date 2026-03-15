# 高保真全栈SSD模拟器（HFSSS）详细设计文档

**文档名称**：Bootloader与上下电管理详细设计
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
7. [测试要点](#7-测试要点)

---

## 1. 模块概述

### 1.1 模块定位与职责

Bootloader与上下电管理模块负责仿真SSD固件的完整生命周期：从上电启动到系统就绪，以及从正常/异常掉电到下次上电恢复之间的状态持久化与一致性保证。

本模块横跨两个功能域：

1. **Bootloader仿真**（REQ-073、REQ-074）：以软件方式复现真实SSD固件启动的六个阶段，注入可配置的时序延迟（合计3–8秒），实现双NOR镜像（Slot A / Slot B）的固件选择与完整性校验，并将启动日志写入NOR Log分区；
2. **上下电服务**（REQ-075、REQ-076）：在上电时检测前次掉电类型（正常/异常/首次格式化），执行对应的恢复路径（checkpoint加载、WAL重放、全量扫描）；在掉电时按正常/异常两条路径安全保存SSD状态，并通过SMART计数器记录电源事件。

### 1.2 与其他模块的关系

| 方向 | 关联模块 | 交互内容 |
|------|----------|----------|
| 启动阶段调用 | 介质线程（NOR/NAND） | nand_device_init、nor_init、BBT加载 |
| 启动阶段调用 | FTL（地址映射/GC/WL） | L2P checkpoint加载、WAL重放、块池初始化 |
| 启动阶段调用 | 主控线程（仲裁/调度/Write Buffer/读缓存） | 各子模块初始化 |
| 启动阶段调用 | NVMe/PCIe仿真层 | 队列结构初始化、CSTS.RDY置位 |
| 全生命周期调用 | OOB管理模块 | 启动OOB监听线程、SMART更新 |
| 掉电时写入 | NOR Flash（SysInfo分区） | clean_shutdown_marker / crash_marker |
| 掉电时写入 | WAL文件 | 增量日志条目 |

### 1.3 NOR Flash分区布局

本模块直接依赖NOR Flash的分区结构（REQ-052），具体如下：

```
NOR Flash 256MB
┌──────────────────────────────────┐
│  Bootloader      4MB  [0x000_0000 – 0x03F_FFFF] │
├──────────────────────────────────┤
│  Firmware Slot A 64MB [0x040_0000 – 0x43F_FFFF] │
├──────────────────────────────────┤
│  Firmware Slot B 64MB [0x440_0000 – 0x83F_FFFF] │
├──────────────────────────────────┤
│  Config Area     8MB  [0x840_0000 – 0x8BF_FFFF] │
├──────────────────────────────────┤
│  BBT             8MB  [0x8C0_0000 – 0x93F_FFFF] │
├──────────────────────────────────┤
│  Log Area        16MB [0x940_0000 – 0xA3F_FFFF] │
├──────────────────────────────────┤
│  SysInfo         4MB  [0xA40_0000 – 0xA7F_FFFF] │
└──────────────────────────────────┘
```

SysInfo分区是上下电管理的核心持久化区域，存储clean shutdown marker、crash marker、上电次数、上次掉电类型等元信息。

### 1.4 设计约束

- 整个Boot序列在单独的仿真线程中顺序执行，不接受主机I/O，直到Phase 5完成并置CSTS.RDY=1；
- 所有相位延迟通过 `nanosleep` 或高精度时钟实现，确保可测量性；
- 异常掉电路径不得调用堆分配函数（避免在信号处理上下文中引发未定义行为）；
- NOR Flash读写操作通过HAL层 `nor_partition_read / nor_partition_write` 完成，不直接操作内存。

---

## 2. 功能需求详细分解

| 需求ID | 需求描述 | 优先级 | 目标版本 |
|--------|----------|--------|----------|
| REQ-073 | 启动序列六阶段仿真：阶段0硬件初始化、阶段1 POST自检、阶段2元数据加载、阶段3控制器初始化、阶段4 NVMe初始化、阶段5系统就绪；总启动时延3–8秒可配置 | P0 | V1.5 |
| REQ-074 | Bootloader特性：启动时间仿真、双镜像冗余Slot A/B切换、安全启动CRC校验、启动日志写入NOR Log分区 | P0 | V2.0 |
| REQ-075 | 上电服务：检测上次掉电类型（正常/异常/首次格式化），执行对应恢复路径，POST自检，SMART记录上电次数与异常掉电次数 | P0 | V1.5 |
| REQ-076 | 掉电服务：正常掉电（NVMe Shutdown通知）七步序列 + CSTS.SHST=0x02；异常掉电通过atexit/SIGTERM/SIGINT信号处理执行尽力而为持久化，维护WAL日志 | P0 | V1.5 |

---

## 3. 数据结构详细设计

### 3.1 启动阶段枚举与启动类型

```c
/*
 * boot_phase – SSD firmware boot sequence phases (REQ-073).
 *
 * The phases are executed strictly in order by boot_init().
 * BOOT_PHASE_5_READY is the terminal state; the simulator accepts
 * host I/O only after this phase is entered.
 */
enum boot_phase {
    BOOT_PHASE_0_HW_INIT    = 0,  /* hardware init: NAND/NOR init, memory pools */
    BOOT_PHASE_1_POST       = 1,  /* POST self-test: scan NOR slots, verify CRC */
    BOOT_PHASE_2_META_LOAD  = 2,  /* metadata load: BBT, L2P checkpoint, WAL replay */
    BOOT_PHASE_3_CTRL_INIT  = 3,  /* controller init: arbitration, WB, cache, LB */
    BOOT_PHASE_4_NVME_INIT  = 4,  /* NVMe init: queue structs, CSTS.RDY=1 */
    BOOT_PHASE_5_READY      = 5,  /* system ready: OOB listener, monitoring, I/O */
};

/*
 * boot_type – outcome of the power-on detection step (REQ-075).
 *
 * Determined by reading the SysInfo NOR partition before any recovery.
 * Controls which recovery path boot_init() executes in Phase 2.
 */
enum boot_type {
    BOOT_FIRST    = 0,  /* all-0xFF SysInfo: first power-on, format required */
    BOOT_NORMAL   = 1,  /* clean shutdown marker present: minimal WAL replay */
    BOOT_RECOVERY = 2,  /* crash marker or missing marker: full WAL replay */
    BOOT_DEGRADED = 3,  /* WAL replay succeeded but data integrity warnings exist */
};

/*
 * shutdown_type – classification of the last power-down event (REQ-076).
 *
 * Persisted to SysInfo partition on every shutdown and read back on boot.
 */
enum shutdown_type {
    SHUTDOWN_NORMAL      = 0,  /* NVMe shutdown notification, CSTS.SHST=0x02 reached */
    SHUTDOWN_ABNORMAL    = 1,  /* SIGTERM/SIGINT caught, best-effort persist executed */
    SHUTDOWN_POWER_LOSS  = 2,  /* SIGKILL / sudden power loss, WAL-only recovery */
};
```

### 3.2 NOR固件镜像Slot描述符

```c
#define NOR_SLOT_MAGIC      0x48465353U  /* "HFSS" */
#define NOR_SLOT_VERSION_INVALID  0xFFFFFFFFU

/*
 * nor_firmware_slot – header stored at the beginning of each firmware
 * partition (Slot A and Slot B) in NOR Flash (REQ-074).
 *
 * Layout (64 bytes):
 *   [0-3]   magic         – fixed to NOR_SLOT_MAGIC; 0xFFFFFFFF means erased
 *   [4-7]   version       – monotonically increasing build number
 *   [8-11]  crc32         – CRC-32 over the firmware image bytes
 *   [12]    active        – 1 = this slot is the primary boot candidate
 *   [13-15] reserved0
 *   [16-19] image_size    – firmware image size in bytes
 *   [20-23] build_timestamp – Unix epoch of firmware build (seconds)
 *   [24-63] reserved1
 *
 * Selection policy (boot_select_firmware_slot):
 *   1. Both slots valid CRC → pick higher version number.
 *   2. Only one valid → use that slot.
 *   3. Neither valid → enter recovery mode (return -1).
 */
struct nor_firmware_slot {
    uint32_t magic;            /* NOR_SLOT_MAGIC or 0xFFFFFFFF */
    uint32_t version;          /* firmware build number */
    uint32_t crc32;            /* CRC-32 of firmware image */
    uint8_t  active;           /* 1 = primary boot candidate */
    uint8_t  reserved0[3];
    uint32_t image_size;       /* bytes */
    uint32_t build_timestamp;  /* Unix epoch seconds */
    uint8_t  reserved1[40];
} __attribute__((packed));     /* 64 bytes */
```

### 3.3 SysInfo分区

```c
#define SYSINFO_MAGIC              0x53594E46U  /* "SYNF" */
#define SYSINFO_CLEAN_MARKER       0xC1EA4E00U  /* "CLEAN\0\0\0" */
#define SYSINFO_CRASH_MARKER       0xCCA5DEADU

/*
 * sysinfo_partition – persisted to NOR SysInfo partition on every
 * power-down and read back on every power-up (REQ-075, REQ-076).
 *
 * The entire structure is written atomically (sector-aligned) to avoid
 * partial-write corruption.  A CRC-32 over bytes [0..size-5] is stored
 * in the last 4 bytes to detect torn writes.
 *
 * Size: 128 bytes (fits within one 64KB NOR sector erase unit).
 */
struct sysinfo_partition {
    uint32_t magic;                /* SYSINFO_MAGIC; 0xFFFFFFFF = unformatted */
    uint32_t boot_count;           /* total power cycles (including current) */
    uint32_t unsafe_shutdown_count;/* incremented when shutdown_type != NORMAL */
    uint8_t  last_shutdown_type;   /* enum shutdown_type, 1 byte */
    uint8_t  clean_shutdown_marker_valid; /* 1 = clean_shutdown_marker is valid */
    uint8_t  crash_marker_valid;   /* 1 = crash_marker is set */
    uint8_t  reserved0;
    uint32_t clean_shutdown_marker;/* SYSINFO_CLEAN_MARKER when set */
    uint32_t crash_marker;         /* SYSINFO_CRASH_MARKER when set */
    uint64_t last_shutdown_ns;     /* CLOCK_REALTIME at last shutdown */
    uint64_t last_boot_ns;         /* CLOCK_REALTIME at last successful boot */
    uint64_t total_power_on_ns;    /* cumulative power-on time in nanoseconds */
    uint32_t wal_sequence_at_shutdown; /* WAL head sequence number */
    uint32_t checkpoint_seq_at_shutdown; /* L2P checkpoint sequence number */
    uint8_t  active_slot;          /* 0 = Slot A, 1 = Slot B, 0xFF = unknown */
    uint8_t  boot_type_last;       /* enum boot_type from last boot */
    uint8_t  reserved1[50];
    uint32_t crc32;                /* CRC-32 of bytes [0..123] */
} __attribute__((packed));         /* 128 bytes */
```

### 3.4 启动上下文

```c
#define BOOT_PHASE_COUNT    6
#define BOOT_LOG_ENTRY_MAX  64
#define BOOT_LOG_MSG_LEN    128

/*
 * boot_log_entry – one timestamped entry in the in-memory boot log.
 * Entries are also flushed to the NOR Log partition at Phase 5.
 */
struct boot_log_entry {
    uint64_t timestamp_ns;        /* CLOCK_MONOTONIC nanoseconds since process start */
    uint8_t  phase;               /* enum boot_phase when the entry was produced */
    uint8_t  level;               /* 0=INFO 1=WARN 2=ERROR */
    char     msg[BOOT_LOG_MSG_LEN];
};

/*
 * boot_ctx – runtime state for the entire boot sequence (REQ-073, REQ-074).
 *
 * Allocated statically inside the simulator root context (sssim_ctx).
 * After boot_init() returns 0, phase == BOOT_PHASE_5_READY and the
 * simulator is ready for host I/O.
 */
struct boot_ctx {
    enum boot_phase  phase;           /* current or last completed phase */
    enum boot_type   boot_type;       /* determined during Phase 1 POST */
    enum shutdown_type last_shutdown; /* read from SysInfo during Phase 0 */

    uint64_t boot_start_ns;           /* CLOCK_MONOTONIC when boot_init() entered */
    uint64_t phase_start_ns[BOOT_PHASE_COUNT]; /* per-phase start timestamps */
    uint64_t phase_duration_ns[BOOT_PHASE_COUNT]; /* per-phase elapsed time */
    uint64_t total_boot_duration_ns;  /* sum of all phase durations */

    uint8_t  active_slot;             /* 0 = Slot A, 1 = Slot B, selected in Phase 1 */
    bool     recovery_mode;           /* true when both NOR slots are corrupt */
    bool     degraded_mode;           /* true when WAL replay produced integrity warnings */

    struct boot_log_entry log[BOOT_LOG_ENTRY_MAX];
    uint32_t log_count;

    /* back-pointer to simulator root (non-owning) */
    void *sssim_ctx;
};
```

### 3.5 掉电管理上下文

```c
#define POWER_DOWN_IO_DRAIN_TIMEOUT_S  30  /* wait at most 30s for in-flight I/O */
#define POWER_DOWN_WB_FLUSH_TIMEOUT_S  60  /* wait at most 60s for WB flush */

/*
 * power_mgmt_ctx – runtime state for power-down and power-up sequences
 * (REQ-075, REQ-076).
 *
 * power_down_normal() and power_down_abnormal() both write to this struct
 * as they progress.  After shutdown completes, sysinfo_flush() persists
 * the relevant fields to NOR.
 */
struct power_mgmt_ctx {
    enum shutdown_type shutdown_type;  /* set by the initiating path */

    uint64_t shutdown_start_ns;   /* CLOCK_MONOTONIC when shutdown was initiated */
    uint64_t io_drain_end_ns;     /* timestamp when in-flight I/O drained */
    uint64_t wb_flush_end_ns;     /* timestamp when Write Buffer flush completed */
    uint64_t l2p_persist_end_ns;  /* timestamp when L2P checkpoint was written */
    uint64_t nor_update_end_ns;   /* timestamp when BBT/PE tables updated in NOR */

    bool     io_drain_ok;         /* true = all in-flight I/O completed before timeout */
    bool     wb_flush_ok;         /* true = Write Buffer fully flushed */
    bool     l2p_persist_ok;      /* true = L2P checkpoint written successfully */
    bool     nor_update_ok;       /* true = BBT/PE tables updated in NOR */

    uint32_t inflight_io_at_start; /* snapshot of in-flight I/O count at shutdown start */
    uint32_t wal_entries_flushed;  /* number of WAL entries written during shutdown */

    /* back-pointer to simulator root (non-owning) */
    void *sssim_ctx;
};
```

---

## 4. 头文件设计

### 4.1 include/common/boot.h

```c
/* include/common/boot.h */
#ifndef __HFSSS_BOOT_H
#define __HFSSS_BOOT_H

#include <stdint.h>
#include <stdbool.h>

/* Forward declarations */
struct sssim_ctx;
struct nor_ctx;
struct nvme_smart_log;

/* ------------------------------------------------------------------ */
/* Enumerations                                                         */
/* ------------------------------------------------------------------ */

enum boot_phase {
    BOOT_PHASE_0_HW_INIT    = 0,
    BOOT_PHASE_1_POST       = 1,
    BOOT_PHASE_2_META_LOAD  = 2,
    BOOT_PHASE_3_CTRL_INIT  = 3,
    BOOT_PHASE_4_NVME_INIT  = 4,
    BOOT_PHASE_5_READY      = 5,
};

enum boot_type {
    BOOT_FIRST    = 0,
    BOOT_NORMAL   = 1,
    BOOT_RECOVERY = 2,
    BOOT_DEGRADED = 3,
};

enum shutdown_type {
    SHUTDOWN_NORMAL      = 0,
    SHUTDOWN_ABNORMAL    = 1,
    SHUTDOWN_POWER_LOSS  = 2,
};

/* ------------------------------------------------------------------ */
/* Bootloader lifecycle                                                 */
/* ------------------------------------------------------------------ */

/*
 * boot_init – execute the full six-phase boot sequence.
 *
 * Blocks the calling thread until Phase 5 (READY) is reached or a
 * fatal error occurs.  All phase timing is injected via nanosleep()
 * to simulate realistic SSD startup latencies.
 *
 * Returns 0 on success (system ready for I/O).
 * Returns -ENODEV if both NOR firmware slots are corrupt (recovery mode).
 * Returns negative errno on any other unrecoverable error.
 */
int boot_init(struct boot_ctx *ctx, struct sssim_ctx *sssim);

/*
 * boot_get_phase – return the current boot phase.
 *
 * Safe to call from any thread.  Reads phase with acquire semantics.
 */
enum boot_phase boot_get_phase(const struct boot_ctx *ctx);

/*
 * boot_select_firmware_slot – scan both NOR firmware slots and choose
 * the active one (REQ-074).
 *
 * Selection policy:
 *   - Both valid CRC → slot with higher version number.
 *   - One valid        → that slot.
 *   - Neither valid    → return -1 (recovery mode).
 *
 * Returns 0 (Slot A) or 1 (Slot B) on success, -1 on failure.
 */
int boot_select_firmware_slot(struct nor_ctx *nor_ctx,
                              struct nor_firmware_slot *slot_out);

/*
 * boot_verify_slot_crc – verify the CRC-32 of a firmware slot header.
 *
 * Returns true if the magic number matches and crc32 is valid.
 */
bool boot_verify_slot_crc(const struct nor_firmware_slot *slot);

/*
 * boot_log_flush_to_nor – write accumulated in-memory boot log entries
 * to the NOR Log partition.  Called at end of Phase 5.
 *
 * Returns number of entries flushed, or negative errno on I/O error.
 */
int boot_log_flush_to_nor(struct boot_ctx *ctx, struct nor_ctx *nor_ctx);

/*
 * boot_get_phase_duration_ms – return the simulated duration of a
 * completed phase in milliseconds.  Returns 0 for phases not yet run.
 */
uint64_t boot_get_phase_duration_ms(const struct boot_ctx *ctx,
                                    enum boot_phase phase);

/* ------------------------------------------------------------------ */
/* Firmware slot update (used by Firmware Commit admin command)        */
/* ------------------------------------------------------------------ */

/*
 * boot_firmware_update – write new firmware image to the inactive slot,
 * verify its CRC, then atomically flip the active flags.
 *
 * new_image:      pointer to firmware image bytes.
 * image_size:     size in bytes.
 * new_version:    build number for the new image.
 *
 * Returns 0 on success, negative errno on failure.
 * On failure the previously active slot remains active (atomic swap).
 */
int boot_firmware_update(struct nor_ctx *nor_ctx,
                         const uint8_t *new_image, uint32_t image_size,
                         uint32_t new_version);

#endif /* __HFSSS_BOOT_H */
```

### 4.2 include/common/power_mgmt.h

```c
/* include/common/power_mgmt.h */
#ifndef __HFSSS_POWER_MGMT_H
#define __HFSSS_POWER_MGMT_H

#include <stdint.h>
#include <stdbool.h>
#include "boot.h"   /* enum shutdown_type, enum boot_type */

/* Forward declarations */
struct sssim_ctx;
struct nor_ctx;
struct nvme_smart_log;
struct sysinfo_partition;

/* ------------------------------------------------------------------ */
/* Power-down paths                                                     */
/* ------------------------------------------------------------------ */

/*
 * power_down_normal – execute the seven-step graceful NVMe shutdown
 * sequence (REQ-076, NVMe SHN=0x01 or 0x02).
 *
 * Steps:
 *   1. Stop accepting new host I/O commands.
 *   2. Wait for all in-flight I/O to complete (timeout: 30s).
 *   3. Flush Write Buffer to NAND.
 *   4. Update L2P checkpoint file.
 *   5. Update BBT and P/E count tables in NOR.
 *   6. Write clean-shutdown marker to SysInfo NOR partition.
 *   7. Set CSTS.SHST = 0x02 (shutdown complete).
 *
 * Returns 0 on success.
 * Returns -ETIMEDOUT if in-flight I/O did not drain within the timeout;
 * in that case the function still completes the remaining steps and sets
 * shutdown_type = SHUTDOWN_ABNORMAL in the persisted SysInfo.
 */
int power_down_normal(struct power_mgmt_ctx *ctx);

/*
 * power_down_abnormal – best-effort emergency persist on SIGTERM/SIGINT
 * (REQ-076).
 *
 * Executes in signal-safe fashion (no malloc, no complex locking).
 * Flushes WAL entries for any Write Buffer data not yet persisted.
 * Writes crash marker to SysInfo NOR partition.
 *
 * Returns 0 if the WAL was flushed; -EIO if even the WAL write failed.
 */
int power_down_abnormal(struct power_mgmt_ctx *ctx);

/* ------------------------------------------------------------------ */
/* Power-up detection and recovery                                      */
/* ------------------------------------------------------------------ */

/*
 * power_detect_last_shutdown – read the SysInfo partition and classify
 * the previous power-down event (REQ-075).
 *
 * sysinfo_buf: caller-provided buffer read from NOR SysInfo partition.
 *
 * Returns the detected enum shutdown_type.
 * Special case: if magic == 0xFFFFFFFF (erased), returns SHUTDOWN_NORMAL
 * and the caller should treat it as BOOT_FIRST.
 */
enum shutdown_type power_detect_last_shutdown(
    const struct sysinfo_partition *sysinfo_buf);

/*
 * power_recovery – execute the appropriate metadata recovery path based
 * on the previous shutdown type (REQ-075).
 *
 * SHUTDOWN_NORMAL  → load L2P checkpoint + minimal WAL replay (delta only).
 * SHUTDOWN_ABNORMAL/POWER_LOSS → full WAL replay from last checkpoint,
 *                                 verify data integrity via OOB region spot-check.
 * BOOT_FIRST (detected externally) → format NAND, initialize all structures.
 *
 * Returns 0 on clean recovery.
 * Returns -ENODATA if WAL is missing or corrupt (degraded mode).
 * Returns negative errno on unrecoverable error.
 */
int power_recovery(struct power_mgmt_ctx *ctx, enum shutdown_type stype);

/*
 * power_recovery_first_boot – format NAND media, initialize BBT,
 * zero-initialize L2P table, write initial SysInfo to NOR.
 * Called when power_detect_last_shutdown returns BOOT_FIRST.
 *
 * Returns 0 on success.
 */
int power_recovery_first_boot(struct power_mgmt_ctx *ctx);

/* ------------------------------------------------------------------ */
/* SMART update on power-on                                             */
/* ------------------------------------------------------------------ */

/*
 * boot_update_smart_on_poweron – increment SMART counters appropriate
 * for the current boot event (REQ-075).
 *
 * Always increments power_cycles[0].
 * Increments unsafe_shutdowns[0] when stype != SHUTDOWN_NORMAL.
 *
 * Thread safety: caller must hold the SMART log write lock.
 */
void boot_update_smart_on_poweron(struct nvme_smart_log *smart_log,
                                  enum shutdown_type stype);

/* ------------------------------------------------------------------ */
/* SysInfo NOR partition helpers                                        */
/* ------------------------------------------------------------------ */

/*
 * sysinfo_read – read and CRC-validate the SysInfo partition from NOR.
 *
 * Returns 0 on success (CRC matched).
 * Returns -EBADMSG if CRC failed (caller should treat as crash/unformatted).
 */
int sysinfo_read(struct nor_ctx *nor_ctx, struct sysinfo_partition *out);

/*
 * sysinfo_write – compute CRC and write the SysInfo partition to NOR.
 *
 * Performs sector erase + page program internally via nor_partition_write.
 * Returns 0 on success, negative errno on NOR I/O error.
 */
int sysinfo_write(struct nor_ctx *nor_ctx,
                  const struct sysinfo_partition *sysinfo);

/* ------------------------------------------------------------------ */
/* Signal handler integration                                           */
/* ------------------------------------------------------------------ */

/*
 * power_install_signal_handlers – install SIGTERM and SIGINT handlers
 * that invoke power_down_abnormal() on the global simulator context.
 *
 * Must be called after the simulator root context is fully initialised.
 * Idempotent; safe to call multiple times.
 */
void power_install_signal_handlers(struct sssim_ctx *sssim);

/*
 * power_install_atexit – register the atexit() handler that calls
 * power_down_abnormal() if CSTS.SHST != 0x02 at process exit.
 */
void power_install_atexit(struct sssim_ctx *sssim);

#endif /* __HFSSS_POWER_MGMT_H */
```

---

## 5. 函数接口详细设计

### 5.1 boot_init — 六阶段启动主序列

`boot_init(ctx, sssim)` 是启动模块的顶层入口，顺序执行全部六个阶段：

```
boot_init(ctx, sssim):
  ctx->boot_start_ns = clock_monotonic_ns()
  ctx->sssim_ctx = sssim

  [Phase 0] boot_phase0_hw_init(ctx)
    ├── nand_device_init(sssim->nand_ctx)        // 初始化所有Channel/Chip/Die结构
    ├── nor_init(sssim->nor_ctx)                 // 初始化NOR Flash仿真层
    ├── mempool_init(sssim->pool_ctx)            // 初始化各尺寸内存池
    └── nanosleep(50ms 仿真延迟)

  [Phase 1] boot_phase1_post(ctx)
    ├── sysinfo_read(nor_ctx, &sysinfo)          // 读取SysInfo分区
    │       ├── CRC失败 → BOOT_RECOVERY
    │       └── magic==0xFFFFFFFF → BOOT_FIRST
    ├── power_detect_last_shutdown(&sysinfo)     // 分类上次掉电类型
    ├── ctx->last_shutdown = detected_type
    ├── boot_select_firmware_slot(nor_ctx, &slot)
    │       ├── 两slot均损坏 → ctx->recovery_mode=true, return -ENODEV
    │       └── 选取active_slot
    ├── boot_verify_slot_crc(&slot)              // 校验固件CRC32
    └── nanosleep(100ms 仿真延迟)

  [Phase 2] boot_phase2_meta_load(ctx)
    ├── power_recovery(pmgmt_ctx, ctx->last_shutdown)
    │       ├── BOOT_FIRST  → power_recovery_first_boot()
    │       ├── BOOT_NORMAL → load_l2p_checkpoint() + wal_replay_delta()
    │       └── BOOT_RECOVERY → wal_replay_full() + oob_integrity_spot_check()
    ├── bbt_load_from_nor(nor_ctx, ftl_ctx)      // 从NOR BBT分区加载坏块表
    ├── ftl_block_pool_init(ftl_ctx)             // 从已加载状态初始化块池
    └── nanosleep(500ms – 2000ms 仿真延迟，与SSD容量成比例)

  [Phase 3] boot_phase3_ctrl_init(ctx)
    ├── arb_init(sssim->arb_ctx)                 // 命令仲裁模块初始化
    ├── sched_init(sssim->sched_ctx)             // I/O调度器初始化
    ├── write_buffer_init(sssim->wb_ctx)         // Write Buffer初始化
    ├── read_cache_init(sssim->rc_ctx)           // 读缓存LRU初始化
    ├── channel_lb_init(sssim->lb_ctx)           // Channel负载均衡初始化
    └── nanosleep(200ms 仿真延迟)

  [Phase 4] boot_phase4_nvme_init(ctx)
    ├── nvme_ctrl_init(sssim->nvme_ctx)          // 初始化PCIe/NVMe控制器结构
    ├── nvme_queues_init(sssim->nvme_ctx)        // Admin Queue + I/O Queue初始化
    ├── nvme_set_csts_rdy(sssim->nvme_ctx, 1)   // 置位CSTS.RDY = 1
    ├── boot_update_smart_on_poweron(smart_log, ctx->last_shutdown)
    └── nanosleep(50ms 仿真延迟)

  [Phase 5] boot_phase5_ready(ctx)
    ├── oob_init(sssim->oob_ctx, sock_path, sssim)   // 启动OOB监听线程
    ├── monitoring_thread_start(sssim)               // 启动监控线程（SMART/温度）
    ├── boot_log_flush_to_nor(ctx, nor_ctx)          // 将启动日志写入NOR Log
    └── ctx->phase = BOOT_PHASE_5_READY              // 允许主机I/O

  ctx->total_boot_duration_ns = clock_monotonic_ns() - ctx->boot_start_ns
  return 0
```

### 5.2 boot_select_firmware_slot — 双镜像选择

```
boot_select_firmware_slot(nor_ctx, slot_out):
  nor_partition_read(NOR_PARTITION_SLOT_A, &slot_a)
  nor_partition_read(NOR_PARTITION_SLOT_B, &slot_b)

  valid_a = boot_verify_slot_crc(&slot_a)   // magic匹配 + CRC32验证
  valid_b = boot_verify_slot_crc(&slot_b)

  if valid_a && valid_b:
      if slot_a.version >= slot_b.version:
          *slot_out = slot_a;  return 0   // Slot A优先（版本相同时）
      else:
          *slot_out = slot_b;  return 1
  elif valid_a:
      *slot_out = slot_a;  return 0
  elif valid_b:
      *slot_out = slot_b;  return 1
  else:
      log_error("both NOR firmware slots corrupt, entering recovery mode")
      return -1
```

### 5.3 power_down_normal — 正常掉电七步序列

```
power_down_normal(ctx):
  sssim = ctx->sssim_ctx
  ctx->shutdown_type = SHUTDOWN_NORMAL
  ctx->shutdown_start_ns = clock_monotonic_ns()

  // Step 1: 停止接受新I/O
  nvme_set_accept_io(sssim->nvme_ctx, false)

  // Step 2: 等待在途I/O排空（超时30秒）
  deadline_ns = ctx->shutdown_start_ns + POWER_DOWN_IO_DRAIN_TIMEOUT_S * 1e9
  while (nvme_get_inflight_count(sssim->nvme_ctx) > 0):
      if clock_monotonic_ns() > deadline_ns:
          log_warn("I/O drain timeout, proceeding with best-effort shutdown")
          ctx->io_drain_ok = false
          ctx->shutdown_type = SHUTDOWN_ABNORMAL
          goto persist_state
      nanosleep(1ms)
  ctx->io_drain_ok = true
  ctx->io_drain_end_ns = clock_monotonic_ns()

  // Step 3: 下刷Write Buffer
persist_state:
  write_buffer_flush_all(sssim->wb_ctx)
  ctx->wb_flush_ok = (write_buffer_dirty_count(sssim->wb_ctx) == 0)
  ctx->wb_flush_end_ns = clock_monotonic_ns()

  // Step 4: 更新L2P Checkpoint
  rc = l2p_checkpoint_write(sssim->ftl_ctx)
  ctx->l2p_persist_ok = (rc == 0)
  ctx->l2p_persist_end_ns = clock_monotonic_ns()

  // Step 5: 更新BBT和P/E计数到NOR
  bbt_persist_to_nor(sssim->nor_ctx, sssim->ftl_ctx)
  pe_table_persist_to_nor(sssim->nor_ctx, sssim->ftl_ctx)
  ctx->nor_update_ok = true
  ctx->nor_update_end_ns = clock_monotonic_ns()

  // Step 6: 写入clean_shutdown_marker到SysInfo
  sysinfo.clean_shutdown_marker = SYSINFO_CLEAN_MARKER
  sysinfo.clean_shutdown_marker_valid = 1
  sysinfo.crash_marker_valid = 0
  sysinfo.last_shutdown_type = ctx->shutdown_type
  sysinfo.last_shutdown_ns = clock_realtime_ns()
  sysinfo.boot_count = sssim->boot_count
  sysinfo_write(sssim->nor_ctx, &sysinfo)

  // Step 7: 置位CSTS.SHST = 0x02
  nvme_set_csts_shst(sssim->nvme_ctx, 0x02)

  return (ctx->shutdown_type == SHUTDOWN_NORMAL) ? 0 : -ETIMEDOUT
```

### 5.4 power_down_abnormal — 异常掉电紧急持久化

```
power_down_abnormal(ctx):
  // 信号处理上下文：禁止malloc/free，禁止pthread锁
  sssim = ctx->sssim_ctx
  ctx->shutdown_type = SHUTDOWN_ABNORMAL

  // 写入crash_marker（NOR写操作为原子扇区程序）
  sysinfo.magic = SYSINFO_MAGIC
  sysinfo.crash_marker = SYSINFO_CRASH_MARKER
  sysinfo.crash_marker_valid = 1
  sysinfo.clean_shutdown_marker_valid = 0
  sysinfo.last_shutdown_type = SHUTDOWN_ABNORMAL
  sysinfo.wal_sequence_at_shutdown = wal_get_head_seq(sssim->wal_ctx)
  sysinfo_write_signal_safe(sssim->nor_ctx, &sysinfo)  // 使用预分配静态缓冲区

  // 刷新WAL：保证Write Buffer中的修改可通过WAL在下次启动重放
  n = wal_flush_pending(sssim->wal_ctx)
  ctx->wal_entries_flushed = (n >= 0) ? (uint32_t)n : 0

  return (n >= 0) ? 0 : -EIO
```

### 5.5 power_detect_last_shutdown — 上电类型检测

```
power_detect_last_shutdown(sysinfo_buf):
  if sysinfo_buf->magic == 0xFFFFFFFF:
      return SHUTDOWN_NORMAL  // 调用方应将其处理为BOOT_FIRST

  if sysinfo_buf->clean_shutdown_marker_valid &&
     sysinfo_buf->clean_shutdown_marker == SYSINFO_CLEAN_MARKER:
      return SHUTDOWN_NORMAL

  if sysinfo_buf->crash_marker_valid &&
     sysinfo_buf->crash_marker == SYSINFO_CRASH_MARKER:
      return SHUTDOWN_ABNORMAL

  // 标志既无CLEAN也无CRASH：说明上次shutdown未完成写入SysInfo
  return SHUTDOWN_POWER_LOSS
```

### 5.6 power_recovery — 分类恢复路径

```
power_recovery(ctx, stype):
  sssim = ctx->sssim_ctx

  switch stype:

  case SHUTDOWN_NORMAL (BOOT_NORMAL):
      // 加载最近Checkpoint
      rc = l2p_checkpoint_load(sssim->ftl_ctx)
      if rc != 0: goto full_wal_replay

      // 仅重放checkpoint之后的WAL增量
      rc = wal_replay_delta(sssim->wal_ctx, sssim->ftl_ctx,
                            sysinfo->checkpoint_seq_at_shutdown)
      return rc

  case SHUTDOWN_ABNORMAL / SHUTDOWN_POWER_LOSS (BOOT_RECOVERY):
full_wal_replay:
      rc = l2p_checkpoint_load(sssim->ftl_ctx)
      // checkpoint加载失败则从空白L2P开始
      if rc != 0: l2p_init_empty(sssim->ftl_ctx)

      rc = wal_replay_full(sssim->wal_ctx, sssim->ftl_ctx)
      if rc != 0: return rc

      // OOB区抽样验证：随机检测512页的LPN与L2P一致性
      warn_count = oob_integrity_spot_check(sssim->nand_ctx, sssim->ftl_ctx, 512)
      if warn_count > 0:
          ctx->degraded_mode = true
          log_warn("recovery: %d OOB integrity warnings, entering degraded mode",
                   warn_count)
      return 0

  case BOOT_FIRST:
      return power_recovery_first_boot(ctx)
```

### 5.7 boot_update_smart_on_poweron — SMART电源计数更新

```
boot_update_smart_on_poweron(smart_log, stype):
  // 原子递增128位计数器（低64位足够表示上电次数）
  smart_log->power_cycles[0]++

  if stype != SHUTDOWN_NORMAL:
      smart_log->unsafe_shutdowns[0]++
      log_warn("unsafe shutdown detected (type=%d): unsafe_shutdowns=%llu",
               stype, smart_log->unsafe_shutdowns[0])
```

### 5.8 boot_firmware_update — 在线固件升级（原子双Slot翻转）

```
boot_firmware_update(nor_ctx, new_image, image_size, new_version):
  // 读取当前活跃Slot
  cur_slot_idx = sysinfo->active_slot   // 0=A, 1=B
  target_slot_idx = 1 - cur_slot_idx    // 写入非活跃Slot

  // 1. 擦除目标Slot分区
  nor_partition_erase(target_slot_partition)

  // 2. 写入新固件镜像
  nor_partition_write(target_slot_partition, new_image, image_size)

  // 3. 构造并写入新Slot Header
  new_slot.magic = NOR_SLOT_MAGIC
  new_slot.version = new_version
  new_slot.crc32 = crc32(new_image, image_size)
  new_slot.active = 1
  new_slot.image_size = image_size
  nor_write_slot_header(target_slot_partition, &new_slot)

  // 4. 验证写入的CRC
  if !boot_verify_slot_crc(&new_slot): return -EIO

  // 5. 原子翻转active标志：先激活新Slot，再取消旧Slot
  nor_set_slot_active(target_slot_idx, 1)
  nor_set_slot_active(cur_slot_idx, 0)

  // 6. 更新SysInfo中的active_slot记录
  sysinfo->active_slot = target_slot_idx
  sysinfo_write(nor_ctx, sysinfo)

  return 0
```

---

## 6. 流程图

### 6.1 六阶段启动主流程

```
boot_init()
    │
    ▼
[Phase 0] 硬件初始化（~50ms）
    ├── nand_device_init()
    ├── nor_init()
    └── mempool_init()
    │
    ▼
[Phase 1] POST自检（~100ms）
    ├── sysinfo_read()
    │       ├─ CRC失败 ──────────────────────────────► BOOT_RECOVERY
    │       └─ magic=0xFFFFFFFF ───────────────────── ► BOOT_FIRST
    ├── power_detect_last_shutdown()
    │       ├─ clean_marker OK ──────────────────────► BOOT_NORMAL
    │       ├─ crash_marker set ────────────────────► BOOT_RECOVERY
    │       └─ 两个标志均缺失 ─────────────────────► BOOT_RECOVERY (POWER_LOSS)
    └── boot_select_firmware_slot()
            ├─ 两Slot均损坏 ── ctx->recovery_mode=true ─► return -ENODEV
            ├─ 仅Slot A有效 ─────────────────────────► active_slot=0
            ├─ 仅Slot B有效 ─────────────────────────► active_slot=1
            └─ 两Slot均有效 ── 选高版本 ──────────────► active_slot=0或1
    │
    ▼
[Phase 2] 元数据加载（~500ms – 2000ms）
    │
    ├─ BOOT_FIRST ──────────────────────────────────────────────────────┐
    │   power_recovery_first_boot()                                     │
    │   ├── format_nand_all()                                           │
    │   ├── bbt_init_factory_scan()                                     │
    │   └── l2p_init_empty()                                            │
    │                                                                   │
    ├─ BOOT_NORMAL ─────────────────────────────────────────────────────┤
    │   l2p_checkpoint_load() + wal_replay_delta()                      │
    │                                                                   │
    └─ BOOT_RECOVERY ──────────────────────────────────────────────────►│
        l2p_checkpoint_load() + wal_replay_full()                       │
        + oob_integrity_spot_check()                                    │
            └─ warn_count > 0 → ctx->degraded_mode = true              │
    │                                                                   │
    ◄──────────────────────────────────────────────────────────────────┘
    │
    ▼
[Phase 3] 控制器初始化（~200ms）
    ├── arb_init()
    ├── sched_init()
    ├── write_buffer_init()
    ├── read_cache_init()
    └── channel_lb_init()
    │
    ▼
[Phase 4] NVMe初始化（~50ms）
    ├── nvme_ctrl_init()
    ├── nvme_queues_init()
    ├── nvme_set_csts_rdy(1)           ◄─── 主机驱动此时可识别设备
    └── boot_update_smart_on_poweron() ◄─── power_cycles++, unsafe_shutdowns++ if needed
    │
    ▼
[Phase 5] 系统就绪
    ├── oob_init()                     ◄─── OOB JSON-RPC监听线程上线
    ├── monitoring_thread_start()      ◄─── SMART/温度监控线程上线
    └── boot_log_flush_to_nor()        ◄─── 启动日志写入NOR Log分区
    │
    ▼
ctx->phase = BOOT_PHASE_5_READY
主机I/O开始被接受
```

### 6.2 正常掉电流程

```
power_down_normal()
    │
    ▼
Step 1: nvme_set_accept_io(false)  ◄── 停止接受新命令
    │
    ▼
Step 2: 等待在途I/O排空
    ┌───────────────────────────────────────┐
    │  while inflight > 0:                  │
    │      if elapsed > 30s:                │
    │          ├─ 超时 ──► shutdown_type = ABNORMAL
    │          └─ break                     │
    │      nanosleep(1ms)                   │
    └───────────────────────────────────────┘
    │
    ▼
Step 3: write_buffer_flush_all()   ◄── 下刷Write Buffer到NAND
    │
    ▼
Step 4: l2p_checkpoint_write()     ◄── 更新L2P持久化文件
    │
    ▼
Step 5: bbt_persist_to_nor()
        pe_table_persist_to_nor()  ◄── 更新BBT和P/E计数到NOR
    │
    ▼
Step 6: sysinfo_write()            ◄── 写入clean_shutdown_marker
    ├─ clean_marker_valid = 1
    ├─ crash_marker_valid = 0
    └─ last_shutdown_type = NORMAL
    │
    ▼
Step 7: nvme_set_csts_shst(0x02)   ◄── 通知主机Shutdown Complete
    │
    ▼
return 0（或 -ETIMEDOUT 若Step 2超时）
```

### 6.3 异常掉电流程

```
SIGTERM / SIGINT 触发
    │
    ▼
signal_handler()
    │
    ▼
power_down_abnormal()
    │
    ├── 使用预分配静态缓冲区（无堆分配）
    │
    ├── sysinfo_write_signal_safe()    ◄── 写入crash_marker
    │       crash_marker_valid = 1
    │       clean_marker_valid = 0
    │       last_shutdown_type = ABNORMAL
    │       wal_sequence_at_shutdown = 当前WAL head
    │
    ├── wal_flush_pending()            ◄── 刷新所有待写WAL条目
    │       └─ 失败 → return -EIO
    │
    └── return 0

atexit() 触发路径:
    │
    ▼
检查 CSTS.SHST == 0x02?
    ├─ 是 → 已完成正常关机，跳过
    └─ 否 → power_down_abnormal()
```

### 6.4 上电恢复决策树

```
boot_init() Phase 1:
    │
    ▼
sysinfo_read() → CRC检查
    ├─ CRC失败 ──────────────────────────────────────────► BOOT_RECOVERY
    │                                                      （WAL全量重放）
    ├─ magic = 0xFFFFFFFF ──────────────────────────────► BOOT_FIRST
    │                                                      （格式化NAND）
    └─ CRC正确 ──► power_detect_last_shutdown()
                        │
                        ├─ clean_marker OK ─────────────► BOOT_NORMAL
                        │                                  （checkpoint + 增量WAL）
                        ├─ crash_marker set ────────────► BOOT_RECOVERY
                        │                                  （全量WAL重放）
                        └─ 两标志均无效 ─────────────────► BOOT_RECOVERY
                                                           （POWER_LOSS路径）

每条路径末尾:
    │
    ▼
boot_update_smart_on_poweron()
    ├─ power_cycles++（总是）
    └─ unsafe_shutdowns++（当stype != SHUTDOWN_NORMAL）
```

### 6.5 双NOR Slot固件升级原子切换

```
boot_firmware_update(new_image, image_size, new_version)
    │
    ▼
读取当前 active_slot（假设当前为 Slot A）
    │
    ▼
目标写入 Slot B:
    ├── nor_partition_erase(Slot B)
    ├── nor_partition_write(Slot B, new_image)
    └── nor_write_slot_header(Slot B, {magic, version, crc32, active=1})
    │
    ▼
boot_verify_slot_crc(Slot B)
    ├─ 失败 → return -EIO（Slot A仍为活跃，回退安全）
    └─ 成功 →
    │
    ▼
原子翻转（先激活新Slot，再取消旧Slot）:
    ├── nor_set_slot_active(Slot B, 1)   ← 写入NOR（幂等）
    └── nor_set_slot_active(Slot A, 0)   ← 写入NOR（幂等）
    │
    ▼
sysinfo->active_slot = 1  （记录Slot B为活跃）
sysinfo_write()
    │
    ▼
return 0
```

---

## 7. 测试要点

| 测试ID | 测试描述 | 验证点 |
|--------|----------|--------|
| BL-001 | 正常冷启动（首次格式化）全流程 | boot_type==BOOT_FIRST，NAND被格式化，L2P全零初始化，SysInfo写入，启动耗时在3–8秒范围内 |
| BL-002 | 正常冷启动（clean shutdown后重启）全流程 | boot_type==BOOT_NORMAL，从最近checkpoint加载L2P，仅重放增量WAL，CSTS.RDY最终置1 |
| BL-003 | 异常重启（crash_marker存在）全流程 | boot_type==BOOT_RECOVERY，执行全量WAL重放，smart_log.unsafe_shutdowns递增 |
| BL-004 | 两个NOR Slot均有效时选高版本 | 注入Slot A version=10 / Slot B version=15，验证active_slot=1（Slot B被选中） |
| BL-005 | Slot A CRC损坏时回退Slot B | 写入Slot A时破坏CRC32，验证active_slot=1，boot未返回错误 |
| BL-006 | 两Slot均损坏时进入recovery_mode | 同时破坏两Slot CRC，验证boot_init返回-ENODEV，ctx->recovery_mode==true |
| BL-007 | 各启动阶段耗时可测量 | 读取boot_get_phase_duration_ms(ctx, phase)，Phase 0约50ms，Phase 1约100ms，Phase 4约50ms，误差<10% |
| BL-008 | 启动日志写入NOR Log分区 | boot_init完成后，读取NOR Log分区，验证包含各阶段INFO条目及时间戳 |
| BL-009 | 正常掉电七步序列完整性 | 调用power_down_normal，验证CSTS.SHST=0x02，SysInfo.clean_marker_valid=1，L2P checkpoint文件更新 |
| BL-010 | 正常掉电时在途I/O排空超时（30s）触发降级 | 注入故意挂起的I/O命令，验证超过30s后shutdown_type改为ABNORMAL，WAL被刷新，流程不阻塞 |
| BL-011 | 正常掉电后重启数据完整性 | 写入1000个LBA，正常掉电，重启后读回，md5sum验证100%一致 |
| BL-012 | SIGTERM触发异常掉电路径 | 向进程发送SIGTERM，验证power_down_abnormal被调用，crash_marker写入SysInfo，WAL entries已刷新 |
| BL-013 | SIGKILL模拟断电后WAL重放恢复 | SIGKILL终止进程（不触发atexit），重启后验证WAL全量重放，BOOT_RECOVERY路径执行，数据无损 |
| BL-014 | atexit防护：进程非正常exit时调用power_down_abnormal | 调用exit()而未完成正常关机流程，验证atexit handler检测CSTS.SHST!=0x02并执行应急持久化 |
| BL-015 | SMART power_cycles在每次启动后递增 | 连续启动5次，验证smart_log.power_cycles[0]==5 |
| BL-016 | SMART unsafe_shutdowns仅在异常掉电后递增 | 1次正常掉电 + 2次SIGTERM，验证unsafe_shutdowns==2 |
| BL-017 | SysInfo CRC校验失败触发BOOT_RECOVERY | 手动破坏SysInfo分区的CRC字段，验证sysinfo_read返回-EBADMSG，启动走BOOT_RECOVERY路径 |
| BL-018 | 固件在线升级后重启使用新Slot | boot_firmware_update写入Slot B version=100，重启，验证active_slot=1，运行固件为新版本 |
| BL-019 | 固件升级写入失败时旧Slot保持活跃 | 注入NOR写错误使Slot B写入失败，验证boot_firmware_update返回-EIO，active_slot仍为0 |
| BL-020 | Write Buffer下刷失败时掉电标记为ABNORMAL | 注入wb_flush失败，调用power_down_normal，验证sysinfo.last_shutdown_type==SHUTDOWN_ABNORMAL，crash_marker_valid==1 |
| BL-021 | OOB线程在Phase 5之后才上线 | 在boot_phase<BOOT_PHASE_5_READY期间尝试连接OOB Socket，验证连接被拒；Phase 5后连接成功 |
| BL-022 | Phase 2元数据加载时延与SSD容量成比例 | 小容量配置（1TB）约500ms，大容量配置（4TB）约2000ms，误差<15% |
| BL-023 | BBT从NOR正确加载并与NAND OOB一致 | 预先在NOR BBT分区写入特定坏块条目，重启后验证ftl_ctx中对应Block状态为BAD |
| BL-024 | SysInfo首次格式化（all-0xFF）触发BOOT_FIRST | 擦除NOR SysInfo分区（全0xFF），重启，验证boot_type==BOOT_FIRST，NAND格式化被执行 |
| BL-025 | 降级模式（DEGRADED）下OOB状态查询返回degraded字段 | 人为制造OOB integrity spot-check警告，启动后查询OOB status.get，验证"degraded":true字段存在 |

---

**文档统计**：
- 覆盖需求：4个（REQ-073、REQ-074、REQ-075、REQ-076）
- 新增头文件：`include/common/boot.h`、`include/common/power_mgmt.h`
- 新增源文件：`src/common/boot.c`、`src/common/power_mgmt.c`、`src/common/sysinfo.c`
- 函数接口数：20+
- 测试用例：25个
