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

### LLD_07_OOB_MANAGEMENT.md
- **字数**：约10,000字
- **内容**：
  - Unix Domain Socket JSON-RPC 2.0服务器
  - NVMe SMART/Health Log Page（0x02）仿真
  - 性能计数器与延迟直方图
  - 温度模型与异常告警
  - 命令Trace环形缓冲区
  - /proc/hfsss/文件系统接口
  - hfsss-ctrl CLI工具
  - YAML配置文件加载
- **头文件**：`include/common/oob.h`
- **函数**：25+个
- **覆盖需求**：REQ-077至REQ-079，REQ-083，REQ-086，REQ-123至REQ-126

### LLD_08_FAULT_INJECTION.md
- **字数**：约12,000字
- **内容**：
  - NAND介质故障注入（坏块/读错误/写错误/擦除错误/位翻转/读扰动风暴/数据保持加速）
  - 电源故障注入（空闲/写入中/GC中/Checkpoint中掉电）
  - 控制器故障注入（Panic/内存池耗尽/命令超时风暴/L2P腐败）
  - 故障注册表（二分查找O(log N)热路径检查）
  - OOB fault.inject接口集成
  - WAL恢复与崩溃标记设计
- **头文件**：`include/common/fault_inject.h`
- **函数**：18+个
- **测试用例**：28个（FI-001至FI-028）
- **覆盖需求**：REQ-128至REQ-131

### LLD_09_BOOTLOADER.md
- **字数**：约11,000字
- **内容**：
  - 六阶段启动序列仿真（Phase 0-5，总耗时3-8秒）
  - NOR Flash双固件槽（Slot A/B）热备与原子切换
  - SysInfo分区设计（启动类型检测/clean-shutdown标记）
  - 正常掉电服务（七步有序关闭）
  - 异常掉电服务（信号处理+WAL尽力持久化）
  - 上电恢复路径（首次启动/正常恢复/异常恢复/降级模式）
- **头文件**：`include/common/boot.h`、`include/common/power_mgmt.h`
- **函数**：15+个
- **测试用例**：25个（BL-001至BL-025）
- **覆盖需求**：REQ-073至REQ-076

### LLD_10_PERFORMANCE_VALIDATION.md
- **字数**：约13,000字
- **内容**：
  - 内置基准测试引擎（顺序/随机/混合/Zipfian负载）
  - NAND时序精度验证（tR/tPROG/tERS，误差<5%）
  - 可扩展性测试（Amdahl并行效率评估）
  - 数据完整性验证（md5sum端到端校验）
  - 长时稳定性框架（72小时压力测试、TSan/ASan）
  - 验证报告生成（JSON + 人类可读文本）
- **头文件**：`include/common/perf_validator.h`
- **函数**：15+个
- **测试用例**：40个（PV-001至PV-040）
- **覆盖需求**：REQ-112至REQ-119，REQ-122，REQ-131至REQ-134

### LLD_11_FTL_RELIABILITY.md
- **字数**：约14,000字
- **内容**：
  - 命令状态机（8态，FTL_CMD_RECEIVED → FTL_CMD_COMPLETE）
  - Read Retry机制（最多15次电压偏移调整，软判决LDPC）
  - Write Retry/Write Verify（写后读验证，ECC检查）
  - 多级IO流量控制（主机层/FTL层/NAND层三级令牌桶）
  - RAID-Like数据保护（L2P双副本、BBT双镜像、Die级XOR奇偶校验）
  - NVMe错误状态码映射与错误日志页（Error Log Page）
- **头文件**：`include/ftl/ftl_reliability.h`
- **函数**：35+个
- **测试用例**：24个（FR-001至FR-024）
- **覆盖需求**：REQ-110至REQ-115

### LLD_12_REALTIME_SERVICES.md
- **字数**：约12,000字
- **内容**：
  - CPU亲和性绑定（pthread_setaffinity_np，macOS降级处理）
  - 实时调度优先级（SCHED_FIFO/SCHED_RR，macOS降级处理）
  - SPSC Ring Buffer IPC（缓存行对齐_Atomic uint64_t，容量4096，eventfd唤醒）
  - 系统资源监控（CPU使用率、内存压力、线程状态周期采样）
  - 性能异常检测与温度仿真（P99.9延迟告警、热节流≥75°C）
- **头文件**：`include/common/rt_services.h`
- **函数**：30+个
- **测试用例**：25个（RT-001至RT-025）
- **覆盖需求**：REQ-074，REQ-075，REQ-085，REQ-087，REQ-088

### LLD_13_HAL_ADVANCED.md
- **字数**：约10,000字
- **内容**：
  - NVMe异步事件请求（AER）管理（16个Pending队列，epoll驱动投递）
  - PCIe链路状态机（L0/L0s/L1/L2/RESET/FLR六态）
  - PCIe配置空间完整仿真（256字节标准空间 + 4KB扩展空间）
- **头文件**：`include/hal/hal_advanced.h`
- **函数**：20+个
- **测试用例**：24个（HA-001至HA-024）
- **覆盖需求**：REQ-063，REQ-064，REQ-069

### LLD_14_NOR_FLASH.md
- **字数**：约11,000字
- **内容**：
  - NOR Flash规格（256MB，512B页，64KB扇区，PE寿命100,000次）
  - 7分区布局（Bootloader/SlotA/SlotB/Config/BBT/EventLog/SysInfo）
  - AND语义编程（image[i] &= buf[i]，只能清零）
  - 完整操作命令集（读/编程/扇区擦除/片擦除/状态寄存器读写）
  - mmap持久化（MAP_SHARED文件映射，掉电安全）
- **头文件**：`include/media/nor_flash.h`，`include/hal/hal_nor_full.h`
- **函数**：20+个
- **测试用例**：22个（NF-001至NF-022）
- **覆盖需求**：REQ-053至REQ-056

### LLD_15_PERSISTENCE_FORMAT.md
- **字数**：约10,000字
- **内容**：
  - NAND数据文件格式（80字节头 + 数据区，Magic "HFSSS_ND"，CRC32校验）
  - OOB区域格式（每页384字节：LPN/时间戳/ECC综合征/OOB_CRC32）
  - L2P Checkpoint格式（64字节头 + uint64_t[]数组，CRC64完整性）
  - WAL记录格式（64字节定长，0xDEADBEEF终止符，8种记录类型）
  - static_assert大小检查，`persistence_fmt.h`集中管理所有格式常量
- **头文件**：`include/common/persistence_fmt.h`
- **函数**：20+个
- **测试用例**：22个（PF-001至PF-022）
- **覆盖需求**：REQ-131（补充LLD_07持久化格式细节）

---

## 4. 使用指南

### 编码顺序

1. LLD_05：通用平台层（基础）
2. LLD_12：实时服务（CPU亲和性/IPC/资源监控，依赖LLD_05）
3. LLD_09：Bootloader与上下电服务（依赖LLD_05）
4. LLD_14：NOR Flash完整实现（依赖LLD_03/LLD_04基础）
5. LLD_03：介质线程
6. LLD_04：HAL层
7. LLD_13：HAL高级特性（AER/PCIe链路，依赖LLD_04）
8. LLD_15：持久化数据格式（依赖LLD_03/LLD_06）
9. LLD_06：算法任务层
10. LLD_11：FTL可靠性（Read/Write Retry/RAID保护，依赖LLD_06）
11. LLD_02：主控线程
12. LLD_01：PCIe/NVMe设备仿真
13. LLD_07：带外管理（依赖LLD_01至LLD_06）
14. LLD_08：故障注入框架（依赖LLD_03/LLD_06/LLD_07）
15. LLD_10：性能验证（最后集成，依赖全栈）

---

## 5. 后续工作

- 编码实现（按上述编码顺序进行）
- 单元测试（各模块独立测试）
- 集成测试（端到端流程验证）
- 性能测试（LLD_10验证框架）
- 故障注入测试（LLD_08框架）

---

**文档统计**：
- 总LLD文档数：15个 + 1个总览
- 总字数：约295,000字
- 头文件数：45+个
- 函数接口数：550+个
- 总测试用例（设计态）：230个新增（OOB 16 + 故障注入 28 + Bootloader 25 + 性能验证 40 + FTL可靠性 24 + 实时服务 25 + HAL高级 24 + NOR Flash 22 + 持久化格式 22 + 其他共230）
