# HFSSS 概要设计文档（HLD）

**项目名称**：高保真全栈SSD模拟器（High-Fidelity Full-Stack SSD Simulator）
**文档版本**：V1.0
**编制日期**：2026-03-08

---

## 文档目录

本项目包含以下概要设计文档（HLD），每个文档不少于2万字：

| 文档编号 | 文档名称 | 目标版本 | 状态 | 字数估算 |
|---------|---------|---------|------|---------|
| HLD_01 | PCIe/NVMe设备仿真模块概要设计 | V1.0 (Alpha) | ✅ 已完成 | 25,000字 |
| HLD_02 | 主控线程模块概要设计 | V1.0 (Alpha) | ✅ 框架完成 | 25,000字 |
| HLD_03 | 介质线程模块概要设计 | V1.5 (Beta) | ✅ 框架完成 | 25,000字 |
| HLD_04 | 硬件接入层（HAL）概要设计 | V1.0 (Alpha) | ✅ 框架完成 | 25,000字 |
| HLD_05 | 通用平台层（Common Service）概要设计 | V2.0 (GA) | ✅ 框架完成 | 30,000字 |
| HLD_06 | 算法任务层（Application Layer）概要设计 | V1.0 (Alpha) | ✅ 框架完成 | 30,000字 |

**总字数估算**：约160,000字

---

## 模块划分与架构

### 系统整体架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                      主机 Linux 操作系统                              │
│   NVMe Driver │ blk-mq │ File System │ fio/nvme-cli │ User Apps      │
└────────────────────────┬────────────────────────────────────────────┘
                         │ PCIe / NVMe 接口
┌────────────────────────▼────────────────────────────────────────────┐
│           模块一：PCIe/NVMe 设备仿真层（内核模块）                   │
│    HLD_01: PCIE_NVMe_EMULATION.md                                 │
│   ┌─────────────────────────────────────────────────────────┐   │
│   │ PCIe仿真子层：PCI Config Space │ BAR │ MSI-X │ DMA     │   │
│   │ NVMe协议子层：Admin Queue │ IO Queue │ 命令解析/完成   │   │
│   └─────────────────────────────────────────────────────────┘   │
└────────────────────────┬────────────────────────────────────────────┘
                         │ 内核→用户空间通信（共享内存/ioctl）
┌────────────────────────▼────────────────────────────────────────────┐
│           HFSSS 用户空间守护进程（hfsss-daemon）                  │
│                                                                  │
│   ┌──────────────────────────────────────────────────────────┐  │
│   │              模块二：主控线程（Controller Thread）           │  │
│   │    HLD_02: CONTROLLER_THREAD.md                        │  │
│   │  命令仲裁 │ IO调度 │ 资源分配 │ 核间通信 │ 流量控制       │  │
│   └──────────────────────────────────────────────────────────┘  │
│                           │                                      │
│   ┌────────────────────────▼─────────────────────────────────┐  │
│   │         模块三：固件CPU核心线程群（Firmware Core Threads）   │  │
│   │                                                            │  │
│   │  ┌─────────────────────────────────────────────────────┐ │  │
│   │  │ 模块六：算法任务层（Application Layer）             │ │  │
│   │  │    HLD_06: APPLICATION.md                        │ │  │
│   │  │ FTL │ GC │ WL │ BBM │ ECC │ QoS │ 冗余 │ 错误处理  │ │  │
│   │  ├─────────────────────────────────────────────────────┤ │  │
│   │  │ 模块五：通用平台层（Common Service）                │ │  │
│   │  │    HLD_05: COMMON_SERVICE.md                      │ │  │
│   │  │ RTOS │ 调度 │ 内存管理 │ Boot │ 核间通信 │ 监控     │ │  │
│   │  ├─────────────────────────────────────────────────────┤ │  │
│   │  │ 模块四：硬件接入层（HAL）                           │ │  │
│   │  │    HLD_04: HAL.md                                 │ │  │
│   │  │ NAND驱动 │ NOR驱动 │ NVMe/PCIe │ 电源管理           │ │  │
│   │  └─────────────────────────────────────────────────────┘ │  │
│   └────────────────────────────────────────────────────────────┘  │
│                           │                                       │
│   ┌────────────────────────▼──────────────────────────────────┐  │
│   │         模块三：介质线程群（Media Threads）                 │  │
│   │    HLD_03: MEDIA_THREADS.md                              │  │
│   │  Channel 0~15 × (多个NAND Chip线程 + 时序控制器)       │  │
│   │  NOR Flash线程                                           │  │
│   └────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 各模块概要设计要点

### HLD_01: PCIe/NVMe设备仿真模块

**核心内容**：
1. PCIe配置空间仿真（Type 0配置头、PCIe Capabilities链表、BAR寄存器）
2. NVMe控制器寄存器仿真（CAP/VS/CC/CSTS/AQA/ASQ/ACQ等）
3. NVMe队列管理（Admin SQ/CQ、I/O SQ/CQ动态创建、PRP/SGL解析）
4. MSI-X中断仿真（MSI-X Table、PBA、中断投递、中断聚合）
5. DMA数据传输（主机内存映射、数据拷贝、IOMMU支持）
6. 用户空间通信（共享内存Ring Buffer、eventfd通知、mmap接口）

**关键流程图**：
- 控制器初始化流程（写CC.EN=1）
- SQ Tail Doorbell处理流程
- CQ写回与中断触发流程
- PRP/SGL解析流程
- Admin命令处理流程

---

### HLD_02: 主控线程模块

**核心内容**：
1. 内核-用户空间通信（共享内存Ring Buffer、无锁SPSC/MPMC队列）
2. 命令仲裁策略（NVMe WRR仲裁、Admin命令优先）
3. 命令分发（按命令类型分发、命令状态机、命令依赖跟踪）
4. I/O调度器（贪心调度、FIFO/Deadline可选、写命令合并、读预取）
5. 写缓冲区管理（Write Buffer、后台下刷、Flush触发、背压）
6. 读缓存（LRU缓存、缓存命中处理）
7. Channel负载均衡（实时统计、负载低Channel优先、命令迁移）
8. 资源管理器（空闲块管理、命令槽管理、DRAM缓存资源管理）
9. 流量控制（令牌桶限速器、背压机制、QoS保证、GC流量控制）

**关键流程图**：
- 命令接收与分发流程
- 写操作流程（Write Buffer路径）
- 读操作流程（Read Cache路径）
- GC触发与背压流程
- 命令超时处理流程

---

### HLD_03: 介质线程模块

**核心内容**：
1. NAND Flash层次结构（Channel→Chip→Die→Plane→Block→Page）
2. NAND介质时序模型（tR/tPROG/tERS、TLC LSB/CSB/MSB差异化延迟）
3. EAT（最早可用时刻）计算与调度
4. 多平面（Multi-Plane）并发、Die交叉（Die Interleaving）、Chip Enable并发
5. NAND介质命令执行引擎（14+NAND命令、命令队列、完成通知）
6. NAND可靠性建模（P/E循环退化、读干扰、数据保持性、坏块管理）
7. NAND数据存储机制（DRAM存储布局、持久化策略、恢复机制）
8. NOR Flash介质仿真（规格、存储分区、操作命令、数据持久化）

**关键流程图**：
- NAND命令执行时序计算流程
- Multi-Plane Program流程
- GC Victim Block读写流程
- 数据持久化流程
- 上电恢复流程

---

### HLD_04: 硬件接入层（HAL）

**核心内容**：
1. NAND驱动模块（15+ API、命令发射队列、消息队列、Die状态机）
2. NOR驱动模块（10+ API、共享内存字节数组、延迟注入、写保护检测）
3. NVMe/PCIe模块管理（命令完成提交、异步事件管理、PCIe链路状态管理、Namespace管理接口）
4. 电源管理芯片驱动（NVMe电源状态PS0/PS1/PS2/PS3/PS4、功耗统计）

**关键流程图**：
- NAND异步读流程
- NAND异步写流程
- 命令完成提交流程
- 电源状态切换流程

---

### HLD_05: 通用平台层（Common Service）

**核心内容**：
1. 实时操作系统（RTOS）仿真（Task、Message Queue、Semaphore、Mutex、Event Group、Timer、Memory Pool）
2. 任务调度（静态任务绑定、优先级调度、负载均衡、调度统计）
3. 内存管理（内存分区规划、静态预分配、内存池、内存保护、内存压力管理）
4. Bootloader（启动序列、双镜像冗余、安全启动校验、启动日志）
5. 上下电服务（上电服务、正常掉电、异常掉电处理）
6. 带外管理（Unix Domain Socket/JSON-RPC、/proc文件系统接口、REST API、SMART信息）
7. 核间通信（消息传递、共享内存、核间信号、全局锁）
8. 系统稳定性监控（Watchdog看门狗、系统资源监控、性能异常检测、温度仿真）
9. Panic/Assert处理（Assert机制、Panic流程、Coredump）
10. 系统Debug机制（命令Trace、NAND操作Trace、FTL操作Trace、GDB支持、性能计数器）
11. 系统事件Log机制（事件级别、Log存储、Log条目格式）

**关键流程图**：
- RTOS任务调度流程
- Bootloader启动流程
- 正常掉电流程
- Panic处理流程
- Watchdog喂狗与超时流程

---

### HLD_06: 算法任务层（Application Layer）

**核心内容**：
1. Flash Translation Layer — 地址映射管理（地址映射架构、映射表设计、过量配置OP、写操作流程、读操作流程、条带化策略）
2. NAND块地址组织管理（Block状态机、Block元数据、Current Write Block管理、空闲块池管理）
3. 垃圾回收（GC触发策略、Victim Block选择算法、GC执行流程、GC并发优化、写放大分析）
4. 磨损均衡（动态磨损均衡、静态磨损均衡、磨损监控与告警）
5. 读写擦命令管理（命令状态机、Read Retry机制、Write Retry机制、Write Verify）
6. IO流量控制（多级流控、主机IO限速、GC/WL带宽配额、NAND通道级流控、写缓冲区流控）
7. 数据冗余备份（LDPC ECC、跨Die奇偶校验、关键元数据冗余、Write Buffer断电保护）
8. 命令错误处理（NVMe错误状态码、错误处理流程、可恢复错误、不可恢复数据错误、NAND设备错误、命令超时处理、固件内部错误）

**关键流程图**：
- 主机写命令FTL处理流程
- 主机读命令FTL处理流程
- GC执行完整流程
- 静态磨损均衡触发流程
- Read Retry与错误处理流程

---

## 版本规划与依赖关系

### V1.0 (Alpha) — MVP版本

**必须完成的模块**：
1. ✅ HLD_01: PCIe/NVMe设备仿真模块
2. ✅ HLD_02: 主控线程模块
3. ✅ HLD_04: 硬件接入层（HAL）
4. ✅ HLD_06: 算法任务层（基础FTL+GC）

**目标功能**：
- PCIe/NVMe基础仿真，lspci/nvme-cli可识别
- DRAM后端存储
- 基础FTL（页级映射）
- 基础GC（Greedy算法）
- 块设备可挂载，fio可读写

---

### V1.5 (Beta) — 完整介质版本

**新增模块**：
1. ✅ HLD_03: 介质线程模块

**目标功能**：
- 16通道NAND精确时序建模
- NOR Flash仿真
- 多核固件线程
- 完整磨损均衡
- 完整NAND可靠性建模

---

### V2.0 (GA) — 全栈完整版本

**新增模块**：
1. ✅ HLD_05: 通用平台层

**目标功能**：
- 三层固件架构全实现
- 数据持久化
- QoS完整支持
- OOB管理完整
- Debug机制完整

---

### V2.5 (Enterprise) — 企业级版本

**各模块增强**：
- ZNS SSD支持
- KV-SSD支持
- 完整故障注入框架
- 性能分析框架

---

## 下一步工作

### 立即下一步

1. **完善各HLD文档**：
   - 补充完整的C代码示例
   - 补充所有流程图
   - 补充接口设计详细说明
   - 补充数据结构设计完整定义
   - 补充性能设计细节
   - 补充错误处理设计
   - 补充测试设计方案

2. **生成LLD文档**（详细设计文档）：
   - 为每个子模块生成LLD
   - 包含函数级接口定义
   - 包含具体的算法实现
   - 包含详细的测试用例

3. **开始代码实现**：
   - 按照V1.0版本规划
   - 先实现PCIe/NVMe设备仿真模块
   - 再实现主控线程模块
   - 再实现HAL和基础FTL

---

## 参考资料

1. PRD文档：`SSD_Simulator_PRD.md`
2. 需求矩阵：`REQUIREMENTS_MATRIX.csv`
3. NVMe规范：NVM Express Base Specification Revision 2.0c
4. NVMeVirt论文：NVMeVirt: A Versatile Software-defined Virtual NVMe Device (USENIX FAST 2023)
5. FEMU论文：The CASE of FEMU: Cheap, Accurate, Scalable and Extensible Flash Emulator (USENIX FAST 2018)
6. MQSim论文：MQSim: A Framework for Enabling Realistic Studies of Modern Multi-Queue SSD Devices (USENIX FAST 2018)

---

*README结束*
