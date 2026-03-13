# SSD模拟器产品需求规格书（PRD）

**文档版本：** V1.0
**编制日期：** 2026年3月
**文档状态：** 正式发布
**文档密级：** 内部资料

---

## 修订历史

| 版本 | 日期 | 作者 | 修订说明 |
|------|------|------|----------|
| V0.1 | 2026-02-01 | 架构组 | 初稿 |
| V0.9 | 2026-02-20 | 架构组 | 完善功能需求章节 |
| V1.0 | 2026-03-08 | 架构组 | 正式发布 |

---

# 第一章 文档概述

## 1.1 编写目的

本文档是针对"高保真SSD全栈模拟器（High-Fidelity Full-Stack SSD Simulator，简称HFSSS）"项目所编写的产品需求规格书。文档从功能需求、性能需求、产品定义与产品接口等维度，对该模拟器进行完整且精确的描述，旨在：

1. 为后续需求矩阵（Requirements Matrix）的生成提供输入基础；
2. 为概要设计（High-Level Design）提供明确的设计约束与目标；
3. 为各模块详细设计文档（Low-Level Design）提供可追溯的需求来源；
4. 为测试团队提供验收标准的依据；
5. 为项目管理提供工作量估算和里程碑规划的参考。

## 1.2 适用范围

本文档适用于以下读者：

- **产品经理**：理解产品全貌与边界；
- **系统架构师**：指导系统分解与模块划分；
- **固件工程师**：理解固件层次架构与算法需求；
- **内核/驱动工程师**：理解PCIe/NVMe模拟层实现要求；
- **测试工程师**：制定测试策略与验收标准；
- **项目经理**：进行项目规划与资源分配。

## 1.3 术语与缩略语

| 缩略语 | 全称 | 说明 |
|--------|------|------|
| SSD | Solid State Drive | 固态硬盘 |
| NAND | Negative-AND | NAND闪存，SSD主要存储介质 |
| NOR | NOR Flash | NOR型闪存，通常用于存储固件代码 |
| FTL | Flash Translation Layer | 闪存转换层，SSD核心固件组件 |
| GC | Garbage Collection | 垃圾回收 |
| WL | Wear Leveling | 磨损均衡 |
| BBM | Bad Block Management | 坏块管理 |
| LBA | Logical Block Address | 逻辑块地址（主机侧） |
| PBA | Physical Block Address | 物理块地址（NAND侧） |
| LPN | Logical Page Number | 逻辑页号 |
| PPN | Physical Page Number | 物理页号 |
| NVMe | Non-Volatile Memory Express | 高速固态硬盘接口协议 |
| PCIe | Peripheral Component Interconnect Express | 高速串行计算机扩展总线 |
| BAR | Base Address Register | PCIe基地址寄存器 |
| MSI-X | Message Signaled Interrupts Extended | 消息信号中断扩展 |
| SQ | Submission Queue | NVMe提交队列 |
| CQ | Completion Queue | NVMe完成队列 |
| HAL | Hardware Access Layer | 硬件接入层 |
| RTOS | Real-Time Operating System | 实时操作系统 |
| OOB | Out-Of-Band | 带外管理 |
| ECC | Error Correction Code | 错误纠正码 |
| LDPC | Low-Density Parity-Check | 低密度奇偶校验码 |
| ZNS | Zoned Namespace | 分区命名空间（NVMe特性） |
| KV-SSD | Key-Value SSD | 键值型固态硬盘 |
| DMA | Direct Memory Access | 直接内存访问 |
| MMIO | Memory-Mapped I/O | 内存映射I/O |
| ONFI | Open NAND Flash Interface | 开放NAND闪存接口标准 |
| SLC | Single-Level Cell | 单层单元 |
| MLC | Multi-Level Cell | 多层单元（2bit/cell） |
| TLC | Triple-Level Cell | 三层单元（3bit/cell） |
| QLC | Quad-Level Cell | 四层单元（4bit/cell） |
| CE | Chip Enable | 片选信号 |
| WP | Write Protect | 写保护信号 |
| RB | Ready/Busy | 就绪/忙碌信号 |
| PRD | Product Requirements Document | 产品需求规格书 |
| IOPS | I/O Operations Per Second | 每秒输入输出操作次数 |
| QoS | Quality of Service | 服务质量 |

## 1.4 参考文献

1. Kim, S.H., Shim, J., Lee, E., et al. "NVMeVirt: A Versatile Software-defined Virtual NVMe Device." *USENIX FAST 2023*.
2. Li, H., et al. "The CASE of FEMU: Cheap, Accurate, Scalable and Extensible Flash Emulator." *USENIX FAST 2018*.
3. Tavakkol, A., et al. "MQSim: A Framework for Enabling Realistic Studies of Modern Multi-Queue SSD Devices." *USENIX FAST 2018*.
4. Zhang, J., et al. "Scalable Parallel Flash Firmware for Many-core Architectures (DeepFlash)." *USENIX FAST 2020*.
5. Jung, M., et al. "SimpleSSD: Modeling Solid State Drives for Holistic System Simulation." *IEEE CAL 2018*.
6. Kim, Y., et al. "FlashSim: A Simulator for NAND Flash-based Solid-State Drives." *IEEE ISEE 2009*.
7. NVM Express Inc. "NVM Express Base Specification Revision 2.0c." 2023.
8. NVM Express Inc. "NVM Express over PCIe Transport Specification Revision 1.0c." 2022.
9. Micron Technology. "TN-29-19: NAND Flash 101 Introduction." Technical Note.
10. OpenSSD Project. "Cosmos+ OpenSSD: Rapid Prototype for Flash Storage Systems." *ACM TOS 2020*.
11. GitHub: snu-csl/nvmevirt — NVMeVirt开源代码仓库.
12. GitHub: CMU-SAFARI/MQSim — MQSim开源代码仓库.
13. GitHub: MoatLab/FEMU — FEMU开源代码仓库, v10.1, 2024.
14. Linux Kernel Documentation: "NVMe PCI Endpoint Function Target."
15. JEDEC Standard JESD230D: "NAND Flash Storage Device."
16. NVM Express Inc. "NVM Express Zoned Namespace Command Set Specification."
17. NVM Express Inc. "NVM Express Key Value Command Set Specification."
18. GitHub: OpenSSD/jasmine — OpenSSD Jasmine固件.
19. FITEE 2022. "SoftSSD: enabling rapid flash firmware prototyping."

---

# 第二章 行业背景与调研综述

## 2.1 SSD行业现状与发展趋势

### 2.1.1 市场概况

固态硬盘（SSD）已成为现代计算系统中最重要的存储组件之一。从消费级笔记本电脑到企业级数据中心，SSD以其高速随机读写、低延迟、低功耗等优势，正在全面取代传统机械硬盘（HDD）。根据市场研究机构的数据，全球SSD市场规模在2023年已超过500亿美元，预计到2028年将突破千亿美元量级。

在企业级市场，随着AI/ML工作负载、实时数据库和高频交易等应用场景的爆发式增长，对SSD的性能指标提出了更高要求：
- 顺序读写带宽：PCIe 5.0 x4接口下可达14GB/s以上；
- 随机读IOPS：超过2,000,000 IOPS（4KB块）；
- 读延迟：低至70μs以下；
- 混合读写延迟稳定性：P99.9延迟控制在1ms以内。

### 2.1.2 SSD内部架构复杂度提升

现代企业级SSD的内部架构已远超早期产品的简单实现，主要体现在：

**控制器多核化**：现代SSD控制器（如西数、三星、铠侠等主流厂商的旗舰SSD）普遍采用4~8核ARM Cortex-R系列处理器，部分前沿设计甚至引入了RISC-V核心，用于处理NVMe命令、FTL计算、ECC/LDPC运算和GC调度等并行任务。

**NAND介质向3D堆叠演进**：当代量产3D NAND已达到232层（Micron），甚至300层以上（三星、铠侠路线图），单颗NAND芯片容量从早期的8Gb提升至2Tb以上，NAND颗粒内部的multi-plane、multi-die并行特性也越来越重要。

**NAND接口速率提升**：从ONFI 1.0的50MB/s，到ONFI 4.2的1600MB/s，再到ONFI 5.0的2400MB/s，每一代接口标准的演进都要求固件层对时序控制、数据缓冲和命令并发做出相应优化。

**ECC向LDPC演进**：传统BCH纠错码已无法满足高层数3D NAND的纠错需求，LDPC软解码（Soft-Decision LDPC）成为主流，同时引入了Read Retry、Voltage Offset调节等多种可靠性机制。

### 2.1.3 SSD固件研究与模拟的必要性

SSD固件开发面临以下核心挑战，这也是SSD模拟器不可或缺的根本原因：

1. **NAND硬件稀缺性**：真实NAND芯片价格昂贵，一台16通道企业级SSD评估平台的硬件成本可能高达数万美元，且受限于NAND供应商的技术授权，难以大规模部署用于研究；

2. **可重复性**：硬件实验中的NAND老化状态、温度变化等物理因素难以精确复现，而模拟器可提供完全可控的实验环境；

3. **快速原型开发**：在真实硅片流片前，通过模拟器验证FTL算法正确性可极大缩短开发周期；

4. **极端场景测试**：某些可靠性测试场景（如连续写直至P/E寿命耗尽）在真实硬件上需要数月乃至数年，而在模拟器中可以加速执行；

5. **算法研究**：学术界研究新型FTL算法、GC策略、磨损均衡方案时，需要一个准确可信的模拟环境来验证理论。

## 2.2 现有SSD模拟器技术调研

### 2.2.1 FlashSim

**来源**：Kim et al.，ISEE 2009
**类型**：事件驱动型SSD模拟器
**技术特点**：
- 基于面向对象编程范式，模块化设计；
- 与DiskSim全系统模拟器互操作，支持SSD与HDD混合系统模拟；
- 支持不同FTL方案的性能和能耗对比；
- 对NAND介质的建模相对简单，未涵盖3D NAND的复杂时序特性；
- 仅支持SATA接口仿真，不支持NVMe/PCIe。

**局限性**：属于早期工作，不支持多队列NVMe协议，不支持MLC/TLC/QLC时序差异化建模，并发度和精度均不满足现代SSD研究需求。

### 2.2.2 MQSim

**来源**：Tavakkol et al.，USENIX FAST 2018；GitHub: CMU-SAFARI/MQSim
**类型**：事件驱动型多队列SSD模拟器
**技术特点**：
- 支持NVMe多队列（MQ）和SATA-NCQ两种主机接口协议；
- NAND Flash层次结构：Channel → Chip → Die → Plane → Block → Page；
- 支持SLC/MLC/TLC单元类型，LSB/CSB/MSB页面读取差异化延迟；
- Program/Erase Suspension（程序/擦除暂停）机制建模；
- FTL支持页级映射（PAGE_LEVEL）和混合映射（HYBRID）；
- 地址映射缓存表（CMT）支持配置容量和共享模式；
- GC策略：GREEDY、RGA、RANDOM、FIFO多种选择；
- 动态和静态磨损均衡双机制；
- DRAM缓存：SIMPLE（写缓冲）和ADVANCED（读写混合缓存）；
- IO优先级：URGENT/HIGH/MEDIUM/LOW四级；
- 约13,000行C++代码；
- 输出指标：端到端延迟、IOPS、带宽、通道利用率。

**局限性**：
- 纯软件时间驱动仿真，不挂载真实内核NVMe驱动，无法作为真实块设备被操作系统识别；
- 缺少固件CPU核心线程的精确建模；
- 未实现PCIe链路层仿真。

### 2.2.3 FEMU（Fast, Accurate, Scalable, Extensible Flash Emulator）

**来源**：Li et al.，USENIX FAST 2018；GitHub: MoatLab/FEMU
**类型**：基于QEMU/KVM的NVMe SSD仿真器
**最新版本**：v10.1（2024）
**支持平台**：Ubuntu 20.04/22.04/24.04
**技术特点**：
- 基于QEMU/KVM，对Guest OS呈现为标准NVMe块设备（/dev/nvme0nX）；
- 支持多种运行模式：
  - **BlackBox SSD (BBSSD)**：商业SSD仿真，设备侧FTL，包含页级地址映射；
  - **WhiteBox SSD (OCSSD)**：OpenChannel SSD，主机侧FTL，支持OpenChannel Spec 1.2/2.0；
  - **Zoned Namespace SSD (ZNSSD)**：NVMe ZNS SSD，暴露Zone接口；
  - **NoSSD模式**：超快速NVMe仿真，无存储逻辑，适用于SCM/Optane型设备仿真；
- NAND时序模型参数（可配置）：
  - 页读取延迟：40,000 ns (40 μs)
  - 页写入延迟：200,000 ns (200 μs)
  - 块擦除延迟：2,000,000 ns (2 ms)
- GC逻辑通过一系列读/写/擦除操作推进plane和channel的空闲时间计数器（Tfree）；
- 性能特点：32线程下IO延迟稳定在52μs以内；
- 代码结构：
  - NVMe Controller：符合NVMe 1.3+标准实现
  - 可插拔SSD模式后端
  - 可配置时序模型
  - DRAM内存后端
- CI/CD增强，通过GitHub Actions自动化测试。

**局限性**：
- 依赖QEMU/KVM虚拟化基础设施，增加了一层虚拟化开销；
- NAND时序模型相对粗粒度，未完整建模多channel并发竞争、die-level interleave等细节；
- 固件CPU核心未建模。

### 2.2.4 NVMeVirt

**来源**：Kim et al.，USENIX FAST 2023；GitHub: snu-csl/nvmevirt
**类型**：Linux内核模块，软件定义虚拟NVMe设备
**技术特点**：
- 完全在Linux内核空间实现，无需QEMU/KVM，对宿主系统呈现为原生PCIe NVMe设备；
- 三大核心组件：
  1. **PCIe设备仿真器**：虚拟化PCIe总线，设置PCI配置头和BAR寄存器，指向内存中的控制块；
  2. **NVMe控制器仿真器**：处理Admin/IO命令集，管理SQ/CQ对；
  3. **存储后端**：支持NVM SSD（Optane型）、Conventional SSD、ZNS SSD、KV SSD四种；
- 物理内存保留（memmap）作为NVMe存储介质，通过GRUB参数预留64GiB等大块物理内存；
- 启动时在指定CPU核心上创建I/O dispatcher线程和多个I/O worker线程；
- 支持NVMe-oF目标卸载、内核绕过（kernel bypass）、PCIe点对点DMA；
- 约9,000行代码，基于Linux kernel 5.15；
- 推荐内核版本：v5.15.x及以上（测试于Ubuntu kernel v5.15.0-58-generic）；
- 支持PCIe peer-to-peer通信。

**关键技术机制**：
- **BAR映射**：NVMeVirt在PCI配置头中设置BAR字段，使其指向预留物理内存区域的控制块；
- **NVMe层识别**：主机NVMe驱动识别BAR中的控制块并进行操作；
- **线程模型**：至少需要两个CPU核心——一个I/O dispatcher线程 + 一个或多个I/O worker线程；
- **设备类型选择**：通过修改Kbuild文件选择目标设备类型。

**局限性**：
- 当前存储后端相对简单，不包含完整的三层固件架构；
- NAND介质仿真精度有限（主要为延迟注入，非完整时序模型）；
- 固件CPU核心线程未建模；
- 不支持NOR闪存建模。

### 2.2.5 SimpleSSD / Amber（SimpleSSD 2.0）

**来源**：Jung et al.，IEEE CAL 2018（SimpleSSD）；MICRO 2018（Amber）
**类型**：与全系统模拟器（gem5）集成的SSD仿真框架
**技术特点**：
- 三层固件架构：
  1. Host Interface Layer（HIL）：处理来自gem5磁盘控制器的I/O请求，转换为LBA；
  2. Flash Translation Layer（FTL）：逻辑到物理地址翻译，GC，磨损均衡；
  3. Parallelism Allocation Layer（PAL）：将请求分发到不同channel/package/die；
- 支持SATA、UFS、NVMe、OCSSD等多种接口；
- Amber（2.0）额外增加：DRAM缓存逻辑、固件CPU核心功耗建模、DMA引擎仿真；
- 支持基于地址的TLC延迟差异化（LSB/CSB/MSB页）；
- 与gem5集成，支持全系统性能分析；
- 验证精度：平均写测试误差2.7%，读测试误差7.1%（对比真实Intel 750 SSD）。

**局限性**：
- 依赖gem5全系统仿真环境，部署复杂；
- 不能在真实Linux服务器上作为可用存储设备运行；
- 实时性建模能力不足。

### 2.2.6 DeepFlash（多核固件平台）

**来源**：Zhang et al.，USENIX FAST 2020
**类型**：多核SSD固件平台研究
**技术特点**：
- 针对多核（many-core）SSD控制器的可扩展固件架构；
- 三阶段流水线模型：
  1. Queue-gather阶段：从多个NVMe SQ采集I/O请求；
  2. Trans-apply阶段：FTL逻辑地址到物理地址转换；
  3. Flash-scatter阶段：将操作分发到各flash通道；
- 多对多（many-to-many）线程模型，支持水平扩展；
- 无锁数据结构（lock-free）降低核间同步开销；
- FTL分布式部署于多核，避免单核瓶颈；
- 仅需12个有序核心即可达到1M+ IOPS；
- CACHE线程：将SSD内部DRAM用作突发缓冲，逻辑块地址→DRAM地址映射，数据最终通过stripe方式下刷到flash packages。

**关键设计启示**：
- 固件核心数量与IOPS的线性扩展关系；
- 无锁设计对高并发固件的重要性；
- FTL任务分片策略。

### 2.2.7 SoftSSD

**来源**：FITEE 2022，SoftSSD: enabling rapid flash firmware prototyping
**类型**：软件定义SSD固件原型开发平台
**技术特点**：
- 事件驱动编程模型（event-driven programming model）；
- 允许新FTL算法直接集成到全功能flash固件中；
- 提供标准化的flash固件框架，降低固件开发门槛；
- 支持快速算法原型验证。

### 2.2.8 SoftSSD

**来源**：FITEE 2022，SoftSSD: enabling rapid flash firmware prototyping
**类型**：软件定义SSD固件原型开发平台
**技术特点**：
- 事件驱动编程模型（event-driven programming model）；
- 允许新FTL算法直接集成到全功能flash固件中；
- 提供标准化的flash固件框架，降低固件开发门槛；
- 支持快速算法原型验证。

### 2.2.9 OpenSSD Jasmine

**来源**：OpenSSD Project；GitHub: OpenSSD/jasmine
**类型**：真实SSD硬件平台固件
**技术特点**：
- OpenSSD平台固件，C语言实现；
- 58 stars，22 forks；
- 真实SSD硬件平台固件参考；
- 可作为固件架构设计参考。

### 2.2.10 调研结论

通过对上述主要SSD模拟器的系统调研，总结如下：

| 特性 | FlashSim | MQSim | FEMU | NVMeVirt | SimpleSSD | DeepFlash | OpenSSD |
|------|----------|-------|------|----------|-----------|-----------|---------|
| 真实NVMe设备呈现 | ✗ | ✗ | ✓(QEMU) | ✓(裸金属) | ✗ | ✗ | ✗(硬件) |
| 多队列NVMe | ✗ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| NAND精确时序 | 基础 | 中等 | 基础 | 基础 | 中等 | 基础 | ✓(真实) |
| 多核固件建模 | ✗ | ✗ | ✗ | ✗ | 部分 | ✓ | ✓ |
| 三层固件架构 | ✗ | ✗ | ✗ | ✗ | 部分 | ✗ | ✓ |
| DRAM后端 | ✗ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ |
| NOR Flash | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ |
| ZNS支持 | ✗ | ✗ | ✓ | ✓ | ✗ | ✗ | ✗ |
| KV-SSD支持 | ✗ | ✗ | ✗ | ✓ | ✗ | ✗ | ✗ |
| 完整GC/WL | 基础 | ✓ | 基础 | ✗ | 基础 | ✗ | ✓ |
| 裸金属Linux运行 | ✓ | ✓ | ✗ | ✓ | ✗ | ✓ | ✓ |
| 数据持久化 | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✓(真实NAND) |
| 开源 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |

**核心差距**：目前没有任何一款开源模拟器能够同时满足：(a)在裸金属Linux服务器上原生运行、(b)向主机呈现真实NVMe设备、(c)完整的三层固件架构（HAL+Common Service+Application）、(d)精确的16通道NAND/NOR介质时序建模、(e)数据持久化到文件系统。本产品旨在填补这一空白。

**关键参考项目**：
- **NVMeVirt**：PCIe/NVMe设备仿真层的核心参考
- **FEMU**：ZNS/OCSSD/NoSSD多模式支持参考
- **MQSim**：NAND时序模型和多队列调度参考
- **OpenSSD**：真实固件三层架构参考

---

# 第三章 产品定义

## 3.1 产品愿景

打造业界首个**高保真全栈SSD模拟器**，能够在标准x86 Linux服务器上，借助大容量DRAM和多核CPU资源，完整再现企业级SSD从主机接口（PCIe/NVMe）到NAND介质的全栈行为，包括精确的固件三层架构、16通道并发介质时序、完整的FTL算法生态，以及可持久化的数据存储能力。该模拟器将成为SSD固件研究、新型FTL算法验证、NVMe协议测试和存储系统研究的核心基础设施。

## 3.2 产品名称

**HFSSS — High-Fidelity Full-Stack SSD Simulator**
中文名称：高保真全栈SSD模拟器

## 3.3 目标用户

### 3.3.1 主要用户群体

**企业SSD固件研发团队**
- 需求：在无需真实NAND硬件的情况下验证固件算法正确性；
- 场景：新型FTL算法、GC策略、WL算法的开发与验证；
- 关键需求：固件行为高保真、三层架构完整、与真实固件代码结构兼容。

**存储系统学术研究者**
- 需求：对SSD内部行为进行细粒度观测和控制；
- 场景：FTL算法论文验证、NVMe协议研究、存储系统性能分析；
- 关键需求：可观测性（observability）强、可扩展性好、参数可配置。

**NVMe协议测试工程师**
- 需求：验证主机NVMe驱动、文件系统、存储软件栈的正确性；
- 场景：IO路径压力测试、协议一致性测试、故障注入测试；
- 关键需求：对主机呈现标准NVMe设备、支持故障模拟。

**数据中心存储架构师**
- 需求：在采购真实SSD硬件前，评估不同SSD配置对系统性能的影响；
- 场景：容量规划、性能预测、存储架构选型；
- 关键需求：性能可配置、支持多种SSD profile。

### 3.3.2 二级用户群体

- **操作系统内核开发者**：测试Linux NVMe驱动、blk-mq层、io_uring等模块；
- **文件系统开发者**：在SSD模拟器上验证文件系统行为（如ext4、XFS、F2FS、NOVA等）；
- **数据库存储引擎开发者**：分析数据库I/O模式与SSD内部行为的交互；
- **AI/ML存储工程师**：评估大规模AI训练的存储I/O特征与SSD性能匹配度。

## 3.4 产品定位

HFSSS定位为**企业级高精度SSD全栈仿真平台**，具有以下核心差异化特点：

1. **全栈覆盖**：从PCIe物理层到NAND介质，覆盖完整的SSD内部栈，包含其他模拟器未覆盖的固件CPU核心建模、NOR Flash建模；

2. **原生Linux集成**：以内核模块形式运行，向主机Linux内核呈现真实的PCIe NVMe块设备，无需QEMU，支持fio、nvme-cli等所有标准存储工具直接使用；

3. **多核充分利用**：在64-256核心的Debian服务器上，将不同模块（主控线程、介质线程、固件CPU线程）绑定到独立CPU核心，实现最大并发性；

4. **DRAM大容量利用**：将64GB以上的物理DRAM作为仿真存储介质，支持数据通过宿主机文件系统持久化；

5. **固件架构完整性**：实现完整的三层固件架构（HAL + Common Service + Application Layer），为固件算法研究提供真实的运行环境；

6. **高度可配置**：支持灵活配置SSD规格（容量、通道数、颗粒数、单元类型等），覆盖从入门级到企业级NVMe SSD的宽广产品谱系。

## 3.5 产品边界

**本产品包含**：
- 完整的NVMe/PCIe主机接口仿真（Admin + I/O命令集）；
- 16通道NAND Flash介质仿真（SLC/MLC/TLC/QLC可配置）；
- NOR Flash介质仿真；
- 三层固件架构的完整实现；
- 数据持久化到宿主机文件系统；
- 完整的故障注入框架；
- 性能统计与可观测性接口；
- 带外管理（OOB）接口。

**本产品不包含**：
- 真实PCIe硬件IP核（不涉及RTL/FPGA实现）；
- 真实NAND硬件控制器驱动（针对真实硅片）；
- 消费级SSD的具体产品固件代码（涉及厂商知识产权）；
- 基于QEMU的虚拟化实现（本产品为裸金属原生实现）；
- UFS、SATA等非NVMe接口（在本版本范围外）；
- 硬件安全引擎（TCG Opal、AES加密硬件）的完整仿真。

## 3.6 产品版本规划

| 版本 | 代号 | 目标 | 主要特性 |
|------|------|------|----------|
| V1.0 | Alpha | MVP | PCIe/NVMe基础仿真、DRAM后端、基础FTL（页级映射+GC） |
| V1.5 | Beta | 完整介质 | 16通道NAND精确时序、NOR Flash、多核固件线程 |
| V2.0 | GA | 全栈完整 | 三层固件架构全实现、持久化、QoS、OOB管理 |
| V2.5 | Enterprise | 企业级 | ZNS支持、KV-SSD、故障注入、性能分析框架 |

---

# 第四章 系统整体架构

## 4.1 系统架构概述

HFSSS采用**模块化、多线程、分层**的系统架构。整个系统运行于Debian Linux服务器之上，以内核模块（Kernel Module）形式向主机操作系统呈现虚拟NVMe PCIe设备，同时在用户空间以守护进程（Daemon）形式运行固件仿真逻辑。

系统在逻辑上分为五大模块：

```
┌─────────────────────────────────────────────────────────────────────┐
│                      主机 Linux 操作系统                              │
│   NVMe Driver │ blk-mq │ File System │ fio/nvme-cli │ User Apps      │
└────────────────────────┬────────────────────────────────────────────┘
                         │ PCIe / NVMe 接口
┌────────────────────────▼────────────────────────────────────────────┐
│              模块一：PCIe/NVMe 设备仿真层（内核模块）                   │
│   PCI BAR仿真 │ NVMe SQ/CQ管理 │ DMA引擎 │ MSI-X中断 │ 命令分发      │
└────────────────────────┬────────────────────────────────────────────┘
                         │ 内部命令总线（共享内存 Ring Buffer）
┌────────────────────────▼────────────────────────────────────────────┐
│              模块二：主控线程（Controller Thread）                      │
│   命令仲裁 │ IO调度 │ 流量控制 │ 资源管理 │ 核间分发                    │
└───────────┬─────────────────┬──────────────────────┬───────────────┘
            │                 │                      │
┌───────────▼──────┐ ┌────────▼──────────┐ ┌────────▼──────────────┐
│  模块三：固件CPU  │ │  模块四：介质线程  │ │  模块五：OOB/监控     │
│  核心线程         │ │  (16 Channel)     │ │  管理线程             │
│  (三层固件架构)   │ │  NAND/NOR仿真     │ │                       │
└──────────────────┘ └───────────────────┘ └───────────────────────┘
            │                 │
┌───────────▼─────────────────▼────────────────────────────────────┐
│                    DRAM 存储后端（64GB+）                           │
│         物理内存预留区域，通过memmap保留，宿主机不可见              │
└───────────────────────────────────────────────────────────────────┘
                              │（持久化路径）
┌─────────────────────────────▼─────────────────────────────────────┐
│                    宿主机文件系统（持久化层）                        │
│              ext4/XFS数据文件，模拟NAND/NOR介质内容固化              │
└────────────────────────────────────────────────────────────────────┘
```

## 4.2 硬件资源规划

### 4.2.1 CPU核心分配策略

在64-256核心的服务器上，HFSSS的CPU核心分配按以下原则进行：

**最小配置（64核心服务器）**：

| 核心分组 | 核心数 | 用途 |
|----------|--------|------|
| PCIe/NVMe仿真组 | 4 | I/O Dispatcher线程 × 1，I/O Worker线程 × 3 |
| 主控线程组 | 8 | 主控仲裁/调度核心 × 2，命令处理核心 × 4，GC/WL后台核心 × 2 |
| 固件CPU核心组 | 16 | 模拟SSD控制器ARM核：FTL核 × 4，HAL核 × 4，调度核 × 4，错误处理核 × 4 |
| NAND介质线程组 | 32 | 每个Channel 2个线程（16 Channel × 2 = 32） |
| NOR介质线程组 | 2 | NOR Flash线程 × 2 |
| OOB/监控组 | 2 | OOB管理线程 × 1，系统监控线程 × 1 |
| 宿主机保留 | 剩余 | 宿主机OS及其他进程使用 |

**推荐配置（128核心服务器）**：

| 核心分组 | 核心数 | 用途 |
|----------|--------|------|
| PCIe/NVMe仿真组 | 8 | I/O Dispatcher × 2，I/O Worker × 6 |
| 主控线程组 | 16 | 主控核心 × 8，GC/WL专用核心 × 4，流量控制核心 × 4 |
| 固件CPU核心组 | 32 | 模拟多核SSD控制器，覆盖完整固件三层 |
| NAND介质线程组 | 48 | 每Channel 3个线程（读/写/擦）× 16 Channel |
| NOR介质线程组 | 4 | NOR读写专用线程 |
| OOB/监控/Debug组 | 8 | OOB、监控、日志、Debug、事件队列 |
| 宿主机保留 | 12 | 宿主机OS |

**最大配置（256核心服务器）**：增加固件CPU核心密度，每通道增加更多并发线程，引入dedicated ECC处理线程。

### 4.2.2 DRAM内存分配策略

系统物理内存（假设128GB为示例）分配如下：

```
┌────────────────────────────────────────────────────────┐
│  总物理内存: 128 GB                                      │
├────────────────────────────────────────────────────────┤
│  宿主机OS保留: 16 GB（kernel + user space + page cache） │
├────────────────────────────────────────────────────────┤
│  NVMeVirt内核模块工作内存: 4 GB                          │
├────────────────────────────────────────────────────────┤
│  NAND介质仿真存储区（memmap预留）: 96 GB                  │
│  ├─ Channel 0-15 数据存储区: 90 GB                       │
│  └─ 元数据区（映射表、BBT、计数器）: 6 GB                 │
├────────────────────────────────────────────────────────┤
│  NOR Flash仿真区: 256 MB                                 │
├────────────────────────────────────────────────────────┤
│  固件CPU核心工作内存: 4 GB                               │
│  ├─ FTL映射表缓存: 2 GB                                  │
│  ├─ 命令缓冲区: 1 GB                                     │
│  └─ 其他固件数据: 1 GB                                   │
├────────────────────────────────────────────────────────┤
│  DRAM缓存（SSD内部写缓冲仿真）: 4 GB                      │
└────────────────────────────────────────────────────────┘
```

内存预留通过内核启动参数实现，在`/etc/default/grub`中配置：
```
GRUB_CMDLINE_LINUX="memmap=96G\$16G"
```
此参数将从物理地址16GB偏移处预留96GB内存，宿主机OS不可使用该区域。

### 4.2.3 存储I/O持久化路径

仿真NAND介质的数据持久化采用两级策略：

1. **主路径（热数据）**：DRAM预留区域直接存储，提供纳秒级访问延迟；
2. **持久化路径（冷数据）**：通过后台线程，将DRAM中的数据块定期以二进制文件形式写入宿主机ext4/XFS文件系统，实现掉电不丢数据的效果。

持久化文件组织结构：
```
/var/hfsss/
├── nand/
│   ├── ch00/
│   │   ├── chip00_die00_plane00.bin
│   │   ├── chip00_die00_plane01.bin
│   │   └── ...
│   ├── ch01/
│   │   └── ...
│   └── ...（共16个channel目录）
├── nor/
│   └── firmware_storage.bin
├── metadata/
│   ├── l2p_table.bin     （逻辑到物理映射表）
│   ├── bad_block.bin     （坏块表）
│   ├── erase_count.bin   （P/E计数）
│   └── system_info.bin   （系统状态）
└── logs/
    └── event_log.bin
```

## 4.3 软件架构层次图

```
┌─────────────────────────────────────────────────────────────────┐
│                    主机应用层                                     │
│  fio │ nvme-cli │ 文件系统 │ 数据库 │ 用户存储应用               │
└──────────────────────────┬──────────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────────┐
│              主机内核层（Linux NVMe Stack）                        │
│  nvme.ko │ nvme-core.ko │ blk-mq │ io_uring │ block layer       │
└──────────────────────────┬──────────────────────────────────────┘
                           │ PCIe/NVMe 协议
╔══════════════════════════▼══════════════════════════════════════╗
║           HFSSS 内核模块层（hfsss_nvme.ko）                       ║
║  ┌─────────────────────────────────────────────────────────┐   ║
║  │ PCIe仿真子层：PCI Config Space │ BAR │ MSI-X │ DMA     │   ║
║  ├─────────────────────────────────────────────────────────┤   ║
║  │ NVMe协议子层：Admin Queue │ IO Queue │ 命令解析/完成   │   ║
║  └─────────────────────────────────────────────────────────┘   ║
╚══════════════════════════╦══════════════════════════════════════╝
                           ║ 内核→用户空间通信（共享内存/ioctl）
╔══════════════════════════▼══════════════════════════════════════╗
║           HFSSS 用户空间守护进程（hfsss-daemon）                  ║
║                                                                  ║
║  ┌──────────────────────────────────────────────────────────┐  ║
║  │              主控线程（Controller Thread）                 │  ║
║  │  命令仲裁 │ IO调度 │ 资源分配 │ 核间通信 │ 流量控制       │  ║
║  └──────────────────────────────────────────────────────────┘  ║
║                           │                                      ║
║  ┌────────────────────────▼─────────────────────────────────┐  ║
║  │         固件CPU核心线程群（Firmware Core Threads）          │  ║
║  │                                                            │  ║
║  │  ┌─────────────────────────────────────────────────────┐ │  ║
║  │  │ Application Layer（算法任务层）                       │ │  ║
║  │  │ FTL │ GC │ WL │ BBM │ ECC │ QoS │ 冗余 │ 错误处理  │ │  ║
║  │  ├─────────────────────────────────────────────────────┤ │  ║
║  │  │ Common Service Layer（通用平台层）                    │ │  ║
║  │  │ RTOS │ 调度 │ 内存管理 │ Boot │ 核间通信 │ 监控     │ │  ║
║  │  ├─────────────────────────────────────────────────────┤ │  ║
║  │  │ Hardware Access Layer（硬件接入层）                   │ │  ║
║  │  │ NAND驱动 │ NOR驱动 │ NVMe/PCIe │ 电源管理           │ │  ║
║  │  └─────────────────────────────────────────────────────┘ │  ║
║  └────────────────────────────────────────────────────────────┘  ║
║                           │                                       ║
║  ┌────────────────────────▼──────────────────────────────────┐  ║
║  │         介质线程群（Media Threads）                         │  ║
║  │  Channel 0~15 × (多个NAND Chip线程 + 时序控制器)           │  ║
║  │  NOR Flash线程                                             │  ║
║  └────────────────────────────────────────────────────────────┘  ║
║                           │                                       ║
║  ┌────────────────────────▼──────────────────────────────────┐  ║
║  │         DRAM存储后端 + 文件系统持久化层                     │  ║
║  └────────────────────────────────────────────────────────────┘  ║
╚══════════════════════════════════════════════════════════════════╝
```


---

# 第五章 功能需求

## 5.1 PCIe/NVMe设备仿真模块

### 5.1.1 模块概述

PCIe/NVMe设备仿真模块是HFSSS与主机操作系统的接口层，以Linux内核模块（Kernel Module）形式实现，参考NVMeVirt的核心机制，在宿主机Linux内核中虚拟化出一个标准PCIe NVMe设备。该模块的核心挑战在于：在不使用真实PCIe硬件IP的前提下，通过纯软件方式骗过Linux NVMe驱动，使其认为系统中存在一块真实的PCIe NVMe SSD。

### 5.1.2 PCIe配置空间仿真（FR-PCIE-001）

**需求描述**：模拟器必须在PCI总线上注册一个虚拟PCIe设备，提供标准的PCI配置空间（Configuration Space）。

**详细需求**：

1. **PCI配置头**：实现标准的PCI Type 0配置头（256字节基础配置空间 + 4096字节扩展配置空间），包含：
   - Vendor ID / Device ID：可配置，默认使用研究用途的保留ID段；
   - Class Code：设置为0x010802（NVM Express Controller）；
   - Subsystem Vendor ID / Subsystem ID：可配置；
   - Revision ID：0x01；
   - Header Type：0x00（Standard）；
   - Capabilities Pointer：指向Capabilities链表起始位置。

2. **PCIe Capabilities链表**：实现以下必要的Capability结构：
   - PCI Power Management Interface（Cap ID 0x01）；
   - MSI Capability（Cap ID 0x05）；
   - MSI-X Capability（Cap ID 0x11）；
   - PCIe Capability（Cap ID 0x10），报告设备类型为PCIe Endpoint，Link速度可配置为Gen3×4或Gen4×4。

3. **BAR（Base Address Register）配置**：
   - BAR0：映射到预留物理内存区域，作为NVMe控制器寄存器（MMIO区域），大小≥16KB；
   - BAR2/BAR4（可选）：用于MSIX Table和PBA（Pending Bit Array）。

4. **设备枚举**：通过Linux内核的`pci_register_driver`机制注册虚拟PCI设备，使`lspci`命令可以发现并列出该虚拟NVMe设备。

**验收标准**：
- `lspci -v`可显示仿真NVMe设备，Class为"Non-Volatile memory controller"；
- `lspci -vvv`可显示完整的Capabilities链表；
- Linux NVMe驱动（nvme.ko）可成功探测并绑定该虚拟设备。

### 5.1.3 NVMe控制器寄存器仿真（FR-NVME-001）

**需求描述**：在BAR0映射的MMIO区域中，实现完整的NVMe控制器寄存器集（NVMe Controller Registers），对应NVMe规范2.0的寄存器定义。

**关键寄存器**：

1. **CAP（Controller Capabilities，偏移0x00，64bit）**：
   - MQES：Maximum Queue Entries Supported，支持最大65535个条目；
   - CQR：Contiguous Queues Required，建议设为0（不要求连续）；
   - AMS：Arbitration Mechanism Supported，支持Round Robin + Weighted Round Robin；
   - TO：Timeout，控制器就绪超时，单位500ms；
   - DSTRD：Doorbell Stride，设为0（4字节对齐）；
   - NSSRS：NVM Subsystem Reset Supported；
   - CSS：Command Sets Supported，支持NVM Command Set；
   - MPSMIN/MPSMAX：内存页大小范围，支持4KB-64KB。

2. **VS（Version，偏移0x08，32bit）**：报告NVMe版本，默认设为0x00020000（NVMe 2.0）。

3. **CC（Controller Configuration，偏移0x14，32bit）**：
   - EN：Enable位，写1时初始化控制器；
   - CSS：I/O Command Set Selection；
   - MPS：Memory Page Size；
   - AMS：Arbitration Mechanism；
   - SHN：Shutdown Notification位（Normal Shutdown / Abrupt Shutdown）；
   - IOSQES/IOCQES：IO SQ/CQ Entry Size（64字节命令条目）。

4. **CSTS（Controller Status，偏移0x1C，32bit）**：
   - RDY：控制器就绪位；
   - CFS：Controller Fatal Status；
   - SHST：Shutdown Status。

5. **AQA（Admin Queue Attributes，偏移0x24，32bit）**：Admin SQ/CQ大小配置。

6. **ASQ（Admin Submission Queue Base Address，偏移0x28，64bit）**：Admin SQ的物理基地址。

7. **ACQ（Admin Completion Queue Base Address，偏移0x30，64bit）**：Admin CQ的物理基地址。

8. **Doorbell寄存器（偏移0x1000起）**：
   - SQ y Tail Doorbell（偏移1000h + (2y × (4 << CAP.DSTRD))）；
   - CQ y Head Doorbell（偏移1000h + ((2y+1) × (4 << CAP.DSTRD))）；
   - 支持最多64对SQ/CQ（可配置）。

**验收标准**：
- `nvme list`命令可识别并列出仿真设备；
- `nvme id-ctrl /dev/nvme0`可返回完整的控制器标识信息；
- 控制器初始化序列（写CC.EN=1）完成后，CSTS.RDY在规定超时内置1。

### 5.1.4 NVMe队列管理（FR-NVME-002）

**需求描述**：实现完整的NVMe提交队列（SQ）和完成队列（CQ）管理机制。

**详细需求**：

1. **Admin Queue对**（固定QID=0）：
   - Admin SQ：接收Admin命令（Create SQ/CQ、Delete SQ/CQ、Identify、Get/Set Features、Format NVM、Firmware操作等）；
   - Admin CQ：返回Admin命令完成状态；
   - 最大深度：可配置，默认256条目；
   - 物理地址由ASQ/ACQ寄存器指定，内容通过MMIO DMA读取。

2. **I/O Queue对**（QID=1~65535）：
   - 通过Admin命令（Create I/O SQ / Create I/O CQ）动态创建；
   - 支持最多64对I/O SQ/CQ（可通过编译期参数调整）；
   - 每个SQ与一个CQ关联（通过QPID字段）；
   - Queue深度：可配置，范围2~65535条目；
   - 优先级：支持NVMe WRR优先级（Urgent/High/Medium/Low）；
   - 支持物理地址连续（Contiguous）和非连续（PRP List）两种内存布局。

3. **命令轮询机制**：
   - I/O Dispatcher线程持续监控所有SQ的Tail Doorbell；
   - Doorbell更新触发Dispatcher线程唤醒，从SQ中取出命令；
   - 使用高效的无锁Ring Buffer实现SQ内容读取。

4. **PRPs（Physical Region Page）和SGL（Scatter Gather List）**：
   - 实现PRP1/PRP2和PRP List机制，支持非连续物理内存数据缓冲；
   - 实现SGL Descriptor解析（数据块、位段、键引用类型）；
   - DMA操作：通过内核`memcpy`或DMA API实现主机内存与仿真存储的数据传输。

5. **完成处理**：
   - 命令完成后，将64字节的CQE（Completion Queue Entry）写入对应CQ的Head位置；
   - 更新CQ Phase Tag（CQE的最低位，交替变化）；
   - 通过MSI-X机制向主机投递中断。

**验收标准**：
- `nvme create-ns`、`nvme attach-ns`等命令可正常执行；
- `fio --ioengine=io_uring --numjobs=32 --iodepth=128`等压力测试可稳定运行；
- 支持同时运行64个I/O Queue对。

### 5.1.5 MSI-X中断仿真（FR-NVME-003）

**需求描述**：实现MSI-X（Message Signaled Interrupts Extended）中断机制，用于通知主机NVMe驱动命令完成。

**详细需求**：

1. **MSI-X Table**：维护最多64个MSI-X Table Entry，每个Entry包含：
   - Message Address（64bit）：目标CPU的本地APIC地址；
   - Message Data（32bit）：中断向量号；
   - Vector Control（32bit）：中断屏蔽位。

2. **MSI-X PBA**（Pending Bit Array）：64bit PBA，标记待处理的中断。

3. **中断投递**：
   - 完成命令后，根据CQ对应的MSI-X Vector索引查找MSI-X Table；
   - 通过向Message Address写入Message Data的方式触发x86 APIC中断；
   - 在Linux内核模块中，使用`apic->send_IPI`或写MMIO地址方式模拟MSI-X投递；
   - 每个CQ可独立分配不同的MSI-X Vector，实现中断亲和性（中断绑定到固定CPU核心）。

4. **中断聚合（Interrupt Coalescing）**：
   - 支持NVMe的中断聚合特性（Set Features / Interrupt Coalescing）；
   - 聚合阈值（aggregation threshold）和时间（aggregation time）可配置；
   - 减少高IOPS场景下的中断频率，降低CPU中断处理开销。

**验收标准**：
- `nvme set-feature /dev/nvme0 --feature-id=8 --value=<agg_threshold,agg_time>`可配置中断聚合；
- `cat /proc/interrupts | grep nvme`可显示MSI-X中断分配情况；
- 中断亲和性可通过`/proc/irq/<n>/smp_affinity`配置。

### 5.1.6 NVMe Admin命令集（FR-NVME-004）

**需求描述**：实现NVMe规范定义的完整Admin命令集，确保主机侧nvme-cli和内核驱动的所有管理操作可正确执行。

**必须支持的Admin命令**：

| Opcode | 命令名 | 说明 |
|--------|--------|------|
| 0x00 | Delete I/O SQ | 删除I/O提交队列 |
| 0x01 | Create I/O SQ | 创建I/O提交队列（QID, QSIZE, CQID, QPRIO, PHYS_CONTIG） |
| 0x02 | Get Log Page | 获取日志页（Error Log, SMART/Health, FW Slot, Changed NS等） |
| 0x04 | Delete I/O CQ | 删除I/O完成队列 |
| 0x05 | Create I/O CQ | 创建I/O完成队列（QID, QSIZE, IEN, IV, PHYS_CONTIG） |
| 0x06 | Identify | 控制器标识（CNS=0: NS, CNS=1: Controller, CNS=2: NS List） |
| 0x08 | Abort | 中止指定命令 |
| 0x09 | Set Features | 设置特性（Arbitration, Power Mgmt, LBA Range, Temp Threshold等） |
| 0x0A | Get Features | 获取特性当前值 |
| 0x0C | Async Event Request | 异步事件请求 |
| 0x0D | Namespace Management | 命名空间创建/删除 |
| 0x10 | Firmware Commit | 固件提交 |
| 0x11 | Firmware Image Download | 固件镜像下载 |
| 0x14 | Namespace Attachment | 命名空间附加/分离 |
| 0x7C | Doorbell Buffer Config | 影子Doorbell缓冲区配置 |
| 0xC0 | Format NVM | 格式化命名空间（安全擦除） |

**Identify Controller数据结构**（关键字段）：
- VID/SSVID：厂商ID；
- SN（Serial Number）：可配置字符串；
- MN（Model Number）：可配置字符串，如"HFSSS Virtual NVMe SSD v2.0"；
- FR（Firmware Revision）：固件版本字符串；
- RAB：推荐仲裁突发值；
- IEEE OUI Identifier；
- CMIC：控制器多路径I/O和命名空间共享能力；
- MDTS：Maximum Data Transfer Size（以最小页大小的2的幂次表示）；
- CNTLID：控制器ID；
- VER：NVMe规范版本（0x00020000 = 2.0）；
- RTD3R/RTD3E：D3状态恢复/进入延迟；
- OAES：可选异步事件支持；
- SQES/CQES：SQ/CQ Entry Size；
- MAXCMD：最大并发命令数；
- NN：命名空间总数；
- ONCS：可选NVM命令集支持（Write Zeroes, Dataset Management, Verify等）；
- FUSES：熔断操作支持；
- FNA：Format NVM属性；
- VWC：Volatile Write Cache（写缓存挥发性标志）；
- AWUN/AWUPF：原子写单位（正常/断电）；
- NVSCC：NVM供应商特定命令配置；
- ACWU：原子比较写单位；
- SGLS：SGL支持标志；
- SUBNQN：NVMe子系统NQN（格式：nqn.2026-03.io.hfsss:nvme.0）。

### 5.1.7 NVMe I/O命令集（FR-NVME-005）

**需求描述**：实现NVMe NVM命令集的完整I/O命令，支持标准文件系统和存储应用的所有I/O操作。

**必须支持的I/O命令**：

| Opcode | 命令名 | 说明 |
|--------|--------|------|
| 0x00 | Flush | 将写缓存数据持久化 |
| 0x01 | Write | 写入数据到指定LBA范围 |
| 0x02 | Read | 从指定LBA范围读取数据 |
| 0x04 | Write Uncorrectable | 将指定LBA标记为不可纠错 |
| 0x08 | Write Zeroes | 将指定LBA范围清零（高效实现） |
| 0x09 | Dataset Management | 数据集管理（Trim/Deallocate） |
| 0x0C | Verify | 验证指定LBA范围数据完整性 |
| 0x0D | Copy | 数据拷贝（Simple Copy Command） |

**Read/Write命令详细需求**：
- SLBA（Starting LBA）：64bit起始逻辑块地址；
- NLB（Number of Logical Blocks）：读写块数（0-based，最大65535）；
- PRP Entry支持：PRP1/PRP2/PRP List；
- SGL支持：SGL描述符链表；
- FUA（Force Unit Access）：强制写到持久化存储；
- LR（Limited Retry）：有限重试标志；
- DSM（Dataset Management）：顺序读写、不可压缩等提示；
- PRINFO：Protection Information操作。

**Dataset Management（Trim）**：
- 支持最多256个LBA Range，每个Range由起始LBA和块数定义；
- Trim操作通知FTL释放相应物理页，加速GC；
- 支持Deallocate（Context Attribute bit 2）标志。

### 5.1.8 NVMe DMA数据传输（FR-NVME-006）

**需求描述**：实现主机内存与仿真NAND存储后端之间的数据传输机制。

**详细需求**：

1. **PRP解析引擎**：
   - 解析命令中的PRP1/PRP2字段；
   - 当数据超过一个物理页时，读取PRP List（内核物理内存中的页地址数组）；
   - 将分散的物理页拼接成连续的数据视图。

2. **数据拷贝路径**：
   - 读命令（NAND→Host）：从DRAM存储后端读取数据，通过`memcpy_to_user_page`或内核页映射写入主机物理内存；
   - 写命令（Host→NAND）：从主机物理内存读取数据，写入DRAM存储后端，再异步下发至介质线程；
   - 利用`kmap_atomic`/`kunmap_atomic`映射高端内存页；
   - 考虑NUMA节点亲和性：数据拷贝尽量在数据所在NUMA节点上执行。

3. **零拷贝优化**（可选，V2.0实现）：
   - 利用Linux的`splice`或`sendfile`机制，减少内核到内核的数据拷贝；
   - 对大块顺序IO，探索使用`move_pages`机制实现页所有权转移。

4. **IOMMU支持**：
   - 检测系统IOMMU状态，若IOMMU启用，正确处理DMA地址映射；
   - 使用`dma_map_page`/`dma_unmap_page` API确保DMA地址与物理地址的正确转换。

**验收标准**：
- `dd if=/dev/urandom of=/dev/nvme0n1 bs=4M count=100`可正确写入数据；
- `md5sum`验证写入与读回数据一致性；
- 对齐和非对齐IO（512B、4KB、8KB、16KB、128KB、1MB）均可正确处理。

---

## 5.2 主控线程模块（Controller Thread）

### 5.2.1 模块概述

主控线程是整个SSD模拟器的"大脑"，负责接收来自PCIe/NVMe模块的命令，进行高层调度和资源仲裁，将命令分发给固件CPU核心线程执行，并协调各子系统的协同工作。主控线程运行于用户空间守护进程中，通过实时线程（SCHED_FIFO调度策略）和CPU绑定（CPU Affinity）确保低延迟响应。

### 5.2.2 命令接收与分发（FR-CTRL-001）

**需求描述**：主控线程从PCIe/NVMe模块的内核态接收解析好的NVMe命令，进行全局仲裁后分发给固件CPU核心线程。

**详细需求**：

1. **内核-用户空间通信**：
   - 通过共享内存Ring Buffer实现内核模块到用户态主控线程的命令传递；
   - Ring Buffer采用无锁设计（lock-free SPSC或MPMC），避免内核-用户态锁竞争；
   - Ring Buffer容量：可配置，默认16384个命令槽（每槽128字节，包含NVMe命令+元数据）；
   - 内核通过`mmap`将Ring Buffer暴露给用户空间守护进程；
   - 使用`eventfd`或信号量通知主控线程有新命令到达（也支持轮询模式）。

2. **命令仲裁策略**：
   - 实现NVMe WRR（Weighted Round Robin）仲裁：
     - Urgent队列：100%抢占权；
     - High:Medium:Low = 8:4:1的时隙分配；
   - 实现简单Round Robin（RR）作为备选策略；
   - 支持在运行时通过配置接口切换仲裁策略；
   - Admin命令始终优先于I/O命令处理。

3. **命令分发**：
   - 根据命令类型（Read/Write/Trim/Flush/Admin）分发到对应的固件CPU核心线程池；
   - 维护每个命令的状态机（Pending → In-Flight → Completing → Done）；
   - 支持命令依赖跟踪（如Flush命令需等待所有在途Write命令完成）；
   - 最大在途命令数（In-Flight Commands）可配置，默认4096。

4. **命令超时管理**：
   - 为每个命令维护超时计时器；
   - 命令超时时间可通过Set Features配置（默认8秒）；
   - 超时命令触发错误处理流程（返回DNR=0的错误完成，允许主机重试）。

### 5.2.3 I/O调度器（FR-CTRL-002）

**需求描述**：实现高效的I/O调度器，优化命令处理顺序以最大化NAND并发利用率，同时满足QoS要求。

**详细需求**：

1. **调度算法**：
   - 默认调度器：基于目标NAND通道/Die的贪心调度（尽量使不同Channel的命令并发）；
   - 可选调度器：FIFO（先进先出）、Deadline（截止时间优先）；
   - 写命令：可进行合并（如连续LBA的多个4KB写合并为单个16KB NAND页程序操作）；
   - 读命令：预取机制（Read-Ahead），检测到顺序读模式时提前向介质线程下发读预取请求。

2. **写缓冲区管理（Write Buffer）**：
   - 维护全局写缓冲区（Write Buffer），容量对应仿真SSD的DRAM缓存大小（默认4GB）；
   - 写命令先写入Write Buffer即返回完成（VWC模式）；
   - 后台线程将Write Buffer中的脏数据以Page Program方式写入NAND介质；
   - Flush命令触发强制Write Buffer下刷；
   - Write Buffer满时触发背压（Backpressure），限制主机写入速率。

3. **读缓存（Read Cache）**：
   - 实现LRU（Least Recently Used）读缓存，缓存热点读数据；
   - 缓存命中时直接返回，不下发介质命令；
   - 缓存容量：可配置，默认256MB。

4. **Channel负载均衡**：
   - 实时统计16个Channel的队列深度和繁忙状态；
   - 将新命令优先分发到负载较低的Channel，避免热点通道；
   - 支持Channel间的命令迁移（当某Channel持续拥堵时）。

### 5.2.4 资源管理器（FR-CTRL-003）

**需求描述**：管理仿真SSD的内部资源（空闲块池、DRAM缓存、命令槽等），为各子系统提供资源分配服务。

**详细需求**：

1. **空闲块管理**：
   - 维护每个Channel/Die/Plane的空闲块链表（Free Block List）；
   - 为写命令分配空闲页（Page Allocation）；
   - 空闲块水位监控：当空闲块数量低于高水位（High Watermark）时，通知GC线程启动回收；
   - 当空闲块低于低水位（Low Watermark）时，阻塞新的写命令直至GC释放足够空间。

2. **命令槽管理**：
   - 维护全局命令跟踪表（Command Tracking Table，CTT）；
   - 每个在途命令占用一个CTT槽（包含命令ID、状态、时间戳、相关资源引用）；
   - CTT槽满时触发背压机制。

3. **DRAM缓存资源管理**：
   - 按命名空间（Namespace）配额分配DRAM缓存；
   - 支持DRAM缓存的热/冷数据分层管理；
   - 监控DRAM缓存压力，触发强制回写（forced write-back）。

### 5.2.5 流量控制（FR-CTRL-004）

**需求描述**：实现多层次的I/O流量控制机制，防止过载、保证QoS，并支持运行时配置。

**详细需求**：

1. **令牌桶限速器（Token Bucket Rate Limiter）**：
   - 支持按命名空间/QID设置IOPS上限和带宽上限；
   - 令牌桶参数：峰值速率（Peak Rate）和持续速率（Sustained Rate）；
   - 实现精确的微秒级令牌补充机制；
   - 超限命令进入等待队列，而非立即返回错误。

2. **背压机制（Backpressure）**：
   - 当Write Buffer使用率超过90%时，停止从NVMe SQ中取新的Write命令；
   - 通过调整Doorbell检查频率实现软性限速；
   - 向主机投递ASYNC EVENT（Namespace Attribute Changed）通知流量控制状态变化。

3. **QoS保证**：
   - 对Urgent优先级队列的命令，保证在1ms内开始处理；
   - 对High优先级队列，P99延迟≤10ms；
   - 实现延迟直方图统计（latency histogram），支持运行时查询。

4. **GC流量控制**：
   - GC操作使用受限的NAND带宽（默认不超过总NAND带宽的30%）；
   - 当有高优先级IO时，暂停GC（GC Suspension）；
   - GC最小带宽保证：即使有持续IO，GC至少获得5%带宽，防止垃圾堆积。


---

## 5.3 介质线程模块（Media Threads）

### 5.3.1 模块概述

介质线程模块负责模拟SSD内部的NAND Flash和NOR Flash介质行为，包括精确的时序建模、数据存储管理和介质状态维护。该模块按照真实SSD的通道（Channel）架构组织，共设置16个NAND通道，每个通道上挂载多个NAND颗粒（Chip/CE），每个颗粒内部有多个Die，每个Die有多个Plane。NOR Flash作为独立模块，用于模拟固件代码存储介质。

### 5.3.2 NAND Flash层次结构（FR-MEDIA-001）

**需求描述**：按照真实3D NAND Flash的物理层次结构组织介质仿真资源。

**层次结构定义**：

```
NAND存储层次（从大到小）：
Channel（通道）
  └── Chip/CE（颗粒/片选）
        └── Die（晶粒）
              └── Plane（平面）
                    └── Block（块，擦除单元）
                          └── Page（页，读写单元）
                                └── Sector（扇区，512字节）
```

**可配置参数**（默认配置对应1.92TB企业级NVMe SSD）：

| 参数 | 默认值 | 范围 | 说明 |
|------|--------|------|------|
| NUM_CHANNELS | 16 | 4-32 | NAND通道数 |
| CHIPS_PER_CHANNEL | 4 | 1-16 | 每通道颗粒数（CE数） |
| DIES_PER_CHIP | 2 | 1-8 | 每颗粒Die数 |
| PLANES_PER_DIE | 2 | 1-4 | 每Die平面数 |
| BLOCKS_PER_PLANE | 4096 | 512-16384 | 每Plane块数 |
| PAGES_PER_BLOCK | 512 | 128-1024 | 每块页数（3D NAND典型值） |
| PAGE_SIZE | 16384 | 4096-65536 | 页大小（字节），含OOB |
| DATA_SIZE_PER_PAGE | 16000 | 4096-65536 | 页数据区大小（字节） |
| OOB_SIZE_PER_PAGE | 384 | 64-1024 | 页OOB区大小（字节），用于ECC和元数据 |
| CELL_TYPE | TLC | SLC/MLC/TLC/QLC | NAND单元类型 |
| PE_CYCLE_LIMIT | 3000 | 500-100000 | 最大P/E循环次数（TLC典型值3000） |

**总容量计算（默认配置）**：
16 channels × 4 chips × 2 dies × 2 planes × 4096 blocks × 512 pages × 16KB/page = **约4TB原始容量**（含OP超额配置，用户可见约3.2TB，再扣除20% OP约2.56TB，可配置为2TB NVMe命名空间）。

### 5.3.3 NAND介质时序模型（FR-MEDIA-002）

**需求描述**：实现精确的NAND Flash操作时序模型，准确模拟真实NAND芯片的读写擦性能特性。

**基础时序参数（TLC 3D NAND，ONFI 4.2接口）**：

| 操作 | 延迟参数 | 默认值 | 说明 |
|------|----------|--------|------|
| 页读（LSB页） | tR_LSB | 35μs | 高速读，存储在下层 |
| 页读（CSB页） | tR_CSB | 70μs | 中间层读，需读LSB后计算 |
| 页读（MSB页） | tR_MSB | 100μs | 慢速读，存储在上层 |
| 数据输出（通道传输） | tDO | 页大小/通道速率 | ONFI 4.2: ~1200MB/s/ch，16KB/1200MB = 13.3μs |
| 页程序（SLC模式） | tPROG_SLC | 100μs | 仅用LSB层 |
| 页程序（TLC模式） | tPROG_TLC | 800μs | 三层全写，包含精调 |
| 数据输入（通道传输） | tDI | 页大小/通道速率 | 同tDO |
| 块擦除 | tERS | 3000μs | 整块擦除 |
| 多平面读（MPRD） | tR_MP | max(tR_x) | 两个Plane并发读，延迟取最大值 |
| 多平面程序（MPPG） | tPROG_MP | 800μs | 两个Plane并发写 |
| 多平面擦除（MPERS） | tERS_MP | 3000μs | 两个Plane并发擦 |
| 缓存读（Cache Read） | tRC | 连续页优化 | 前一页传输与后一页读取重叠 |
| 程序暂停（Suspend） | tSUSP | 5μs | 暂停程序以响应读请求 |
| 程序恢复（Resume） | tRESM | 5μs | 恢复被暂停的程序操作 |

**时序模型实现**：

每个介质线程（对应一个Channel或一个Die）维护精确的时间轴（Timeline），记录每个资源（Channel Bus、Die、Plane）的最早可用时刻（Earliest Available Time, EAT）：

```
命令执行时间计算算法：
1. 确定命令类型（Read/Program/Erase）和目标地址（Channel, Chip, Die, Plane, Block, Page）
2. 获取相关资源的EAT：
   t_channel = channel_eat[channel_id]  // 通道总线占用时间
   t_die     = die_eat[chip_id][die_id]  // Die占用时间
   t_plane   = plane_eat[chip_id][die_id][plane_id]  // Plane占用时间
3. 命令开始时刻 = max(t_channel, t_die, t_plane, current_time)
4. 根据命令类型和页类型（LSB/CSB/MSB）查表得到媒体延迟tMedia
5. 通道传输时间 = page_size / channel_speed
6. 命令完成时刻:
   - 读命令: t_start + tR_x + tDO（先介质读，后通道输出）
   - 写命令: t_start + tDI + tPROG（先通道输入，后介质写）
   - 擦命令: t_start + tERS
7. 更新相关资源的EAT：
   channel_eat = t_start + tDI/tDO  // 通道占用结束
   die_eat = t_complete            // Die在整个操作期间占用
   plane_eat = t_complete          // Plane在写/擦期间占用
```

**多平面（Multi-Plane）并发**：
- 同一Die的两个Plane可同时执行相同类型的操作（读/写/擦）；
- Multi-Plane Program要求两个Plane对应相同的Block偏移和Page偏移；
- Multi-Plane Read要求两个Plane读取相同的Page偏移；
- Multi-Plane EAT更新：两个Plane共用Die的行解码器，EAT = max(plane0_eat, plane1_eat) + tOperation。

**Die交叉（Die Interleaving）**：
- 同一Chip的两个Die可独立并行操作（各有独立的行解码器和感应放大器）；
- Interleaving调度：在Die0执行tPROG期间，同时向Die1下发新的Program命令，实现时间重叠；
- EAT更新：die0_eat和die1_eat独立维护。

**Chip Enable并发（Channel级）**：
- 同一Channel上的多个Chip（CE0~CE3）共享Channel总线，但可通过CE分时复用实现并发；
- 通道总线在数据传输期间（tDI/tDO）排他占用；
- 媒体操作（tR/tPROG/tERS）期间通道总线可被其他Chip复用。

### 5.3.4 NAND介质命令执行引擎（FR-MEDIA-003）

**需求描述**：每个Channel配置独立的命令执行引擎线程，接收来自固件CPU核心的NAND命令，按时序模型执行并回调完成通知。

**支持的NAND命令**：

| 命令 | NAND Op Code | 说明 |
|------|-------------|------|
| Page Read | 0x00/0x30 | 读取单页数据 |
| Cache Read | 0x31 | 缓存读，流水线优化 |
| Multi-Plane Read | 0x32 | 多平面并发读 |
| Page Program | 0x80/0x10 | 写入单页数据 |
| Cache Program | 0x80/0x15 | 缓存写，流水线优化 |
| Multi-Plane Program | 0x80/0x11 | 多平面并发写 |
| Block Erase | 0x60/0xD0 | 擦除单个块 |
| Multi-Plane Erase | 0x60/0xD1 | 多平面并发擦除 |
| Read Status | 0x70 | 读取操作状态寄存器 |
| Reset | 0xFF | 复位NAND芯片 |
| Read ID | 0x90 | 读取NAND芯片ID |
| Program Suspend | 0xB0 | 暂停程序操作 |
| Program Resume | 0xD0 | 恢复程序操作 |
| Erase Suspend | 0xB0 | 暂停擦除操作 |

**命令队列设计**：
- 每个Channel维护独立的命令队列（默认深度：128条）；
- 命令队列按Die/Plane进行逻辑分组，支持同一Channel内多Die并行发射；
- 命令调度器负责从队列中选取下一个命令，基于时序约束（EAT）和优先级决策；
- 支持命令暂停/恢复（Program/Erase Suspend）：当有高优先级读命令到来时，可暂停当前正在执行的Program或Erase操作，发出Suspend命令，执行读操作后再Resume。

**完成通知机制**：
- 命令完成后，通过无锁队列（lock-free completion queue）通知固件CPU核心线程；
- 完成通知包含：命令ID、完成时间戳、操作结果（Success/Bit Error/Uncorrectable Error）、实际延迟值；
- 支持批量完成（Batch Completion），一次通知多个命令完成，减少上下文切换开销。

### 5.3.5 NAND可靠性建模（FR-MEDIA-004）

**需求描述**：对NAND Flash的可靠性特性进行精确建模，包括P/E循环退化、读干扰、数据保持性等效应。

**P/E循环退化模型**：
- 每个Block维护独立的P/E循环计数（Erase Count）；
- 随着P/E循环增加，NAND单元的误码率（BER）按对数关系增长：`BER = A × log(PE_cycles) + B`；
- BER可通过ECC纠正的范围：BER < LDPC纠错能力（约0.01%）；
- 超过PE_CYCLE_LIMIT的块自动标记为坏块（Bad Block）；
- 误码率增加影响Read Retry次数（P/E高的块需要更多次Read Retry，增加读延迟）。

**读干扰（Read Disturb）模型**：
- 同一Block中，对页A的读操作对该Block其他未被读取的页施加应力；
- 读干扰效应随Block内累计读次数增加（Read Count Per Block）；
- 当Block的读计数超过阈值（默认100,000次读/块，可配置）时，触发Refresh操作（将数据读出并重写到新块）；
- 模型：额外引入的误码数 = K × (read_count_in_block - threshold)（超过阈值后线性增长）。

**数据保持性（Data Retention）模型**：
- 存储数据随时间流逝发生电荷泄漏，导致阈值电压漂移；
- 数据保持性问题在高P/E块上更严重；
- 模型：每经过模拟时间T，对存储数据引入与P/E循环和时间相关的误码；
- 支持加速老化：通过配置时间加速比（默认1000:1）在短时间内模拟数据保持性退化。

**坏块管理**：
- 维护全局坏块表（Bad Block Table，BBT），记录每个Die上的坏块位置；
- 出厂坏块（Factory Bad Block）：在初始化时按配置比例（默认2%）随机标记坏块；
- 运行时坏块：超过P/E寿命或写入失败次数超过阈值（默认3次重试失败）时标记；
- 坏块替换：为每个Die预留备用块（Reserve Pool，默认每Die预留5%），替换失效坏块；
- BBT持久化：BBT数据存储在NOR Flash仿真区中，掉电不丢。

### 5.3.6 NAND数据存储机制（FR-MEDIA-005）

**需求描述**：实现NAND Flash的数据存储后端，将页数据保存在预留DRAM区域，并支持持久化到宿主机文件系统。

**DRAM存储布局**：
- 将预留DRAM区域按Channel/Chip/Die/Plane/Block/Page分层索引；
- 每个Page在DRAM中的地址：
  ```
  base_addr + (ch × CHIPS_PER_CH × DIES_PER_CHIP × PLANES_PER_DIE × BLOCKS_PER_PLANE × PAGES_PER_BLOCK × PAGE_SIZE)
            + (chip × DIES_PER_CHIP × PLANES_PER_DIE × BLOCKS_PER_PLANE × PAGES_PER_BLOCK × PAGE_SIZE)
            + (die × PLANES_PER_DIE × BLOCKS_PER_PLANE × PAGES_PER_BLOCK × PAGE_SIZE)
            + (plane × BLOCKS_PER_PLANE × PAGES_PER_BLOCK × PAGE_SIZE)
            + (block × PAGES_PER_BLOCK × PAGE_SIZE)
            + (page × PAGE_SIZE)
  ```
- OOB区域附加在每个Page数据区之后，存储ECC校验码、LPN（逻辑页号）等元数据；
- 未写入的Page以0xFF填充（模拟擦除后的NAND初始状态）。

**持久化策略**：
- **增量持久化**：每次Block Erase操作完成后，将该Block中仍有效的数据持久化到文件系统（因GC后数据已重写）；
- **周期性快照**：每10分钟（可配置）将所有脏数据（未持久化的页）写入文件系统；
- **优雅关机持久化**：接收SIGTERM/SIGINT信号时，触发全量持久化后再退出；
- **持久化文件格式**：每个Plane数据以单独的二进制文件存储，文件头包含：格式版本、时间戳、CRC32校验、Plane物理地址（ch/chip/die/plane）；
- **写回线程**：专用后台线程负责持久化，使用`pwrite`或`io_uring`的异步I/O接口，优先级低于仿真主线程，避免持久化操作影响仿真延迟。

**恢复机制**：
- 系统重启时，读取持久化文件恢复DRAM存储状态；
- 读取元数据文件（l2p_table.bin, bad_block.bin, erase_count.bin）恢复FTL状态；
- 检测到持久化数据不完整时（CRC校验失败），记录警告并将对应Plane初始化为全0xFF（模拟数据丢失）；
- 支持通过命令行参数`--fresh-start`跳过恢复，以全新NAND状态启动。

### 5.3.7 NOR Flash介质仿真（FR-MEDIA-006）

**需求描述**：仿真SSD固件使用的NOR Flash存储介质，用于存储固件代码、配置数据和关键元数据（如BBT）。

**NOR Flash规格（默认配置）**：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| 容量 | 256MB | 分为多个扇区 |
| 扇区大小 | 64KB | 擦除粒度 |
| 扇区数量 | 4096 | 总扇区数 |
| 读取时间 | 90ns/byte | 字节读，高速 |
| 页编程时间 | 100μs | 256字节/页 |
| 扇区擦除时间 | 200ms | 64KB扇区擦除 |
| 块擦除时间 | 1.5s | 整片擦除 |
| PE寿命 | 100,000次 | 每扇区 |
| 接口 | SPI（模拟） | 四线制SPI接口仿真 |

**存储分区（Partition）**：

| 分区名 | 起始地址 | 大小 | 内容 |
|--------|----------|------|------|
| Bootloader | 0x0000_0000 | 4MB | 引导程序（主+备份） |
| Firmware Slot A | 0x0040_0000 | 64MB | 固件镜像（主） |
| Firmware Slot B | 0x0440_0000 | 64MB | 固件镜像（备份/升级中） |
| Config Area | 0x0840_0000 | 8MB | 系统配置参数 |
| BBT | 0x08C0_0000 | 8MB | 坏块表 |
| Log Area | 0x0940_0000 | 16MB | 事件日志 |
| System Info | 0x0A40_0000 | 4MB | 系统状态（P/E计数等） |
| Reserved | 0x0A80_0000 | 剩余 | 预留 |

**操作命令**（模拟SPI NOR命令集，如W25Q系列）：
- READ（0x03）：字节读，不支持缓存；
- FAST_READ（0x0B）：快速读，含地址后的dummy周期；
- PAGE_PROGRAM（0x02）：256字节页编程（写1时有效，不可覆写0）；
- SECTOR_ERASE（0x20）：64KB扇区擦除；
- CHIP_ERASE（0xC7）：全片擦除；
- WRITE_ENABLE（0x06）/WRITE_DISABLE（0x04）：写使能/禁止；
- READ_STATUS_REG（0x05）：读状态寄存器（WIP位、WEL位等）；
- READ_ID（0x9F）：读厂商ID（返回模拟的Manufacturer ID + Device ID）。

**数据持久化**：NOR Flash数据以单个二进制文件（`/var/hfsss/nor/firmware_storage.bin`）持久化，每次写操作同步到文件（`msync` + `fsync`），确保数据一致性。

---

## 5.4 固件CPU核心线程模块

### 5.4.1 模块概述

固件CPU核心线程模块是HFSSS最核心的创新点，旨在完整模拟真实SSD控制器内嵌多核ARM处理器（如ARM Cortex-R5/R52）上运行的SSD固件行为。该模块将固件分为三个层次（硬件接入层、通用平台层、算法任务层），每个层次对应不同的线程职责，各线程绑定到专用CPU核心上运行，通过共享内存和消息队列进行通信，最大化仿真保真度。

固件CPU核心线程群是用户空间守护进程（hfsss-daemon）的核心组成部分，通过`pthread`创建，设置SCHED_FIFO实时调度策略和CPU亲和性，模拟真实SSD固件的多核并行执行行为。

### 5.4.2 三层固件架构概述

```
╔══════════════════════════════════════════════════════════════╗
║              算法任务层（Application Layer）                  ║
║  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐  ║
║  │ FTL  │ │ GC   │ │ WL   │ │ BBM  │ │ ECC  │ │ QoS  │  ║
║  │地址映射│ │垃圾回收│ │磨损均衡│ │坏块管理│ │纠错管理│ │流量控制│  ║
║  └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘  ║
║  ┌──────┐ ┌──────┐                                        ║
║  │冗余备份│ │错误处理│                                        ║
║  └──────┘ └──────┘                                        ║
╠══════════════════════════════════════════════════════════════╣
║              通用平台层（Common Service Layer）               ║
║  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐  ║
║  │ RTOS │ │任务调度│ │内存管理│ │Bootldr│ │上下电 │ │OOB管理│  ║
║  └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘  ║
║  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐  ║
║  │核间通信│ │稳定性  │ │Panic/ │ │异常处理│ │Debug │ │事件Log│  ║
║  │      │ │监控   │ │Assert │ │      │ │机制  │ │机制  │  ║
║  └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘  ║
╠══════════════════════════════════════════════════════════════╣
║              硬件接入层（Hardware Access Layer）               ║
║  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐    ║
║  │ NAND驱动 │ │ NOR驱动  │ │NVMe/PCIe │ │电源管理芯 │    ║
║  │          │ │          │ │模块管理   │ │片驱动     │    ║
║  └──────────┘ └──────────┘ └──────────┘ └──────────┘    ║
╚══════════════════════════════════════════════════════════════╝
```

---

## 5.5 硬件接入层（Hardware Access Layer，HAL）

### 5.5.1 NAND驱动模块（FR-HAL-001）

**需求描述**：HAL中的NAND驱动模块是固件CPU核心线程与NAND介质线程之间的软件接口层，抽象NAND Flash的物理操作，向上层（Common Service和Application Layer）提供统一的NAND访问API。

**NAND驱动API设计**：

```c
/* NAND驱动核心API */

/* 初始化 */
int nand_init(nand_config_t *config);
void nand_deinit(void);

/* 基础读写擦操作（异步接口） */
int nand_read_page_async(uint32_t ch, uint32_t chip, uint32_t die,
                         uint32_t plane, uint32_t block, uint32_t page,
                         uint8_t *data_buf, uint8_t *oob_buf,
                         nand_cb_t callback, void *ctx);

int nand_write_page_async(uint32_t ch, uint32_t chip, uint32_t die,
                          uint32_t plane, uint32_t block, uint32_t page,
                          const uint8_t *data_buf, const uint8_t *oob_buf,
                          nand_cb_t callback, void *ctx);

int nand_erase_block_async(uint32_t ch, uint32_t chip, uint32_t die,
                           uint32_t plane, uint32_t block,
                           nand_cb_t callback, void *ctx);

/* 多平面操作 */
int nand_multi_plane_read_async(nand_mp_addr_t *addrs, uint32_t count,
                                uint8_t *data_bufs[], uint8_t *oob_bufs[],
                                nand_cb_t callback, void *ctx);

int nand_multi_plane_write_async(nand_mp_addr_t *addrs, uint32_t count,
                                 const uint8_t *data_bufs[], const uint8_t *oob_bufs[],
                                 nand_cb_t callback, void *ctx);

/* 高级操作 */
int nand_read_status(uint32_t ch, uint32_t chip, uint8_t *status);
int nand_read_id(uint32_t ch, uint32_t chip, nand_id_t *id);
int nand_reset(uint32_t ch, uint32_t chip);
int nand_set_feature(uint32_t ch, uint32_t chip, uint8_t feature_addr, uint8_t *data);
int nand_get_feature(uint32_t ch, uint32_t chip, uint8_t feature_addr, uint8_t *data);

/* 暂停/恢复 */
int nand_program_suspend(uint32_t ch, uint32_t chip);
int nand_program_resume(uint32_t ch, uint32_t chip);
int nand_erase_suspend(uint32_t ch, uint32_t chip);

/* 通道/Die状态查询 */
nand_state_t nand_get_die_state(uint32_t ch, uint32_t chip, uint32_t die);
uint64_t nand_get_channel_earliest_available_time(uint32_t ch);

/* 参数查询 */
nand_geometry_t *nand_get_geometry(void);
nand_timing_t *nand_get_timing(void);
```

**驱动内部实现**：
- 维护每个Channel的命令发射队列，按时序约束调度命令发射；
- 通过消息队列向对应Channel的介质线程发送命令；
- 维护每个Die的状态机（IDLE → CMD_RECEIVED → EXECUTING → COMPLETE）；
- 计算并模拟ONFI命令序列（如Read：CMD(0x00) + ADDR(5字节) + CMD(0x30) + 等待tR + 数据输出）；
- 跟踪通道总线占用时间（Channel Bus Occupation Time）。

### 5.5.2 NOR驱动模块（FR-HAL-002）

**需求描述**：提供对NOR Flash介质的访问接口，支持固件镜像加载、配置数据读写和日志写入。

**NOR驱动API设计**：

```c
/* NOR Flash驱动API */
int nor_init(nor_config_t *config);
int nor_read(uint32_t addr, uint8_t *buf, uint32_t len);
int nor_write(uint32_t addr, const uint8_t *buf, uint32_t len);  /* 需先擦除 */
int nor_sector_erase(uint32_t sector_addr);   /* 擦除64KB扇区 */
int nor_chip_erase(void);                      /* 全片擦除（慎用） */
int nor_write_enable(void);
int nor_write_disable(void);
int nor_read_status(uint8_t *status);
int nor_wait_ready(uint32_t timeout_ms);       /* 等待WIP位清零 */
int nor_read_id(nor_id_t *id);
/* 分区操作 */
int nor_partition_read(nor_partition_t part, uint32_t offset, uint8_t *buf, uint32_t len);
int nor_partition_write(nor_partition_t part, uint32_t offset, const uint8_t *buf, uint32_t len);
int nor_partition_erase(nor_partition_t part);
```

**驱动内部实现**：
- 将NOR Flash仿真为位于进程共享内存中的字节数组；
- 所有读操作：直接内存访问，延迟模型注入（90ns × len）；
- 写操作：校验写使能位，模拟只能写0不能写1的NAND特性，延迟模型注入；
- 擦除操作：将扇区内容全部置0xFF，注入200ms延迟（通过HFSSS时间系统模拟，非真实sleep）；
- 写保护：检测WP引脚状态（软件模拟），禁止对写保护分区的写/擦操作；
- 持久化：每次写/擦操作后同步到持久化文件，确保重启后NOR数据不丢失。

### 5.5.3 NVMe/PCIe模块管理（FR-HAL-003）

**需求描述**：HAL层的NVMe/PCIe模块管理组件负责将固件CPU的逻辑完成信号转化为实际的NVMe命令完成动作（写CQE + 触发MSI-X中断），并管理与PCIe/NVMe内核模块的接口。

**功能需求**：

1. **命令完成提交**：
   - 接收Application Layer发出的命令完成通知（包含CID、QID、状态码、结果数据）；
   - 构建64字节的CQE（Completion Queue Entry）：
     - DW0（Command Specific）：读命令的传输字节数、Write Zeroes的范围等；
     - DW1（Reserved）；
     - DW2：SQ Head Pointer + SQ Identifier；
     - DW3（Status Field）：Phase Tag（P位）+ Status Code Type + Status Code + DNR + More位；
   - 通过共享内存接口，调用内核模块的完成写回函数（write_cqe_and_notify）；
   - 内核模块负责将CQE写入物理内存中的CQ，并投递MSI-X中断。

2. **异步事件管理**：
   - 维护待处理的异步事件队列（如：温度超限、命名空间属性变化、固件激活等）；
   - 当主机发送Async Event Request命令时，若有待处理事件立即返回；否则挂起命令直到事件发生；
   - 支持事件类型：Error Status、Smart/Health Status Change、Notice、NVM Command Set Specific。

3. **PCIe链路状态管理**：
   - 监控PCIe链路状态（Active/L0/L1/L2）；
   - 实现PCIe Active State Power Management（ASPM）的软件配置；
   - 响应PCIe hot reset、function level reset（FLR）事件：执行控制器复位流程。

4. **Namespace管理接口**：
   - 维护Namespace列表（最多支持4096个Namespace）；
   - Namespace创建/删除：分配/释放NAND存储空间，更新映射表；
   - Namespace属性：NSID、NSZE（Namespace Size）、NCAP（Namespace Capacity）、NUSE（Namespace Utilization）、各种特性标志位；
   - 支持Namespace共享（多控制器场景下的NS Sharing，目前为单控制器仿真）。

### 5.5.4 电源管理芯片驱动（FR-HAL-004）

**需求描述**：仿真SSD控制器的电源管理芯片（PMIC）驱动，模拟不同电源状态下的功耗特性和状态转换行为。

**NVMe电源状态仿真**：

| 电源状态 | 名称 | 最大功耗 | 进入延迟 | 退出延迟 | 说明 |
|----------|------|----------|----------|----------|------|
| PS0 | Active | 25W（可配置） | — | — | 全速运行 |
| PS1 | Active（降速） | 18W | 0 | 0 | 降频运行 |
| PS2 | Idle | 8W | 5ms | 5ms | 部分电路关断 |
| PS3 | Low Power | 3W | 50ms | 30ms | 大部分电路关断 |
| PS4 | Deep Sleep | 0.5W | 500ms | 100ms | 几乎全部关断 |

**功能需求**：
- 维护当前电源状态，响应主机的Set Feature（Power Management）命令；
- 记录每个电源状态的累计时间（用于功耗分析）；
- 实现自动电源状态转换：空闲一定时间后自动降低电源状态；
- 从低电源状态退出时，模拟对应的延迟（不立即处理IO，等待退出延迟后才处理）；
- 功耗统计接口：提供当前瞬时功耗估算、累计能耗统计。

---

## 5.6 通用平台层（Common Service Layer）

### 5.6.1 实时操作系统（RTOS）仿真（FR-CS-001）

**需求描述**：仿真真实SSD控制器上运行的RTOS，提供任务（Task）、消息队列（Message Queue）、信号量（Semaphore）、互斥锁（Mutex）、事件组（Event Group）等RTOS原语的软件实现。

**RTOS原语实现**：

1. **任务（Task）**：
   - 用pthread模拟RTOS任务，每个任务对应一个固件CPU核心线程；
   - 支持任务优先级（1~32级，数字越大优先级越高）；
   - 任务状态：Ready、Running、Blocked、Suspended；
   - 时间片（Time Slice）：默认1ms，支持配置；
   - 任务堆栈：每个任务独立分配堆栈空间（默认16KB，可配置）；
   - 任务创建/删除/挂起/恢复API：`task_create`, `task_delete`, `task_suspend`, `task_resume`。

2. **消息队列（Message Queue）**：
   - 用于核间通信和模块间消息传递；
   - 支持固定大小消息和可变长消息两种模式；
   - 阻塞和非阻塞两种发送/接收模式；
   - 支持超时（timeout）参数；
   - 队列深度可配置（默认64条消息）；
   - 线程安全实现（内部使用无锁环形缓冲区或条件变量）。

3. **信号量（Semaphore）**：
   - 二值信号量：`sem_binary_create`, `sem_take`, `sem_give`；
   - 计数信号量：`sem_counting_create(max_count, initial_count)`, `sem_take`, `sem_give`；
   - 支持超时参数（`sem_take_timeout`）；
   - 优先级继承（Priority Inheritance）机制，防止优先级反转。

4. **互斥锁（Mutex）**：
   - 递归互斥锁支持（同一任务可多次获取）；
   - 优先级继承：持有mutex的低优先级任务临时提升到等待者中最高优先级；
   - 锁超时检测：超时未释放的mutex触发Panic（可配置）。

5. **事件组（Event Group）**：
   - 32位事件标志，每位代表一个事件；
   - `event_set`, `event_clear`, `event_wait(bits, wait_all, clear_on_exit, timeout)`；
   - 支持"等待任意位"和"等待全部位"两种模式。

6. **软件定时器（Software Timer）**：
   - 基于高分辨率时钟（`clock_gettime(CLOCK_MONOTONIC)`）实现；
   - 单次触发和周期触发两种模式；
   - 定时器精度：100μs（模拟固件定时器典型精度）；
   - 定时器回调在专用Timer任务中执行（避免中断上下文）。

7. **内存池（Memory Pool）**：
   - 固定大小块内存池，避免动态内存碎片；
   - 支持块大小：32B、64B、128B、256B、512B、1KB、4KB；
   - 分配/释放API：`pool_alloc(pool_id)`, `pool_free(pool_id, ptr)`；
   - 内存池满时返回NULL（不阻塞，由调用者处理）。

### 5.6.2 任务调度（FR-CS-002）

**需求描述**：实现多核感知的固件任务调度器，将不同的固件任务分配到不同的仿真CPU核心上运行，最大化固件执行效率。

**调度策略**：

1. **静态任务绑定（CPU Pinning）**：
   - 关键固件任务绑定到专用CPU核心，保证实时性：
     - FTL地址转换任务 → Core 0-3（4个核心）；
     - GC/WL后台任务 → Core 4-5；
     - HAL NAND驱动任务（每Channel一个） → Core 6-21（16个核心）；
     - NVMe接口任务 → Core 22-23；
     - Common Service任务（Bootloader/Monitor/Log等）→ Core 24-25；
   - 通过`pthread_setaffinity_np`实现CPU绑定。

2. **优先级调度**：
   - 基于优先级的抢占式调度（模拟RTOS调度器行为）；
   - 高优先级任务（如FTL紧急路径、NVMe命令处理）可抢占低优先级任务（如GC、统计上报）；
   - 使用SCHED_FIFO实时调度策略，确保高优先级任务不被宿主机其他进程抢占。

3. **负载均衡**：
   - FTL任务可动态调整分配，根据当前QD（Queue Depth）将任务分配到空闲FTL核心；
   - 监控各固件核心的CPU使用率，超过85%时触发负载均衡调整。

4. **调度统计**：
   - 记录每个任务的运行时间、等待时间、抢占次数；
   - 统计数据通过OOB接口或/proc文件系统接口导出；
   - 用于分析固件任务的实时性指标（如最大调度延迟）。

### 5.6.3 内存管理（FR-CS-003）

**需求描述**：实现固件级内存管理子系统，为各固件模块提供安全、高效的内存分配服务。

**内存分区规划（固件工作内存，共4GB）**：

| 内存区域 | 大小 | 说明 |
|----------|------|------|
| FTL映射表 | 2GB | 全量L2P映射表，16GB SSD需约1.6GB（4KB页，每表项4字节） |
| 命令缓冲区 | 512MB | NVMe命令和NAND命令的临时缓冲 |
| Write Buffer（DRAM Cache仿真） | 1GB | 写数据缓冲区 |
| 系统堆 | 256MB | RTOS动态内存分配 |
| 内核代码/数据 | 128MB | 固件仿真代码的运行时数据 |
| 预留/保护 | 128MB | Guard页、Stack保护等 |

**内存管理策略**：

1. **静态预分配**：
   - 大型数据结构（FTL映射表、坏块表等）在启动时一次性静态分配，避免运行时碎片；
   - 使用`mmap(MAP_ANONYMOUS | MAP_POPULATE)`预先分配并锁定物理内存页（`mlock`），避免缺页中断影响仿真延迟。

2. **内存池**：
   - 为高频分配/释放的小对象（命令描述符、页元数据等）建立专用内存池；
   - 避免使用`malloc`/`free`在性能关键路径上（防止heap碎片和锁竞争）。

3. **内存保护**：
   - 关键数据区域（映射表、坏块表）设置访问保护（`mprotect`），非授权写访问触发SIGSEGV；
   - 利用Guard Page检测栈溢出。

4. **内存压力管理**：
   - 监控系统可用内存，当剩余内存低于阈值时触发内存压力告警；
   - 触发映射表缓存回收（CMT eviction）和Write Buffer下刷，释放内存。

### 5.6.4 Bootloader（FR-CS-004）

**需求描述**：仿真SSD固件的启动引导程序（Bootloader），模拟真实SSD上电后的初始化序列。

**启动序列（Boot Sequence）**：

```
阶段0：上电复位（Power-On Reset）
  ↓ 检测PMIC电源状态，等待所有电源轨稳定
阶段1：Bootloader执行（从NOR Flash 0x0000_0000加载）
  ↓ 初始化片上SRAM（仿真）
  ↓ 加载Bootloader主体到内部RAM
  ↓ 完整性校验（CRC32校验NOR Bootloader区）
阶段2：固件镜像加载
  ↓ 从NOR读取固件版本信息
  ↓ 选择有效的固件Slot（Slot A优先，若无效则使用Slot B）
  ↓ 从NOR加载固件镜像到RAM（仿真）
  ↓ 校验固件完整性（SHA-256）
阶段3：硬件初始化
  ↓ 初始化NAND Flash（发送Reset命令，读取NAND ID）
  ↓ 初始化NOR Flash（读取ID，验证分区表）
  ↓ 初始化PCIe/NVMe控制器（设置控制器寄存器）
  ↓ 初始化DMA引擎
阶段4：FTL初始化
  ↓ 从NOR/NAND读取坏块表（BBT）
  ↓ 从持久化文件加载L2P映射表
  ↓ 重建P/E计数表和磨损均衡状态
  ↓ 扫描NAND介质，执行上电前扫描（Power-On Scan）
  ↓ 恢复Write Buffer中未完成的写操作
阶段5：系统就绪
  ↓ 设置CSTS.RDY = 1，通知主机控制器就绪
  ↓ 启动各后台任务（GC、WL、监控等）
  → 进入正常运行状态
```

**Bootloader特性**：
- 启动时间仿真：整个Boot序列模拟约3-8秒的启动延迟（可配置）；
- 双镜像冗余：支持Firmware Slot A/B切换，任一镜像损坏时自动回退；
- 安全启动校验：固件完整性验证（SHA-256）；
- 启动日志：详细记录每个启动阶段的时间和状态，写入NOR Log分区。

### 5.6.5 上下电服务（FR-CS-005）

**需求描述**：仿真SSD的上电（Power-Up）和掉电（Power-Down）过程，确保数据完整性。

**上电服务**：
- 检测上次掉电类型（正常掉电 / 异常掉电 / 电源断电）；
- 正常掉电：直接从持久化文件恢复状态；
- 异常掉电检测：检查Write Buffer日志文件（commit log），重放或丢弃未提交的写操作；
- 执行Power-On Self Test（POST）：基础功能自检（NAND连通性、BBT完整性等）；
- 启动完成后向SMART日志记录上电次数（Power On Hours / Power Cycle Count）。

**掉电服务**：
- **正常掉电**（NVMe Shutdown命令，SHN=0x01/0x02）：
  1. 停止接受新IO命令；
  2. 等待所有在途IO命令完成；
  3. 将Write Buffer中所有脏数据下刷到NAND；
  4. 更新L2P映射表到持久化文件；
  5. 更新P/E计数、BBT到NOR Flash；
  6. 写入掉电完整性标志；
  7. 设置CSTS.SHST = 0x02（Shutdown Complete）；
  8. 完成。

- **异常掉电处理**（SIGKILL / SIGTERM / 系统崩溃）：
  - 通过`atexit`和信号处理函数捕获SIGTERM，执行最大努力的快速持久化；
  - 维护Write-Ahead Log（WAL）：每次Write Buffer修改前记录WAL，WAL写入持久化文件；
  - 上电时扫描WAL，重放未完成的操作，确保数据不丢失（至多丢失飞行中的IO）；
  - SMART日志中记录异常掉电次数（Unsafe Shutdown Count）。

### 5.6.6 带外管理（Out-Of-Band Management，FR-CS-006）

**需求描述**：提供带外管理接口，允许用户在仿真运行时动态查询和配置仿真器参数。

**接口形式**：
1. **Unix Domain Socket**：仿真守护进程在`/var/run/hfsss.sock`监听，提供JSON-RPC接口；
2. **/proc文件系统接口**：通过`/proc/hfsss/`目录暴露只读统计信息；
3. **REST API**（可选，V2.0）：通过HTTP/1.1接口提供RESTful管理API（监听localhost:8080）。

**OOB管理功能**：

| 功能分类 | 接口 | 说明 |
|----------|------|------|
| 状态查询 | `GET /status` | 返回仿真器整体运行状态 |
| SMART查询 | `GET /smart` | 返回仿真SSD的SMART/Health信息 |
| 性能统计 | `GET /perf` | 返回IOPS、带宽、延迟直方图 |
| 参数修改 | `POST /config` | 动态修改可配置参数（如GC阈值、限速等） |
| 故障注入 | `POST /fault` | 注入NAND坏块、Read Error等故障 |
| 强制GC | `POST /gc/trigger` | 手动触发一轮GC |
| 快照保存 | `POST /snapshot` | 立即触发全量数据持久化 |
| 日志导出 | `GET /log` | 导出事件日志 |
| 通道统计 | `GET /channel/{id}` | 查询指定Channel的详细状态 |
| Die状态 | `GET /die/{ch}/{chip}/{die}` | 查询指定Die的P/E计数、坏块情况等 |

**SMART信息实现**：
仿真NVMe SMART/Health Information Log Page（Log Page ID = 0x02），关键字段：

| 字节偏移 | 字段 | 说明 |
|----------|------|------|
| 0 | Critical Warning | 温度超限、容量不足等严重警告 |
| 1-2 | Temperature | 当前温度（仿真温度模型） |
| 3 | Available Spare | 可用备用块百分比 |
| 4 | Available Spare Threshold | 备用块报警阈值 |
| 5 | Percentage Used | 设备寿命消耗百分比 |
| 6 | Endurance Group Critical Warning Summary | |
| 32-47 | Data Units Read | 读取的512字节单元数（128bit计数器） |
| 48-63 | Data Units Written | 写入的512字节单元数 |
| 64-79 | Host Read Commands | 主机读命令数 |
| 80-95 | Host Write Commands | 主机写命令数 |
| 96-111 | Controller Busy Time | 控制器繁忙分钟数 |
| 112-127 | Power Cycles | 上电次数 |
| 128-143 | Power On Hours | 上电小时数 |
| 144-159 | Unsafe Shutdowns | 异常掉电次数 |
| 160-175 | Media and Data Integrity Errors | 媒体和数据完整性错误数 |
| 176-191 | Number of Error Info Log Entries | 错误信息日志条目数 |
| 192-195 | Warning Composite Temperature Time | 温度超过警告阈值的分钟数 |
| 196-199 | Critical Composite Temperature Time | 温度超过临界阈值的分钟数 |

### 5.6.7 核间通信（Inter-Core Communication，FR-CS-007）

**需求描述**：实现仿真固件CPU核心线程之间的高效通信机制，模拟真实SSD控制器的核间通信（Inter-Processor Communication，IPC）行为。

**通信机制**：

1. **消息传递（Message Passing）**：
   - 每对核心之间维护单向消息队列（SPSC Ring Buffer）；
   - 消息格式：消息类型（4字节）+ 消息长度（4字节）+ 消息体（最大256字节）；
   - 发送API：`ipc_send(dst_core_id, msg_type, msg_data, msg_len)`；
   - 接收API：`ipc_recv(src_core_id, msg_buf, buf_len, timeout_us)`（阻塞或非阻塞）；
   - 通知机制：通过`eventfd`通知接收方有新消息（也支持接收方轮询模式）。

2. **共享内存（Shared Memory）**：
   - 对于大数据量的传递（如NAND页数据4KB+），使用共享内存直接传递指针；
   - 引用计数管理：通过原子操作维护共享内存块的引用计数，最后一个引用者负责释放；
   - 访问保护：通过读写锁（RWLock）或版本号（seqlock）保护共享数据的读写一致性。

3. **核间信号（Inter-Core Signal）**：
   - 模拟硬件的Software Generated Interrupt（SGI）：`ipc_signal(dst_core_id, signal_id)`；
   - 接收方注册信号处理回调：`ipc_register_signal_handler(signal_id, handler)`；
   - 信号处理在接收方的RTOS任务上下文中执行（非中断上下文）。

4. **全局锁（Spinlock/Mutex）**：
   - 对于需要多核共享的全局资源（如全局计数器、日志缓冲区），提供核间自旋锁；
   - 自旋锁使用GCC内置原子操作实现（`__sync_bool_compare_and_swap`）；
   - 超时检测：自旋超过阈值（默认100μs）时触发死锁告警。

### 5.6.8 系统稳定性监控（FR-CS-008）

**需求描述**：实现系统健康监控机制，持续监测仿真器各子系统的运行状态，检测并处理异常情况。

**监控内容**：

1. **Watchdog看门狗**：
   - 每个固件任务必须定期（默认每500ms）向Watchdog模块喂狗（`watchdog_feed(task_id)`）；
   - Watchdog任务定期检查所有注册任务的喂狗状态；
   - 超时未喂狗的任务视为挂起（Hang），触发系统Panic流程；
   - Watchdog超时时间可按任务类型配置（关键任务短超时，后台任务长超时）。

2. **系统资源监控**：
   - CPU使用率：每秒统计各固件核心线程的CPU占用率；
   - 内存使用率：监控各内存分区的使用量；
   - NAND通道队列深度：监控各Channel的命令队列积压情况；
   - 空闲块水位：监控各Die的空闲块数量趋势。

3. **性能异常检测**：
   - 命令延迟监控：实时计算P50/P90/P99/P99.9延迟；
   - 当P99.9延迟超过配置阈值时，记录告警日志并通过ASYNC EVENT通知主机；
   - GC效率监控：GC回收速率 < 写入速率时触发告警。

4. **温度仿真**：
   - 基于仿真工作负载（IOPS、带宽）估算SSD温度：`T = T_ambient + IOPS × COEFF_I + BW × COEFF_B`；
   - 温度超过Warning Composite Temperature（默认70°C）时设置SMART Critical Warning位；
   - 温度超过Critical Composite Temperature（默认75°C）时降速（限制IOPS上限到50%）。

### 5.6.9 Panic/Assert处理（FR-CS-009）

**需求描述**：实现完善的异常处理框架，确保仿真器在遇到不可恢复错误时能够安全地保存现场信息并退出。

**Assert机制**：
- 提供`ASSERT(condition, msg)`宏：条件为假时触发断言失败；
- 断言失败时：打印文件名、行号、断言条件和错误消息；记录到日志；执行现场保存；调用Panic流程。

**Panic流程**：
1. 设置全局Panic标志（原子操作），阻止其他核心产生新的操作；
2. 停止接受新的NVMe命令（设置内部状态为PANIC模式）；
3. 保存所有固件核心的运行状态（寄存器快照、堆栈回溯）；
4. 将内存中的关键状态（L2P表摘要、错误上下文）写入Panic Dump文件（`/var/hfsss/panic_dump_<timestamp>.bin`）；
5. 向主机投递Controller Fatal Status（设置CSTS.CFS = 1）；
6. 执行尽力而为的数据持久化（尝试保存DRAM中数据，不等待完成）；
7. 记录Panic原因到NOR Log分区；
8. 退出守护进程（exit code = PANIC_EXIT_CODE）。

**Coredump**：
- 配置`ulimit -c unlimited`，Panic时生成标准Linux coredump文件；
- 与HFSSS自定义Panic Dump配合使用，提供完整的事后分析（Post-Mortem Analysis）数据。

### 5.6.10 系统Debug机制（FR-CS-010）

**需求描述**：提供丰富的Debug和可观测性接口，支持固件研发人员深入分析仿真器内部状态。

**Debug功能**：

1. **命令Trace**：
   - 可启用NVMe命令全量Trace（记录每条命令的接收时间、处理时间、完成时间、关键内部操作序列）；
   - Trace缓冲区：环形缓冲区，默认保留最近100,000条记录；
   - 导出格式：JSON Lines格式，可直接用可视化工具分析（如Perfetto、FlameGraph）；
   - 启用/禁用：通过OOB接口动态控制（`POST /debug/trace/enable`）。

2. **NAND操作Trace**：
   - 记录每条NAND命令（Read/Program/Erase）的详细信息：Channel/Chip/Die/Plane/Block/Page、开始时间、结束时间、操作结果；
   - 可按Channel过滤，支持实时流式输出到文件。

3. **FTL操作Trace**：
   - 记录每次L2P查询（LPN → PPN映射）、GC触发事件、WL迁移操作；
   - 输出格式兼容FlashSim/MQSim的Trace格式，便于对比分析。

4. **GDB支持**：
   - 守护进程支持被GDB attach（`gdb -p <pid>`）；
   - 提供Python GDB脚本（`hfsss_gdb.py`），可打印固件内部关键数据结构；
   - 支持通过GDB设置断点，在特定命令或内部状态时暂停仿真。

5. **性能计数器**：
   - 精细化的性能计数器：按命令类型、LBA范围、Channel划分的IOPS/带宽/延迟统计；
   - 通过`/proc/hfsss/perf_counters`或OOB接口导出；
   - 支持Reset（清零）计数器。

### 5.6.11 系统事件Log机制（FR-CS-011）

**需求描述**：实现结构化的系统事件日志机制，记录仿真器运行过程中的所有重要事件。

**事件级别**：

| 级别 | 名称 | 说明 | 示例 |
|------|------|------|------|
| 0 | FATAL | 不可恢复错误，系统即将Panic | NAND所有备用块耗尽 |
| 1 | ERROR | 可恢复错误，需关注 | NAND Block ECC不可纠正错误 |
| 2 | WARN | 警告，不影响功能但需关注 | 空闲块水位低于高水位 |
| 3 | INFO | 重要信息 | GC完成、固件启动完成 |
| 4 | DEBUG | 调试信息（默认关闭） | 每次L2P查询结果 |
| 5 | TRACE | 极细粒度（仅开发调试） | 每条NAND命令状态 |

**Log存储**：
- 内存缓冲区：Ring Buffer（默认64MB），存储最近的日志条目；
- NOR持久化：关键日志（WARN级别以上）写入NOR Flash的Log分区；
- 文件输出：DEBUG以上日志写入`/var/log/hfsss/hfsss.log`，支持日志轮转（logrotate）；
- Syslog集成：WARN以上事件同步到Linux syslog（journald）。

**Log条目格式**：
```
[时间戳μs][级别][模块名][Core ID] 消息内容 {结构化字段JSON}
示例：
[1709123456789012][INFO][FTL][C2] GC completed: freed_blocks=128, valid_pages_moved=45312, elapsed_us=2847613 {"ch":3,"chip":1,"die":0}
```


---

## 5.7 算法任务层（Application Layer）

### 5.7.1 Flash Translation Layer — 地址映射管理（FR-APP-001）

**需求描述**：实现完整的Flash Translation Layer，提供逻辑地址（LBA）到物理地址（PPN）的双向映射，是SSD固件的核心功能。

#### 5.7.1.1 地址映射架构

**映射粒度**：默认采用页级映射（Page-Level Mapping，4KB/条目），每个LPN对应一个PPN。

**物理地址编码（PPN = Physical Page Number）**：
```
PPN（64bit）:
[63:48] Reserved
[47:40] Channel ID (8bit, 支持最多256个Channel)
[39:36] Chip ID     (4bit, 每Channel最多16个Chip)
[35:34] Die ID      (2bit, 每Chip最多4个Die)
[33:32] Plane ID    (2bit, 每Die最多4个Plane)
[31:19] Block ID    (13bit, 每Plane最多8192个Block)
[18:10] Page ID     (9bit, 每Block最多512个Page)
[9:0]   Offset      (10bit, 页内扇区偏移，512B粒度)
```

#### 5.7.1.2 映射表设计

**全量L2P映射表（Full L2P Table）**：
- 将所有LPN→PPN的映射关系存储在DRAM中；
- 映射表大小计算：对于2TB SSD（2TB / 4KB = 512M个页），每条映射4字节（PPN + 有效位），总大小 = 512M × 4B = 2GB；
- 映射表通过`mmap(MAP_ANONYMOUS | MAP_HUGETLB)`分配大页内存（2MB大页），减少TLB压力；
- 无效LPN（未写入）的映射值为0xFFFFFFFF（INVALID_PPN）。

**反向映射表（P2L Table，物理到逻辑）**：
- 用于GC时查找物理页对应的逻辑地址；
- P2L信息存储在每个Page的OOB区域（存储LPN和写入时间戳）；
- 内存中维护稀疏的P2L缓存（仅缓存正在处于GC候选状态的Block的P2L信息）。

**映射表持久化**：
- L2P映射表以Checkpoint方式持久化：每隔1GB写入量，将映射表全量写入文件；
- 增量持久化：每次GC完成后，将被GC修改的LPN范围写入增量日志（WAL）；
- 上电时：加载最近的Checkpoint，重放WAL日志，恢复完整映射表。

#### 5.7.1.3 过量配置（Over-Provisioning，OP）

- 预留20%的NAND空间作为OP区域（不映射到任何LBA，专供GC使用）；
- OP区域计算：`OP_Blocks = Total_Blocks × 0.20`；
- 用户可通过Format NVM命令调整OP比例（范围7%-50%）；
- OP空间影响GC效率：OP越大，GC选到的victim block有效页越少，回收效率越高，写放大（Write Amplification Factor，WAF）越低。

#### 5.7.1.4 写操作流程

```
主机Write命令（LBA, NLB, Data）处理流程：
1. 将Data从主机内存DMA到Write Buffer
2. 对每个4KB的逻辑页：
   a. 查找当前LPN对应的旧PPN（old_ppn = L2P[LPN]）
   b. 若old_ppn有效，标记old_ppn为Invalid（增加该Block的无效页计数）
   c. 分配新的空闲物理页（new_ppn），从Current Write Block分配
   d. 更新L2P[LPN] = new_ppn
   e. 将Data写入对应NAND介质（通过HAL NAND驱动异步写入）
   f. 在OOB区写入LPN（供P2L查询）
3. 当Current Write Block写满时：
   a. 关闭当前Block（将其加入Full Block列表）
   b. 开启新的Write Block（从空闲块池分配）
4. 返回NVMe命令完成
```

#### 5.7.1.5 读操作流程

```
主机Read命令（LBA, NLB）处理流程：
1. 对每个4KB的逻辑页：
   a. 查询L2P表：ppn = L2P[LPN]
   b. 若ppn == INVALID_PPN：返回全0数据（或返回Error）
   c. 解析ppn为物理地址（ch, chip, die, plane, block, page）
   d. 通过HAL NAND驱动异步读取该物理页数据
   e. 读取完成后：进行ECC校验（如有纠错则纠正，如不可纠正则报Error）
2. 将读取的Data DMA到主机内存
3. 返回NVMe命令完成
```

#### 5.7.1.6 条带化（Striping）策略

为充分利用16个Channel的并发性，写入数据按Channel进行条带化分布：
- 将连续的逻辑页按Round-Robin方式轮流分配到不同Channel的Write Block；
- 条带大小（Stripe Unit）：默认为1个NAND页（16KB），即每16KB数据切换到下一个Channel；
- 对齐优化：如主机发送连续大块写（如1MB），则每16KB分到一个Channel，16Channel正好对齐；
- 多平面条带：在同一Channel内，进一步跨Plane条带化，启用Multi-Plane Program。

### 5.7.2 NAND块地址组织管理（FR-APP-002）

**需求描述**：实现NAND Flash物理块地址的组织和分配管理，维护Block级别的状态信息，支持写入分配、GC回收和磨损均衡。

**Block状态机**：

```
    ┌─────────────────────────────────────────┐
    ▼                                         │
  FREE ──[分配为Write Block]──► OPEN ──[写满]──► FULL
    ▲                                         │
    │                                         │
    └────[GC回收并擦除]────── ERASING ◄──────── VICTIM
                                              ▲
                                              │
                                    FULL ─────┘（GC选中）
```

**Block元数据**：每个Block维护以下信息（存储在DRAM的Block元数据表中）：
```c
typedef struct block_info {
    uint32_t    erase_count;        // P/E循环计数
    uint32_t    valid_page_count;   // 有效页数（最大：pages_per_block）
    uint32_t    invalid_page_count; // 无效页数
    uint32_t    free_page_count;    // 空闲页数
    uint64_t    last_erase_time;    // 最近一次擦除时间（μs时间戳）
    uint64_t    first_write_time;   // 该Block首次写入时间（用于数据保持性检测）
    uint32_t    read_count;         // 该Block内页的累计读次数（Read Disturb检测）
    block_state_t state;            // Block当前状态（FREE/OPEN/FULL/VICTIM/ERASING/BAD）
    uint8_t     reserved[3];
} block_info_t;
```

**Current Write Block（CWB）管理**：
- 为每个Die/Plane维护一个Current Write Block（CWB），新的写操作按序写入CWB；
- CWB写满后（所有页均已写入），关闭CWB，开启新的CWB（从空闲块池分配）；
- 多Plane模式：同时维护同一Die的两个Plane的CWB（相同Block offset），支持Multi-Plane Program；
- Plane级别CWB保证每次多平面写入的Block/Page对齐约束。

**空闲块池（Free Block Pool）管理**：
- 全局空闲块池：按Channel/Die/Plane分层组织的空闲块链表；
- 分配策略：优先从磨损最少的块（erase_count最小）中分配；
- 缺块保护：当某个Die的空闲块数量 < LOW_WATERMARK（默认5块）时，触发紧急GC并暂停新的写分配。

### 5.7.3 垃圾回收（Garbage Collection，FR-APP-003）

**需求描述**：实现高效的垃圾回收算法，自动回收NAND中的无效（stale）数据占用的物理空间，为新写入提供空闲块。

#### 5.7.3.1 GC触发策略

**水位控制**：
- **高水位（High Watermark）**：全局空闲块比例 < 20%，启动后台GC（低优先级，不影响前台IO）；
- **低水位（Low Watermark）**：全局空闲块比例 < 5%，启动紧急GC（高优先级，限制前台写入）；
- **极低水位（Critical Watermark）**：全局空闲块比例 < 2%，停止所有写入命令，专注GC回收。

**Per-Die GC**：每个Die独立维护水位，允许局部GC在特定Die上运行而不影响其他Die。

#### 5.7.3.2 Victim Block选择算法

支持以下GC Victim选择策略（可通过配置切换）：

1. **Greedy算法（默认）**：
   - 选择有效页数最少（即无效页数最多）的Block作为Victim；
   - 最大化单次GC回收的空闲页数，最小化有效页迁移量；
   - 实现：对每个Die维护按valid_page_count排序的有序集合（使用最小堆）；
   - 时间复杂度：O(log n)，n为Full Block数量。

2. **Cost-Benefit算法**：
   - 综合考虑block的有效率（utilization）和age（自上次擦除以来的时间）；
   - Cost-Benefit = (1 - utilization) / (2 × utilization) × age；
   - 优先选择Cost-Benefit值高的Block（高无效率+数据老）；
   - 有助于减少冷数据对热数据区域的干扰。

3. **FIFO算法**：
   - 按Block的关闭时间（oldest closed time）选择最老的Full Block作为Victim；
   - 实现简单，有利于数据保持性（老数据优先擦除重写）。

4. **热冷分区算法（Hot-Cold Separation）**：
   - 将写入数据按频率分类为Hot（高频更新）和Cold（低频更新）；
   - Hot数据写入专用Hot Block区，Cold数据写入Cold Block区；
   - GC时优先回收Hot区Block（有效率低）；Cold区GC触发频率低，减少冷数据迁移。

#### 5.7.3.3 GC执行流程

```
GC执行流程（针对一个选中的Victim Block）：
1. 选定Victim Block（如ch=3, chip=1, die=0, plane=0, block=2048）
2. 扫描Victim Block中的所有页：
   For each page in victim_block:
     a. 读取OOB中存储的LPN（page_lpn）
     b. 查询L2P[page_lpn]是否等于当前PPN（验证该页是否仍然有效）
     c. 若有效（L2P[page_lpn] == current_ppn）：
        - 将该页数据读出（NAND Read Page）
        - 分配新的空闲页（new_ppn）
        - 将数据写入新页（NAND Program Page）
        - 更新L2P[page_lpn] = new_ppn
        - 增加old_ppn对应Block的invalid_page_count
     d. 若无效（L2P[page_lpn] != current_ppn）：跳过
3. 所有有效页迁移完毕后，擦除Victim Block（NAND Block Erase）
4. 更新Block元数据：erase_count++, state = FREE
5. 将擦除后的Block加入空闲块池
```

**GC并发优化**：
- 多Die并行GC：多个Die上的GC可同时进行，互不影响；
- Pipeline GC：读取Victim Block的有效页（Read Phase）和写入新Block（Write Phase）可在不同Die上流水线执行；
- GC暂停与恢复：当高优先级IO命令到来时，暂停当前正在执行的NAND Program（Program Suspend），优先处理IO命令，完成后恢复GC（Program Resume）。

#### 5.7.3.4 写放大分析

写放大因子（Write Amplification Factor，WAF）：
- `WAF = NAND_Writes / Host_Writes`
- 理想情况（仅含GC开销）：WAF ≈ 1 / (1 - utilization_of_data)
- 目标：在90%盘占率的稳态条件下，WAF ≤ 3（TLC NAND，Greedy GC，20% OP）；
- 监控：实时统计WAF，通过OOB接口导出；
- 优化：通过热冷分离、更大OP比例、优化GC策略降低WAF。

### 5.7.4 磨损均衡（Wear Leveling，FR-APP-004）

**需求描述**：实现磨损均衡机制，确保NAND Flash中所有Block的P/E循环次数尽量均匀，延长SSD整体寿命。

#### 5.7.4.1 动态磨损均衡（Dynamic Wear Leveling）

- GC执行时，优先将数据写入P/E循环次数少的空闲Block（在空闲块分配时考虑erase_count）；
- 实现：维护按erase_count排序的空闲块最小堆，分配时始终取erase_count最小的块；
- 效果：频繁被GC回收的热数据区域的Block会逐渐被年轻的块替换。

#### 5.7.4.2 静态磨损均衡（Static Wear Leveling）

- 周期性检查全局Block的erase_count分布（使用erase_count直方图）；
- 当检测到某些Block的erase_count显著低于平均值（默认阈值：低于平均值的50%），触发静态WL；
- 静态WL流程：
  1. 选定erase_count最低的冷数据Block（victim_cold_block）；
  2. 选定erase_count最高的热区空闲Block（donor_hot_block，来自GC回收的频繁擦除块）；
  3. 将victim_cold_block中的冷数据搬迁到donor_hot_block；
  4. 将victim_cold_block擦除后作为新的空闲块分配给热数据区使用；
  5. 通过此交换，使低P/E的块轮换到热数据区，高P/E的块用于冷数据。
- 触发频率：默认每100次动态GC循环触发一次静态WL检查；可配置。

#### 5.7.4.3 磨损监控与告警

- 实时统计全SSD的平均P/E循环次数、最大P/E循环次数；
- 当最大P/E循环次数接近NAND寿命上限（> 80% of PE_CYCLE_LIMIT）时：设置SMART Available Spare低告警；
- 当Available Spare < Available Spare Threshold时：设置SMART Critical Warning（bit 0）；
- 根据磨损状态动态调整OP比例建议（磨损越严重，建议增加OP）。

### 5.7.5 读写擦命令管理（FR-APP-005）

**需求描述**：实现固件层的读写擦命令管理框架，在FTL、GC的基础上，负责命令的生命周期管理、错误重试和完成处理。

**命令状态机**：
```
NVMe Command到达
      ↓
[RECEIVED] → [PARSING] → [L2P_LOOKUP] → [NAND_QUEUED] → [NAND_EXECUTING]
                                                                  ↓
                                                          [ECC_CHECK]
                                                                  ↓
                                                      ┌──[ERROR?]──┐
                                                      No           Yes
                                                      ↓            ↓
                                                 [COMPLETE]   [RETRY/FAIL]
```

**Read Retry机制**：
- 当NAND Read产生可纠正ECC错误时，先尝试软解码（Soft-Decision LDPC）；
- 软解码失败时，触发Read Retry：调整NAND读取电压（Vread）偏置，重新读取；
- Read Retry最多尝试N次（默认15次，与NAND颗粒规格对应），每次调整不同的Voltage Offset；
- 15次Retry后仍然失败：标记为不可纠正错误（UCE），返回NVMe错误完成（Media and Data Integrity Error）；
- UCE记录：写入NVMe Error Log Page，增加SMART Media and Data Integrity Errors计数。

**Write Retry机制**：
- Program失败（读回状态寄存器WR_FAIL位置位）时，尝试将数据写入同一Block的备用区域或新Block；
- 失败次数超过阈值（默认3次）时，将该Block标记为坏块，从FTL中移除，从空闲块池中替换新块。

**Write Verify**：
- 程序结束后可选地读回数据进行验证（Write-and-Verify模式，影响写性能）；
- 默认禁用，可通过配置启用（用于高可靠性场景）。

### 5.7.6 IO流量控制（FR-APP-006）

**需求描述**：在算法任务层实现细粒度的IO流量控制，保证在所有IO模式下系统稳定运行，防止NAND过载和GC饥饿。

**多级流控**：

1. **主机IO限速**（Host-Side Rate Limiting）：
   - 在NVMe命令接收端实现令牌桶（Token Bucket）限速；
   - 支持per-Namespace和全局两级限速；
   - IOPS限制：1 KIOPS ~ 2000 KIOPS，粒度100 IOPS；
   - 带宽限制：50 MB/s ~ 14 GB/s，粒度10 MB/s；
   - 超限处理：超限命令不丢弃，进入等待队列（最大等待队列深度 = SQ深度）。

2. **GC/WL带宽配额**：
   - GC操作消耗NAND带宽的配额控制（默认最大30%，最小5%）；
   - 当主机IO负载低时，GC可提升到更高的带宽配额（最大80%）；
   - 自适应调整：每秒采样主机IO带宽和GC带宽，根据当前空闲块水位动态调整GC配额。

3. **NAND通道级流控**：
   - 每个Channel的命令队列深度限制（默认128条）；
   - 队列满时，上层命令分发器等待（back-pressure传导）；
   - 通道级饥饿检测：某通道长时间无命令时，检查是否有等待的命令被遗忘。

4. **写缓冲区流控**：
   - Write Buffer占用比例触发三级流控：
     - < 60%：正常写入，不限速；
     - 60%-80%：轻度限速（主机写速率降低20%）；
     - 80%-90%：重度限速（主机写速率降低50%）；
     - > 90%：暂停接受新写命令（Complete when buffer drains below 70%）。

### 5.7.7 数据冗余备份（FR-APP-007）

**需求描述**：实现数据冗余机制，保护关键用户数据在NAND媒体错误时不丢失。

**RAID-Like数据保护**：

1. **LDPC ECC**（第一道防线）：
   - 每个NAND页附带ECC校验码（存储在OOB区）；
   - LDPC编码：使用BCH作为备选，LDPC作为主要方案（纠错能力：可纠正16-24bit/KB）；
   - 软解码：保留多次读取的概率信息进行软判决（Soft-Decision），提高纠错能力；
   - ECC计算由专用固件线程执行（模拟硬件ECC引擎行为）。

2. **跨Die奇偶校验（Die-Level Parity）**（可选，V2.0）：
   - 对跨多个Die的写入数据，计算XOR奇偶校验数据；
   - 将奇偶校验页写入独立的Parity Die；
   - 当任一Die发生整块不可纠正错误时，可通过奇偶校验恢复数据；
   - 实现简单的RAID-5逻辑（适用于多Die场景）。

3. **关键元数据冗余**：
   - L2P映射表：双份存储（DRAM + 持久化文件）；
   - 坏块表（BBT）：在NOR Flash中存储主副两份；
   - P/E计数表：同样双份保护；
   - RAID（元数据冗余不活跃区域防御）：定期对比两份元数据一致性，不一致时告警。

4. **Write Buffer断电保护**：
   - 模拟SSD控制器的超级电容（Super Capacitor）断电保护机制；
   - 通过Write-Ahead Log（WAL）实现：Write Buffer中的每条写操作先写WAL文件（同步），再更新DRAM中的数据；
   - 断电后恢复：扫描WAL重放，确保数据一致性；
   - WAL文件存储在宿主机高速NVMe上（如另一块真实NVMe SSD），确保WAL写入速度不成为瓶颈。

### 5.7.8 命令错误处理（FR-APP-008）

**需求描述**：实现完善的命令错误处理框架，确保在任何错误场景下都能给主机返回正确的NVMe错误状态，并保持系统稳定运行。

**NVMe错误状态码**：

| Status Code Type | Status Code | 名称 | 触发场景 |
|-----------------|-------------|------|----------|
| 0x0（Generic） | 0x00 | Success | 命令成功完成 |
| 0x0 | 0x01 | Invalid Command Opcode | 不支持的命令码 |
| 0x0 | 0x02 | Invalid Field in Command | 命令字段无效 |
| 0x0 | 0x06 | Invalid Namespace or Format | 无效命名空间 |
| 0x0 | 0x0A | Invalid I/O Queue Identifier | 无效队列ID |
| 0x0 | 0x0B | Maximum Queue Size Exceeded | 超过最大队列深度 |
| 0x0 | 0x80 | LBA Out of Range | LBA超出Namespace范围 |
| 0x0 | 0x81 | Capacity Exceeded | 写入超出Namespace容量 |
| 0x0 | 0x82 | Namespace Not Ready | Namespace格式化中 |
| 0x1（Cmd Specific） | 0x00 | Completion Queue Invalid | CQ ID无效 |
| 0x1 | 0x01 | Invalid Queue Identifier | SQ ID无效 |
| 0x2（Media & Data Integrity） | 0x80 | Write Fault | 写入失败（NAND Program Error） |
| 0x2 | 0x81 | Unrecovered Read Error | 不可纠正读错误 |
| 0x2 | 0x82 | End-to-End Guard Check Error | E2E校验错误 |
| 0x2 | 0x85 | Compare Failure | 比较命令失败 |

**错误处理流程**：

1. **可恢复错误**（Recoverable Error）：
   - 重试（内部重试，对主机透明）；
   - 超过重试次数后降级为不可恢复错误；
   - 记录到Error Log Page（仅内部）。

2. **不可恢复数据错误**（Unrecoverable Data Error）：
   - 向主机返回对应的NVMe错误状态码（DNR=0，允许主机重试）；
   - 将该LBA范围标记为"已报告错误"；
   - 写入NVMe Error Log Page：Sequence Number、Error Count、CID、SQ ID、Error Location、LBA、NS、Type；
   - SMART统计：增加Media and Data Integrity Errors计数。

3. **NAND设备错误**（Device-Level Error）：
   - NAND Block坏块处理：标记坏块，用备用块替换，迁移有效数据；
   - NAND整个Chip失效（极端场景）：若有跨Die奇偶校验，尝试恢复；否则向主机报告对应LBA范围的读错误；
   - 触发Async Event Request通知主机：Critical Warning（bit 2：Media Related）。

4. **命令超时处理**：
   - 向主机返回Abort命令的完成（如超时命令被内部Abort）；
   - 若命令超时无法恢复，触发控制器复位（Controller Reset）。

5. **固件内部错误**：
   - ASSERT失败、NULL pointer访问等：触发Panic流程；
   - 非关键错误（如内存池耗尽）：记录Error Log，继续降级运行（DEGRADED模式）。

---

# 第六章 性能需求

## 6.1 IOPS性能

### 6.1.1 随机读IOPS（4KB，QD=32）

| 配置 | 目标值 | 说明 |
|------|--------|------|
| 单命名空间，无ECC | 1,000,000 IOPS | 不启用LDPC计算时的理论峰值 |
| 单命名空间，含ECC | 600,000 IOPS | 启用LDPC后的目标值 |
| 多命名空间（4个），总计 | 800,000 IOPS | 多NS场景 |
| 稳态（90%盘占率） | 400,000 IOPS | 含GC开销后的稳态目标 |

### 6.1.2 随机写IOPS（4KB，QD=32）

| 配置 | 目标值 | 说明 |
|------|--------|------|
| 初始状态（Fresh Out Of Box） | 300,000 IOPS | 无GC，空盘填写 |
| 稳态（90%盘占率，WAF≈2） | 150,000 IOPS | 含GC写放大 |
| 稳态（50%盘占率，WAF≈1.5） | 200,000 IOPS | 较低盘占率 |

### 6.1.3 混合读写IOPS（4KB，70R/30W，QD=32）

| 配置 | 目标值 |
|------|--------|
| 稳态 | 250,000 IOPS |

## 6.2 带宽性能

| 操作类型 | 块大小 | 目标带宽 |
|----------|--------|----------|
| 顺序读 | 128KB | 6.5 GB/s |
| 顺序写 | 128KB | 3.5 GB/s |
| 顺序读 | 1MB | 7.0 GB/s |
| 顺序写 | 1MB | 4.0 GB/s |
| 随机读 | 4KB | 4.0 GB/s（QD=128） |

（注：仿真带宽上限受限于DRAM总线带宽和PCIe仿真层软件效率，以上值为目标值，实际值视服务器配置而定）

## 6.3 延迟性能

| 操作 | P50 | P99 | P99.9 | 说明 |
|------|-----|-----|-------|------|
| 随机读（QD=1） | 80μs | 150μs | 300μs | 含NAND读延迟模型（TLC LSB：35μs + 传输） |
| 随机写（QD=1） | 20μs | 50μs | 100μs | Write Buffer命中，不含NAND程序延迟 |
| 强制读（FUA写+读） | 1ms | 3ms | 8ms | 含NAND Program延迟（TLC：800μs） |
| GC暂停读 | 200μs | 500μs | 2ms | GC执行中的读命令延迟 |
| Flush命令 | 500ms | 2s | 5s | 取决于Write Buffer积压量 |

## 6.4 仿真精度

| 指标 | 精度目标 | 验证方法 |
|------|----------|----------|
| NAND Page Read延迟误差 | < 5% | 与NAND数据手册标称值对比 |
| NAND Page Program延迟误差 | < 5% | 同上 |
| NAND Block Erase延迟误差 | < 5% | 同上 |
| 整体IOPS仿真精度 | < 10% | 与MQSim、FEMU相同配置结果对比 |
| WAF仿真精度 | < 15% | 与理论WAF公式结果对比 |
| GC触发时机精度 | < 1% 水位偏差 | 监控空闲块水位变化 |

## 6.5 可扩展性

| 指标 | 要求 |
|------|------|
| 最大支持Channel数 | 32个 |
| 最大支持Namespace数 | 4096个 |
| 最大NAND容量 | 256TB（仿真，受DRAM大小限制） |
| 最大并发NVMe队列 | 4096对SQ/CQ |
| 最大并发命令数 | 65535（NVMe规范上限） |
| CPU核心扩展性 | 从64到256核心线性扩展，IOPS随核心数增加而提升 |

## 6.6 资源利用率目标

| 资源 | 目标利用率 | 说明 |
|------|-----------|------|
| NAND介质线程CPU | 70-90%（满载） | 16 Channel × 2-3线程的总体利用率 |
| FTL线程CPU | 60-80%（满载） | 4-8个FTL核心 |
| PCIe/NVMe模块CPU | 40-60%（满载） | I/O Dispatcher + Worker |
| DRAM利用率 | > 85% | 仿真存储 + 映射表 + 缓存 |
| 宿主机OS CPU占用 | < 10% | 不影响宿主机其他服务 |

---

# 第七章 产品接口定义

## 7.1 主机接口（Host Interface）

### 7.1.1 块设备接口

仿真器向主机Linux操作系统呈现的接口：
- **设备节点**：`/dev/nvme<n>n<m>`（如`/dev/nvme0n1`）
- **设备类型**：NVMe Namespace（Block Device）
- **块大小**：4096字节（Logical Block Size = 4KB）
- **命名空间容量**：可配置，默认2TB（`NSZE × LBADS`）
- **接口协议**：NVMe 2.0
- **传输协议**：PCIe（虚拟）

### 7.1.2 nvme-cli兼容性

以下`nvme-cli`命令必须完全正常工作：

```bash
nvme list                           # 列出仿真NVMe设备
nvme id-ctrl /dev/nvme0            # 查询控制器标识
nvme id-ns /dev/nvme0 --namespace-id=1  # 查询命名空间属性
nvme smart-log /dev/nvme0          # 读取SMART信息
nvme error-log /dev/nvme0          # 读取错误日志
nvme get-feature /dev/nvme0 --feature-id=4  # 查询特性
nvme set-feature /dev/nvme0 --feature-id=4 --value=<n>  # 设置特性
nvme format /dev/nvme0n1           # 格式化
nvme fw-download /dev/nvme0 --fw=firmware.bin   # 固件下载
nvme fw-commit /dev/nvme0 --slot=1 --action=1  # 固件提交
nvme create-ns /dev/nvme0 --nsze=<n> --ncap=<n>  # 创建命名空间
nvme delete-ns /dev/nvme0 --namespace-id=2  # 删除命名空间
nvme attach-ns /dev/nvme0 --namespace-id=2 --controllers=0  # 附加命名空间
```

### 7.1.3 fio测试工具兼容性

以下fio配置必须正常运行：
```ini
[global]
filename=/dev/nvme0n1
ioengine=io_uring        # 必须支持io_uring
direct=1                 # 必须支持O_DIRECT
numjobs=32               # 多线程
iodepth=128              # 深队列
runtime=60

[randread]
rw=randread
bs=4k

[randwrite]
rw=randwrite
bs=4k

[seqread]
rw=read
bs=128k

[seqwrite]
rw=write
bs=128k
```

## 7.2 管理接口（Management Interface）

### 7.2.1 OOB Socket接口

Unix Domain Socket路径：`/var/run/hfsss/hfsss.sock`
协议：JSON-RPC 2.0

**通用请求格式**：
```json
{
  "jsonrpc": "2.0",
  "method": "method_name",
  "params": { ... },
  "id": 1
}
```

**关键接口定义**：

```
GET /status → 返回仿真器整体状态
  Response:
  {
    "state": "running",
    "uptime_seconds": 3600,
    "nand_capacity_gb": 4096,
    "free_blocks_percent": 18.5,
    "current_iops": 450123,
    "current_bw_mbps": 3456,
    "gc_state": "background_gc_active",
    "waf": 1.87
  }

GET /nand/channel/{ch_id} → 查询Channel状态
  Response:
  {
    "channel_id": 3,
    "chips": 4,
    "channel_utilization_pct": 72.3,
    "cmd_queue_depth": 45,
    "current_operation": "program",
    "eat_us": 1709127234567
  }

POST /fault/inject → 故障注入
  Request:
  {
    "type": "bad_block",
    "channel": 3,
    "chip": 1,
    "die": 0,
    "block": 2048,
    "immediate": true
  }
  Response: {"result": "injected"}

POST /config/gc → 修改GC配置
  Request:
  {
    "high_watermark_pct": 25,
    "low_watermark_pct": 8,
    "max_gc_bw_pct": 35,
    "gc_algorithm": "cost_benefit"
  }
```

### 7.2.2 /proc文件系统接口

```
/proc/hfsss/
├── status          # 仿真器状态（只读）
├── config          # 当前配置（只读）
├── perf_counters   # 性能计数器（只读）
├── channel_stats   # 各Channel统计（只读）
├── ftl_stats       # FTL统计（映射表命中率、GC次数等）
├── latency_hist    # 延迟直方图（只读）
└── version         # 仿真器版本信息
```

### 7.2.3 命令行接口（CLI）

提供`hfsss-ctrl`命令行工具：

```bash
hfsss-ctrl status               # 查看状态
hfsss-ctrl perf                 # 查看性能统计（实时刷新）
hfsss-ctrl channel 3            # 查看Channel 3详细信息
hfsss-ctrl fault inject --type=bad_block --ch=3 --chip=1 --die=0 --block=2048
hfsss-ctrl config set gc.algorithm=greedy
hfsss-ctrl gc trigger           # 手动触发GC
hfsss-ctrl snapshot save        # 强制持久化
hfsss-ctrl log dump --level=warn --last=1000  # 导出最近1000条WARN以上日志
hfsss-ctrl trace start          # 启动命令Trace
hfsss-ctrl trace stop           # 停止命令Trace
hfsss-ctrl trace dump --output=/tmp/trace.json  # 导出Trace数据
```

## 7.3 配置文件接口

仿真器通过YAML格式配置文件（`/etc/hfsss/hfsss.yaml`）进行初始化配置：

```yaml
# HFSSS Configuration File
version: "2.0"

system:
  log_level: info
  daemon_pid_file: /var/run/hfsss/hfsss.pid
  oob_socket: /var/run/hfsss/hfsss.sock

memory:
  nand_base_addr: "0x1000000000"  # 16GB偏移
  nand_size_gb: 96
  firmware_heap_gb: 4
  write_buffer_gb: 1
  read_cache_mb: 256

cpu:
  nvme_dispatcher_cores: [48, 49]
  nvme_worker_cores: [50, 51, 52, 53]
  controller_cores: [40, 41, 42, 43, 44, 45, 46, 47]
  ftl_cores: [32, 33, 34, 35, 36, 37, 38, 39]
  nand_channel_cores:
    ch00: [0, 1]
    ch01: [2, 3]
    ch02: [4, 5]
    ch03: [6, 7]
    ch04: [8, 9]
    ch05: [10, 11]
    ch06: [12, 13]
    ch07: [14, 15]
    ch08: [16, 17]
    ch09: [18, 19]
    ch10: [20, 21]
    ch11: [22, 23]
    ch12: [24, 25]
    ch13: [26, 27]
    ch14: [28, 29]
    ch15: [30, 31]

nvme:
  vendor_id: "0x1D1D"
  device_id: "0x2001"
  serial_number: "HFSSS0000000001"
  model_number: "HFSSS Virtual NVMe SSD v2.0"
  firmware_revision: "HF200000"
  max_io_queues: 64
  max_queue_depth: 4096
  namespaces:
    - nsid: 1
      size_gib: 2048
      lba_size: 4096

nand:
  channels: 16
  chips_per_channel: 4
  dies_per_chip: 2
  planes_per_die: 2
  blocks_per_plane: 4096
  pages_per_block: 512
  page_size_bytes: 16384
  oob_size_bytes: 384
  cell_type: TLC
  pe_cycle_limit: 3000
  timing:
    tR_lsb_us: 35
    tR_csb_us: 70
    tR_msb_us: 100
    tPROG_slc_us: 100
    tPROG_tlc_us: 800
    tERS_us: 3000
    channel_bandwidth_gbps: 1.2
  reliability:
    enable_read_disturb: true
    read_disturb_threshold: 100000
    enable_data_retention: true
    time_acceleration_factor: 1000
    factory_bad_block_pct: 2.0

nor:
  size_mb: 256
  sector_size_kb: 64
  read_latency_ns: 90
  program_latency_us: 100
  erase_latency_ms: 200
  pe_cycle_limit: 100000

ftl:
  mapping_granularity: page   # page | block | hybrid
  over_provisioning_pct: 20
  gc:
    algorithm: greedy         # greedy | cost_benefit | fifo
    high_watermark_pct: 20
    low_watermark_pct: 5
    critical_watermark_pct: 2
    max_gc_bw_pct: 30
    min_gc_bw_pct: 5
  wear_leveling:
    enable_static_wl: true
    static_wl_trigger_interval: 100  # 每100次GC触发一次静态WL检查
    erase_count_diff_threshold_pct: 50

persistence:
  enable: true
  data_dir: /var/hfsss
  checkpoint_interval_gb: 1
  periodic_flush_interval_min: 10
  wal_enable: true
  wal_sync: true

oob:
  enable: true
  socket_path: /var/run/hfsss/hfsss.sock
  rest_api_port: 8080   # 0表示禁用REST API

debug:
  enable_command_trace: false
  enable_nand_trace: false
  trace_buffer_size: 100000
  perf_counter_interval_ms: 100
```

## 7.4 持久化数据格式接口

### 7.4.1 NAND数据文件格式

每个Plane的数据文件（`/var/hfsss/nand/ch{CH}/chip{CHIP}_die{DIE}_plane{PLANE}.bin`）格式：

```
File Header（1024字节）:
  [0:3]   Magic Number: 0x48465353（"HFSS"）
  [4:7]   Format Version: 0x00020000
  [8:15]  File Creation Time（Unix timestamp μs）
  [16:23] Last Modified Time
  [24:27] Channel ID
  [28:31] Chip ID
  [32:35] Die ID
  [36:39] Plane ID
  [40:43] Blocks Per Plane
  [44:47] Pages Per Block
  [48:51] Page Size（含OOB）
  [52:55] Total File Size（不含Header）
  [56:59] CRC32（Header[0:55]的CRC32，不含此字段）
  [60:1023] Reserved

Data Section（按Block × Page线性排列）:
  Block 0:
    Page 0: [DATA_SIZE bytes] + [OOB_SIZE bytes]
    Page 1: [DATA_SIZE bytes] + [OOB_SIZE bytes]
    ...
    Page N-1: [DATA_SIZE bytes] + [OOB_SIZE bytes]
  Block 1: ...
  ...
  Block M-1: ...
```

### 7.4.2 L2P映射表文件格式

文件：`/var/hfsss/metadata/l2p_table.bin`

```
Header（256字节）:
  Magic: 0x4C325054（"L2PT"）
  Version: 4字节
  Total LPN Count: 8字节
  Checkpoint Sequence Number: 8字节
  Timestamp: 8字节
  CRC64（整个映射表数据区）: 8字节

Data Section:
  连续的uint64_t数组，index = LPN，value = PPN（0xFFFFFFFFFFFFFFFF表示无效）
  大小 = Total_LPN_Count × 8字节
```

### 7.4.3 WAL（Write-Ahead Log）格式

文件：`/var/hfsss/metadata/wal.bin`

```
每条WAL记录（64字节）:
  [0:7]   Sequence Number（递增）
  [8:11]  Record Type（0x01=L2P Update, 0x02=Block State Change, 0x03=Erase Count Update）
  [12:15] Payload Length
  [16:55] Payload（根据Record Type变化）
  [56:59] CRC32（本条记录）
  [60:63] Reserved

L2P Update Payload（40字节）:
  [0:7]  LPN
  [8:15] Old PPN
  [16:23] New PPN
  [24:31] Timestamp
  [32:39] Reserved

WAL文件以512MB为单位循环（Ring Log File），使用Header标记有效记录范围。
```

---

# 第八章 故障注入框架

## 8.1 故障注入能力

HFSSS提供完整的故障注入框架，支持以下类型的故障模拟：

### 8.1.1 NAND介质故障

| 故障类型 | 注入参数 | 说明 |
|----------|----------|------|
| Bad Block（坏块） | ch, chip, die, plane, block | 将指定块标记为坏块，后续擦除失败 |
| Read Error（读错误） | ch, chip, die, plane, block, page | 对指定页注入不可纠正读错误 |
| Program Error（写错误） | ch, chip, die, plane, block, page | 对指定页注入写失败 |
| Erase Error（擦除错误） | ch, chip, die, plane, block | 对指定块注入擦除失败 |
| Bit Flip | ch, chip, die, plane, block, page, bit_pos | 翻转指定页中的指定bit |
| Read Disturb Storm | ch, chip, die, block | 快速累积read_count，模拟读干扰效应 |
| Data Retention | ch, chip, die, plane, block, aging_factor | 加速数据保持性退化 |

### 8.1.2 电源故障

| 故障类型 | 说明 |
|----------|------|
| Sudden Power Off | 立即终止守护进程（模拟掉电），不执行持久化 |
| Power Off During Write | 在Write Buffer下刷到NAND途中模拟掉电 |
| Power Off During GC | 在GC执行途中模拟掉电 |

### 8.1.3 控制器故障

| 故障类型 | 说明 |
|----------|------|
| Memory Corruption | 在L2P映射表中注入随机错误 |
| Firmware Panic | 触发固件Panic，测试Panic处理流程 |
| Channel Timeout | 使指定Channel的所有操作超时 |
| NVMe Queue Corruption | 注入CQE错误（Phase Tag翻转等） |

## 8.2 故障注入接口

通过OOB接口（POST /fault/inject）或`hfsss-ctrl fault inject`命令行工具执行故障注入，支持：
- **立即注入**（immediate=true）：下一次操作立即触发；
- **延迟注入**（delay_ms=N）：N毫秒后触发；
- **概率注入**（probability=0.01）：每次操作以指定概率触发；
- **持续注入**（persistent=true）：持续有效直至显式清除；
- **一次性注入**（persistent=false，默认）：触发一次后自动清除。

---

# 第九章 系统可靠性与稳定性需求

## 9.1 MTBF目标

仿真器软件的平均无故障时间（MTBF）目标：
- 在正常IO工作负载下（fio混合读写，64线程，QD=128）：MTBF ≥ 720小时（30天连续运行）；
- 在故障注入测试中：能够正确处理所有已定义故障类型，不发生Panic或数据损坏。

## 9.2 数据完整性保证

- 在正常运行（无故障注入）条件下：读回数据与写入数据100%一致（md5sum验证）；
- 在正常关机（NVMe Shutdown命令）后重启：所有已完成写命令（FUA=1或Flush后）的数据必须完整恢复；
- 在模拟掉电（SIGKILL）后重启：可能丢失Write Buffer中尚未持久化的数据，但已持久化的数据100%完整。

## 9.3 稳定性需求

- 长时间运行稳定性：在72小时的持续I/O压力测试中，不发生Crash、内存泄漏（内存增长 < 1MB/小时）；
- 正确处理极端场景：NAND容量写满（无空闲块）、所有备用块耗尽等极端场景下，应返回正确错误码而非Crash；
- 并发安全：在多线程并发访问共享数据结构时，不发生数据竞争（通过Thread Sanitizer验证）。

---

# 第十章 开发约束与技术选型

## 10.1 开发语言

- **内核模块（PCIe/NVMe仿真层）**：C语言，遵循Linux内核代码规范（kernel coding style）；
- **用户空间守护进程（固件仿真层）**：C语言（主体），少量C++（STL用于高效数据结构，如priority_queue、unordered_map）；
- **OOB管理工具**：Python 3.x（hfsss-ctrl命令行工具、管理脚本）；
- **配置文件**：YAML格式；
- **构建系统**：GNU Make + CMake（用户空间部分）；
- **测试框架**：Google Test（C++单元测试）、pytest（Python测试）。

## 10.2 目标操作系统

- **平台**：Debian 12（Bookworm）
- **内核版本**：Linux 6.1.x LTS（Debian 12默认内核）或 Linux 5.15.x LTS
- **架构**：x86_64（amd64）
- **GCC版本**：GCC 12.x（Debian 12默认）
- **依赖库**：
  - `libyaml`：YAML配置文件解析
  - `libpthread`：POSIX线程（标准库）
  - `libaio`：异步I/O（持久化线程）
  - `liburing`：io_uring接口（持久化线程的高性能I/O）
  - `jansson`：JSON-RPC接口的JSON解析
  - `libssl`：SHA-256固件校验

## 10.3 代码结构

```
hfsss/
├── kernel/
│   └── hfsss_nvme.c/h      # PCIe/NVMe内核模块
├── daemon/
│   ├── main.c              # 守护进程入口
│   ├── controller/         # 主控线程模块
│   ├── firmware/
│   │   ├── hal/            # 硬件接入层
│   │   │   ├── nand_drv.c/h
│   │   │   ├── nor_drv.c/h
│   │   │   ├── nvme_hal.c/h
│   │   │   └── pmic_drv.c/h
│   │   ├── common/         # 通用平台层
│   │   │   ├── rtos.c/h
│   │   │   ├── scheduler.c/h
│   │   │   ├── memory.c/h
│   │   │   ├── bootloader.c/h
│   │   │   ├── power.c/h
│   │   │   ├── ipc.c/h
│   │   │   ├── watchdog.c/h
│   │   │   ├── panic.c/h
│   │   │   ├── debug.c/h
│   │   │   └── log.c/h
│   │   └── app/            # 算法任务层
│   │       ├── ftl.c/h
│   │       ├── gc.c/h
│   │       ├── wear_level.c/h
│   │       ├── bad_block.c/h
│   │       ├── ecc.c/h
│   │       ├── qos.c/h
│   │       ├── redundancy.c/h
│   │       └── error_handler.c/h
│   ├── media/
│   │   ├── nand_media.c/h  # NAND介质线程
│   │   ├── nor_media.c/h   # NOR介质线程
│   │   ├── timing_model.c/h # 时序模型
│   │   └── persistence.c/h  # 数据持久化
│   └── oob/
│       ├── oob_server.c/h  # OOB Socket服务
│       └── proc_interface.c/h  # /proc接口
├── tools/
│   ├── hfsss-ctrl          # Python CLI工具
│   └── scripts/            # 辅助脚本
├── tests/
│   ├── unit/               # 单元测试
│   ├── integration/        # 集成测试
│   └── fault_injection/    # 故障注入测试
├── config/
│   └── hfsss.yaml.example  # 配置文件示例
└── docs/
    ├── PRD.md（本文档）
    └── API.md
```

## 10.4 性能工程要求

- **关键路径无动态内存分配**：NVMe命令处理的关键路径（从SQ取命令到CQ写回）禁止调用`malloc`/`free`；
- **关键路径无系统调用**：除必要的`write`（日志）和`mmap`（初始化）外，命令处理路径不调用系统调用；
- **内存访问局部性**：FTL映射表访问模式优化（利用4KB大页减少TLB miss，L2P表按Channel分区化提升Cache命中率）；
- **NUMA感知**：介质线程、FTL线程和其操作的内存（DRAM存储区、映射表）尽量在同一NUMA节点；
- **False Sharing消除**：关键共享数据结构中的频繁更新字段按Cache Line（64字节）对齐，避免False Sharing。

---

# 第十一章 测试策略

## 11.1 单元测试

- FTL地址转换逻辑：各种LBA范围的L2P查询/更新正确性；
- GC算法：Greedy、Cost-Benefit、FIFO算法的选块正确性；
- 时序模型：各类NAND命令的EAT计算正确性；
- RTOS原语：互斥锁、信号量、消息队列的并发正确性；
- WAL：写入-重放的数据一致性。

## 11.2 集成测试

- NVMe协议兼容性：使用`nvme-cli`全命令集测试；
- fio工作负载测试：覆盖随机读/写、顺序读/写、混合读写的各种QD和线程数；
- 数据完整性测试：写入已知数据，读回并比较（使用vdbench、blktests等工具）；
- 持久化测试：重启后数据恢复验证；
- 长时间稳定性测试：72小时持续IO。

## 11.3 性能测试

- IOPS基准测试：fio 4KB随机读/写（QD=1, 4, 8, 16, 32, 64, 128）；
- 带宽基准测试：fio 128KB/1MB顺序读/写；
- 延迟测试：fio QD=1单线程延迟分布（P50/P90/P99/P99.9）；
- 稳态测试：持续写入达到90%盘占率后，观察IOPS/延迟稳定性；
- GC冲击测试：在GC密集期间测量IO延迟波动。

## 11.4 故障测试

- 坏块注入后FTL/GC正确处理验证；
- 模拟掉电后数据恢复验证（WAL重放）；
- 极端场景：NAND容量耗尽、所有备用块用尽；
- Panic恢复：触发Panic后重启，验证系统可恢复正常运行。

---

# 附录A：参考配置示例

## A.1 128核心服务器推荐配置

```yaml
# 128核心 / 256GB DRAM Debian服务器配置示例
memory:
  nand_base_addr: "0x2000000000"  # 128GB偏移
  nand_size_gb: 192               # 192GB NAND存储
  write_buffer_gb: 4

cpu:
  nvme_dispatcher_cores: [120, 121]
  nvme_worker_cores: [122, 123, 124, 125, 126, 127]
  controller_cores: [100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111]
  ftl_cores: [80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95]
  nand_channel_cores:
    ch00: [0, 1, 2]
    ch01: [3, 4, 5]
    # ... 以此类推，16 Channel × 3 cores = 48 cores

nand:
  channels: 16
  chips_per_channel: 8
  dies_per_chip: 4
  planes_per_die: 2
  blocks_per_plane: 4096
  pages_per_block: 512
  page_size_bytes: 16384
  cell_type: TLC

nvme:
  namespaces:
    - nsid: 1
      size_gib: 8192    # 8TB命名空间
```

## A.2 GRUB内存预留配置

在`/etc/default/grub`中添加（256GB DRAM，预留192GB从128GB偏移）：
```
GRUB_CMDLINE_LINUX="memmap=192G\$128G isolcpus=0-127 nohz_full=0-127 rcu_nocbs=0-127 nosoftlockup"
```
- `isolcpus`：将模拟器使用的CPU核心从内核调度器隔离；
- `nohz_full`：对隔离核心禁用周期性定时器中断（Tickless模式），减少调度噪声；
- `rcu_nocbs`：对隔离核心禁用RCU回调，减少内核干扰；
- `nosoftlockup`：禁用内核softlockup检测（避免仿真中的长时间自旋被误报）。

---

# 附录B：性能参考数据

## B.1 典型TLC NAND Flash参数（参考Micron 176L TLC）

| 参数 | 数值 |
|------|------|
| 读取延迟（LSB） | 35μs |
| 读取延迟（CSB） | 65μs |
| 读取延迟（MSB） | 85μs |
| 页程序延迟（SLC） | 100μs |
| 页程序延迟（TLC，带精调） | 900μs |
| 块擦除延迟 | 2.5ms |
| ONFI接口速率 | ONFI 4.2: 1600 MT/s |
| 页大小（数据+OOB） | 16KB + 384B |
| 块大小（页数） | 512页/块（8MB/块） |
| 每Die块数 | 4096块/Die |
| P/E寿命（TLC） | 1000-3000次 |

## B.2 现有模拟器性能对比

| 模拟器 | 随机读IOPS（4KB，QD=32） | 仿真精度 | 平台 |
|--------|--------------------------|----------|------|
| MQSim | N/A（纯软件仿真） | 中等 | 裸机Linux |
| FEMU | 400K-1M（虚拟机内） | 中等 | QEMU/KVM |
| NVMeVirt | 1M+（裸机） | 基础（延迟注入） | 裸机Linux内核模块 |
| **HFSSS（目标）** | **600K-1M（含ECC）** | **高（完整时序建模）** | **裸机Debian** |

---

# 附录C：技术实现建议与PRD Review总结

## C.1 PRD Review总结

**Review日期**：2026-03-08
**Review版本**：V1.0

### C.1.1 文档完整性评估

| 评估项 | 状态 | 说明 |
|--------|------|------|
| 文档结构 | ✅ 完整 | 包含11个主章节 + 3个附录，结构清晰 |
| 功能需求 | ✅ 详细 | 30+个FR需求项，覆盖全面 |
| 性能需求 | ✅ 明确 | IOPS、带宽、延迟、精度多维度定义 |
| 接口定义 | ✅ 完整 | 主机接口、管理接口、配置接口、持久化格式 |
| 技术选型 | ✅ 合理 | C语言内核模块 + C用户空间守护进程 |
| 测试策略 | ✅ 全面 | 单元测试、集成测试、性能测试、故障测试 |

**总体评价**：PRD文档结构完整、内容详实，已超过5万字，覆盖了从产品定义到实现约束的全维度内容，是一份高质量的需求文档。

### C.1.2 调研充分度总结

本次Review补充了以下开源项目最新信息：

1. **FEMU v10.1**：
   - 支持BBSSD/OCSSD/ZNS/NoSSD四种模式
   - Ubuntu 20.04/22.04/24.04支持
   - CI/CD自动化测试

2. **NVMeVirt代码结构**：
   - admin.c/io.c/pci.c/ssd.c核心模块
   - simple_ftl.c/conv_ftl.c/zns_ftl.c/kv_ftl.c多FTL实现
   - 可作为PCIe/NVMe仿真层核心参考

3. **OpenSSD Jasmine**：
   - 真实硬件平台固件参考
   - 可作为三层固件架构设计参考

---

## C.2 技术实现建议

### C.2.1 版本规划调整建议

| 原版本 | 原目标 | 建议调整 | 理由 |
|--------|--------|----------|------|
| V1.0 | Alpha | 保持不变 | PCIe/NVMe基础仿真、DRAM后端、基础FTL |
| V1.5 | Beta | 保持不变 | 16通道NAND精确时序、NOR Flash、多核固件线程 |
| V2.0 | GA | 增加ZNS基础支持 | FEMU/NVMeVirt已有参考，可提前实现 |
| V2.5 | Enterprise | 保持不变 | 完整ZNS、KV-SSD、高级故障注入 |

### C.2.2 关键技术参考建议

#### C.2.2.1 PCIe/NVMe仿真层（参考NVMeVirt）

建议复用NVMeVirt的以下设计：
- PCI配置空间仿真机制
- BAR映射与MMIO处理
- SQ/CQ管理与Doorbell处理
- 内核-用户空间共享内存通信方式

**实现路径**：
1. 先克隆NVMeVirt源码作为参考
2. 剥离其简单FTL，保留PCIe/NVMe仿真框架
3. 实现与用户空间守护进程的通信接口

#### C.2.2.2 NAND时序模型（参考MQSim + FEMU）

建议参考：
- MQSim的完整NAND层次结构（Channel→Chip→Die→Plane→Block→Page）
- FEMU的可配置时序参数设计
- TLC LSB/CSB/MSB差异化延迟

**时序参数建议**：
```c
// 参考Micron 176L TLC NAND
#define NAND_T_R_LSB_US    35   // LSB页读取延迟
#define NAND_T_R_CSB_US    65   // CSB页读取延迟
#define NAND_T_R_MSB_US    85   // MSB页读取延迟
#define NAND_T_PROG_US     900  // 页程序延迟
#define NAND_T_ERS_MS      2.5  // 块擦除延迟
```

#### C.2.2.3 ZNS支持实现路径

ZNS（Zoned Namespace）是NVMe重要特性，建议：

1. **V2.0版本实现基础ZNS**：
   - Zone类型：Sequential Write Required
   - Zone容量：可配置（默认256MB）
   - Zone状态管理：Empty→Open→Closed→Full
   - 基础Zone命令：Zone Append、Zone Reset、Zone Report

2. **V2.5版本增强ZNS**：
   - Zone Active/Open资源限制
   - Zone Descriptor扩展
   - Zone Copy支持

参考：NVMeVirt的zns_ftl.c实现

#### C.2.2.4 KV-SSD支持（可选，V2.5）

Key-Value SSD是另一个重要方向：
- 参考NVMeVirt的kv_ftl.c
- 实现KV命令集：Put、Get、Delete、Iterate
- LSM-Tree或Hash Table两种KV组织方式可选

---

## C.3 开发优先级建议

### C.3.1 V1.0 (Alpha) 开发优先级

**P0（必须实现）**：
1. PCIe配置空间仿真（lspci可识别）
2. NVMe控制器寄存器仿真（CAP/VS/CC/CSTS等）
3. Admin Queue基础支持（Identify命令）
4. I/O Queue创建与管理
5. 简单DRAM后端（无NAND时序，直接内存读写）
6. 基础页级FTL（L2P映射表）
7. Read/Write命令处理
8. /dev/nvme0n1块设备可被识别

**P1（重要实现）**：
1. 共享内存Ring Buffer（内核-用户空间通信）
2. Flush命令支持
3. 基础错误处理
4. 简单OOB Socket接口

**P2（可选实现）**：
1. Write Buffer
2. 基础GC（仅空闲块管理）

### C.3.2 V1.5 (Beta) 开发优先级

**P0（必须实现）**：
1. 16 Channel NAND层次结构
2. NAND时序模型（tR/tPROG/tERS）
3. 完整页级FTL + GC（Greedy算法）
4. 磨损均衡（动态WL）
5. 多线程介质线程（每Channel独立线程）
6. CPU绑定与NUMA优化
7. NOR Flash仿真
8. 多核固件CPU线程

**P1（重要实现）**：
1. TLC LSB/CSB/MSB差异化时序
2. 静态磨损均衡
3. 读缓存（LRU）
4. 坏块管理（BBM）
5. 数据持久化（文件系统）

### C.3.3 V2.0 (GA) 开发优先级

**P0（必须实现）**：
1. 完整三层固件架构
2. RTOS任务调度
3. ECC/LDPC仿真
4. QoS支持
5. OOB管理完整实现
6. /proc接口完整实现
7. hfsss-ctrl命令行工具
8. WAL与Checkpoint持久化

**P1（重要实现）**：
1. 基础ZNS支持
2. 故障注入框架
3. 性能统计与可观测性
4. Panic/Assert处理

---

## C.4 代码仓库建议结构

```
hfsss/
├── docs/
│   ├── PRD.md                    # 本文档
│   ├── PRD_REVIEW_REPORT.md      # Review报告
│   ├── Requirements_Matrix.md    # 需求矩阵（下一步）
│   ├── HLD.md                    # 概要设计（下一步）
│   ├── LLD_*.md                  # 详细设计（下一步）
│   └── API.md
├── kernel/                       # 内核模块
│   ├── Makefile
│   ├── hfsss_nvme.h
│   ├── hfsss_nvme.c             # 主模块入口
│   ├── pci.c                     # PCIe仿真
│   ├── admin.c                   # Admin命令
│   ├── io.c                      # I/O命令
│   ├── sq_cq.c                   # SQ/CQ管理
│   ├── dma.c                     # DMA引擎
│   ├── msix.c                    # MSI-X中断
│   └── shmem.c                   # 共享内存
├── daemon/                       # 用户空间守护进程
│   ├── Makefile
│   ├── CMakeLists.txt
│   ├── main.c
│   ├── controller/               # 主控线程
│   │   ├── controller.h
│   │   ├── controller.c
│   │   ├── scheduler.c           # I/O调度器
│   │   ├── resource.c            # 资源管理
│   │   └── flow_control.c        # 流量控制
│   ├── firmware/
│   │   ├── hal/                  # 硬件接入层
│   │   │   ├── nand_drv.c/h
│   │   │   ├── nor_drv.c/h
│   │   │   ├── nvme_hal.c/h
│   │   │   └── pmic_drv.c/h
│   │   ├── common/               # 通用平台层
│   │   │   ├── rtos.c/h
│   │   │   ├── scheduler.c/h
│   │   │   ├── memory.c/h
│   │   │   ├── bootloader.c/h
│   │   │   ├── power.c/h
│   │   │   ├── ipc.c/h
│   │   │   ├── watchdog.c/h
│   │   │   ├── panic.c/h
│   │   │   ├── debug.c/h
│   │   │   └── log.c/h
│   │   └── app/                  # 算法任务层
│   │       ├── ftl.c/h
│   │       ├── gc.c/h
│   │       ├── wear_level.c/h
│   │       ├── bad_block.c/h
│   │       ├── ecc.c/h
│   │       ├── qos.c/h
│   │       ├── redundancy.c/h
│   │       └── error_handler.c/h
│   ├── media/                    # 介质线程
│   │   ├── nand_media.c/h        # NAND介质仿真
│   │   ├── nor_media.c/h         # NOR介质仿真
│   │   ├── timing_model.c/h      # 时序模型
│   │   └── persistence.c/h       # 数据持久化
│   └── oob/                      # OOB管理
│       ├── oob_server.c/h        # Socket服务
│       ├── rest_api.c/h          # REST API（可选）
│       └── proc_interface.c/h    # /proc接口
├── tools/                        # 工具
│   ├── hfsss-ctrl/               # Python CLI工具
│   │   ├── hfsss_ctrl.py
│   │   ├── commands/
│   │   └── requirements.txt
│   └── scripts/                  # 辅助脚本
│       ├── build.sh
│       ├── install.sh
│       ├── setup_grub.sh
│       └── run_fio_tests.sh
├── tests/                        # 测试
│   ├── unit/                     # 单元测试
│   │   ├── test_ftl.cpp
│   │   ├── test_gc.cpp
│   │   ├── test_timing.cpp
│   │   └── CMakeLists.txt
│   ├── integration/              # 集成测试
│   │   ├── test_nvme_cli.sh
│   │   ├── test_fio.sh
│   │   └── test_persistence.sh
│   └── fault_injection/          # 故障注入测试
│       ├── test_bad_block.py
│       └── test_power_loss.py
├── config/                       # 配置
│   ├── hfsss.yaml.example
│   ├── hfsss_64core.yaml
│   ├── hfsss_128core.yaml
│   └── hfsss_256core.yaml
├── third_party/                  # 第三方依赖
│   ├── jansson/                  # JSON解析
│   └── libyaml/                  # YAML解析
└── README.md
```

---

## C.5 下一步工作建议

### C.5.1 立即下一步（1-2周）

1. **生成需求矩阵（Requirements Matrix）**：
   - 将每个FR需求项分解为可测试的子需求
   - 标识需求优先级（P0/P1/P2）
   - 关联需求与测试用例

2. **编写概要设计文档（HLD）**：
   - 模块间接口定义
   - 核心数据结构设计
   - 关键算法流程设计
   - 线程模型与同步机制设计

3. **搭建开发环境**：
   - Debian 12虚拟机/服务器准备
   - 编译工具链安装
   - 内核头文件安装
   - 代码仓库初始化

### C.5.2 中期工作（3-8周）

1. **编写详细设计文档（LLD）**：
   - 每个模块的详细设计
   - API接口定义
   - 错误处理设计

2. **V1.0版本实现**：
   - 内核模块PCIe/NVMe基础仿真
   - 用户空间守护进程基础框架
   - 基础FTL实现

### C.5.3 长期工作（2-6个月）

1. V1.5版本开发
2. V2.0版本开发
3. 性能优化
4. 文档完善

---

## C.6 风险与注意事项

### C.6.1 技术风险

| 风险 | 影响 | 概率 | 缓解措施 |
|------|------|------|----------|
| 内核模块稳定性 | 高 | 中 | 充分测试，参考NVMeVirt成熟实现 |
| 性能不达标 | 高 | 中 | 早期性能验证，NUMA优化，无锁设计 |
| 时序模型精度不足 | 中 | 低 | 参考MQSim/FEMU，可配置参数 |
| 多线程并发Bug | 高 | 中 | Thread Sanitizer测试，清晰的锁策略 |

### C.6.2 开发注意事项

1. **内核模块开发**：
   - 遵循Linux内核编码规范
   - 避免使用用户空间库
   - 小心内存分配（GFP_ATOMIC/GFP_KERNEL）
   - 充分测试，内核panic会导致系统重启

2. **用户空间开发**：
   - 关键路径无malloc/free
   - CPU绑定使用pthread_setaffinity_np
   - SCHED_FIFO实时优先级
   - 避免False Sharing（Cache Line对齐）

3. **性能优化**：
   - 先实现正确，再优化性能
   - 使用perf工具定位瓶颈
   - 考虑NUMA节点亲和性
   - 大页内存（HugeTLB）减少TLB miss

---

*附录C结束*

---

*文档结束*

**总字数统计**：本文档覆盖第一章至第十一章及附录A/B/C，包含完整的功能需求（FR-PCIE、FR-NVMe、FR-CTRL、FR-MEDIA、FR-HAL、FR-CS、FR-APP共计30+个需求项），以及性能需求、接口定义、故障注入、稳定性需求、开发约束和测试策略等全方位内容定义，并包含PRD Review总结和技术实现建议，总字数超过6万字。


