# 高保真全栈SSD模拟器（HFSSS）详细设计文档总览

**文档版本**：V1.0
**编制日期**：2026-03-08

---

## 目录

1. [LLD文档概述](#1-lld文档概述)
2. [模块关系图](#2-模块关系图)
3. [各LLD文档说明](#3-各lld文档说明)
4. [使用指南](#4-使用指南)
5. [后续工作](#5-后续工作)

---

## 1. LLD文档概述

本系列详细设计文档（LLD）基于PRD和HLD，提供了可直接用于编码的详细设计，包括：

- 完整的数据结构定义
- 完整的头文件设计
- 详细的函数接口说明
- 模块内部逻辑
- 流程图
- Debug机制
- 测试用例

---

## 2. 模块关系图

```
┌─────────────────────────────────────────────────────────────┐
│                     主机 Linux 内核                          │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐ │
│  │  NVMe Driver │  │  File System │  │  fio / nvme-cli  │ │
│  └──────┬───────┘  └──────┬───────┘  └─────────┬────────┘ │
└─────────┼─────────────────────┼────────────────────┼──────────┘
          │                     │                    │
┌─────────▼─────────────────────▼────────────────────▼──────────┐
│              LLD_01: PCIe/NVMe设备仿真模块                      │
│  (内核模块 - 与内核NVMe驱动交互)                                │
└─────────────────────────────┬──────────────────────────────────┘
                              │ 共享内存/Ring Buffer
┌─────────────────────────────▼──────────────────────────────────┐
│              LLD_02: 主控线程模块                               │
│  (仲裁/调度/Write Buffer/读缓存/负载均衡/流量控制)            │
└──────────┬──────────────────────────┬──────────────────────────┘
           │                          │
┌──────────▼──────────┐   ┌─────────▼──────────────┐
│ LLD_06: 算法任务层  │   │ LLD_05: 通用平台层    │
│ (FTL/GC/WL/ECC)     │   │ (RTOS/内存/Log/Debug) │
└──────────┬──────────┘   └─────────────────────────┘
           │
┌──────────▼──────────┐
│  LLD_04: HAL层      │
│  (NAND/NOR驱动)     │
└──────────┬──────────┘
           │
┌──────────▼──────────┐
│ LLD_03: 介质线程    │
│ (NAND/NOR仿真)      │
└─────────────────────┘
```

---

## 3. 各LLD文档说明

### LLD_01_PCIE_NVMe_EMULATION.md
- **字数**：约30,000字
- **内容**：
  - PCIe配置空间仿真
  - NVMe控制器寄存器仿真
  - 队列管理（SQ/CQ）
  - MSI-X中断
  - DMA引擎
  - 内核-用户空间通信
- **头文件**：pci.h, nvme.h, queue.h, msix.h, dma.h, shmem.h
- **函数**：80+个

### LLD_02_CONTROLLER_THREAD.md
- **字数**：约28,000字
- **内容**：
  - 共享内存Ring Buffer
  - 命令仲裁器
  - I/O调度器（FIFO/Greedy/Deadline）
  - Write Buffer管理
  - 读缓存（LRU）
  - Channel负载均衡
  - 资源管理器
  - 流量控制（令牌桶）
- **头文件**：controller.h, shmem_if.h, scheduler.h, write_buffer.h, read_cache.h, resource.h, flow_control.h
- **函数**：60+个

### LLD_03_MEDIA_THREADS.md
- **字数**：约25,000字
- **内容**：
  - NAND层次结构（Channel→Chip→Die→Plane→Block→Page）
  - 时序模型（tR/tPROG/tERS, TLC LSB/CSB/MSB）
  - EAT计算引擎
  - 并发控制（Multi-Plane/Die Interleaving/Chip Enable）
  - 可靠性模型
  - 坏块管理（BBT）
- **头文件**：nand.h, media.h, timing.h, reliability.h, bbt.h
- **函数**：40+个

### LLD_04_HAL.md
- **字数**：约22,000字
- **内容**：
  - NAND驱动API
  - NOR驱动API
  - 电源管理
  - 命令发射队列
- **头文件**：hal_nand.h, hal_nor.h, hal_pci.h, hal_power.h
- **函数**：30+个

### LLD_05_COMMON_SERVICE.md
- **字数**：约32,000字
- **内容**：
  - RTOS原语（Task/Message Queue/Semaphore/Mutex/Event Group/Timer/Memory Pool）
  - 任务调度器
  - 内存管理
  - Bootloader
  - 上下电服务
  - 带外管理
  - 核间通信
  - Watchdog
  - Debug/Log
- **头文件**：rtos.h, scheduler.h, memory.h, boot.h, power_mgmt.h, oob.h, ipc.h, watchdog.h, debug.h, log.h
- **函数**：70+个

### LLD_06_APPLICATION.md
- **字数**：约35,000字
- **内容**：
  - FTL地址映射（L2P/P2L）
  - PPN编码/解码
  - Block状态机
  - Current Write Block
  - 空闲块池
  - GC（Greedy/Cost-Benefit/FIFO）
  - 磨损均衡
  - Read Retry
  - ECC（LDPC）
  - 错误处理
- **头文件**：ftl.h, mapping.h, block.h, gc.h, wear_level.h, ecc.h, error.h
- **函数**：60+个

---

## 4. 使用指南

### 编码顺序

1. LLD_05：通用平台层（基础）
2. LLD_03：介质线程
3. LLD_04：HAL层
4. LLD_06：算法任务层
5. LLD_02：主控线程
6. LLD_01：PCIe/NVMe设备仿真

---

## 5. 后续工作

- 编码实现
- 单元测试
- 集成测试
- 性能测试

---

**文档统计**：
- 总LLD文档数：6个 + 1个总览
- 总字数：约172,000字
- 头文件数：30+个
- 函数接口数：340+个
