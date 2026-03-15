# 高保真全栈SSD模拟器（HFSSS）详细设计文档

**文档名称**：HAL高级功能详细设计
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

HAL高级功能模块（HAL Advanced）在LLD_04_HAL.md所定义的基础HAL之上，覆盖NVMe异步事件机制、PCIe链路状态管理以及PCI配置空间管理三个高级能力域。这三个能力域在基础HAL中缺失或仅有框架占位，本文档提供完整的数据结构、函数接口及行为规范。

**REQ-063：异步事件管理（Async Event Management）**
NVMe规范定义了异步事件请求（AER）命令机制，允许主机在控制器发生重要状态变化时被异步通知，而无需主机持续轮询。控制器维护一个待决事件队列和一个主机已提交的AER命令队列；事件发生时，若存在待决AER命令则立即完成，否则将事件缓存至环形队列，待主机下次提交AER命令时返回。事件类型包括Error、SMART/Health、Notice及NVM Command Set Specific四类。

**REQ-064：PCIe链路状态管理（PCIe Link State Management）**
PCIe链路支持L0（正常工作）、L0s（快速待机）、L1（低功耗待机）、L2（掉电）四种工作状态，以及热复位（Hot Reset）和功能级复位（FLR）两种复位模式。ASPM（主动状态电源管理）策略决定了哪些低功耗状态可以自动进入。本模块负责状态转换合法性校验、退出延迟仿真以及复位处理流程。

**REQ-069：PCI管理（PCI Management）**
PCI配置空间提供设备标识、BAR映射、能力链表等基础信息。本模块提供配置空间的字节/字/双字读写接口，支持对只读字段的写保护，并提供BAR注册和能力链表遍历接口。

**覆盖需求**：REQ-063、REQ-064、REQ-069。

---

## 2. 功能需求详细分解

| 需求ID | 需求描述 | 优先级 | 目标版本 |
|--------|----------|--------|----------|
| REQ-063 | NVMe异步事件请求（AER）命令队列；事件类型（Error、SMART/Health、Notice、NVM Command Set Specific）；主机通知流程 | P0 | V2.0 |
| REQ-064 | PCIe链路状态（L0/L0s/L1/L2）管理；ASPM策略控制；热复位与FLR处理 | P0 | V2.0 |
| REQ-069 | PCI配置空间读写；BAR管理；设备/厂商ID；能力链表遍历 | P0 | V2.0 |

---

## 3. 数据结构详细设计

```c
// ====== REQ-063: Async Event Management ======

enum nvme_async_event_type {
    NVME_AER_TYPE_ERROR           = 0,
    NVME_AER_TYPE_SMART_HEALTH    = 1,
    NVME_AER_TYPE_NOTICE          = 2,
    NVME_AER_TYPE_NVM_CMD_SET     = 6,
};

enum nvme_async_event_info {
    // Error type events
    NVME_AEI_ERROR_WRITE_TO_INVALID_DOORBELL = 0x00,
    NVME_AEI_ERROR_INVALID_DOORBELL_VALUE    = 0x01,
    NVME_AEI_ERROR_DIAGNOSTIC_FAILURE        = 0x02,
    // SMART/Health type events
    NVME_AEI_SMART_NVM_SUBSYSTEM_RELIABILITY = 0x00,
    NVME_AEI_SMART_TEMPERATURE_THRESHOLD     = 0x01,
    NVME_AEI_SMART_SPARE_BELOW_THRESHOLD     = 0x02,
    // Notice type events
    NVME_AEI_NOTICE_NS_ATTRIBUTE_CHANGED     = 0x00,
    NVME_AEI_NOTICE_FW_ACTIVATION_STARTING   = 0x01,
    NVME_AEI_NOTICE_TELEMETRY_LOG_CHANGED    = 0x02,
};

struct nvme_aer_pending {
    enum nvme_async_event_type type;
    enum nvme_async_event_info info;
    uint8_t  log_page_id;   // which log page was updated
    uint64_t timestamp_ns;
};

#define AER_PENDING_MAX  16
#define AER_REQUEST_MAX  16   // host can submit up to 16 outstanding AER commands

struct hal_aer_ctx {
    // Pending events waiting for an outstanding AER command
    struct nvme_aer_pending pending[AER_PENDING_MAX];
    uint32_t                pending_head;
    uint32_t                pending_tail;
    // Outstanding AER commands from host (each is a CID waiting for completion)
    uint16_t                outstanding_cids[AER_REQUEST_MAX];
    uint32_t                outstanding_count;
    pthread_mutex_t         lock;
};

// ====== REQ-064: PCIe Link State ======

enum pcie_link_state {
    PCIE_LINK_L0   = 0,  // Active — normal operation
    PCIE_LINK_L0s  = 1,  // L0s — fast entry standby
    PCIE_LINK_L1   = 2,  // L1 — low-power standby
    PCIE_LINK_L2   = 3,  // L2 — powered down
    PCIE_LINK_RESET = 4, // Hot reset in progress
    PCIE_LINK_FLR   = 5, // Function Level Reset in progress
};

enum pcie_aspm_policy {
    ASPM_DISABLED     = 0,
    ASPM_L0s          = 1,
    ASPM_L1           = 2,
    ASPM_L0s_L1       = 3,
};

struct pcie_link_ctx {
    enum pcie_link_state  state;
    enum pcie_link_state  prev_state;
    enum pcie_aspm_policy aspm_policy;
    uint64_t              state_enter_ns;     // timestamp of current state entry
    uint32_t              l0s_exit_latency_us;
    uint32_t              l1_exit_latency_us;
    uint32_t              l2_recovery_ms;
    bool                  flr_in_progress;
    pthread_mutex_t       lock;
};

// ====== REQ-069: PCI Management ======

#define PCI_CFG_SPACE_SIZE      256
#define PCI_EXT_CFG_SPACE_SIZE  4096

struct hal_pci_cfg {
    uint8_t  cfg_space[PCI_CFG_SPACE_SIZE];      // standard config space
    uint8_t  ext_cfg_space[PCI_EXT_CFG_SPACE_SIZE]; // PCIe extended config
    uint32_t bar_base[6];     // BAR base addresses
    uint32_t bar_size[6];     // BAR sizes (power of 2)
    bool     bar_is_mmio[6];  // true = MMIO, false = I/O port
    uint16_t vendor_id;       // 0x1B36 (QEMU) or custom
    uint16_t device_id;       // 0x0010 (HFSSS NVMe)
    uint8_t  revision_id;
    uint8_t  class_code[3];   // 0x01, 0x08, 0x02 = NVMe
};
```

---

## 4. 头文件设计

```c
// include/hal/hal_advanced.h
#ifndef HFSSS_HAL_ADVANCED_H
#define HFSSS_HAL_ADVANCED_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

// ======================================================================
// REQ-063: Async Event Management
// ======================================================================

enum nvme_async_event_type;
enum nvme_async_event_info;
struct nvme_aer_pending;
struct hal_aer_ctx;

// Initialize AER context: clear pending ring and outstanding array.
int hal_aer_init(struct hal_aer_ctx *ctx);

// Host submitted an AER command identified by cid.
// If a pending event exists in the ring, complete it immediately.
// Otherwise enqueue cid in outstanding_cids.
int hal_aer_submit_request(struct hal_aer_ctx *ctx, uint16_t cid);

// Called internally when an event occurs (temperature alert, spare low, etc.).
// If an outstanding CID exists, complete it with event data immediately.
// Otherwise enqueue the event in the pending ring.
int hal_aer_post_event(struct hal_aer_ctx *ctx,
                       enum nvme_async_event_type type,
                       enum nvme_async_event_info info,
                       uint8_t log_page_id);

// Construct AER completion DW0 and post to the Admin CQ for the given cid.
int hal_aer_complete_event(struct hal_aer_ctx *ctx,
                           uint16_t cid,
                           enum nvme_async_event_type type,
                           enum nvme_async_event_info info,
                           uint8_t log_page_id);

// Called on controller reset.
// Abort all outstanding AER commands with status Aborted Command (SCT=0, SC=0x07).
int hal_aer_abort_pending(struct hal_aer_ctx *ctx);

// ======================================================================
// REQ-064: PCIe Link State Management
// ======================================================================

enum pcie_link_state;
enum pcie_aspm_policy;
struct pcie_link_ctx;

// Validate and execute a link state transition.
// Logs the transition and elapsed time in the previous state.
// Returns -EINVAL for illegal transitions.
int pcie_link_transition(struct pcie_link_ctx *ctx,
                         enum pcie_link_state new_state);

// Record entry timestamp; set state to L0s; notify power management subsystem.
int pcie_link_enter_l0s(struct pcie_link_ctx *ctx);

// Simulate L0s exit latency (10–200 µs per l0s_exit_latency_us); restore to L0.
int pcie_link_exit_l0s(struct pcie_link_ctx *ctx);

// Record entry timestamp; set state to L1; longer exit latency (1–65 ms).
int pcie_link_enter_l1(struct pcie_link_ctx *ctx);

// Simulate L1 exit latency; restore to L0.
int pcie_link_exit_l1(struct pcie_link_ctx *ctx);

// Hot Reset: set state=RESET; quiesce all in-flight operations;
// re-initialize NVMe registers to power-on defaults; restore L0.
int pcie_hot_reset(struct pcie_link_ctx *ctx);

// Function Level Reset: save BAR/config state; reset controller state;
// restore BAR/config; notify host via interrupt.
int pcie_flr(struct pcie_link_ctx *ctx);

// ======================================================================
// REQ-069: PCI Management
// ======================================================================

struct hal_pci_cfg;

// Byte/word/dword read from standard or extended config space.
uint8_t  hal_pci_cfg_read8 (const struct hal_pci_cfg *cfg, uint32_t offset);
uint16_t hal_pci_cfg_read16(const struct hal_pci_cfg *cfg, uint32_t offset);
uint32_t hal_pci_cfg_read32(const struct hal_pci_cfg *cfg, uint32_t offset);

// Byte/word/dword write with protection for read-only fields.
// Returns -EACCES if offset falls within a read-only range; 0 on success.
int hal_pci_cfg_write8 (struct hal_pci_cfg *cfg, uint32_t offset, uint8_t  val);
int hal_pci_cfg_write16(struct hal_pci_cfg *cfg, uint32_t offset, uint16_t val);
int hal_pci_cfg_write32(struct hal_pci_cfg *cfg, uint32_t offset, uint32_t val);

// Register a BAR region: record base address and size for bar_idx (0–5).
int hal_pci_bar_map(struct hal_pci_cfg *cfg,
                    uint8_t bar_idx,
                    uint32_t base,
                    uint32_t size);

// Walk the standard capability list starting at offset 0x34 to find cap_id.
// Returns the offset of the matching capability header, or 0 if not found.
uint8_t hal_pci_capability_find(const struct hal_pci_cfg *cfg, uint8_t cap_id);

#endif /* HFSSS_HAL_ADVANCED_H */
```

---

## 5. 函数接口详细设计

### 5.1 REQ-063：异步事件管理函数

#### `hal_aer_init`

```c
int hal_aer_init(struct hal_aer_ctx *ctx);
```

**行为**：将 `pending_head`、`pending_tail`、`outstanding_count` 清零；对 `pending[]` 和 `outstanding_cids[]` 执行 `memset`；初始化互斥锁。返回0表示成功，-EINVAL表示ctx为NULL。

---

#### `hal_aer_submit_request`

```c
int hal_aer_submit_request(struct hal_aer_ctx *ctx, uint16_t cid);
```

**行为**：
1. 获取锁。
2. 若 `pending_head != pending_tail`（环形队列非空），取出队头事件，调用 `hal_aer_complete_event` 立即完成，释放锁并返回。
3. 若 `outstanding_count >= AER_REQUEST_MAX`，释放锁，返回 `-ENOBUFS`（超过最大待决AER命令数，NVMe规范要求主机不超过CAP.MQES限定）。
4. 将 `cid` 写入 `outstanding_cids[outstanding_count++]`，释放锁，返回0。

**并发安全**：全程持有 `ctx->lock`。

---

#### `hal_aer_post_event`

```c
int hal_aer_post_event(struct hal_aer_ctx *ctx,
                       enum nvme_async_event_type type,
                       enum nvme_async_event_info info,
                       uint8_t log_page_id);
```

**行为**：
1. 获取锁。
2. 若 `outstanding_count > 0`，取出 `outstanding_cids[0]`（FIFO），将计数减一并左移数组，调用 `hal_aer_complete_event`，释放锁并返回。
3. 计算下一个尾指针 `next_tail = (pending_tail + 1) % AER_PENDING_MAX`。若 `next_tail == pending_head`，表示环形队列已满（最多缓存 `AER_PENDING_MAX - 1` 个事件）；记录溢出计数器，丢弃新事件，释放锁，返回 `-ENOSPC`。
4. 将事件写入 `pending[pending_tail]`，记录当前 `clock_gettime(CLOCK_MONOTONIC)` 时间戳，推进 `pending_tail`，释放锁，返回0。

---

#### `hal_aer_complete_event`

```c
int hal_aer_complete_event(struct hal_aer_ctx *ctx,
                           uint16_t cid,
                           enum nvme_async_event_type type,
                           enum nvme_async_event_info info,
                           uint8_t log_page_id);
```

**行为**：按照NVMe规范构造AER完成项DW0：
- Bits [2:0]：Async Event Type（`type`）
- Bits [15:8]：Async Event Information（`info`）
- Bits [23:16]：Associated Log Page Identifier（`log_page_id`）
- Status Field：Success（SCT=0, SC=0）

将完成项投递到Admin CQ并更新CQ头指针，触发MSI-X中断向量0通知主机。调用者须在持锁或无锁场景下均保证线程安全（本函数本身不加锁，由调用方负责）。

---

#### `hal_aer_abort_pending`

```c
int hal_aer_abort_pending(struct hal_aer_ctx *ctx);
```

**行为**：在控制器复位路径上调用。遍历 `outstanding_cids[]`，对每个待决CID构造状态为 Command Abort Requested（SCT=0, SC=0x07）的完成项并投递到Admin CQ；将 `outstanding_count` 清零；清空 `pending` 环形队列；返回已中止的命令数量。

---

### 5.2 REQ-064：PCIe链路状态管理函数

#### `pcie_link_transition`

```c
int pcie_link_transition(struct pcie_link_ctx *ctx,
                         enum pcie_link_state new_state);
```

**合法转换矩阵**：

| 当前状态 | 允许目标状态 |
|----------|-------------|
| L0 | L0s、L1、RESET、FLR |
| L0s | L0、L1 |
| L1 | L0、L2 |
| L2 | L0（恢复） |
| RESET | L0 |
| FLR | L0 |

不在矩阵中的转换返回 `-EINVAL` 并记录警告日志。合法转换记录前一状态、更新 `state_enter_ns` 为当前单调时钟，并输出转换日志（含在上一状态的驻留时长）。

---

#### `pcie_link_enter_l0s`

```c
int pcie_link_enter_l0s(struct pcie_link_ctx *ctx);
```

**行为**：调用 `pcie_link_transition(ctx, PCIE_LINK_L0s)`；记录进入时间戳；通知电源管理模块降低时钟门控。调用方须保证当前处于L0状态。

---

#### `pcie_link_exit_l0s`

```c
int pcie_link_exit_l0s(struct pcie_link_ctx *ctx);
```

**行为**：按 `l0s_exit_latency_us`（范围10–200 µs）调用 `usleep` 仿真退出延迟；调用 `pcie_link_transition(ctx, PCIE_LINK_L0)`；通知电源管理模块恢复全速时钟。

---

#### `pcie_link_enter_l1`

```c
int pcie_link_enter_l1(struct pcie_link_ctx *ctx);
```

**行为**：与 `pcie_link_enter_l0s` 类似，但目标状态为 `PCIE_LINK_L1`；L1进入需要完成所有待决事务（通过流量仲裁器确认）；通知功耗管理模块关断收发器PLL。

---

#### `pcie_link_exit_l1`

```c
int pcie_link_exit_l1(struct pcie_link_ctx *ctx);
```

**行为**：按 `l1_exit_latency_us`（范围1000–65000 µs，即1–65 ms）仿真退出延迟；调用 `pcie_link_transition(ctx, PCIE_LINK_L0)`。

---

#### `pcie_hot_reset`

```c
int pcie_hot_reset(struct pcie_link_ctx *ctx);
```

**行为**：
1. 调用 `pcie_link_transition(ctx, PCIE_LINK_RESET)`。
2. 向所有进行中的NVMe命令队列发送中止信号，等待队列排空（超时100 ms）。
3. 将NVMe控制器寄存器（CC、CSTS、AQA、ASQ、ACQ等）复位至上电默认值。
4. 保留PCI配置空间内容（BAR、命令寄存器等不受Hot Reset影响）。
5. 调用 `pcie_link_transition(ctx, PCIE_LINK_L0)`，恢复正常链路。
6. 返回0表示成功，负值表示排队超时或复位失败。

---

#### `pcie_flr`

```c
int pcie_flr(struct pcie_link_ctx *ctx);
```

**行为**：
1. 检测到FLR（通过PCIE Device Control Register bit[15]置位触发）。
2. 保存当前BAR基地址和PCI命令寄存器快照。
3. 调用 `pcie_link_transition(ctx, PCIE_LINK_FLR)`。
4. 执行控制器状态全清：命令队列、完成队列、寄存器状态、DMA上下文均复位至初始值。
5. 从快照恢复BAR基地址和PCI命令寄存器（FLR不影响配置空间）。
6. 调用 `pcie_link_transition(ctx, PCIE_LINK_L0)`。
7. 通过MSI-X通知主机FLR完成，主机可重新枚举并下发NVMe初始化命令序列。

---

### 5.3 REQ-069：PCI管理函数

#### `hal_pci_cfg_read8/16/32`

```c
uint8_t  hal_pci_cfg_read8 (const struct hal_pci_cfg *cfg, uint32_t offset);
uint16_t hal_pci_cfg_read16(const struct hal_pci_cfg *cfg, uint32_t offset);
uint32_t hal_pci_cfg_read32(const struct hal_pci_cfg *cfg, uint32_t offset);
```

**行为**：当 `offset < PCI_CFG_SPACE_SIZE` 时从 `cfg_space[]` 读取；当 `offset >= PCI_CFG_SPACE_SIZE` 且 `offset < PCI_EXT_CFG_SPACE_SIZE` 时从 `ext_cfg_space[]` 读取。越界偏移返回全1（0xFF / 0xFFFF / 0xFFFFFFFF），与PCIe总线未响应行为一致。`read16` 和 `read32` 要求偏移自然对齐，非对齐偏移返回全1并记录警告。

---

#### `hal_pci_cfg_write8/16/32`

```c
int hal_pci_cfg_write8 (struct hal_pci_cfg *cfg, uint32_t offset, uint8_t  val);
int hal_pci_cfg_write16(struct hal_pci_cfg *cfg, uint32_t offset, uint16_t val);
int hal_pci_cfg_write32(struct hal_pci_cfg *cfg, uint32_t offset, uint32_t val);
```

**只读字段保护范围**（偏移均为标准PCI配置空间）：

| 偏移 | 字段 | 说明 |
|------|------|------|
| 0x00–0x01 | Vendor ID | 只读 |
| 0x02–0x03 | Device ID | 只读 |
| 0x08 | Revision ID | 只读 |
| 0x09–0x0B | Class Code | 只读 |

向只读字段写入时返回 `-EACCES`，配置空间内容不变。其余可写字段按值写入并返回0。

---

#### `hal_pci_bar_map`

```c
int hal_pci_bar_map(struct hal_pci_cfg *cfg,
                    uint8_t bar_idx,
                    uint32_t base,
                    uint32_t size);
```

**行为**：校验 `bar_idx` 在0–5范围内；校验 `size` 为2的整数次幂（通过 `size & (size - 1) == 0` 判断）；将 `base` 和 `size` 写入 `cfg->bar_base[bar_idx]` 和 `cfg->bar_size[bar_idx]`；同步更新标准配置空间中对应BAR寄存器（偏移0x10 + bar_idx * 4）的值。非法参数返回 `-EINVAL`。

---

#### `hal_pci_capability_find`

```c
uint8_t hal_pci_capability_find(const struct hal_pci_cfg *cfg, uint8_t cap_id);
```

**行为**：从标准配置空间偏移0x34读取能力链表起始指针（Capabilities Pointer）；沿链表逐项遍历，对比每项能力头字节（Cap ID）与目标 `cap_id`；找到则返回该能力结构的起始偏移；链表末尾（Next Cap Pointer = 0x00）仍未找到则返回0。为防止链表成环导致死循环，最多遍历48次（标准配置空间容量上限）。

---

## 6. 流程图

### 6.1 AER生命周期

**情形A：主机先提交AER命令，事件后发生**

```
Host                        Controller (hal_aer_submit_request)
 |                                |
 |--- AER Command (CID=N) ------->|
 |                         pending队列为空
 |                         outstanding_cids[0] = N
 |                         outstanding_count = 1
 |                                |
 |            (温度超阈值触发 hal_aer_post_event)
 |                                |
 |                         outstanding_count > 0
 |                         取出CID=N
 |                         hal_aer_complete_event(N, SMART_HEALTH, TEMP, 0x02)
 |                                |
 |<-- AER Completion (CID=N) -----|
 |    DW0: type=1, info=0x01,     |
 |         log_page_id=0x02       |
```

**情形B：事件先发生，主机后提交AER命令**

```
Controller (hal_aer_post_event)   Host
 |                                 |
 | (备用空间不足触发)               |
 outstanding_count == 0            |
 pending[tail] = {SMART, SPARE, 0x02}
 pending_tail++                    |
 |                                 |
 |          (主机下发 AER Command, CID=M)
 |<--- AER Command (CID=M) --------|
 hal_aer_submit_request            |
 pending队列非空                   |
 取出队头事件                      |
 hal_aer_complete_event(M, SMART_HEALTH, SPARE, 0x02)
 |--- AER Completion (CID=M) ----->|
 |    DW0: type=1, info=0x02,      |
 |         log_page_id=0x02        |
```

---

### 6.2 PCIe链路状态机

```
                    ASPM触发 / NVMe PS3
        ┌──────────────────────────────────────┐
        │                                      │
        ▼        快速退出(<200µs)              │
  ┌─────────┐ ◄──────────────────── ┌──────────┴──┐
  │   L0    │                       │    L0s      │
  │ (Active)│ ──────────────────► ──┤  (Standby)  │
  └────┬────┘  ASPM触发/NVMe PS3    └─────────────┘
       │
       │ ASPM触发 / NVMe PS4
       ▼        退出(1~65ms)
  ┌─────────┐ ◄────────────────────────────────────
  │   L1    │
  │(Low Pwr)│ ──────────────────────────────────►
  └────┬────┘  掉电触发
       │
       │ 掉电
       ▼        恢复上电
  ┌─────────┐ ─────────────────────────────────► L0
  │   L2    │
  │(Pwr Off)│
  └─────────┘

  任意状态 ──► RESET（热复位检测）──► L0（复位完成）
  L0       ──► FLR（FLR检测）    ──► L0（FLR完成）
```

---

### 6.3 FLR序列

```
检测FLR触发
(Device Control Reg bit[15] = 1)
        │
        ▼
保存快照
  ├─ bar_base[0..5]
  ├─ bar_size[0..5]
  └─ PCI Command Register
        │
        ▼
pcie_link_transition(FLR)
        │
        ▼
控制器状态全清
  ├─ 所有SQ/CQ清空
  ├─ NVMe寄存器复位至上电默认值
  ├─ DMA上下文清零
  └─ 内部状态机复位
        │
        ▼
从快照恢复
  ├─ bar_base[0..5] → cfg_space[0x10..0x27]
  ├─ bar_size[0..5]
  └─ PCI Command Register
        │
        ▼
pcie_link_transition(L0)
        │
        ▼
MSI-X中断通知主机
(主机可重新下发 Identify → Create SQ/CQ 序列)
```

---

## 7. 集成要点

### 7.1 AER事件触发源

| 触发场景 | 调用形式 | 关联日志页 |
|----------|----------|-----------|
| OOB温度模型检测到WCTEMP/CCTEMP超阈值（LLD_07） | `hal_aer_post_event(ctx, NVME_AER_TYPE_SMART_HEALTH, NVME_AEI_SMART_TEMPERATURE_THRESHOLD, 0x02)` | SMART/Health Log（0x02） |
| 可用备用空间低于阈值（SMART spare_thresh） | `hal_aer_post_event(ctx, NVME_AER_TYPE_SMART_HEALTH, NVME_AEI_SMART_SPARE_BELOW_THRESHOLD, 0x02)` | SMART/Health Log（0x02） |
| 命名空间创建或删除完成 | `hal_aer_post_event(ctx, NVME_AER_TYPE_NOTICE, NVME_AEI_NOTICE_NS_ATTRIBUTE_CHANGED, 0x04)` | Changed Namespace List Log（0x04） |
| 固件激活流程启动 | `hal_aer_post_event(ctx, NVME_AER_TYPE_NOTICE, NVME_AEI_NOTICE_FW_ACTIVATION_STARTING, 0x03)` | Firmware Slot Information（0x03） |

`hal_aer_post_event` 由各触发模块直接调用；AER上下文指针通过全局设备上下文（`hfsss_dev_ctx`）访问，无需跨模块传参。

### 7.2 PCIe链路状态转换触发源

| 触发场景 | 调用形式 |
|----------|----------|
| NVMe电源状态进入PS3（NVMe Power Management） | `pcie_link_enter_l1(link_ctx)` |
| NVMe电源状态进入PS4 | 先 `pcie_link_enter_l1`，再视需要进入L2 |
| HAL看门狗超时（命令响应超过阈值） | `pcie_flr(link_ctx)` |
| QEMU设备总线复位信号 | `pcie_hot_reset(link_ctx)` |

### 7.3 PCI配置空间访问

PCI配置空间读写接口主要被以下场景调用：
- **NVMe测试框架**：在枚举阶段验证 VID（0x1B36）、DID（0x0010）、Class Code（0x01/0x08/0x02）；
- **BAR初始化**：仿真器启动时调用 `hal_pci_bar_map` 注册MMIO区域，QEMU前端据此将BAR映射到虚拟机地址空间；
- **能力链表遍历**：NVMe驱动初始化时通过 `hal_pci_capability_find(cfg, 0x05)` 定位MSI能力结构，通过 `hal_pci_capability_find(cfg, 0x10)` 定位PCIe能力结构。

---

## 8. 测试要点

| 测试编号 | 测试场景 | 期望结果 |
|----------|----------|----------|
| HA-001 | 提交AER命令（CID=1），随后触发温度超阈值事件 | 在1 ms内收到AER完成项；type=1，info=0x01，log_page_id=0x02 |
| HA-002 | 先触发温度事件，再提交AER命令（CID=2） | 事件进入pending队列；AER命令提交后立即完成；pending队列清空 |
| HA-003 | 先触发备用空间低事件，再提交AER命令 | 完成项type=1，info=0x02，log_page_id=0x02 |
| HA-004 | 连续触发17个事件，pending队列仅能容纳16个 | 第17个事件被丢弃；`hal_aer_post_event` 返回 `-ENOSPC`；已缓存的16个事件不受影响 |
| HA-005 | 提交16个AER命令后，再提交第17个 | 第17个返回 `-ENOBUFS`；前16个命令正常排队 |
| HA-006 | 控制器复位时存在3个待决AER命令 | `hal_aer_abort_pending` 返回3；3个完成项均携带SC=0x07（Command Abort Requested） |
| HA-007 | AER完成项DW0格式校验 | Bits[2:0]=event type，Bits[15:8]=event info，Bits[23:16]=log_page_id |
| HA-008 | 多线程并发：1个线程持续 `hal_aer_post_event`，另1个持续 `hal_aer_submit_request` | 无死锁、无数据竞争；Valgrind Helgrind无报告 |
| HA-009 | PCIe L0→L0s转换，随后L0s→L0 | 退出延迟在10–200 µs范围内；状态日志记录驻留时长 |
| HA-010 | PCIe L0→L1转换，随后L1→L0 | 退出延迟在1–65 ms范围内；电源管理模块收到通知 |
| HA-011 | 非法转换：L0s→L2 | 返回 `-EINVAL`；状态保持L0s不变；记录警告日志 |
| HA-012 | 非法转换：L2→L0s | 返回 `-EINVAL` |
| HA-013 | PCIe热复位（Hot Reset） | NVMe CC、CSTS寄存器复位至上电默认值；BAR内容不变；测试程序可重新提交NVMe初始化序列 |
| HA-014 | 热复位期间有4条在途NVMe命令 | 复位前命令队列排空（超时100 ms）；完成项携带中止状态 |
| HA-015 | FLR执行后验证控制器状态清零 | SQ/CQ寄存器为0；DMA上下文清零；BAR基地址与FLR前一致 |
| HA-016 | FLR后主机重新枚举 | 主机可成功下发Identify Controller并收到有效响应 |
| HA-017 | PCI配置空间读：VID、DID、Class Code | VID=0x1B36，DID=0x0010，Class Code=0x01/0x08/0x02 |
| HA-018 | PCI配置空间写保护：写入VID偏移0x00 | 返回 `-EACCES`；VID值保持0x1B36不变 |
| HA-019 | PCI配置空间写保护：写入Revision ID偏移0x08 | 返回 `-EACCES`；Revision ID不变 |
| HA-020 | `hal_pci_cfg_read16` 非对齐访问（偏移0x01） | 返回0xFFFF；记录警告日志 |
| HA-021 | `hal_pci_bar_map` 注册BAR0：base=0xFE000000，size=0x4000 | 配置空间偏移0x10读取值与base一致；size校验通过 |
| HA-022 | `hal_pci_bar_map` 使用非2次幂size（如0x3000） | 返回 `-EINVAL`；BAR不注册 |
| HA-023 | `hal_pci_capability_find` 查找PCIe能力（cap_id=0x10） | 返回能力结构起始偏移；与手动计算值一致 |
| HA-024 | `hal_pci_capability_find` 查找不存在的能力（cap_id=0xFF） | 返回0；遍历不超过48次 |

---

**文档统计**

| 项目 | 内容 |
|------|------|
| 覆盖需求 | REQ-063、REQ-064、REQ-069 |
| 头文件 | `include/hal/hal_advanced.h` |
| 函数接口数量 | 20个 |
| 测试用例数量 | 24个（HA-001 ~ HA-024） |
