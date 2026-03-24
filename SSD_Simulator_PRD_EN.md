# SSD Simulator Product Requirements Document (PRD)

**Document Version:** V2.0
**Date:** March 2026
**Document Status:** Officially Released
**Classification:** Internal

---

## Revision History

| Version | Date | Author | Description |
|---------|------|--------|-------------|
| V0.1 | 2026-02-01 | Architecture Team | Initial draft |
| V0.9 | 2026-02-20 | Architecture Team | Functional requirements refinement |
| V1.0 | 2026-03-08 | Architecture Team | Official release |
| V2.0 | 2026-03-22 | Architecture Team | English translation; added Chapter 12 (Enterprise SSD Features): UPLP, QoS Determinism, T10 DIF/PI, Data-at-Rest Encryption, Multi-Namespace Management, Thermal Management, Advanced Telemetry; updated Section 3.5 to include TCG Opal/AES at simulation level; updated Section 3.6 with V3.0 Enterprise milestone; added Appendix D (Enterprise SSD Standards Reference) |

---

# Chapter 1: Document Overview

## 1.1 Purpose

This document is the Product Requirements Document for the "High-Fidelity Full-Stack SSD Simulator (HFSSS)" project. It provides a complete and precise description of the simulator across functional requirements, performance requirements, product definition, and product interfaces, with the following goals:

1. To serve as the foundational input for generating the subsequent Requirements Matrix;
2. To provide clear design constraints and objectives for the High-Level Design (HLD);
3. To provide traceable requirement sources for each module's Low-Level Design (LLD) documents;
4. To provide acceptance criteria for the testing team;
5. To provide work estimation and milestone planning references for project management.

## 1.2 Intended Audience

This document is intended for the following readers:

- **Product Managers**: To understand the full product scope and boundaries;
- **System Architects**: To guide system decomposition and module partitioning;
- **Firmware Engineers**: To understand the firmware layer hierarchy and algorithm requirements;
- **Kernel/Driver Engineers**: To understand PCIe/NVMe simulation layer implementation requirements;
- **Test Engineers**: To develop test strategies and acceptance criteria;
- **Project Managers**: To perform project planning and resource allocation.

## 1.3 Terms and Abbreviations

| Abbreviation | Full Name | Description |
|--------------|-----------|-------------|
| SSD | Solid State Drive | Solid-state storage device |
| NAND | Negative-AND | NAND Flash, the primary storage medium of SSDs |
| NOR | NOR Flash | NOR-type flash, typically used for firmware code storage |
| FTL | Flash Translation Layer | Core firmware component of SSDs |
| GC | Garbage Collection | Reclamation of invalid data blocks |
| WL | Wear Leveling | Balancing erase counts across blocks |
| BBM | Bad Block Management | Tracking and handling defective blocks |
| LBA | Logical Block Address | Host-side address |
| PBA | Physical Block Address | NAND-side address |
| LPN | Logical Page Number | Logical page identifier |
| PPN | Physical Page Number | Physical page identifier |
| NVMe | Non-Volatile Memory Express | High-speed SSD interface protocol |
| PCIe | Peripheral Component Interconnect Express | High-speed serial computer expansion bus |
| BAR | Base Address Register | PCIe Base Address Register |
| MSI-X | Message Signaled Interrupts Extended | Extended message-signaled interrupts |
| SQ | Submission Queue | NVMe Submission Queue |
| CQ | Completion Queue | NVMe Completion Queue |
| HAL | Hardware Access Layer | Hardware abstraction layer |
| RTOS | Real-Time Operating System | Real-time OS |
| OOB | Out-Of-Band | Out-of-band management |
| ECC | Error Correction Code | Error correction coding |
| LDPC | Low-Density Parity-Check | LDPC error correction code |
| ZNS | Zoned Namespace | NVMe Zoned Namespace feature |
| KV-SSD | Key-Value SSD | Key-value solid-state drive |
| DMA | Direct Memory Access | Direct memory access |
| MMIO | Memory-Mapped I/O | Memory-mapped I/O |
| ONFI | Open NAND Flash Interface | Open NAND Flash interface standard |
| SLC | Single-Level Cell | 1 bit per cell |
| MLC | Multi-Level Cell | 2 bits per cell |
| TLC | Triple-Level Cell | 3 bits per cell |
| QLC | Quad-Level Cell | 4 bits per cell |
| CE | Chip Enable | Chip select signal |
| WP | Write Protect | Write protect signal |
| RB | Ready/Busy | Ready/Busy signal |
| PRD | Product Requirements Document | This document |
| IOPS | I/O Operations Per Second | I/O throughput metric |
| QoS | Quality of Service | Service quality guarantees |
| UPLP | Unexpected Power Loss Protection | Enterprise power-fail protection |
| T10 DIF | T10 Data Integrity Field | SCSI/NVMe data protection standard |
| PI | Protection Information | End-to-end data integrity metadata |
| DWRR | Deficit Weighted Round Robin | Fair scheduling algorithm |
| DEK | Data Encryption Key | Per-data-unit encryption key |
| KEK | Key Encryption Key | Key wrapping key |
| TCG | Trusted Computing Group | Security standards body |
| SSC | Security Subsystem Class | TCG Opal security profile |

## 1.4 References

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
11. GitHub: snu-csl/nvmevirt -- NVMeVirt open-source repository.
12. GitHub: CMU-SAFARI/MQSim -- MQSim open-source repository.
13. GitHub: MoatLab/FEMU -- FEMU open-source repository, v10.1, 2024.
14. Linux Kernel Documentation: "NVMe PCI Endpoint Function Target."
15. JEDEC Standard JESD230D: "NAND Flash Storage Device."
16. NVM Express Inc. "NVM Express Zoned Namespace Command Set Specification."
17. NVM Express Inc. "NVM Express Key Value Command Set Specification."
18. GitHub: OpenSSD/jasmine -- OpenSSD Jasmine firmware.
19. FITEE 2022. "SoftSSD: enabling rapid flash firmware prototyping."

---

# Chapter 2: Industry Background and Survey

## 2.1 SSD Industry Status and Trends

### 2.1.1 Market Overview

Solid-State Drives (SSDs) have become one of the most critical storage components in modern computing systems. From consumer-grade laptops to enterprise data centers, SSDs are comprehensively replacing traditional Hard Disk Drives (HDDs) with advantages such as high-speed random read/write, low latency, and low power consumption. According to market research data, the global SSD market exceeded $50 billion in 2023 and is projected to surpass $100 billion by 2028.

In the enterprise market, the explosive growth of AI/ML workloads, real-time databases, and high-frequency trading applications has driven increasingly demanding SSD performance requirements:
- Sequential read/write bandwidth: Over 14 GB/s on PCIe 5.0 x4 interfaces;
- Random read IOPS: Exceeding 2,000,000 IOPS (4KB blocks);
- Read latency: Below 70 us;
- Mixed read/write latency stability: P99.9 latency controlled within 1 ms.

### 2.1.2 Increasing SSD Internal Architecture Complexity

Modern enterprise SSD internal architecture has evolved far beyond early simple implementations, primarily in the following areas:

**Multi-core Controllers**: Modern SSD controllers (from major vendors such as Western Digital, Samsung, Kioxia) commonly employ 4-8 core ARM Cortex-R series processors. Some cutting-edge designs even incorporate RISC-V cores for handling NVMe commands, FTL computation, ECC/LDPC operations, and GC scheduling in parallel.

**3D NAND Stacking Evolution**: Current production 3D NAND has reached 232 layers (Micron), with roadmaps extending beyond 300 layers (Samsung, Kioxia). Single NAND die capacity has grown from 8 Gb in early generations to over 2 Tb, and internal multi-plane, multi-die parallelism features have become increasingly important.

**NAND Interface Speed Increases**: From ONFI 1.0 at 50 MB/s, to ONFI 4.2 at 1600 MB/s, to ONFI 5.0 at 2400 MB/s -- each generation of interface standards requires corresponding firmware-level optimizations for timing control, data buffering, and command concurrency.

**ECC Evolution toward LDPC**: Traditional BCH error correction codes can no longer meet the error correction demands of high-layer-count 3D NAND. Soft-Decision LDPC decoding has become mainstream, along with Read Retry and Voltage Offset adjustment mechanisms.

### 2.1.3 Necessity of SSD Firmware Research and Simulation

SSD firmware development faces the following core challenges, which are the fundamental reasons SSD simulators are indispensable:

1. **NAND Hardware Scarcity**: Real NAND chips are expensive. A 16-channel enterprise SSD evaluation platform can cost tens of thousands of dollars and is limited by NAND vendor licensing, making large-scale research deployment difficult;

2. **Reproducibility**: Physical factors in hardware experiments such as NAND aging state and temperature variations are difficult to precisely reproduce, while simulators provide fully controllable experimental environments;

3. **Rapid Prototyping**: Validating FTL algorithm correctness through simulation before actual silicon tape-out can greatly reduce development cycles;

4. **Extreme Scenario Testing**: Certain reliability test scenarios (such as continuous writes until P/E lifetime exhaustion) require months or even years on real hardware but can be accelerated in simulators;

5. **Algorithm Research**: Academic research on novel FTL algorithms, GC strategies, and wear leveling schemes requires an accurate and trustworthy simulation environment for theory validation.

## 2.2 Existing SSD Simulator Technology Survey

### 2.2.1 FlashSim

**Source**: Kim et al., ISEE 2009
**Type**: Event-driven SSD simulator
**Technical Features**:
- Object-oriented programming paradigm with modular design;
- Interoperable with DiskSim full-system simulator for SSD-HDD hybrid system simulation;
- Supports performance and energy comparison of different FTL schemes;
- Relatively simple NAND media modeling without 3D NAND complex timing characteristics;
- Only supports SATA interface simulation, not NVMe/PCIe.

**Limitations**: Early work; does not support multi-queue NVMe protocol, MLC/TLC/QLC timing differentiation modeling, and lacks the concurrency and accuracy required for modern SSD research.

### 2.2.2 MQSim

**Source**: Tavakkol et al., USENIX FAST 2018; GitHub: CMU-SAFARI/MQSim
**Type**: Event-driven multi-queue SSD simulator
**Technical Features**:
- Supports NVMe multi-queue (MQ) and SATA-NCQ host interface protocols;
- NAND Flash hierarchy: Channel -> Chip -> Die -> Plane -> Block -> Page;
- Supports SLC/MLC/TLC cell types with LSB/CSB/MSB page read latency differentiation;
- Program/Erase Suspension mechanism modeling;
- FTL supports page-level mapping (PAGE_LEVEL) and hybrid mapping (HYBRID);
- Address mapping cache table (CMT) with configurable capacity and sharing modes;
- GC strategies: GREEDY, RGA, RANDOM, FIFO;
- Dynamic and static wear leveling dual mechanisms;
- DRAM cache: SIMPLE (write buffer) and ADVANCED (read/write hybrid cache);
- IO priority: URGENT/HIGH/MEDIUM/LOW four levels;
- Approximately 13,000 lines of C++ code;
- Output metrics: End-to-end latency, IOPS, bandwidth, channel utilization.

**Limitations**:
- Pure software time-driven simulation; does not mount real kernel NVMe drivers and cannot be recognized as a real block device by the operating system;
- Lacks precise firmware CPU core thread modeling;
- Does not implement PCIe link-layer simulation.

### 2.2.3 FEMU (Fast, Accurate, Scalable, Extensible Flash Emulator)

**Source**: Li et al., USENIX FAST 2018; GitHub: MoatLab/FEMU
**Type**: QEMU/KVM-based NVMe SSD emulator
**Latest Version**: v10.1 (2024)
**Supported Platforms**: Ubuntu 20.04/22.04/24.04
**Technical Features**:
- Based on QEMU/KVM, presenting as a standard NVMe block device (/dev/nvme0nX) to Guest OS;
- Supports multiple operation modes:
  - **BlackBox SSD (BBSSD)**: Commercial SSD emulation with device-side FTL including page-level address mapping;
  - **WhiteBox SSD (OCSSD)**: OpenChannel SSD with host-side FTL, supporting OpenChannel Spec 1.2/2.0;
  - **Zoned Namespace SSD (ZNSSD)**: NVMe ZNS SSD exposing Zone interface;
  - **NoSSD mode**: Ultra-fast NVMe emulation without storage logic, suitable for SCM/Optane-type device emulation;
- NAND timing model parameters (configurable):
  - Page read latency: 40,000 ns (40 us)
  - Page write latency: 200,000 ns (200 us)
  - Block erase latency: 2,000,000 ns (2 ms)
- GC logic advances plane and channel free-time counters (Tfree) through a series of read/write/erase operations;
- Performance characteristics: IO latency stable within 52 us under 32 threads;
- Code structure:
  - NVMe Controller: NVMe 1.3+ standard implementation
  - Pluggable SSD mode backends
  - Configurable timing model
  - DRAM memory backend
- CI/CD enhancement via GitHub Actions automated testing.

**Limitations**:
- Depends on QEMU/KVM virtualization infrastructure, adding a layer of virtualization overhead;
- Relatively coarse-grained NAND timing model without complete modeling of multi-channel concurrency contention, die-level interleave, and other details;
- Firmware CPU core not modeled.

### 2.2.4 NVMeVirt

**Source**: Kim et al., USENIX FAST 2023; GitHub: snu-csl/nvmevirt
**Type**: Linux kernel module, software-defined virtual NVMe device
**Technical Features**:
- Fully implemented in Linux kernel space without QEMU/KVM, presenting as a native PCIe NVMe device to the host system;
- Three core components:
  1. **PCIe Device Emulator**: Virtualizes the PCIe bus, sets up PCI configuration header and BAR registers pointing to in-memory control blocks;
  2. **NVMe Controller Emulator**: Handles Admin/IO command sets, manages SQ/CQ pairs;
  3. **Storage Backend**: Supports NVM SSD (Optane-type), Conventional SSD, ZNS SSD, and KV SSD;
- Physical memory reservation (memmap) as NVMe storage medium, reserving large physical memory blocks (e.g., 64 GiB) via GRUB parameters;
- Creates I/O dispatcher and multiple I/O worker threads on designated CPU cores at startup;
- Supports NVMe-oF target offload, kernel bypass, and PCIe peer-to-peer DMA;
- Approximately 9,000 lines of code based on Linux kernel 5.15;
- Recommended kernel version: v5.15.x and above (tested on Ubuntu kernel v5.15.0-58-generic);
- Supports PCIe peer-to-peer communication.

**Key Technical Mechanisms**:
- **BAR Mapping**: NVMeVirt sets BAR fields in the PCI configuration header, pointing to control blocks in reserved physical memory;
- **NVMe Layer Recognition**: Host NVMe driver recognizes and operates on the control blocks in BAR;
- **Thread Model**: Requires at least two CPU cores -- one I/O dispatcher thread plus one or more I/O worker threads;
- **Device Type Selection**: Target device type is selected by modifying the Kbuild file.

**Limitations**:
- Current storage backend is relatively simple, lacking a complete three-layer firmware architecture;
- Limited NAND media emulation accuracy (primarily latency injection, not a complete timing model);
- Firmware CPU core threads not modeled;
- Does not support NOR Flash modeling.

### 2.2.5 SimpleSSD / Amber (SimpleSSD 2.0)

**Source**: Jung et al., IEEE CAL 2018 (SimpleSSD); MICRO 2018 (Amber)
**Type**: SSD emulation framework integrated with full-system simulator (gem5)
**Technical Features**:
- Three-layer firmware architecture:
  1. Host Interface Layer (HIL): Processes I/O requests from the gem5 disk controller, translating to LBA;
  2. Flash Translation Layer (FTL): Logical-to-physical address translation, GC, wear leveling;
  3. Parallelism Allocation Layer (PAL): Distributes requests to different channels/packages/dies;
- Supports SATA, UFS, NVMe, OCSSD, and other interfaces;
- Amber (2.0) additionally includes: DRAM cache logic, firmware CPU core power modeling, DMA engine emulation;
- Supports address-based TLC latency differentiation (LSB/CSB/MSB pages);
- Integrated with gem5 for full-system performance analysis;
- Validation accuracy: Average write test error 2.7%, read test error 7.1% (compared to real Intel 750 SSD).

**Limitations**:
- Depends on the gem5 full-system simulation environment, making deployment complex;
- Cannot run as a usable storage device on a real Linux server;
- Insufficient real-time modeling capability.

### 2.2.6 DeepFlash (Multi-core Firmware Platform)

**Source**: Zhang et al., USENIX FAST 2020
**Type**: Multi-core SSD firmware platform research
**Technical Features**:
- Scalable firmware architecture for many-core SSD controllers;
- Three-stage pipeline model:
  1. Queue-gather stage: Collects I/O requests from multiple NVMe SQs;
  2. Trans-apply stage: FTL logical-to-physical address translation;
  3. Flash-scatter stage: Distributes operations to flash channels;
- Many-to-many thread model supporting horizontal scaling;
- Lock-free data structures to reduce inter-core synchronization overhead;
- FTL distributed across multiple cores to avoid single-core bottlenecks;
- Requires only 12 ordered cores to achieve 1M+ IOPS;
- CACHE thread: Uses SSD internal DRAM as burst buffer, mapping logical block addresses to DRAM addresses, with data ultimately flushed to flash packages via striping.

**Key Design Insights**:
- Linear scaling relationship between firmware core count and IOPS;
- Importance of lock-free design for high-concurrency firmware;
- FTL task partitioning strategies.

### 2.2.7 SoftSSD

**Source**: FITEE 2022, SoftSSD: enabling rapid flash firmware prototyping
**Type**: Software-defined SSD firmware prototyping platform
**Technical Features**:
- Event-driven programming model;
- Allows new FTL algorithms to be directly integrated into a full-function flash firmware;
- Provides a standardized flash firmware framework, lowering the firmware development barrier;
- Supports rapid algorithm prototype validation.

### 2.2.8 SoftSSD

**Source**: FITEE 2022, SoftSSD: enabling rapid flash firmware prototyping
**Type**: Software-defined SSD firmware prototyping platform
**Technical Features**:
- Event-driven programming model;
- Allows new FTL algorithms to be directly integrated into a full-function flash firmware;
- Provides a standardized flash firmware framework, lowering the firmware development barrier;
- Supports rapid algorithm prototype validation.

### 2.2.9 OpenSSD Jasmine

**Source**: OpenSSD Project; GitHub: OpenSSD/jasmine
**Type**: Real SSD hardware platform firmware
**Technical Features**:
- OpenSSD platform firmware, implemented in C;
- 58 stars, 22 forks;
- Reference firmware for real SSD hardware platforms;
- Can serve as a firmware architecture design reference.

### 2.2.10 Survey Conclusions

Based on the systematic survey of the above major SSD simulators, the summary is as follows:

| Feature | FlashSim | MQSim | FEMU | NVMeVirt | SimpleSSD | DeepFlash | OpenSSD |
|---------|----------|-------|------|----------|-----------|-----------|---------|
| Real NVMe Device Presentation | No | No | Yes (QEMU) | Yes (bare-metal) | No | No | No (hardware) |
| Multi-queue NVMe | No | Yes | Yes | Yes | Yes | Yes | Yes |
| Accurate NAND Timing | Basic | Medium | Basic | Basic | Medium | Basic | Yes (real) |
| Multi-core Firmware Modeling | No | No | No | No | Partial | Yes | Yes |
| Three-layer Firmware Architecture | No | No | No | No | Partial | No | Yes |
| DRAM Backend | No | Yes | Yes | Yes | Yes | Yes | No |
| NOR Flash | No | No | No | No | No | No | Yes |
| ZNS Support | No | No | Yes | Yes | No | No | No |
| KV-SSD Support | No | No | No | Yes | No | No | No |
| Complete GC/WL | Basic | Yes | Basic | No | Basic | No | Yes |
| Bare-metal Linux Execution | Yes | Yes | No | Yes | No | Yes | Yes |
| Data Persistence | No | No | No | No | No | No | Yes (real NAND) |
| Open Source | Yes | Yes | Yes | Yes | Yes | Yes | Yes |

**Core Gap**: Currently, no single open-source simulator can simultaneously satisfy: (a) native execution on bare-metal Linux servers, (b) presenting a real NVMe device to the host, (c) a complete three-layer firmware architecture (HAL + Common Service + Application), (d) accurate 16-channel NAND/NOR media timing modeling, and (e) data persistence to the file system. This product aims to fill this gap.

**Key Reference Projects**:
- **NVMeVirt**: Core reference for the PCIe/NVMe device emulation layer
- **FEMU**: Reference for ZNS/OCSSD/NoSSD multi-mode support
- **MQSim**: Reference for NAND timing model and multi-queue scheduling
- **OpenSSD**: Reference for real firmware three-layer architecture

---

# Chapter 3: Product Definition

## 3.1 Product Vision

To build the industry's first **High-Fidelity Full-Stack SSD Simulator** that can completely reproduce the full-stack behavior of an enterprise SSD -- from host interface (PCIe/NVMe) to NAND media -- on a standard x86 Linux server, leveraging large-capacity DRAM and multi-core CPU resources. This includes accurate three-layer firmware architecture, 16-channel concurrent media timing, complete FTL algorithm ecosystem, and persistent data storage capability. The simulator will serve as core infrastructure for SSD firmware research, novel FTL algorithm validation, NVMe protocol testing, and storage system research.

## 3.2 Product Name

**HFSSS -- High-Fidelity Full-Stack SSD Simulator**

## 3.3 Target Users

### 3.3.1 Primary User Groups

**Enterprise SSD Firmware R&D Teams**
- Need: Validate firmware algorithm correctness without real NAND hardware;
- Scenario: Development and validation of novel FTL algorithms, GC strategies, and WL algorithms;
- Key Requirement: High-fidelity firmware behavior, complete three-layer architecture, compatibility with real firmware code structure.

**Storage System Academic Researchers**
- Need: Fine-grained observation and control of SSD internal behavior;
- Scenario: FTL algorithm paper validation, NVMe protocol research, storage system performance analysis;
- Key Requirement: Strong observability, good extensibility, configurable parameters.

**NVMe Protocol Test Engineers**
- Need: Validate correctness of host NVMe drivers, file systems, and storage software stacks;
- Scenario: IO path stress testing, protocol conformance testing, fault injection testing;
- Key Requirement: Standard NVMe device presentation to host, fault simulation support.

**Data Center Storage Architects**
- Need: Evaluate the performance impact of different SSD configurations on systems before purchasing real SSD hardware;
- Scenario: Capacity planning, performance prediction, storage architecture selection;
- Key Requirement: Configurable performance, support for multiple SSD profiles.

### 3.3.2 Secondary User Groups

- **Operating System Kernel Developers**: Testing Linux NVMe drivers, blk-mq layer, io_uring, and other modules;
- **File System Developers**: Validating file system behavior on the SSD simulator (e.g., ext4, XFS, F2FS, NOVA);
- **Database Storage Engine Developers**: Analyzing database I/O patterns and their interaction with SSD internal behavior;
- **AI/ML Storage Engineers**: Evaluating storage I/O characteristics of large-scale AI training and SSD performance matching.

## 3.4 Product Positioning

HFSSS is positioned as an **enterprise-grade, high-accuracy, full-stack SSD simulation platform** with the following core differentiators:

1. **Full-Stack Coverage**: Covering the complete SSD internal stack from PCIe physical layer to NAND media, including firmware CPU core modeling and NOR Flash modeling not covered by other simulators;

2. **Native Linux Integration**: Running as a kernel module, presenting a real PCIe NVMe block device to the host Linux kernel without QEMU, supporting direct use by all standard storage tools such as fio and nvme-cli;

3. **Multi-core Utilization**: On 64-256 core Debian servers, binding different modules (controller threads, media threads, firmware CPU threads) to independent CPU cores to maximize concurrency;

4. **Large-capacity DRAM Utilization**: Using 64 GB or more of physical DRAM as emulated storage media, with data persistence through the host file system;

5. **Firmware Architecture Completeness**: Implementing the complete three-layer firmware architecture (HAL + Common Service + Application Layer), providing a realistic runtime environment for firmware algorithm research;

6. **High Configurability**: Supporting flexible SSD specification configuration (capacity, channel count, die count, cell type, etc.), covering a wide product spectrum from entry-level to enterprise NVMe SSDs.

## 3.5 Product Boundaries

**Included in this product**:
- Complete NVMe/PCIe host interface emulation (Admin + I/O command sets);
- 16-channel NAND Flash media emulation (SLC/MLC/TLC/QLC configurable);
- NOR Flash media emulation;
- Complete implementation of the three-layer firmware architecture;
- Data persistence to host file system;
- Complete fault injection framework;
- Performance statistics and observability interfaces;
- Out-of-Band (OOB) management interface;
- TCG Opal SSC basic command set simulation (simulation level);
- AES-XTS 256-bit data-at-rest encryption simulation (simulation level);
- T10 DIF/PI end-to-end data protection simulation (simulation level).

**Not included in this product**:
- Real PCIe hardware IP cores (no RTL/FPGA implementation);
- Real NAND hardware controller drivers (for real silicon);
- Consumer SSD-specific product firmware code (vendor intellectual property);
- QEMU-based virtualization implementation (this product is a bare-metal native implementation);
- UFS, SATA, and other non-NVMe interfaces (out of scope for this version).

## 3.6 Product Version Planning

| Version | Codename | Goal | Key Features |
|---------|----------|------|--------------|
| V1.0 | Alpha | MVP | PCIe/NVMe basic emulation, DRAM backend, basic FTL (page-level mapping + GC) |
| V1.5 | Beta | Complete Media | 16-channel NAND accurate timing, NOR Flash, multi-core firmware threads |
| V2.0 | GA | Full Stack | Complete three-layer firmware architecture, persistence, QoS, OOB management |
| V2.5 | Enterprise | Enterprise-grade | ZNS support, KV-SSD, fault injection, performance analysis framework |
| V3.0 | Enterprise+ | Enterprise Security & Reliability | Unexpected Power Loss Protection (UPLP), QoS Determinism (DWRR, P99.9 SLA), T10 DIF/PI end-to-end data protection, TCG Opal / AES-XTS data-at-rest encryption, multi-namespace management (32 NS), thermal management and throttling, advanced telemetry and diagnostics |

---

# Chapter 4: Overall System Architecture

## 4.1 System Architecture Overview

HFSSS adopts a **modular, multi-threaded, layered** system architecture. The entire system runs on a Debian Linux server, presenting a virtual NVMe PCIe device to the host operating system as a kernel module, while running firmware emulation logic as a user-space daemon.

The system is logically divided into five major modules:

```
+---------------------------------------------------------------------+
|                    Host Linux Operating System                       |
|   NVMe Driver | blk-mq | File System | fio/nvme-cli | User Apps    |
+-------------------------------+-------------------------------------+
                                | PCIe / NVMe Interface
+-------------------------------v-------------------------------------+
|         Module 1: PCIe/NVMe Device Emulation Layer (Kernel Module)  |
|   PCI BAR Emulation | NVMe SQ/CQ Mgmt | DMA Engine | MSI-X | Cmd  |
+-------------------------------+-------------------------------------+
                                | Internal Command Bus (Shared Memory Ring Buffer)
+-------------------------------v-------------------------------------+
|         Module 2: Controller Thread                                  |
|   Command Arbitration | IO Scheduling | Flow Control | Resource Mgmt|
+----------+------------------+------------------------+--------------+
           |                  |                        |
+----------v------+ +---------v-----------+ +---------v--------------+
| Module 3:       | | Module 4:           | | Module 5:              |
| Firmware CPU    | | Media Threads       | | OOB/Monitoring         |
| Core Threads    | | (16 Channels)       | | Management Threads     |
| (Three-layer    | | NAND/NOR Emulation  | |                        |
|  FW Arch)       | |                     | |                        |
+-----------------+ +----------+----------+ +------------------------+
           |                   |
+----------v-------------------v----------------------------------+
|                    DRAM Storage Backend (64GB+)                   |
|      Reserved physical memory region via memmap, invisible to    |
|      host OS                                                     |
+----------------------------------+-------------------------------+
                                   | (Persistence Path)
+----------------------------------v-------------------------------+
|                    Host File System (Persistence Layer)           |
|         ext4/XFS data files, emulated NAND/NOR media content     |
+------------------------------------------------------------------+
```

## 4.2 Hardware Resource Planning

### 4.2.1 CPU Core Allocation Strategy

On a 64-256 core server, HFSSS CPU core allocation follows these principles:

**Minimum Configuration (64-core server)**:

| Core Group | Core Count | Purpose |
|------------|------------|---------|
| PCIe/NVMe Emulation Group | 4 | I/O Dispatcher thread x1, I/O Worker threads x3 |
| Controller Thread Group | 8 | Controller arbitration/scheduling cores x2, command processing cores x4, GC/WL background cores x2 |
| Firmware CPU Core Group | 16 | Emulating SSD controller ARM cores: FTL cores x4, HAL cores x4, scheduling cores x4, error handling cores x4 |
| NAND Media Thread Group | 32 | 2 threads per channel (16 channels x 2 = 32) |
| NOR Media Thread Group | 2 | NOR Flash threads x2 |
| OOB/Monitoring Group | 2 | OOB management thread x1, system monitoring thread x1 |
| Host Reserved | Remaining | Host OS and other processes |

**Recommended Configuration (128-core server)**:

| Core Group | Core Count | Purpose |
|------------|------------|---------|
| PCIe/NVMe Emulation Group | 8 | I/O Dispatcher x2, I/O Worker x6 |
| Controller Thread Group | 16 | Controller cores x8, GC/WL dedicated cores x4, flow control cores x4 |
| Firmware CPU Core Group | 32 | Emulating multi-core SSD controller, covering complete three-layer firmware |
| NAND Media Thread Group | 48 | 3 threads per channel (read/write/erase) x 16 channels |
| NOR Media Thread Group | 4 | Dedicated NOR read/write threads |
| OOB/Monitoring/Debug Group | 8 | OOB, monitoring, logging, debug, event queue |
| Host Reserved | 12 | Host OS |

**Maximum Configuration (256-core server)**: Increase firmware CPU core density, add more concurrent threads per channel, introduce dedicated ECC processing threads.

### 4.2.2 DRAM Memory Allocation Strategy

System physical memory (assuming 128 GB as example) is allocated as follows:

```
+--------------------------------------------------------+
|  Total Physical Memory: 128 GB                          |
+--------------------------------------------------------+
|  Host OS Reserved: 16 GB (kernel + user space + cache)  |
+--------------------------------------------------------+
|  NVMeVirt Kernel Module Working Memory: 4 GB            |
+--------------------------------------------------------+
|  NAND Media Emulation Storage (memmap reserved): 96 GB  |
|  +- Channel 0-15 Data Storage: 90 GB                   |
|  +- Metadata (mapping tables, BBT, counters): 6 GB     |
+--------------------------------------------------------+
|  NOR Flash Emulation: 256 MB                            |
+--------------------------------------------------------+
|  Firmware CPU Core Working Memory: 4 GB                 |
|  +- FTL Mapping Table Cache: 2 GB                      |
|  +- Command Buffer: 1 GB                               |
|  +- Other Firmware Data: 1 GB                          |
+--------------------------------------------------------+
|  DRAM Cache (SSD Internal Write Buffer Emulation): 4 GB |
+--------------------------------------------------------+
```

Memory reservation is achieved through kernel boot parameters, configured in `/etc/default/grub`:
```
GRUB_CMDLINE_LINUX="memmap=96G\$16G"
```
This parameter reserves 96 GB of memory starting from a 16 GB physical address offset, making this region unavailable to the host OS.

### 4.2.3 Storage I/O Persistence Path

The emulated NAND media data persistence uses a two-tier strategy:

1. **Primary Path (Hot Data)**: Direct storage in the reserved DRAM region, providing nanosecond-level access latency;
2. **Persistence Path (Cold Data)**: A background thread periodically writes data blocks from DRAM to the host ext4/XFS file system as binary files, ensuring data survives power loss.

Persistence file organization:
```
/var/hfsss/
+-- nand/
|   +-- ch00/
|   |   +-- chip00_die00_plane00.bin
|   |   +-- chip00_die00_plane01.bin
|   |   +-- ...
|   +-- ch01/
|   |   +-- ...
|   +-- ... (16 channel directories total)
+-- nor/
|   +-- firmware_storage.bin
+-- metadata/
|   +-- l2p_table.bin     (Logical-to-Physical mapping table)
|   +-- bad_block.bin     (Bad block table)
|   +-- erase_count.bin   (P/E cycle counts)
|   +-- system_info.bin   (System state)
+-- logs/
    +-- event_log.bin
```

## 4.3 Software Architecture Layer Diagram

```
+----------------------------------------------------------------+
|                    Host Application Layer                        |
|  fio | nvme-cli | File System | Database | User Storage Apps    |
+-----------------------------+----------------------------------+
                              |
+-----------------------------v----------------------------------+
|         Host Kernel Layer (Linux NVMe Stack)                    |
|  nvme.ko | nvme-core.ko | blk-mq | io_uring | block layer     |
+-----------------------------+----------------------------------+
                              | PCIe/NVMe Protocol
+=============================v==================================+
||         HFSSS Kernel Module Layer (hfsss_nvme.ko)            ||
||  +----------------------------------------------------------+||
||  | PCIe Emulation Sublayer: PCI Config Space | BAR | MSI-X   |||
||  +----------------------------------------------------------+||
||  | NVMe Protocol Sublayer: Admin Queue | IO Queue | Cmd Parse|||
||  +----------------------------------------------------------+||
+=============================+==================================+
                              || Kernel->User Space (Shared Memory/ioctl)
+=============================v==================================+
||         HFSSS User-space Daemon (hfsss-daemon)               ||
||                                                               ||
||  +----------------------------------------------------------+||
||  |         Controller Thread                                 |||
||  |  Cmd Arbitration | IO Scheduling | Resource Alloc | IPC   |||
||  +----------------------------------------------------------+||
||                         |                                     ||
||  +-----------------------v----------------------------------+||
||  |    Firmware CPU Core Thread Group                         |||
||  |                                                           |||
||  |  +------------------------------------------------------+|||
||  |  | Application Layer (Algorithm Task Layer)              ||||
||  |  | FTL | GC | WL | BBM | ECC | QoS | Redundancy | Err  ||||
||  |  +------------------------------------------------------+|||
||  |  | Common Service Layer (Platform Services)              ||||
||  |  | RTOS | Scheduler | MemMgmt | Boot | IPC | Monitor    ||||
||  |  +------------------------------------------------------+|||
||  |  | Hardware Access Layer (HAL)                           ||||
||  |  | NAND Driver | NOR Driver | NVMe/PCIe | Power Mgmt    ||||
||  |  +------------------------------------------------------+|||
||  +-----------------------------------------------------------+||
||                         |                                     ||
||  +-----------------------v----------------------------------+||
||  |    Media Thread Group                                     |||
||  |  Channel 0-15 x (NAND Chip threads + Timing Controller)  |||
||  |  NOR Flash Thread                                         |||
||  +-----------------------------------------------------------+||
||                         |                                     ||
||  +-----------------------v----------------------------------+||
||  |    DRAM Storage Backend + File System Persistence Layer    |||
||  +-----------------------------------------------------------+||
+================================================================+
```

---

# Chapter 5: Functional Requirements

## 5.1 PCIe/NVMe Device Emulation Module

### 5.1.1 Module Overview

The PCIe/NVMe device emulation module is the interface layer between HFSSS and the host operating system. Implemented as a Linux kernel module, it references the core mechanisms of NVMeVirt to virtualize a standard PCIe NVMe device within the host Linux kernel. The core challenge of this module is: without using real PCIe hardware IP, to fool the Linux NVMe driver through pure software into believing that a real PCIe NVMe SSD exists in the system.

### 5.1.2 PCIe Configuration Space Emulation (FR-PCIE-001)

**Requirement Description**: The simulator must register a virtual PCIe device on the PCI bus, providing a standard PCI Configuration Space.

**Detailed Requirements**:

1. **PCI Configuration Header**: Implement a standard PCI Type 0 configuration header (256-byte base configuration space + 4096-byte extended configuration space), including:
   - Vendor ID / Device ID: Configurable, defaulting to reserved IDs for research purposes;
   - Class Code: Set to 0x010802 (NVM Express Controller);
   - Subsystem Vendor ID / Subsystem ID: Configurable;
   - Revision ID: 0x01;
   - Header Type: 0x00 (Standard);
   - Capabilities Pointer: Points to the start of the Capabilities linked list.

2. **PCIe Capabilities Linked List**: Implement the following required Capability structures:
   - PCI Power Management Interface (Cap ID 0x01);
   - MSI Capability (Cap ID 0x05);
   - MSI-X Capability (Cap ID 0x11);
   - PCIe Capability (Cap ID 0x10), reporting device type as PCIe Endpoint, Link speed configurable as Gen3x4 or Gen4x4.

3. **BAR (Base Address Register) Configuration**:
   - BAR0: Mapped to reserved physical memory region as the NVMe controller register (MMIO region), size >= 16 KB;
   - BAR2/BAR4 (optional): Used for MSIX Table and PBA (Pending Bit Array).

4. **Device Enumeration**: Register the virtual PCI device through the Linux kernel's `pci_register_driver` mechanism, making it discoverable and listable by the `lspci` command.

**Acceptance Criteria**:
- `lspci -v` displays the emulated NVMe device with Class "Non-Volatile memory controller";
- `lspci -vvv` displays the complete Capabilities linked list;
- Linux NVMe driver (nvme.ko) successfully probes and binds to the virtual device.

### 5.1.3 NVMe Controller Register Emulation (FR-NVME-001)

**Requirement Description**: Implement the complete NVMe controller register set (NVMe Controller Registers) in the BAR0-mapped MMIO region, corresponding to the NVMe Specification 2.0 register definitions.

**Key Registers**:

1. **CAP (Controller Capabilities, offset 0x00, 64-bit)**:
   - MQES: Maximum Queue Entries Supported, up to 65535 entries;
   - CQR: Contiguous Queues Required, recommended to set to 0 (not required);
   - AMS: Arbitration Mechanism Supported, supporting Round Robin + Weighted Round Robin;
   - TO: Timeout, controller ready timeout in units of 500 ms;
   - DSTRD: Doorbell Stride, set to 0 (4-byte aligned);
   - NSSRS: NVM Subsystem Reset Supported;
   - CSS: Command Sets Supported, supporting NVM Command Set;
   - MPSMIN/MPSMAX: Memory page size range, supporting 4 KB-64 KB.

2. **VS (Version, offset 0x08, 32-bit)**: Reports NVMe version, default set to 0x00020000 (NVMe 2.0).

3. **CC (Controller Configuration, offset 0x14, 32-bit)**:
   - EN: Enable bit, writing 1 initializes the controller;
   - CSS: I/O Command Set Selection;
   - MPS: Memory Page Size;
   - AMS: Arbitration Mechanism;
   - SHN: Shutdown Notification (Normal Shutdown / Abrupt Shutdown);
   - IOSQES/IOCQES: IO SQ/CQ Entry Size (64-byte command entries).

4. **CSTS (Controller Status, offset 0x1C, 32-bit)**:
   - RDY: Controller Ready bit;
   - CFS: Controller Fatal Status;
   - SHST: Shutdown Status.

5. **AQA (Admin Queue Attributes, offset 0x24, 32-bit)**: Admin SQ/CQ size configuration.

6. **ASQ (Admin Submission Queue Base Address, offset 0x28, 64-bit)**: Physical base address of Admin SQ.

7. **ACQ (Admin Completion Queue Base Address, offset 0x30, 64-bit)**: Physical base address of Admin CQ.

8. **Doorbell Registers (offset 0x1000 onwards)**:
   - SQ y Tail Doorbell (offset 1000h + (2y x (4 << CAP.DSTRD)));
   - CQ y Head Doorbell (offset 1000h + ((2y+1) x (4 << CAP.DSTRD)));
   - Support for up to 64 SQ/CQ pairs (configurable).

**Acceptance Criteria**:
- `nvme list` identifies and lists the emulated device;
- `nvme id-ctrl /dev/nvme0` returns complete controller identification information;
- After the controller initialization sequence (writing CC.EN=1), CSTS.RDY is set to 1 within the specified timeout.

### 5.1.4 NVMe Queue Management (FR-NVME-002)

**Requirement Description**: Implement complete NVMe Submission Queue (SQ) and Completion Queue (CQ) management mechanisms.

**Detailed Requirements**:

1. **Admin Queue Pair** (fixed QID=0):
   - Admin SQ: Receives Admin commands (Create SQ/CQ, Delete SQ/CQ, Identify, Get/Set Features, Format NVM, Firmware operations, etc.);
   - Admin CQ: Returns Admin command completion status;
   - Maximum depth: Configurable, default 256 entries;
   - Physical address specified by ASQ/ACQ registers, contents read via MMIO DMA.

2. **I/O Queue Pairs** (QID=1-65535):
   - Dynamically created through Admin commands (Create I/O SQ / Create I/O CQ);
   - Support for up to 64 I/O SQ/CQ pairs (adjustable via compile-time parameters);
   - Each SQ is associated with one CQ (via QPID field);
   - Queue depth: Configurable, range 2-65535 entries;
   - Priority: Support for NVMe WRR priority (Urgent/High/Medium/Low);
   - Support for both contiguous (Contiguous) and non-contiguous (PRP List) memory layouts.

3. **Command Polling Mechanism**:
   - I/O Dispatcher thread continuously monitors all SQ Tail Doorbells;
   - Doorbell updates trigger Dispatcher thread wakeup to fetch commands from SQs;
   - Efficient lock-free Ring Buffer implementation for SQ content reading.

4. **PRPs (Physical Region Pages) and SGL (Scatter Gather List)**:
   - Implement PRP1/PRP2 and PRP List mechanisms, supporting non-contiguous physical memory data buffers;
   - Implement SGL Descriptor parsing (data block, bit bucket, key reference types);
   - DMA operations: Host memory to emulated storage data transfer via kernel `memcpy` or DMA API.

5. **Completion Processing**:
   - Upon command completion, write the 64-byte CQE (Completion Queue Entry) to the corresponding CQ's Head position;
   - Update CQ Phase Tag (lowest bit of CQE, alternating);
   - Deliver interrupt to host via MSI-X mechanism.

**Acceptance Criteria**:
- `nvme create-ns`, `nvme attach-ns`, and similar commands execute correctly;
- `fio --ioengine=io_uring --numjobs=32 --iodepth=128` stress tests run stably;
- Support for 64 I/O Queue pairs running simultaneously.

### 5.1.5 MSI-X Interrupt Emulation (FR-NVME-003)

**Requirement Description**: Implement MSI-X (Message Signaled Interrupts Extended) interrupt mechanism for notifying the host NVMe driver of command completion.

**Detailed Requirements**:

1. **MSI-X Table**: Maintain up to 64 MSI-X Table Entries, each containing:
   - Message Address (64-bit): Target CPU's local APIC address;
   - Message Data (32-bit): Interrupt vector number;
   - Vector Control (32-bit): Interrupt mask bit.

2. **MSI-X PBA** (Pending Bit Array): 64-bit PBA marking pending interrupts.

3. **Interrupt Delivery**:
   - After completing a command, look up the MSI-X Table using the CQ's corresponding MSI-X Vector index;
   - Trigger x86 APIC interrupt by writing Message Data to Message Address;
   - In the Linux kernel module, use `apic->send_IPI` or write to MMIO address to simulate MSI-X delivery;
   - Each CQ can be independently assigned different MSI-X Vectors for interrupt affinity (binding interrupts to specific CPU cores).

4. **Interrupt Coalescing**:
   - Support NVMe interrupt coalescing feature (Set Features / Interrupt Coalescing);
   - Configurable aggregation threshold and aggregation time;
   - Reduce interrupt frequency in high-IOPS scenarios, lowering CPU interrupt handling overhead.

**Acceptance Criteria**:
- `nvme set-feature /dev/nvme0 --feature-id=8 --value=<agg_threshold,agg_time>` can configure interrupt coalescing;
- `cat /proc/interrupts | grep nvme` displays MSI-X interrupt allocation;
- Interrupt affinity is configurable via `/proc/irq/<n>/smp_affinity`.

### 5.1.6 NVMe Admin Command Set (FR-NVME-004)

**Requirement Description**: Implement the complete NVMe Admin command set as defined in the NVMe specification, ensuring all management operations from host-side nvme-cli and kernel drivers execute correctly.

**Required Admin Commands**:

| Opcode | Command Name | Description |
|--------|-------------|-------------|
| 0x00 | Delete I/O SQ | Delete I/O Submission Queue |
| 0x01 | Create I/O SQ | Create I/O Submission Queue (QID, QSIZE, CQID, QPRIO, PHYS_CONTIG) |
| 0x02 | Get Log Page | Retrieve log pages (Error Log, SMART/Health, FW Slot, Changed NS, etc.) |
| 0x04 | Delete I/O CQ | Delete I/O Completion Queue |
| 0x05 | Create I/O CQ | Create I/O Completion Queue (QID, QSIZE, IEN, IV, PHYS_CONTIG) |
| 0x06 | Identify | Controller identification (CNS=0: NS, CNS=1: Controller, CNS=2: NS List) |
| 0x08 | Abort | Abort a specified command |
| 0x09 | Set Features | Set features (Arbitration, Power Mgmt, LBA Range, Temp Threshold, etc.) |
| 0x0A | Get Features | Get current feature values |
| 0x0C | Async Event Request | Asynchronous event request |
| 0x0D | Namespace Management | Namespace create/delete |
| 0x10 | Firmware Commit | Firmware commit |
| 0x11 | Firmware Image Download | Firmware image download |
| 0x14 | Namespace Attachment | Namespace attach/detach |
| 0x7C | Doorbell Buffer Config | Shadow Doorbell buffer configuration |
| 0xC0 | Format NVM | Format namespace (secure erase) |

**Identify Controller Data Structure** (key fields):
- VID/SSVID: Vendor ID;
- SN (Serial Number): Configurable string;
- MN (Model Number): Configurable string, e.g., "HFSSS Virtual NVMe SSD v2.0";
- FR (Firmware Revision): Firmware version string;
- RAB: Recommended Arbitration Burst;
- IEEE OUI Identifier;
- CMIC: Controller multi-path I/O and namespace sharing capability;
- MDTS: Maximum Data Transfer Size (expressed as power of 2 of minimum page size);
- CNTLID: Controller ID;
- VER: NVMe specification version (0x00020000 = 2.0);
- RTD3R/RTD3E: D3 state resume/entry latency;
- OAES: Optional Asynchronous Event Support;
- SQES/CQES: SQ/CQ Entry Size;
- MAXCMD: Maximum outstanding commands;
- NN: Total number of namespaces;
- ONCS: Optional NVM Command Set support (Write Zeroes, Dataset Management, Verify, etc.);
- FUSES: Fused operation support;
- FNA: Format NVM Attributes;
- VWC: Volatile Write Cache flag;
- AWUN/AWUPF: Atomic Write Unit (Normal/Power Fail);
- NVSCC: NVM Vendor Specific Command Configuration;
- ACWU: Atomic Compare and Write Unit;
- SGLS: SGL support flags;
- SUBNQN: NVMe Subsystem NQN (format: nqn.2026-03.io.hfsss:nvme.0).

### 5.1.7 NVMe I/O Command Set (FR-NVME-005)

**Requirement Description**: Implement the complete NVMe NVM I/O command set, supporting all I/O operations for standard file systems and storage applications.

**Required I/O Commands**:

| Opcode | Command Name | Description |
|--------|-------------|-------------|
| 0x00 | Flush | Persist write cache data |
| 0x01 | Write | Write data to specified LBA range |
| 0x02 | Read | Read data from specified LBA range |
| 0x04 | Write Uncorrectable | Mark specified LBA as uncorrectable |
| 0x08 | Write Zeroes | Zero out specified LBA range (efficient implementation) |
| 0x09 | Dataset Management | Dataset management (Trim/Deallocate) |
| 0x0C | Verify | Verify data integrity of specified LBA range |
| 0x0D | Copy | Data copy (Simple Copy Command) |

**Read/Write Command Detailed Requirements**:
- SLBA (Starting LBA): 64-bit starting logical block address;
- NLB (Number of Logical Blocks): Block count (0-based, max 65535);
- PRP Entry support: PRP1/PRP2/PRP List;
- SGL support: SGL descriptor chain;
- FUA (Force Unit Access): Force write to persistent storage;
- LR (Limited Retry): Limited retry flag;
- DSM (Dataset Management): Sequential read/write, incompressible hints;
- PRINFO: Protection Information operations.

**Dataset Management (Trim)**:
- Support up to 256 LBA Ranges, each Range defined by starting LBA and block count;
- Trim operations notify FTL to release corresponding physical pages, accelerating GC;
- Support Deallocate (Context Attribute bit 2) flag.

### 5.1.8 NVMe DMA Data Transfer (FR-NVME-006)

**Requirement Description**: Implement data transfer mechanisms between host memory and emulated NAND storage backend.

**Detailed Requirements**:

1. **PRP Parsing Engine**:
   - Parse PRP1/PRP2 fields from commands;
   - When data exceeds one physical page, read PRP List (array of page addresses in kernel physical memory);
   - Assemble scattered physical pages into a contiguous data view.

2. **Data Copy Path**:
   - Read command (NAND -> Host): Read data from DRAM storage backend, write to host physical memory via `memcpy_to_user_page` or kernel page mapping;
   - Write command (Host -> NAND): Read data from host physical memory, write to DRAM storage backend, then asynchronously dispatch to media threads;
   - Use `kmap_atomic`/`kunmap_atomic` for mapping high-memory pages;
   - Consider NUMA node affinity: Data copies should execute on the data's local NUMA node whenever possible.

3. **Zero-copy Optimization** (optional, V2.0 implementation):
   - Use Linux `splice` or `sendfile` mechanisms to reduce kernel-to-kernel data copies;
   - For large sequential IO, explore using `move_pages` mechanism for page ownership transfer.

4. **IOMMU Support**:
   - Detect system IOMMU state; if IOMMU is enabled, correctly handle DMA address mapping;
   - Use `dma_map_page`/`dma_unmap_page` APIs to ensure correct translation between DMA and physical addresses.

**Acceptance Criteria**:
- `dd if=/dev/urandom of=/dev/nvme0n1 bs=4M count=100` writes data correctly;
- `md5sum` verifies write-read data consistency;
- Aligned and unaligned IO (512B, 4KB, 8KB, 16KB, 128KB, 1MB) all handled correctly.

---

## 5.2 Controller Thread Module

### 5.2.1 Module Overview

The Controller Thread is the "brain" of the entire SSD simulator, responsible for receiving commands from the PCIe/NVMe module, performing high-level scheduling and resource arbitration, dispatching commands to firmware CPU core threads for execution, and coordinating the collaborative work of all subsystems. The Controller Thread runs in the user-space daemon, using real-time threads (SCHED_FIFO scheduling policy) and CPU affinity binding to ensure low-latency response.

### 5.2.2 Command Reception and Dispatch (FR-CTRL-001)

**Requirement Description**: The Controller Thread receives parsed NVMe commands from the PCIe/NVMe module's kernel mode, performs global arbitration, and dispatches them to firmware CPU core threads.

**Detailed Requirements**:

1. **Kernel-User Space Communication**:
   - Command passing from kernel module to user-space Controller Thread via shared memory Ring Buffer;
   - Ring Buffer uses lock-free design (lock-free SPSC or MPMC) to avoid kernel-user space lock contention;
   - Ring Buffer capacity: Configurable, default 16384 command slots (128 bytes each, containing NVMe command + metadata);
   - Kernel exposes Ring Buffer to user-space daemon via `mmap`;
   - Uses `eventfd` or semaphore to notify Controller Thread of new command arrival (polling mode also supported).

2. **Command Arbitration Strategy**:
   - Implement NVMe WRR (Weighted Round Robin) arbitration:
     - Urgent queue: 100% preemption rights;
     - High:Medium:Low = 8:4:1 time-slot allocation;
   - Implement simple Round Robin (RR) as an alternative strategy;
   - Support runtime arbitration strategy switching via configuration interface;
   - Admin commands always have priority over I/O commands.

3. **Command Dispatch**:
   - Dispatch to corresponding firmware CPU core thread pools based on command type (Read/Write/Trim/Flush/Admin);
   - Maintain per-command state machine (Pending -> In-Flight -> Completing -> Done);
   - Support command dependency tracking (e.g., Flush commands must wait for all in-flight Write commands to complete);
   - Maximum in-flight commands configurable, default 4096.

4. **Command Timeout Management**:
   - Maintain a timeout timer for each command;
   - Command timeout configurable via Set Features (default 8 seconds);
   - Timed-out commands trigger error handling flow (return error completion with DNR=0, allowing host retry).

### 5.2.3 I/O Scheduler (FR-CTRL-002)

**Requirement Description**: Implement an efficient I/O scheduler that optimizes command processing order to maximize NAND concurrency utilization while meeting QoS requirements.

**Detailed Requirements**:

1. **Scheduling Algorithms**:
   - Default scheduler: Greedy scheduling based on target NAND channel/Die (maximize concurrent commands across different channels);
   - Optional schedulers: FIFO (First-In-First-Out), Deadline (earliest deadline first);
   - Write commands: Merge supported (e.g., merging multiple consecutive 4KB writes into a single 16KB NAND page program operation);
   - Read commands: Read-Ahead prefetch mechanism -- when sequential read pattern is detected, proactively issue prefetch requests to media threads.

2. **Write Buffer Management**:
   - Maintain a global Write Buffer, capacity matching the emulated SSD's DRAM cache size (default 4 GB);
   - Write commands first write to the Write Buffer and return completion (VWC mode);
   - Background thread writes dirty data from Write Buffer to NAND media via Page Program;
   - Flush command triggers forced Write Buffer flush;
   - Write Buffer full triggers backpressure, throttling host write rate.

3. **Read Cache**:
   - Implement LRU (Least Recently Used) read cache for caching hot read data;
   - Cache hits return data directly without issuing media commands;
   - Cache capacity: Configurable, default 256 MB.

4. **Channel Load Balancing**:
   - Real-time statistics of 16 channels' queue depth and busy status;
   - Prioritize dispatching new commands to less-loaded channels to avoid hotspot channels;
   - Support inter-channel command migration (when a channel is persistently congested).

### 5.2.4 Resource Manager (FR-CTRL-003)

**Requirement Description**: Manage the emulated SSD's internal resources (free block pool, DRAM cache, command slots, etc.), providing resource allocation services to all subsystems.

**Detailed Requirements**:

1. **Free Block Management**:
   - Maintain per-Channel/Die/Plane free block linked lists (Free Block List);
   - Allocate free pages for write commands (Page Allocation);
   - Free block watermark monitoring: When free block count falls below High Watermark, notify GC thread to start reclamation;
   - When free blocks fall below Low Watermark, block new write commands until GC frees sufficient space.

2. **Command Slot Management**:
   - Maintain global Command Tracking Table (CTT);
   - Each in-flight command occupies one CTT slot (containing command ID, state, timestamp, related resource references);
   - CTT slot exhaustion triggers backpressure mechanism.

3. **DRAM Cache Resource Management**:
   - Allocate DRAM cache by Namespace quota;
   - Support hot/cold data tiering management for DRAM cache;
   - Monitor DRAM cache pressure, triggering forced write-back.

### 5.2.5 Flow Control (FR-CTRL-004)

**Requirement Description**: Implement multi-level I/O flow control mechanisms to prevent overload, guarantee QoS, and support runtime configuration.

**Detailed Requirements**:

1. **Token Bucket Rate Limiter**:
   - Support per-Namespace/QID IOPS and bandwidth limits;
   - Token bucket parameters: Peak Rate and Sustained Rate;
   - Implement precise microsecond-level token replenishment;
   - Over-limit commands enter a wait queue rather than receiving immediate errors.

2. **Backpressure Mechanism**:
   - When Write Buffer utilization exceeds 90%, stop fetching new Write commands from NVMe SQs;
   - Implement soft throttling by adjusting Doorbell check frequency;
   - Deliver ASYNC EVENT (Namespace Attribute Changed) to host notifying flow control state changes.

3. **QoS Guarantee**:
   - Urgent priority queue commands guaranteed to begin processing within 1 ms;
   - High priority queue P99 latency <= 10 ms;
   - Implement latency histogram statistics, supporting runtime queries.

4. **GC Flow Control**:
   - GC operations use limited NAND bandwidth (default no more than 30% of total NAND bandwidth);
   - Suspend GC when high-priority IO arrives (GC Suspension);
   - GC minimum bandwidth guarantee: Even under sustained IO, GC receives at least 5% bandwidth to prevent garbage accumulation.

---

## 5.3 Media Threads Module

### 5.3.1 Module Overview

The Media Threads module simulates the NAND Flash and NOR Flash media behavior inside the SSD, including accurate timing modeling, data storage management, and media state maintenance. This module is organized according to the real SSD channel architecture, with 16 NAND channels, each channel connecting multiple NAND chips (Chip/CE), each chip containing multiple Dies, and each Die containing multiple Planes. NOR Flash is an independent module for simulating firmware code storage media.

### 5.3.2 NAND Flash Hierarchy (FR-MEDIA-001)

**Requirement Description**: Organize media emulation resources according to the physical hierarchy of real 3D NAND Flash.

**Hierarchy Definition**:

```
NAND Storage Hierarchy (from largest to smallest):
Channel
  +-- Chip/CE (Chip Enable)
        +-- Die
              +-- Plane
                    +-- Block (erase unit)
                          +-- Page (read/write unit)
                                +-- Sector (512 bytes)
```

**Configurable Parameters** (default configuration corresponds to a 1.92 TB enterprise NVMe SSD):

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| NUM_CHANNELS | 16 | 4-32 | NAND channel count |
| CHIPS_PER_CHANNEL | 4 | 1-16 | Chips per channel (CE count) |
| DIES_PER_CHIP | 2 | 1-8 | Dies per chip |
| PLANES_PER_DIE | 2 | 1-4 | Planes per Die |
| BLOCKS_PER_PLANE | 4096 | 512-16384 | Blocks per Plane |
| PAGES_PER_BLOCK | 512 | 128-1024 | Pages per block (3D NAND typical value) |
| PAGE_SIZE | 16384 | 4096-65536 | Page size (bytes), including OOB |
| DATA_SIZE_PER_PAGE | 16000 | 4096-65536 | Page data area size (bytes) |
| OOB_SIZE_PER_PAGE | 384 | 64-1024 | Page OOB area size (bytes), for ECC and metadata |
| CELL_TYPE | TLC | SLC/MLC/TLC/QLC | NAND cell type |
| PE_CYCLE_LIMIT | 3000 | 500-100000 | Maximum P/E cycles (TLC typical: 3000) |

**Total Capacity Calculation (default configuration)**:
16 channels x 4 chips x 2 dies x 2 planes x 4096 blocks x 512 pages x 16 KB/page = **approximately 4 TB raw capacity** (including OP over-provisioning, user-visible approximately 3.2 TB, subtracting 20% OP approximately 2.56 TB, configurable as a 2 TB NVMe namespace).

### 5.3.3 NAND Media Timing Model (FR-MEDIA-002)

**Requirement Description**: Implement an accurate NAND Flash operation timing model that precisely simulates real NAND chip read/write/erase performance characteristics.

**Base Timing Parameters (TLC 3D NAND, ONFI 4.2 interface)**:

| Operation | Latency Parameter | Default | Description |
|-----------|-------------------|---------|-------------|
| Page Read (LSB page) | tR_LSB | 35 us | Fast read, stored in lower layer |
| Page Read (CSB page) | tR_CSB | 70 us | Middle layer read, requires LSB computation |
| Page Read (MSB page) | tR_MSB | 100 us | Slow read, stored in upper layer |
| Data Output (channel transfer) | tDO | page_size/channel_rate | ONFI 4.2: ~1200 MB/s/ch, 16 KB/1200 MB = 13.3 us |
| Page Program (SLC mode) | tPROG_SLC | 100 us | Uses LSB layer only |
| Page Program (TLC mode) | tPROG_TLC | 800 us | Full three-layer write with fine-tuning |
| Data Input (channel transfer) | tDI | page_size/channel_rate | Same as tDO |
| Block Erase | tERS | 3000 us | Full block erase |
| Multi-Plane Read (MPRD) | tR_MP | max(tR_x) | Two Plane concurrent read, latency is maximum |
| Multi-Plane Program (MPPG) | tPROG_MP | 800 us | Two Plane concurrent write |
| Multi-Plane Erase (MPERS) | tERS_MP | 3000 us | Two Plane concurrent erase |
| Cache Read | tRC | Sequential page optimization | Previous page transfer overlaps with next page read |
| Program Suspend | tSUSP | 5 us | Suspend program to respond to read request |
| Program Resume | tRESM | 5 us | Resume suspended program operation |

**Timing Model Implementation**:

Each media thread (corresponding to one Channel or one Die) maintains a precise timeline, recording the Earliest Available Time (EAT) for each resource (Channel Bus, Die, Plane):

```
Command execution time calculation algorithm:
1. Determine command type (Read/Program/Erase) and target address (Channel, Chip, Die, Plane, Block, Page)
2. Retrieve EAT for relevant resources:
   t_channel = channel_eat[channel_id]  // Channel bus occupation time
   t_die     = die_eat[chip_id][die_id]  // Die occupation time
   t_plane   = plane_eat[chip_id][die_id][plane_id]  // Plane occupation time
3. Command start time = max(t_channel, t_die, t_plane, current_time)
4. Look up media latency tMedia based on command type and page type (LSB/CSB/MSB)
5. Channel transfer time = page_size / channel_speed
6. Command completion time:
   - Read command: t_start + tR_x + tDO (media read first, then channel output)
   - Write command: t_start + tDI + tPROG (channel input first, then media write)
   - Erase command: t_start + tERS
7. Update EAT for relevant resources:
   channel_eat = t_start + tDI/tDO  // Channel occupation ends
   die_eat = t_complete            // Die occupied during entire operation
   plane_eat = t_complete          // Plane occupied during write/erase
```

**Multi-Plane Concurrency**:
- Two Planes within the same Die can simultaneously execute same-type operations (read/write/erase);
- Multi-Plane Program requires both Planes to have the same Block offset and Page offset;
- Multi-Plane Read requires both Planes to read the same Page offset;
- Multi-Plane EAT update: Both Planes share the Die's row decoder, EAT = max(plane0_eat, plane1_eat) + tOperation.

**Die Interleaving**:
- Two Dies within the same Chip can operate independently in parallel (each has independent row decoders and sense amplifiers);
- Interleaving scheduling: While Die0 executes tPROG, simultaneously issue new Program commands to Die1, achieving time overlap;
- EAT update: die0_eat and die1_eat are maintained independently.

**Chip Enable Concurrency (Channel level)**:
- Multiple Chips (CE0-CE3) on the same Channel share the Channel bus but can achieve concurrency through CE time-division multiplexing;
- Channel bus is exclusively occupied during data transfer (tDI/tDO);
- Channel bus can be reused by other Chips during media operations (tR/tPROG/tERS).

### 5.3.4 NAND Media Command Execution Engine (FR-MEDIA-003)

**Requirement Description**: Each Channel is configured with an independent command execution engine thread that receives NAND commands from firmware CPU core threads, executes them according to the timing model, and provides completion notifications via callbacks.

**Supported NAND Commands**:

| Command | NAND Op Code | Description |
|---------|-------------|-------------|
| Page Read | 0x00/0x30 | Read single page data |
| Cache Read | 0x31 | Cache read, pipeline optimization |
| Multi-Plane Read | 0x32 | Multi-plane concurrent read |
| Page Program | 0x80/0x10 | Write single page data |
| Cache Program | 0x80/0x15 | Cache write, pipeline optimization |
| Multi-Plane Program | 0x80/0x11 | Multi-plane concurrent write |
| Block Erase | 0x60/0xD0 | Erase single block |
| Multi-Plane Erase | 0x60/0xD1 | Multi-plane concurrent erase |
| Read Status | 0x70 | Read status register |
| Reset | 0xFF | Reset NAND chip |
| Read ID | 0x90 | Read NAND chip ID |
| Program Suspend | 0xB0 | Suspend program operation |
| Program Resume | 0xD0 | Resume program operation |
| Erase Suspend | 0xB0 | Suspend erase operation |

**Command Queue Design**:
- Each Channel maintains an independent command queue (default depth: 128 entries);
- Command queue is logically grouped by Die/Plane, supporting multi-Die parallel dispatch within the same Channel;
- Command scheduler selects the next command from the queue based on timing constraints (EAT) and priority;
- Support for command suspend/resume (Program/Erase Suspend): When a high-priority read command arrives, the currently executing Program or Erase operation can be suspended, the read operation executed, and then resumed.

**Completion Notification Mechanism**:
- Upon command completion, firmware CPU core threads are notified via lock-free completion queue;
- Completion notification includes: Command ID, completion timestamp, operation result (Success/Bit Error/Uncorrectable Error), actual latency value;
- Support for Batch Completion -- notifying multiple command completions at once to reduce context switch overhead.

### 5.3.5 NAND Reliability Modeling (FR-MEDIA-004)

**Requirement Description**: Accurately model NAND Flash reliability characteristics, including P/E cycle degradation, read disturb, and data retention effects.

**P/E Cycle Degradation Model**:
- Each Block maintains an independent P/E cycle count (Erase Count);
- As P/E cycles increase, NAND cell Bit Error Rate (BER) grows logarithmically: `BER = A x log(PE_cycles) + B`;
- BER correctable by ECC: BER < LDPC correction capability (approximately 0.01%);
- Blocks exceeding PE_CYCLE_LIMIT are automatically marked as Bad Blocks;
- Increased error rate affects Read Retry count (high P/E blocks require more Read Retries, increasing read latency).

**Read Disturb Model**:
- Reading page A within a Block applies stress to other unread pages in that Block;
- Read disturb effects increase with cumulative read count per Block (Read Count Per Block);
- When a Block's read count exceeds the threshold (default 100,000 reads/block, configurable), a Refresh operation is triggered (read data and rewrite to a new block);
- Model: Additional bit errors = K x (read_count_in_block - threshold) (linear growth beyond threshold).

**Data Retention Model**:
- Stored data experiences charge leakage over time, causing threshold voltage drift;
- Data retention issues are more severe in high P/E blocks;
- Model: For every simulated time interval T, introduce errors related to P/E cycles and time into stored data;
- Support for accelerated aging: By configuring a time acceleration ratio (default 1000:1), data retention degradation can be simulated in a short time.

**Bad Block Management**:
- Maintain a global Bad Block Table (BBT) recording bad block positions for each Die;
- Factory Bad Blocks: Randomly mark bad blocks at initialization according to configured ratio (default 2%);
- Runtime Bad Blocks: Mark blocks when P/E lifetime is exceeded or write failure count exceeds threshold (default 3 retry failures);
- Bad Block Replacement: Reserve spare blocks per Die (Reserve Pool, default 5% per Die), replacing failed bad blocks;
- BBT Persistence: BBT data is stored in the NOR Flash emulation area, surviving power loss.

### 5.3.6 NAND Data Storage Mechanism (FR-MEDIA-005)

**Requirement Description**: Implement the NAND Flash data storage backend, saving page data in the reserved DRAM region with persistence support to the host file system.

**DRAM Storage Layout**:
- Index reserved DRAM region hierarchically by Channel/Chip/Die/Plane/Block/Page;
- Each Page's address in DRAM:
  ```
  base_addr + (ch x CHIPS_PER_CH x DIES_PER_CHIP x PLANES_PER_DIE x BLOCKS_PER_PLANE x PAGES_PER_BLOCK x PAGE_SIZE)
            + (chip x DIES_PER_CHIP x PLANES_PER_DIE x BLOCKS_PER_PLANE x PAGES_PER_BLOCK x PAGE_SIZE)
            + (die x PLANES_PER_DIE x BLOCKS_PER_PLANE x PAGES_PER_BLOCK x PAGE_SIZE)
            + (plane x BLOCKS_PER_PLANE x PAGES_PER_BLOCK x PAGE_SIZE)
            + (block x PAGES_PER_BLOCK x PAGE_SIZE)
            + (page x PAGE_SIZE)
  ```
- OOB area is appended after each Page's data area, storing ECC check codes, LPN (Logical Page Number), and other metadata;
- Unwritten Pages are filled with 0xFF (simulating the erased initial state of NAND).

**Persistence Strategy**:
- **Incremental Persistence**: After each Block Erase operation completes, persist the still-valid data in that Block to the file system (since data has been rewritten after GC);
- **Periodic Snapshots**: Every 10 minutes (configurable), write all dirty data (unpersisted pages) to the file system;
- **Graceful Shutdown Persistence**: Upon receiving SIGTERM/SIGINT signals, trigger full persistence before exiting;
- **Persistence File Format**: Each Plane's data is stored as a separate binary file, with a file header containing: format version, timestamp, CRC32 checksum, Plane physical address (ch/chip/die/plane);
- **Write-back Thread**: A dedicated background thread handles persistence, using `pwrite` or `io_uring` asynchronous I/O interfaces, with lower priority than the main emulation threads to avoid persistence operations affecting emulation latency.

**Recovery Mechanism**:
- On system restart, read persistence files to restore DRAM storage state;
- Read metadata files (l2p_table.bin, bad_block.bin, erase_count.bin) to restore FTL state;
- When incomplete persistence data is detected (CRC checksum failure), log a warning and initialize the corresponding Plane to all 0xFF (simulating data loss);
- Support `--fresh-start` command-line parameter to skip recovery and start with fresh NAND state.

### 5.3.7 NOR Flash Media Emulation (FR-MEDIA-006)

**Requirement Description**: Emulate the NOR Flash storage media used by SSD firmware, for storing firmware code, configuration data, and critical metadata (such as BBT).

**NOR Flash Specifications (default configuration)**:

| Parameter | Default | Description |
|-----------|---------|-------------|
| Capacity | 256 MB | Divided into multiple sectors |
| Sector Size | 64 KB | Erase granularity |
| Sector Count | 4096 | Total sectors |
| Read Time | 90 ns/byte | Byte read, high speed |
| Page Program Time | 100 us | 256 bytes/page |
| Sector Erase Time | 200 ms | 64 KB sector erase |
| Chip Erase Time | 1.5 s | Full chip erase |
| P/E Lifetime | 100,000 cycles | Per sector |
| Interface | SPI (emulated) | Quad-wire SPI interface emulation |

**Storage Partitions**:

| Partition Name | Start Address | Size | Contents |
|----------------|---------------|------|----------|
| Bootloader | 0x0000_0000 | 4 MB | Boot program (primary + backup) |
| Firmware Slot A | 0x0040_0000 | 64 MB | Firmware image (primary) |
| Firmware Slot B | 0x0440_0000 | 64 MB | Firmware image (backup/upgrade-in-progress) |
| Config Area | 0x0840_0000 | 8 MB | System configuration parameters |
| BBT | 0x08C0_0000 | 8 MB | Bad Block Table |
| Log Area | 0x0940_0000 | 16 MB | Event log |
| System Info | 0x0A40_0000 | 4 MB | System state (P/E counts, etc.) |
| Reserved | 0x0A80_0000 | Remaining | Reserved |

**Operation Commands** (emulating SPI NOR command set, such as W25Q series):
- READ (0x03): Byte read, no cache;
- FAST_READ (0x0B): Fast read with dummy cycles after address;
- PAGE_PROGRAM (0x02): 256-byte page program (write 1 effective, cannot overwrite 0);
- SECTOR_ERASE (0x20): 64 KB sector erase;
- CHIP_ERASE (0xC7): Full chip erase;
- WRITE_ENABLE (0x06) / WRITE_DISABLE (0x04): Write enable/disable;
- READ_STATUS_REG (0x05): Read status register (WIP bit, WEL bit, etc.);
- READ_ID (0x9F): Read manufacturer ID (returns emulated Manufacturer ID + Device ID).

**Data Persistence**: NOR Flash data is persisted as a single binary file (`/var/hfsss/nor/firmware_storage.bin`), with each write operation synchronized to file (`msync` + `fsync`) to ensure data consistency.

---

## 5.4 Firmware CPU Core Thread Module

### 5.4.1 Module Overview

The Firmware CPU Core Thread module is the most innovative core of HFSSS, designed to completely emulate the SSD firmware behavior running on the multi-core ARM processor (e.g., ARM Cortex-R5/R52) embedded in real SSD controllers. This module divides firmware into three layers (Hardware Access Layer, Common Service Layer, Application Layer), each layer corresponding to different thread responsibilities, with each thread bound to a dedicated CPU core, communicating through shared memory and message queues to maximize emulation fidelity.

The Firmware CPU Core Thread group is the core component of the user-space daemon (hfsss-daemon), created via `pthread`, with SCHED_FIFO real-time scheduling policy and CPU affinity set, emulating the multi-core parallel execution behavior of real SSD firmware.

### 5.4.2 Three-Layer Firmware Architecture Overview

```
+==============================================================+
||              Application Layer (Algorithm Task Layer)       ||
||  +------+ +------+ +------+ +------+ +------+ +------+    ||
||  | FTL  | | GC   | | WL   | | BBM  | | ECC  | | QoS  |    ||
||  | Addr | | Garb | | Wear | | Bad  | | Error| | Flow |    ||
||  | Map  | | Coll | | Levl | | Blk  | | Corr | | Ctrl |    ||
||  +------+ +------+ +------+ +------+ +------+ +------+    ||
||  +------+ +------+                                          ||
||  |Redund| |Error |                                          ||
||  |Backup| |Handl |                                          ||
||  +------+ +------+                                          ||
+==============================================================+
||              Common Service Layer (Platform Services)       ||
||  +------+ +------+ +------+ +------+ +------+ +------+    ||
||  | RTOS | |TaskSc| |MemMg | |Bootld| |PwrMg | |OOB   |    ||
||  +------+ +------+ +------+ +------+ +------+ +------+    ||
||  +------+ +------+ +------+ +------+ +------+ +------+    ||
||  | IPC  | |Stabil| |Panic/| |ExcHdl| |Debug | |EvtLog|    ||
||  |      | |Monit | |Assert| |      | |      | |      |    ||
||  +------+ +------+ +------+ +------+ +------+ +------+    ||
+==============================================================+
||              Hardware Access Layer (HAL)                     ||
||  +----------+ +----------+ +----------+ +----------+       ||
||  | NAND Drv | | NOR Drv  | |NVMe/PCIe | |Power Mgmt|       ||
||  |          | |          | |Module Mgr| |Chip Drv  |       ||
||  +----------+ +----------+ +----------+ +----------+       ||
+==============================================================+
```

---

## 5.5 Hardware Access Layer (HAL)

### 5.5.1 NAND Driver Module (FR-HAL-001)

**Requirement Description**: The NAND Driver module in the HAL is the software interface layer between firmware CPU core threads and NAND media threads, abstracting NAND Flash physical operations and providing a unified NAND access API to upper layers (Common Service and Application Layer).

**NAND Driver API Design**:

```c
/* NAND Driver Core API */

/* Initialization */
int nand_init(nand_config_t *config);
void nand_deinit(void);

/* Basic Read/Write/Erase Operations (Asynchronous Interface) */
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

/* Multi-Plane Operations */
int nand_multi_plane_read_async(nand_mp_addr_t *addrs, uint32_t count,
                                uint8_t *data_bufs[], uint8_t *oob_bufs[],
                                nand_cb_t callback, void *ctx);

int nand_multi_plane_write_async(nand_mp_addr_t *addrs, uint32_t count,
                                 const uint8_t *data_bufs[], const uint8_t *oob_bufs[],
                                 nand_cb_t callback, void *ctx);

/* Advanced Operations */
int nand_read_status(uint32_t ch, uint32_t chip, uint8_t *status);
int nand_read_id(uint32_t ch, uint32_t chip, nand_id_t *id);
int nand_reset(uint32_t ch, uint32_t chip);
int nand_set_feature(uint32_t ch, uint32_t chip, uint8_t feature_addr, uint8_t *data);
int nand_get_feature(uint32_t ch, uint32_t chip, uint8_t feature_addr, uint8_t *data);

/* Suspend/Resume */
int nand_program_suspend(uint32_t ch, uint32_t chip);
int nand_program_resume(uint32_t ch, uint32_t chip);
int nand_erase_suspend(uint32_t ch, uint32_t chip);

/* Channel/Die State Query */
nand_state_t nand_get_die_state(uint32_t ch, uint32_t chip, uint32_t die);
uint64_t nand_get_channel_earliest_available_time(uint32_t ch);

/* Parameter Query */
nand_geometry_t *nand_get_geometry(void);
nand_timing_t *nand_get_timing(void);
```

**Driver Internal Implementation**:
- Maintain per-Channel command dispatch queue, scheduling command dispatch according to timing constraints;
- Send commands to the corresponding Channel's media thread via message queue;
- Maintain per-Die state machine (IDLE -> CMD_RECEIVED -> EXECUTING -> COMPLETE);
- Calculate and simulate ONFI command sequences (e.g., Read: CMD(0x00) + ADDR(5 bytes) + CMD(0x30) + wait tR + data output);
- Track Channel Bus Occupation Time.

### 5.5.2 NOR Driver Module (FR-HAL-002)

**Requirement Description**: Provide NOR Flash media access interface, supporting firmware image loading, configuration data read/write, and log writing.

**NOR Driver API Design**:

```c
/* NOR Flash Driver API */
int nor_init(nor_config_t *config);
int nor_read(uint32_t addr, uint8_t *buf, uint32_t len);
int nor_write(uint32_t addr, const uint8_t *buf, uint32_t len);  /* Requires prior erase */
int nor_sector_erase(uint32_t sector_addr);   /* Erase 64 KB sector */
int nor_chip_erase(void);                      /* Full chip erase (use with caution) */
int nor_write_enable(void);
int nor_write_disable(void);
int nor_read_status(uint8_t *status);
int nor_wait_ready(uint32_t timeout_ms);       /* Wait for WIP bit to clear */
int nor_read_id(nor_id_t *id);
/* Partition Operations */
int nor_partition_read(nor_partition_t part, uint32_t offset, uint8_t *buf, uint32_t len);
int nor_partition_write(nor_partition_t part, uint32_t offset, const uint8_t *buf, uint32_t len);
int nor_partition_erase(nor_partition_t part);
```

**Driver Internal Implementation**:
- Emulate NOR Flash as a byte array in process shared memory;
- All read operations: Direct memory access with latency model injection (90 ns x len);
- Write operations: Verify write enable bit, simulate the NAND characteristic of only being able to write 0 not 1, with latency model injection;
- Erase operations: Set sector contents to all 0xFF, inject 200 ms latency (via HFSSS time system simulation, not actual sleep);
- Write protection: Check WP pin state (software emulated), prohibit write/erase operations on write-protected partitions;
- Persistence: Synchronize to persistence file after each write/erase operation, ensuring NOR data survives restarts.

### 5.5.3 NVMe/PCIe Module Management (FR-HAL-003)

**Requirement Description**: The HAL layer NVMe/PCIe module management component converts firmware CPU logical completion signals into actual NVMe command completion actions (write CQE + trigger MSI-X interrupt) and manages the interface with the PCIe/NVMe kernel module.

**Functional Requirements**:

1. **Command Completion Submission**:
   - Receive command completion notifications from the Application Layer (containing CID, QID, status code, result data);
   - Build 64-byte CQE (Completion Queue Entry):
     - DW0 (Command Specific): Read command transfer byte count, Write Zeroes range, etc.;
     - DW1 (Reserved);
     - DW2: SQ Head Pointer + SQ Identifier;
     - DW3 (Status Field): Phase Tag (P bit) + Status Code Type + Status Code + DNR + More bit;
   - Call the kernel module's completion write-back function (write_cqe_and_notify) via shared memory interface;
   - Kernel module writes CQE to physical memory CQ and delivers MSI-X interrupt.

2. **Asynchronous Event Management**:
   - Maintain a pending asynchronous event queue (e.g., temperature threshold exceeded, namespace attribute change, firmware activation, etc.);
   - When the host sends an Async Event Request command, return immediately if there is a pending event; otherwise hold the command until an event occurs;
   - Supported event types: Error Status, Smart/Health Status Change, Notice, NVM Command Set Specific.

3. **PCIe Link State Management**:
   - Monitor PCIe link state (Active/L0/L1/L2);
   - Implement PCIe Active State Power Management (ASPM) software configuration;
   - Respond to PCIe hot reset and function level reset (FLR) events: execute controller reset flow.

4. **Namespace Management Interface**:
   - Maintain namespace list (supporting up to 4096 namespaces);
   - Namespace create/delete: Allocate/release NAND storage space, update mapping tables;
   - Namespace attributes: NSID, NSZE (Namespace Size), NCAP (Namespace Capacity), NUSE (Namespace Utilization), various feature flag bits;
   - Support Namespace sharing (for multi-controller NS Sharing scenarios, currently single-controller emulation).

### 5.5.4 Power Management Chip Driver (FR-HAL-004)

**Requirement Description**: Emulate the SSD controller's Power Management IC (PMIC) driver, simulating power consumption characteristics and state transition behavior under different power states.

**NVMe Power State Emulation**:

| Power State | Name | Max Power | Entry Latency | Exit Latency | Description |
|-------------|------|-----------|---------------|--------------|-------------|
| PS0 | Active | 25 W (configurable) | -- | -- | Full speed operation |
| PS1 | Active (reduced) | 18 W | 0 | 0 | Reduced frequency operation |
| PS2 | Idle | 8 W | 5 ms | 5 ms | Partial circuit shutdown |
| PS3 | Low Power | 3 W | 50 ms | 30 ms | Most circuits shut down |
| PS4 | Deep Sleep | 0.5 W | 500 ms | 100 ms | Nearly all circuits shut down |

**Functional Requirements**:
- Maintain current power state, respond to host Set Feature (Power Management) commands;
- Record cumulative time in each power state (for power analysis);
- Implement automatic power state transitions: Automatically lower power state after idle period;
- When exiting low power state, simulate corresponding latency (do not process IO immediately; wait for exit latency);
- Power consumption statistics interface: Provide current instantaneous power estimate and cumulative energy statistics.

---

## 5.6 Common Service Layer

### 5.6.1 Real-Time Operating System (RTOS) Emulation (FR-CS-001)

**Requirement Description**: Emulate the RTOS running on a real SSD controller, providing software implementations of RTOS primitives including Tasks, Message Queues, Semaphores, Mutexes, Event Groups, and more.

**RTOS Primitive Implementation**:

1. **Tasks**:
   - Emulate RTOS tasks using pthreads, each task corresponding to one firmware CPU core thread;
   - Support task priority (1-32 levels, higher numbers = higher priority);
   - Task states: Ready, Running, Blocked, Suspended;
   - Time slice: Default 1 ms, configurable;
   - Task stack: Each task has an independently allocated stack space (default 16 KB, configurable);
   - Task create/delete/suspend/resume APIs: `task_create`, `task_delete`, `task_suspend`, `task_resume`.

2. **Message Queues**:
   - Used for inter-core communication and inter-module message passing;
   - Support both fixed-size and variable-length message modes;
   - Blocking and non-blocking send/receive modes;
   - Support timeout parameter;
   - Queue depth configurable (default 64 messages);
   - Thread-safe implementation (internal lock-free ring buffer or condition variables).

3. **Semaphores**:
   - Binary semaphore: `sem_binary_create`, `sem_take`, `sem_give`;
   - Counting semaphore: `sem_counting_create(max_count, initial_count)`, `sem_take`, `sem_give`;
   - Support timeout parameter (`sem_take_timeout`);
   - Priority Inheritance mechanism to prevent priority inversion.

4. **Mutexes**:
   - Recursive mutex support (same task can acquire multiple times);
   - Priority Inheritance: Low-priority task holding mutex is temporarily elevated to the highest priority among waiters;
   - Lock timeout detection: Mutex not released within timeout triggers Panic (configurable).

5. **Event Groups**:
   - 32-bit event flags, each bit representing one event;
   - `event_set`, `event_clear`, `event_wait(bits, wait_all, clear_on_exit, timeout)`;
   - Support "wait any bit" and "wait all bits" modes.

6. **Software Timers**:
   - Implemented using high-resolution clock (`clock_gettime(CLOCK_MONOTONIC)`);
   - One-shot and periodic trigger modes;
   - Timer precision: 100 us (emulating typical firmware timer precision);
   - Timer callbacks execute in a dedicated Timer task (avoiding interrupt context).

7. **Memory Pools**:
   - Fixed-size block memory pools to avoid dynamic memory fragmentation;
   - Supported block sizes: 32B, 64B, 128B, 256B, 512B, 1KB, 4KB;
   - Allocation/deallocation APIs: `pool_alloc(pool_id)`, `pool_free(pool_id, ptr)`;
   - Returns NULL when pool is full (non-blocking, caller handles).

### 5.6.2 Task Scheduling (FR-CS-002)

**Requirement Description**: Implement a multi-core-aware firmware task scheduler that assigns different firmware tasks to different emulated CPU cores for maximum firmware execution efficiency.

**Scheduling Strategy**:

1. **Static Task Binding (CPU Pinning)**:
   - Bind critical firmware tasks to dedicated CPU cores for guaranteed real-time performance:
     - FTL address translation tasks -> Core 0-3 (4 cores);
     - GC/WL background tasks -> Core 4-5;
     - HAL NAND driver tasks (one per channel) -> Core 6-21 (16 cores);
     - NVMe interface tasks -> Core 22-23;
     - Common Service tasks (Bootloader/Monitor/Log, etc.) -> Core 24-25;
   - Implement CPU binding via `pthread_setaffinity_np`.

2. **Priority Scheduling**:
   - Priority-based preemptive scheduling (emulating RTOS scheduler behavior);
   - High-priority tasks (e.g., FTL critical path, NVMe command processing) can preempt low-priority tasks (e.g., GC, statistics reporting);
   - Use SCHED_FIFO real-time scheduling policy to ensure high-priority tasks are not preempted by other host processes.

3. **Load Balancing**:
   - FTL tasks can be dynamically rebalanced, assigning tasks to idle FTL cores based on current QD (Queue Depth);
   - Monitor each firmware core's CPU utilization; trigger load balancing when exceeding 85%.

4. **Scheduling Statistics**:
   - Record each task's run time, wait time, and preemption count;
   - Statistics exported via OOB interface or /proc file system interface;
   - Used for analyzing firmware task real-time metrics (e.g., maximum scheduling latency).

### 5.6.3 Memory Management (FR-CS-003)

**Requirement Description**: Implement a firmware-level memory management subsystem, providing safe and efficient memory allocation services to all firmware modules.

**Memory Partition Plan (firmware working memory, total 4 GB)**:

| Memory Region | Size | Description |
|---------------|------|-------------|
| FTL Mapping Table | 2 GB | Full L2P mapping table; 16 GB SSD requires ~1.6 GB (4 KB pages, 4 bytes per entry) |
| Command Buffer | 512 MB | Temporary buffer for NVMe and NAND commands |
| Write Buffer (DRAM Cache Emulation) | 1 GB | Write data buffer |
| System Heap | 256 MB | RTOS dynamic memory allocation |
| Kernel Code/Data | 128 MB | Firmware emulation runtime data |
| Reserved/Protection | 128 MB | Guard pages, stack protection, etc. |

**Memory Management Strategy**:

1. **Static Pre-allocation**:
   - Large data structures (FTL mapping table, bad block table, etc.) are statically allocated at startup in a single allocation, avoiding runtime fragmentation;
   - Use `mmap(MAP_ANONYMOUS | MAP_POPULATE)` to pre-allocate and lock physical memory pages (`mlock`), preventing page faults from affecting emulation latency.

2. **Memory Pools**:
   - Establish dedicated memory pools for frequently allocated/freed small objects (command descriptors, page metadata, etc.);
   - Avoid using `malloc`/`free` on performance-critical paths (preventing heap fragmentation and lock contention).

3. **Memory Protection**:
   - Set access protection for critical data regions (mapping tables, bad block tables) via `mprotect`; unauthorized write access triggers SIGSEGV;
   - Use Guard Pages to detect stack overflow.

4. **Memory Pressure Management**:
   - Monitor system available memory; trigger memory pressure alert when remaining memory falls below threshold;
   - Trigger mapping table cache eviction (CMT eviction) and Write Buffer flush to free memory.

### 5.6.4 Bootloader (FR-CS-004)

**Requirement Description**: Emulate the SSD firmware bootloader, simulating the real SSD power-on initialization sequence.

**Boot Sequence**:

```
Stage 0: Power-On Reset
  | Detect PMIC power state, wait for all power rails to stabilize
Stage 1: Bootloader Execution (loaded from NOR Flash 0x0000_0000)
  | Initialize on-chip SRAM (emulated)
  | Load Bootloader body to internal RAM
  | Integrity check (CRC32 checksum on NOR Bootloader area)
Stage 2: Firmware Image Loading
  | Read firmware version info from NOR
  | Select valid firmware slot (Slot A preferred; if invalid, use Slot B)
  | Load firmware image from NOR to RAM (emulated)
  | Verify firmware integrity (SHA-256)
Stage 3: Hardware Initialization
  | Initialize NAND Flash (send Reset command, read NAND ID)
  | Initialize NOR Flash (read ID, verify partition table)
  | Initialize PCIe/NVMe controller (set controller registers)
  | Initialize DMA engine
Stage 4: FTL Initialization
  | Read Bad Block Table (BBT) from NOR/NAND
  | Load L2P mapping table from persistence file
  | Rebuild P/E count table and wear leveling state
  | Scan NAND media, perform Power-On Scan
  | Recover incomplete write operations from Write Buffer
Stage 5: System Ready
  | Set CSTS.RDY = 1, notify host that controller is ready
  | Start background tasks (GC, WL, monitoring, etc.)
  -> Enter normal operation state
```

**Bootloader Features**:
- Boot time emulation: Entire boot sequence simulates approximately 3-8 seconds of startup latency (configurable);
- Dual image redundancy: Supports Firmware Slot A/B switching; automatically falls back if either image is corrupted;
- Secure boot verification: Firmware integrity verification (SHA-256);
- Boot log: Detailed recording of each boot stage's time and status, written to NOR Log partition.

### 5.6.5 Power-Up/Power-Down Services (FR-CS-005)

**Requirement Description**: Emulate SSD Power-Up and Power-Down processes, ensuring data integrity.

**Power-Up Service**:
- Detect previous power-down type (normal shutdown / abnormal shutdown / power cut);
- Normal shutdown: Directly restore state from persistence files;
- Abnormal shutdown detection: Check Write Buffer log file (commit log), replay or discard uncommitted write operations;
- Execute Power-On Self Test (POST): Basic functionality self-check (NAND connectivity, BBT integrity, etc.);
- After boot completion, record power-on count (Power On Hours / Power Cycle Count) in SMART log.

**Power-Down Service**:
- **Normal Shutdown** (NVMe Shutdown command, SHN=0x01/0x02):
  1. Stop accepting new IO commands;
  2. Wait for all in-flight IO commands to complete;
  3. Flush all dirty data from Write Buffer to NAND;
  4. Update L2P mapping table to persistence file;
  5. Update P/E counts and BBT to NOR Flash;
  6. Write shutdown integrity flag;
  7. Set CSTS.SHST = 0x02 (Shutdown Complete);
  8. Done.

- **Abnormal Power-Down Handling** (SIGKILL / SIGTERM / system crash):
  - Capture SIGTERM via `atexit` and signal handler, perform best-effort rapid persistence;
  - Maintain Write-Ahead Log (WAL): Record WAL before each Write Buffer modification; WAL is written to persistence file;
  - On power-up, scan WAL and replay incomplete operations, ensuring no data loss (at most losing in-flight IOs);
  - Record abnormal shutdown count (Unsafe Shutdown Count) in SMART log.

### 5.6.6 Out-Of-Band Management (FR-CS-006)

**Requirement Description**: Provide an Out-of-Band management interface allowing users to dynamically query and configure simulator parameters during runtime.

**Interface Forms**:
1. **Unix Domain Socket**: The emulation daemon listens on `/var/run/hfsss.sock`, providing a JSON-RPC interface;
2. **/proc File System Interface**: Expose read-only statistics via the `/proc/hfsss/` directory;
3. **REST API** (optional, V2.0): Provide RESTful management API via HTTP/1.1 interface (listening on localhost:8080).

**OOB Management Functions**:

| Function Category | Interface | Description |
|-------------------|-----------|-------------|
| Status Query | `GET /status` | Return simulator overall running state |
| SMART Query | `GET /smart` | Return emulated SSD's SMART/Health information |
| Performance Statistics | `GET /perf` | Return IOPS, bandwidth, latency histograms |
| Parameter Modification | `POST /config` | Dynamically modify configurable parameters (e.g., GC thresholds, rate limits) |
| Fault Injection | `POST /fault` | Inject NAND bad blocks, Read Errors, etc. |
| Force GC | `POST /gc/trigger` | Manually trigger a GC round |
| Snapshot Save | `POST /snapshot` | Immediately trigger full data persistence |
| Log Export | `GET /log` | Export event logs |
| Channel Statistics | `GET /channel/{id}` | Query detailed status of specified Channel |
| Die Status | `GET /die/{ch}/{chip}/{die}` | Query P/E counts, bad block status, etc. of specified Die |

**SMART Information Implementation**:
Emulate NVMe SMART/Health Information Log Page (Log Page ID = 0x02), key fields:

| Byte Offset | Field | Description |
|-------------|-------|-------------|
| 0 | Critical Warning | Temperature exceeded, capacity insufficient, etc. |
| 1-2 | Temperature | Current temperature (emulated temperature model) |
| 3 | Available Spare | Available spare block percentage |
| 4 | Available Spare Threshold | Spare block alarm threshold |
| 5 | Percentage Used | Device lifetime consumption percentage |
| 6 | Endurance Group Critical Warning Summary | |
| 32-47 | Data Units Read | 512-byte units read (128-bit counter) |
| 48-63 | Data Units Written | 512-byte units written |
| 64-79 | Host Read Commands | Host read command count |
| 80-95 | Host Write Commands | Host write command count |
| 96-111 | Controller Busy Time | Controller busy minutes |
| 112-127 | Power Cycles | Power cycle count |
| 128-143 | Power On Hours | Power-on hours |
| 144-159 | Unsafe Shutdowns | Abnormal shutdown count |
| 160-175 | Media and Data Integrity Errors | Media and data integrity error count |
| 176-191 | Number of Error Info Log Entries | Error information log entry count |
| 192-195 | Warning Composite Temperature Time | Minutes above warning temperature threshold |
| 196-199 | Critical Composite Temperature Time | Minutes above critical temperature threshold |

### 5.6.7 Inter-Core Communication (FR-CS-007)

**Requirement Description**: Implement efficient communication mechanisms between emulated firmware CPU core threads, simulating the Inter-Processor Communication (IPC) behavior of real SSD controllers.

**Communication Mechanisms**:

1. **Message Passing**:
   - Maintain a unidirectional message queue (SPSC Ring Buffer) between each pair of cores;
   - Message format: Message type (4 bytes) + Message length (4 bytes) + Message body (max 256 bytes);
   - Send API: `ipc_send(dst_core_id, msg_type, msg_data, msg_len)`;
   - Receive API: `ipc_recv(src_core_id, msg_buf, buf_len, timeout_us)` (blocking or non-blocking);
   - Notification mechanism: Notify receiver of new messages via `eventfd` (receiver polling mode also supported).

2. **Shared Memory**:
   - For large data transfers (e.g., NAND page data 4 KB+), use shared memory with pointer passing;
   - Reference count management: Maintain reference counts for shared memory blocks via atomic operations; last reference holder is responsible for deallocation;
   - Access protection: Protect shared data read/write consistency via read-write locks (RWLock) or version numbers (seqlock).

3. **Inter-Core Signals**:
   - Emulate hardware Software Generated Interrupt (SGI): `ipc_signal(dst_core_id, signal_id)`;
   - Receiver registers signal handler callback: `ipc_register_signal_handler(signal_id, handler)`;
   - Signal processing executes in the receiver's RTOS task context (not interrupt context).

4. **Global Locks (Spinlock/Mutex)**:
   - For global resources shared by multiple cores (e.g., global counters, log buffer), provide inter-core spinlocks;
   - Spinlocks implemented using GCC built-in atomic operations (`__sync_bool_compare_and_swap`);
   - Timeout detection: Spinning beyond threshold (default 100 us) triggers deadlock warning.

### 5.6.8 System Stability Monitoring (FR-CS-008)

**Requirement Description**: Implement system health monitoring mechanisms to continuously monitor the operating state of all simulator subsystems, detecting and handling anomalies.

**Monitoring Content**:

1. **Watchdog**:
   - Each firmware task must periodically (default every 500 ms) feed the Watchdog (`watchdog_feed(task_id)`);
   - Watchdog task periodically checks all registered tasks' feed status;
   - Tasks that fail to feed within timeout are considered hung, triggering system Panic flow;
   - Watchdog timeout is configurable per task type (shorter for critical tasks, longer for background tasks).

2. **System Resource Monitoring**:
   - CPU utilization: Per-second statistics for each firmware core thread's CPU usage;
   - Memory utilization: Monitor usage of each memory partition;
   - NAND channel queue depth: Monitor command queue backlog for each Channel;
   - Free block watermark: Monitor free block count trends for each Die.

3. **Performance Anomaly Detection**:
   - Command latency monitoring: Real-time P50/P90/P99/P99.9 latency computation;
   - When P99.9 latency exceeds configured threshold, log a warning and notify host via ASYNC EVENT;
   - GC efficiency monitoring: Alert when GC reclamation rate < write rate.

4. **Temperature Emulation**:
   - Estimate SSD temperature based on emulated workload (IOPS, bandwidth): `T = T_ambient + IOPS x COEFF_I + BW x COEFF_B`;
   - When temperature exceeds Warning Composite Temperature (default 70 C), set SMART Critical Warning bit;
   - When temperature exceeds Critical Composite Temperature (default 75 C), throttle (limit IOPS ceiling to 50%).

### 5.6.9 Panic/Assert Handling (FR-CS-009)

**Requirement Description**: Implement a comprehensive exception handling framework to ensure the simulator safely saves context information and exits when encountering unrecoverable errors.

**Assert Mechanism**:
- Provide `ASSERT(condition, msg)` macro: Triggers assertion failure when condition is false;
- On assertion failure: Print filename, line number, assertion condition, and error message; log to file; save context; invoke Panic flow.

**Panic Flow**:
1. Set global Panic flag (atomic operation), preventing other cores from generating new operations;
2. Stop accepting new NVMe commands (set internal state to PANIC mode);
3. Save all firmware core running state (register snapshots, stack backtraces);
4. Write critical in-memory state (L2P table summary, error context) to Panic Dump file (`/var/hfsss/panic_dump_<timestamp>.bin`);
5. Deliver Controller Fatal Status to host (set CSTS.CFS = 1);
6. Perform best-effort data persistence (attempt to save DRAM data, do not wait for completion);
7. Record Panic cause to NOR Log partition;
8. Exit daemon process (exit code = PANIC_EXIT_CODE).

**Coredump**:
- Configure `ulimit -c unlimited`; generate standard Linux coredump file on Panic;
- Used in conjunction with HFSSS custom Panic Dump for complete post-mortem analysis data.

### 5.6.10 System Debug Mechanisms (FR-CS-010)

**Requirement Description**: Provide rich debug and observability interfaces supporting firmware R&D engineers in deeply analyzing simulator internal state.

**Debug Features**:

1. **Command Trace**:
   - Enable full NVMe command tracing (recording each command's receive time, processing time, completion time, key internal operation sequence);
   - Trace buffer: Ring buffer, default retaining last 100,000 records;
   - Export format: JSON Lines format, directly usable by visualization tools (e.g., Perfetto, FlameGraph);
   - Enable/disable: Dynamically controlled via OOB interface (`POST /debug/trace/enable`).

2. **NAND Operation Trace**:
   - Record detailed information for each NAND command (Read/Program/Erase): Channel/Chip/Die/Plane/Block/Page, start time, end time, operation result;
   - Filterable by Channel, supporting real-time streaming output to file.

3. **FTL Operation Trace**:
   - Record each L2P query (LPN -> PPN mapping), GC trigger event, and WL migration operation;
   - Output format compatible with FlashSim/MQSim trace formats for comparative analysis.

4. **GDB Support**:
   - Daemon process supports GDB attach (`gdb -p <pid>`);
   - Provide Python GDB script (`hfsss_gdb.py`) for printing firmware internal critical data structures;
   - Support setting breakpoints via GDB to pause emulation at specific commands or internal states.

5. **Performance Counters**:
   - Fine-grained performance counters: IOPS/bandwidth/latency statistics partitioned by command type, LBA range, and Channel;
   - Exported via `/proc/hfsss/perf_counters` or OOB interface;
   - Support counter reset (zeroing).

### 5.6.11 System Event Log Mechanism (FR-CS-011)

**Requirement Description**: Implement a structured system event logging mechanism to record all important events during simulator operation.

**Event Levels**:

| Level | Name | Description | Example |
|-------|------|-------------|---------|
| 0 | FATAL | Unrecoverable error, system about to Panic | All NAND spare blocks exhausted |
| 1 | ERROR | Recoverable error, requires attention | NAND Block uncorrectable ECC error |
| 2 | WARN | Warning, does not affect functionality but requires attention | Free block watermark below high watermark |
| 3 | INFO | Important information | GC completed, firmware boot completed |
| 4 | DEBUG | Debug information (disabled by default) | Each L2P query result |
| 5 | TRACE | Ultra-fine granularity (development debugging only) | Each NAND command status |

**Log Storage**:
- Memory buffer: Ring Buffer (default 64 MB), storing recent log entries;
- NOR persistence: Critical logs (WARN level and above) written to NOR Flash Log partition;
- File output: DEBUG and above logs written to `/var/log/hfsss/hfsss.log`, supporting log rotation (logrotate);
- Syslog integration: WARN and above events synchronized to Linux syslog (journald).

**Log Entry Format**:
```
[timestamp_us][level][module_name][Core ID] message content {structured_fields_JSON}
Example:
[1709123456789012][INFO][FTL][C2] GC completed: freed_blocks=128, valid_pages_moved=45312, elapsed_us=2847613 {"ch":3,"chip":1,"die":0}
```

---

## 5.7 Application Layer (Algorithm Task Layer)

### 5.7.1 Flash Translation Layer -- Address Mapping Management (FR-APP-001)

**Requirement Description**: Implement a complete Flash Translation Layer providing bidirectional mapping between logical addresses (LBA) and physical addresses (PPN), the core function of SSD firmware.

#### 5.7.1.1 Address Mapping Architecture

**Mapping Granularity**: Default page-level mapping (Page-Level Mapping, 4 KB/entry), each LPN corresponding to one PPN.

**Physical Address Encoding (PPN = Physical Page Number)**:
```
PPN (64-bit):
[63:48] Reserved
[47:40] Channel ID (8-bit, supports up to 256 Channels)
[39:36] Chip ID    (4-bit, up to 16 Chips per Channel)
[35:34] Die ID     (2-bit, up to 4 Dies per Chip)
[33:32] Plane ID   (2-bit, up to 4 Planes per Die)
[31:19] Block ID   (13-bit, up to 8192 Blocks per Plane)
[18:10] Page ID    (9-bit, up to 512 Pages per Block)
[9:0]   Offset     (10-bit, intra-page sector offset at 512B granularity)
```

#### 5.7.1.2 Mapping Table Design

**Full L2P Mapping Table**:
- Store all LPN -> PPN mapping relationships in DRAM;
- Mapping table size calculation: For a 2 TB SSD (2 TB / 4 KB = 512M pages), 4 bytes per mapping entry (PPN + valid bit), total size = 512M x 4B = 2 GB;
- Mapping table allocated using `mmap(MAP_ANONYMOUS | MAP_HUGETLB)` with huge pages (2 MB), reducing TLB pressure;
- Unwritten LPN mapping value: 0xFFFFFFFF (INVALID_PPN).

**Reverse Mapping Table (P2L Table, Physical-to-Logical)**:
- Used during GC to find the logical address corresponding to a physical page;
- P2L information stored in each Page's OOB area (storing LPN and write timestamp);
- Sparse P2L cache maintained in memory (caching only P2L information for Blocks in GC candidate state).

**Mapping Table Persistence**:
- L2P mapping table persisted via checkpoint: Full mapping table written to file every 1 GB of write volume;
- Incremental persistence: After each GC completion, write the LPN range modified by GC to an incremental log (WAL);
- On power-up: Load most recent checkpoint, replay WAL log, recover complete mapping table.

#### 5.7.1.3 Over-Provisioning (OP)

- Reserve 20% of NAND space as OP area (not mapped to any LBA, dedicated to GC use);
- OP area calculation: `OP_Blocks = Total_Blocks x 0.20`;
- Users can adjust OP ratio via Format NVM command (range 7%-50%);
- OP space affects GC efficiency: Larger OP means GC-selected victim blocks have fewer valid pages, higher reclamation efficiency, and lower Write Amplification Factor (WAF).

#### 5.7.1.4 Write Operation Flow

```
Host Write Command (LBA, NLB, Data) Processing Flow:
1. DMA Data from host memory to Write Buffer
2. For each 4 KB logical page:
   a. Look up current LPN's old PPN (old_ppn = L2P[LPN])
   b. If old_ppn is valid, mark old_ppn as Invalid (increment that Block's invalid page count)
   c. Allocate a new free physical page (new_ppn) from Current Write Block
   d. Update L2P[LPN] = new_ppn
   e. Write Data to corresponding NAND media (asynchronous write via HAL NAND driver)
   f. Write LPN in OOB area (for P2L queries)
3. When Current Write Block is full:
   a. Close current Block (add to Full Block list)
   b. Open new Write Block (allocate from free block pool)
4. Return NVMe command completion
```

#### 5.7.1.5 Read Operation Flow

```
Host Read Command (LBA, NLB) Processing Flow:
1. For each 4 KB logical page:
   a. Query L2P table: ppn = L2P[LPN]
   b. If ppn == INVALID_PPN: Return all-zero data (or return Error)
   c. Parse ppn into physical address (ch, chip, die, plane, block, page)
   d. Asynchronously read the physical page data via HAL NAND driver
   e. On read completion: Perform ECC check (correct if correctable, report Error if uncorrectable)
2. DMA read Data to host memory
3. Return NVMe command completion
```

#### 5.7.1.6 Striping Strategy

To fully utilize 16 Channel concurrency, write data is striped across Channels:
- Consecutive logical pages are distributed to different Channels' Write Blocks in Round-Robin fashion;
- Stripe Unit: Default is 1 NAND page (16 KB), meaning every 16 KB of data switches to the next Channel;
- Alignment optimization: When host sends large contiguous writes (e.g., 1 MB), each 16 KB goes to one Channel, and 16 Channels align perfectly;
- Multi-plane striping: Within the same Channel, further stripe across Planes, enabling Multi-Plane Program.

### 5.7.2 NAND Block Address Organization Management (FR-APP-002)

**Requirement Description**: Implement NAND Flash physical block address organization and allocation management, maintaining Block-level state information, supporting write allocation, GC reclamation, and wear leveling.

**Block State Machine**:

```
    +------------------------------------------+
    v                                          |
  FREE --[Allocated as Write Block]--> OPEN --[Full]--> FULL
    ^                                          |
    |                                          |
    +----[GC reclaim and erase]---- ERASING <-- VICTIM
                                               ^
                                               |
                                    FULL ------+ (Selected by GC)
```

**Block Metadata**: Each Block maintains the following information (stored in DRAM Block metadata table):
```c
typedef struct block_info {
    uint32_t    erase_count;        // P/E cycle count
    uint32_t    valid_page_count;   // Valid page count (max: pages_per_block)
    uint32_t    invalid_page_count; // Invalid page count
    uint32_t    free_page_count;    // Free page count
    uint64_t    last_erase_time;    // Most recent erase time (us timestamp)
    uint64_t    first_write_time;   // First write time for this Block (for data retention detection)
    uint32_t    read_count;         // Cumulative read count for pages in this Block (Read Disturb detection)
    block_state_t state;            // Block current state (FREE/OPEN/FULL/VICTIM/ERASING/BAD)
    uint8_t     reserved[3];
} block_info_t;
```

**Current Write Block (CWB) Management**:
- Maintain one Current Write Block (CWB) per Die/Plane; new write operations are written sequentially into the CWB;
- When CWB is full (all pages written), close CWB and open new CWB (allocated from free block pool);
- Multi-Plane mode: Simultaneously maintain CWBs for both Planes of the same Die (same Block offset), supporting Multi-Plane Program;
- Plane-level CWB ensures Block/Page alignment constraints for each multi-plane write.

**Free Block Pool Management**:
- Global free block pool: Free block linked lists organized hierarchically by Channel/Die/Plane;
- Allocation strategy: Prioritize allocation from blocks with lowest wear (smallest erase_count);
- Block shortage protection: When a Die's free block count < LOW_WATERMARK (default 5 blocks), trigger emergency GC and pause new write allocation.

### 5.7.3 Garbage Collection (FR-APP-003)

**Requirement Description**: Implement efficient garbage collection algorithms to automatically reclaim NAND physical space occupied by invalid (stale) data, providing free blocks for new writes.

#### 5.7.3.1 GC Trigger Strategy

**Watermark Control**:
- **High Watermark**: Global free block ratio < 20%, start background GC (low priority, does not affect foreground IO);
- **Low Watermark**: Global free block ratio < 5%, start urgent GC (high priority, throttle foreground writes);
- **Critical Watermark**: Global free block ratio < 2%, stop all write commands, focus exclusively on GC reclamation.

**Per-Die GC**: Each Die independently maintains watermarks, allowing localized GC to run on specific Dies without affecting others.

#### 5.7.3.2 Victim Block Selection Algorithms

The following GC Victim selection strategies are supported (switchable via configuration):

1. **Greedy Algorithm (default)**:
   - Select the Block with the fewest valid pages (most invalid pages) as Victim;
   - Maximize free pages reclaimed per GC round, minimize valid page migration;
   - Implementation: Maintain a sorted set ordered by valid_page_count for each Die (using min-heap);
   - Time complexity: O(log n), where n is the number of Full Blocks.

2. **Cost-Benefit Algorithm**:
   - Comprehensively consider block utilization and age (time since last erase);
   - Cost-Benefit = (1 - utilization) / (2 x utilization) x age;
   - Prioritize Blocks with high Cost-Benefit values (high invalidity rate + old data);
   - Helps reduce cold data interference with hot data areas.

3. **FIFO Algorithm**:
   - Select the oldest Full Block (oldest closed time) as Victim;
   - Simple implementation; beneficial for data retention (older data prioritized for erase and rewrite).

4. **Hot-Cold Separation Algorithm**:
   - Classify write data by frequency into Hot (frequently updated) and Cold (infrequently updated);
   - Hot data writes to dedicated Hot Block area; Cold data writes to Cold Block area;
   - GC prioritizes reclaiming Hot area Blocks (low validity rate); Cold area GC triggers less frequently, reducing cold data migration.

#### 5.7.3.3 GC Execution Flow

```
GC Execution Flow (for a selected Victim Block):
1. Select Victim Block (e.g., ch=3, chip=1, die=0, plane=0, block=2048)
2. Scan all pages in Victim Block:
   For each page in victim_block:
     a. Read LPN stored in OOB (page_lpn)
     b. Check if L2P[page_lpn] equals current PPN (verify whether page is still valid)
     c. If valid (L2P[page_lpn] == current_ppn):
        - Read page data (NAND Read Page)
        - Allocate new free page (new_ppn)
        - Write data to new page (NAND Program Page)
        - Update L2P[page_lpn] = new_ppn
        - Increment old_ppn's Block invalid_page_count
     d. If invalid (L2P[page_lpn] != current_ppn): Skip
3. After all valid pages are migrated, erase Victim Block (NAND Block Erase)
4. Update Block metadata: erase_count++, state = FREE
5. Add erased Block to free block pool
```

**GC Concurrency Optimization**:
- Multi-Die parallel GC: GC on multiple Dies can proceed simultaneously without interference;
- Pipeline GC: Reading valid pages from Victim Block (Read Phase) and writing to new Block (Write Phase) can be pipelined across different Dies;
- GC Suspend and Resume: When high-priority IO commands arrive, suspend currently executing NAND Program (Program Suspend), prioritize IO command handling, then resume GC (Program Resume).

#### 5.7.3.4 Write Amplification Analysis

Write Amplification Factor (WAF):
- `WAF = NAND_Writes / Host_Writes`
- Ideal case (GC overhead only): WAF approximately 1 / (1 - utilization_of_data)
- Target: Under steady-state conditions at 90% disk utilization, WAF <= 3 (TLC NAND, Greedy GC, 20% OP);
- Monitoring: Real-time WAF statistics, exported via OOB interface;
- Optimization: Reduce WAF through hot-cold separation, larger OP ratio, and optimized GC strategies.

### 5.7.4 Wear Leveling (FR-APP-004)

**Requirement Description**: Implement wear leveling mechanisms to ensure P/E cycle counts across all NAND Flash Blocks are as uniform as possible, extending overall SSD lifetime.

#### 5.7.4.1 Dynamic Wear Leveling

- During GC, prioritize writing data to free Blocks with lower P/E cycle counts (consider erase_count during free block allocation);
- Implementation: Maintain a min-heap of free blocks sorted by erase_count; always allocate the block with lowest erase_count;
- Effect: Hot data areas' Blocks that are frequently GC-reclaimed gradually get replaced with younger blocks.

#### 5.7.4.2 Static Wear Leveling

- Periodically check global Block erase_count distribution (using erase_count histogram);
- When certain Blocks' erase_count is significantly below average (default threshold: below 50% of average), trigger static WL;
- Static WL flow:
  1. Select cold data Block with lowest erase_count (victim_cold_block);
  2. Select hot area free Block with highest erase_count (donor_hot_block, from frequently erased GC-reclaimed blocks);
  3. Migrate cold data from victim_cold_block to donor_hot_block;
  4. Erase victim_cold_block and allocate as new free block for hot data area use;
  5. Through this swap, low P/E blocks rotate into hot data area, high P/E blocks are used for cold data.
- Trigger frequency: Default every 100 dynamic GC cycles triggers one static WL check; configurable.

#### 5.7.4.3 Wear Monitoring and Alerts

- Real-time statistics of SSD-wide average P/E cycle count and maximum P/E cycle count;
- When maximum P/E cycle count approaches NAND lifetime limit (> 80% of PE_CYCLE_LIMIT): Set SMART Available Spare low alert;
- When Available Spare < Available Spare Threshold: Set SMART Critical Warning (bit 0);
- Dynamically adjust OP ratio recommendation based on wear state (more wear suggests increasing OP).

### 5.7.5 Read/Write/Erase Command Management (FR-APP-005)

**Requirement Description**: Implement firmware-layer read/write/erase command management framework, building on FTL and GC to manage command lifecycle, error retry, and completion processing.

**Command State Machine**:
```
NVMe Command arrives
      |
[RECEIVED] -> [PARSING] -> [L2P_LOOKUP] -> [NAND_QUEUED] -> [NAND_EXECUTING]
                                                                  |
                                                          [ECC_CHECK]
                                                                  |
                                                      +--[ERROR?]--+
                                                      No           Yes
                                                      |            |
                                                 [COMPLETE]   [RETRY/FAIL]
```

**Read Retry Mechanism**:
- When NAND Read produces a correctable ECC error, first attempt Soft-Decision LDPC decoding;
- If soft decode fails, trigger Read Retry: Adjust NAND read voltage (Vread) offset and re-read;
- Read Retry up to N attempts (default 15, matching NAND die specifications), each with a different Voltage Offset;
- If still failing after 15 retries: Mark as Uncorrectable Error (UCE), return NVMe error completion (Media and Data Integrity Error);
- UCE recording: Write to NVMe Error Log Page, increment SMART Media and Data Integrity Errors counter.

**Write Retry Mechanism**:
- On Program failure (status register WR_FAIL bit set), attempt writing data to spare area of same Block or to a new Block;
- If failure count exceeds threshold (default 3 times), mark that Block as bad, remove from FTL, replace with new block from free block pool.

**Write Verify**:
- Optionally read back data after programming for verification (Write-and-Verify mode, impacts write performance);
- Disabled by default, can be enabled via configuration (for high-reliability scenarios).

### 5.7.6 IO Flow Control (FR-APP-006)

**Requirement Description**: Implement fine-grained IO flow control at the Application Layer, ensuring stable system operation under all IO patterns, preventing NAND overload and GC starvation.

**Multi-level Flow Control**:

1. **Host-Side Rate Limiting**:
   - Implement Token Bucket rate limiting at the NVMe command reception point;
   - Support per-Namespace and global two-level rate limiting;
   - IOPS limit: 1 KIOPS - 2000 KIOPS, granularity 100 IOPS;
   - Bandwidth limit: 50 MB/s - 14 GB/s, granularity 10 MB/s;
   - Over-limit handling: Over-limit commands are not dropped but enter a wait queue (max wait queue depth = SQ depth).

2. **GC/WL Bandwidth Quota**:
   - GC operations consume a quota-controlled share of NAND bandwidth (default max 30%, min 5%);
   - When host IO load is low, GC can increase to higher bandwidth quota (max 80%);
   - Adaptive adjustment: Sample host IO bandwidth and GC bandwidth every second, dynamically adjust GC quota based on current free block watermark.

3. **NAND Channel-level Flow Control**:
   - Per-Channel command queue depth limit (default 128 entries);
   - When queue is full, upper-level command dispatcher waits (back-pressure propagation);
   - Channel-level starvation detection: Check for forgotten waiting commands when a channel has no commands for an extended period.

4. **Write Buffer Flow Control**:
   - Write Buffer occupancy ratio triggers three-level flow control:
     - < 60%: Normal writes, no throttling;
     - 60%-80%: Light throttling (host write rate reduced 20%);
     - 80%-90%: Heavy throttling (host write rate reduced 50%);
     - > 90%: Pause accepting new write commands (complete when buffer drains below 70%).

### 5.7.7 Data Redundancy (FR-APP-007)

**Requirement Description**: Implement data redundancy mechanisms to protect critical user data from loss due to NAND media errors.

**RAID-Like Data Protection**:

1. **LDPC ECC** (first line of defense):
   - Each NAND page has an accompanying ECC checksum (stored in OOB area);
   - LDPC encoding: BCH as fallback, LDPC as primary scheme (correction capability: corrects 16-24 bits/KB);
   - Soft decoding: Retains probability information from multiple reads for Soft-Decision, improving correction capability;
   - ECC computation executed by dedicated firmware thread (emulating hardware ECC engine behavior).

2. **Cross-Die Parity (Die-Level Parity)** (optional, V2.0):
   - Calculate XOR parity data for writes spanning multiple Dies;
   - Write parity pages to a dedicated Parity Die;
   - When any Die experiences a full-block uncorrectable error, data can be recovered via parity;
   - Implement simple RAID-5 logic (applicable to multi-Die scenarios).

3. **Critical Metadata Redundancy**:
   - L2P mapping table: Dual storage (DRAM + persistence file);
   - Bad Block Table (BBT): Primary and backup copies stored in NOR Flash;
   - P/E count table: Similarly dual-protected;
   - RAID (inactive metadata region defense): Periodically compare two metadata copies for consistency; alert on inconsistency.

4. **Write Buffer Power-Fail Protection**:
   - Emulate SSD controller Supercapacitor power-fail protection mechanism;
   - Implemented via Write-Ahead Log (WAL): Each Write Buffer write operation writes to WAL file first (synchronous), then updates DRAM data;
   - Post power-loss recovery: Scan WAL and replay, ensuring data consistency;
   - WAL file stored on host's high-speed NVMe (e.g., another real NVMe SSD) to ensure WAL write speed is not a bottleneck.

### 5.7.8 Command Error Handling (FR-APP-008)

**Requirement Description**: Implement a comprehensive command error handling framework, ensuring correct NVMe error status is returned to the host in all error scenarios while maintaining stable system operation.

**NVMe Error Status Codes**:

| Status Code Type | Status Code | Name | Trigger Scenario |
|-----------------|-------------|------|------------------|
| 0x0 (Generic) | 0x00 | Success | Command completed successfully |
| 0x0 | 0x01 | Invalid Command Opcode | Unsupported command opcode |
| 0x0 | 0x02 | Invalid Field in Command | Invalid command field |
| 0x0 | 0x06 | Invalid Namespace or Format | Invalid namespace |
| 0x0 | 0x0A | Invalid I/O Queue Identifier | Invalid queue ID |
| 0x0 | 0x0B | Maximum Queue Size Exceeded | Exceeded max queue depth |
| 0x0 | 0x80 | LBA Out of Range | LBA exceeds Namespace range |
| 0x0 | 0x81 | Capacity Exceeded | Write exceeds Namespace capacity |
| 0x0 | 0x82 | Namespace Not Ready | Namespace being formatted |
| 0x1 (Cmd Specific) | 0x00 | Completion Queue Invalid | Invalid CQ ID |
| 0x1 | 0x01 | Invalid Queue Identifier | Invalid SQ ID |
| 0x2 (Media & Data Integrity) | 0x80 | Write Fault | Write failure (NAND Program Error) |
| 0x2 | 0x81 | Unrecovered Read Error | Uncorrectable read error |
| 0x2 | 0x82 | End-to-End Guard Check Error | E2E checksum error |
| 0x2 | 0x85 | Compare Failure | Compare command failure |

**Error Handling Flow**:

1. **Recoverable Errors**:
   - Retry (internal retry, transparent to host);
   - Degrade to unrecoverable error after exceeding retry count;
   - Record to Error Log Page (internal only).

2. **Unrecoverable Data Errors**:
   - Return corresponding NVMe error status code to host (DNR=0, allowing host retry);
   - Mark the LBA range as "error reported";
   - Write to NVMe Error Log Page: Sequence Number, Error Count, CID, SQ ID, Error Location, LBA, NS, Type;
   - SMART statistics: Increment Media and Data Integrity Errors counter.

3. **NAND Device Errors**:
   - NAND Block bad block handling: Mark bad block, replace with spare block, migrate valid data;
   - Full NAND Chip failure (extreme scenario): If cross-Die parity exists, attempt recovery; otherwise report read errors for corresponding LBA range to host;
   - Trigger Async Event Request to notify host: Critical Warning (bit 2: Media Related).

4. **Command Timeout Handling**:
   - Return Abort command completion to host (when timed-out command is internally aborted);
   - If command timeout cannot be recovered, trigger Controller Reset.

5. **Firmware Internal Errors**:
   - ASSERT failure, NULL pointer access, etc.: Trigger Panic flow;
   - Non-critical errors (e.g., memory pool exhaustion): Record Error Log, continue degraded operation (DEGRADED mode).

---

# Chapter 6: Performance Requirements

## 6.1 IOPS Performance

### 6.1.1 Random Read IOPS (4 KB, QD=32)

| Configuration | Target Value | Description |
|---------------|-------------|-------------|
| Single namespace, no ECC | 1,000,000 IOPS | Theoretical peak without LDPC computation |
| Single namespace, with ECC | 600,000 IOPS | Target with LDPC enabled |
| Multi-namespace (4), total | 800,000 IOPS | Multi-NS scenario |
| Steady state (90% disk utilization) | 400,000 IOPS | Steady-state target including GC overhead |

### 6.1.2 Random Write IOPS (4 KB, QD=32)

| Configuration | Target Value | Description |
|---------------|-------------|-------------|
| Initial state (Fresh Out Of Box) | 300,000 IOPS | No GC, empty disk fill |
| Steady state (90% utilization, WAF~2) | 150,000 IOPS | Including GC write amplification |
| Steady state (50% utilization, WAF~1.5) | 200,000 IOPS | Lower disk utilization |

### 6.1.3 Mixed Read/Write IOPS (4 KB, 70R/30W, QD=32)

| Configuration | Target Value |
|---------------|-------------|
| Steady state | 250,000 IOPS |

## 6.2 Bandwidth Performance

| Operation Type | Block Size | Target Bandwidth |
|----------------|-----------|-----------------|
| Sequential Read | 128 KB | 6.5 GB/s |
| Sequential Write | 128 KB | 3.5 GB/s |
| Sequential Read | 1 MB | 7.0 GB/s |
| Sequential Write | 1 MB | 4.0 GB/s |
| Random Read | 4 KB | 4.0 GB/s (QD=128) |

(Note: Emulation bandwidth ceiling is limited by DRAM bus bandwidth and PCIe emulation layer software efficiency; the above values are targets, actual values depend on server configuration)

## 6.3 Latency Performance

| Operation | P50 | P99 | P99.9 | Description |
|-----------|-----|-----|-------|-------------|
| Random Read (QD=1) | 80 us | 150 us | 300 us | Includes NAND read latency model (TLC LSB: 35 us + transfer) |
| Random Write (QD=1) | 20 us | 50 us | 100 us | Write Buffer hit, excludes NAND program latency |
| Forced Read (FUA write + read) | 1 ms | 3 ms | 8 ms | Includes NAND Program latency (TLC: 800 us) |
| GC-Suspended Read | 200 us | 500 us | 2 ms | Read command latency during GC execution |
| Flush Command | 500 ms | 2 s | 5 s | Depends on Write Buffer backlog |

## 6.4 Emulation Accuracy

| Metric | Accuracy Target | Verification Method |
|--------|----------------|---------------------|
| NAND Page Read latency error | < 5% | Compare with NAND datasheet nominal values |
| NAND Page Program latency error | < 5% | Same as above |
| NAND Block Erase latency error | < 5% | Same as above |
| Overall IOPS emulation accuracy | < 10% | Compare with MQSim, FEMU same-configuration results |
| WAF emulation accuracy | < 15% | Compare with theoretical WAF formula results |
| GC trigger timing accuracy | < 1% watermark deviation | Monitor free block watermark changes |

## 6.5 Scalability

| Metric | Requirement |
|--------|------------|
| Maximum supported Channels | 32 |
| Maximum supported Namespaces | 4096 |
| Maximum NAND capacity | 256 TB (emulated, limited by DRAM size) |
| Maximum concurrent NVMe queues | 4096 SQ/CQ pairs |
| Maximum concurrent commands | 65535 (NVMe specification limit) |
| CPU core scalability | Linear scaling from 64 to 256 cores, IOPS increases with core count |

## 6.6 Resource Utilization Targets

| Resource | Target Utilization | Description |
|----------|-------------------|-------------|
| NAND media thread CPU | 70-90% (full load) | Total utilization of 16 Channel x 2-3 threads |
| FTL thread CPU | 60-80% (full load) | 4-8 FTL cores |
| PCIe/NVMe module CPU | 40-60% (full load) | I/O Dispatcher + Workers |
| DRAM utilization | > 85% | Emulation storage + mapping tables + cache |
| Host OS CPU usage | < 10% | Should not impact other host services |

---

# Chapter 7: Product Interface Definition

## 7.1 Host Interface

### 7.1.1 Block Device Interface

Interface presented by the simulator to the host Linux operating system:
- **Device Node**: `/dev/nvme<n>n<m>` (e.g., `/dev/nvme0n1`)
- **Device Type**: NVMe Namespace (Block Device)
- **Block Size**: 4096 bytes (Logical Block Size = 4 KB)
- **Namespace Capacity**: Configurable, default 2 TB (`NSZE x LBADS`)
- **Interface Protocol**: NVMe 2.0
- **Transport Protocol**: PCIe (virtual)

### 7.1.2 nvme-cli Compatibility

The following `nvme-cli` commands must work correctly:

```bash
nvme list                           # List emulated NVMe devices
nvme id-ctrl /dev/nvme0            # Query controller identification
nvme id-ns /dev/nvme0 --namespace-id=1  # Query namespace properties
nvme smart-log /dev/nvme0          # Read SMART information
nvme error-log /dev/nvme0          # Read error log
nvme get-feature /dev/nvme0 --feature-id=4  # Query features
nvme set-feature /dev/nvme0 --feature-id=4 --value=<n>  # Set features
nvme format /dev/nvme0n1           # Format
nvme fw-download /dev/nvme0 --fw=firmware.bin   # Firmware download
nvme fw-commit /dev/nvme0 --slot=1 --action=1  # Firmware commit
nvme create-ns /dev/nvme0 --nsze=<n> --ncap=<n>  # Create namespace
nvme delete-ns /dev/nvme0 --namespace-id=2  # Delete namespace
nvme attach-ns /dev/nvme0 --namespace-id=2 --controllers=0  # Attach namespace
```

### 7.1.3 fio Test Tool Compatibility

The following fio configurations must run correctly:
```ini
[global]
filename=/dev/nvme0n1
ioengine=io_uring        # Must support io_uring
direct=1                 # Must support O_DIRECT
numjobs=32               # Multi-threaded
iodepth=128              # Deep queue
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

## 7.2 Management Interface

### 7.2.1 OOB Socket Interface

Unix Domain Socket path: `/var/run/hfsss/hfsss.sock`
Protocol: JSON-RPC 2.0

**General Request Format**:
```json
{
  "jsonrpc": "2.0",
  "method": "method_name",
  "params": { ... },
  "id": 1
}
```

**Key Interface Definitions**:

```
GET /status -> Return simulator overall status
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

GET /nand/channel/{ch_id} -> Query Channel status
  Response:
  {
    "channel_id": 3,
    "chips": 4,
    "channel_utilization_pct": 72.3,
    "cmd_queue_depth": 45,
    "current_operation": "program",
    "eat_us": 1709127234567
  }

POST /fault/inject -> Fault injection
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

POST /config/gc -> Modify GC configuration
  Request:
  {
    "high_watermark_pct": 25,
    "low_watermark_pct": 8,
    "max_gc_bw_pct": 35,
    "gc_algorithm": "cost_benefit"
  }
```

### 7.2.2 /proc File System Interface

```
/proc/hfsss/
+-- status          # Simulator status (read-only)
+-- config          # Current configuration (read-only)
+-- perf_counters   # Performance counters (read-only)
+-- channel_stats   # Per-Channel statistics (read-only)
+-- ftl_stats       # FTL statistics (mapping table hit rate, GC count, etc.)
+-- latency_hist    # Latency histogram (read-only)
+-- version         # Simulator version information
```

### 7.2.3 Command Line Interface (CLI)

Provided `hfsss-ctrl` CLI tool:

```bash
hfsss-ctrl status               # View status
hfsss-ctrl perf                 # View performance statistics (live refresh)
hfsss-ctrl channel 3            # View Channel 3 details
hfsss-ctrl fault inject --type=bad_block --ch=3 --chip=1 --die=0 --block=2048
hfsss-ctrl config set gc.algorithm=greedy
hfsss-ctrl gc trigger           # Manually trigger GC
hfsss-ctrl snapshot save        # Force persistence
hfsss-ctrl log dump --level=warn --last=1000  # Export last 1000 WARN+ log entries
hfsss-ctrl trace start          # Start command trace
hfsss-ctrl trace stop           # Stop command trace
hfsss-ctrl trace dump --output=/tmp/trace.json  # Export trace data
```

## 7.3 Configuration File Interface

The simulator is initialized via a YAML configuration file (`/etc/hfsss/hfsss.yaml`):

```yaml
# HFSSS Configuration File
version: "2.0"

system:
  log_level: info
  daemon_pid_file: /var/run/hfsss/hfsss.pid
  oob_socket: /var/run/hfsss/hfsss.sock

memory:
  nand_base_addr: "0x1000000000"  # 16 GB offset
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
    static_wl_trigger_interval: 100  # Check static WL every 100 GC cycles
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
  rest_api_port: 8080   # 0 to disable REST API

debug:
  enable_command_trace: false
  enable_nand_trace: false
  trace_buffer_size: 100000
  perf_counter_interval_ms: 100
```

## 7.4 Persistence Data Format Interface

### 7.4.1 NAND Data File Format

Each Plane's data file (`/var/hfsss/nand/ch{CH}/chip{CHIP}_die{DIE}_plane{PLANE}.bin`) format:

```
File Header (1024 bytes):
  [0:3]   Magic Number: 0x48465353 ("HFSS")
  [4:7]   Format Version: 0x00020000
  [8:15]  File Creation Time (Unix timestamp us)
  [16:23] Last Modified Time
  [24:27] Channel ID
  [28:31] Chip ID
  [32:35] Die ID
  [36:39] Plane ID
  [40:43] Blocks Per Plane
  [44:47] Pages Per Block
  [48:51] Page Size (including OOB)
  [52:55] Total File Size (excluding Header)
  [56:59] CRC32 (CRC32 of Header[0:55], excluding this field)
  [60:1023] Reserved

Data Section (linear layout by Block x Page):
  Block 0:
    Page 0: [DATA_SIZE bytes] + [OOB_SIZE bytes]
    Page 1: [DATA_SIZE bytes] + [OOB_SIZE bytes]
    ...
    Page N-1: [DATA_SIZE bytes] + [OOB_SIZE bytes]
  Block 1: ...
  ...
  Block M-1: ...
```

### 7.4.2 L2P Mapping Table File Format

File: `/var/hfsss/metadata/l2p_table.bin`

```
Header (256 bytes):
  Magic: 0x4C325054 ("L2PT")
  Version: 4 bytes
  Total LPN Count: 8 bytes
  Checkpoint Sequence Number: 8 bytes
  Timestamp: 8 bytes
  CRC64 (entire mapping table data area): 8 bytes

Data Section:
  Contiguous uint64_t array, index = LPN, value = PPN (0xFFFFFFFFFFFFFFFF = invalid)
  Size = Total_LPN_Count x 8 bytes
```

### 7.4.3 WAL (Write-Ahead Log) Format

File: `/var/hfsss/metadata/wal.bin`

```
Each WAL Record (64 bytes):
  [0:7]   Sequence Number (incrementing)
  [8:11]  Record Type (0x01=L2P Update, 0x02=Block State Change, 0x03=Erase Count Update)
  [12:15] Payload Length
  [16:55] Payload (varies by Record Type)
  [56:59] CRC32 (this record)
  [60:63] Reserved

L2P Update Payload (40 bytes):
  [0:7]  LPN
  [8:15] Old PPN
  [16:23] New PPN
  [24:31] Timestamp
  [32:39] Reserved

WAL file cycles in 512 MB units (Ring Log File), with Header marking valid record range.
```

---

# Chapter 8: Fault Injection Framework

## 8.1 Fault Injection Capabilities

HFSSS provides a complete fault injection framework supporting the following types of fault simulation:

### 8.1.1 NAND Media Faults

| Fault Type | Injection Parameters | Description |
|------------|---------------------|-------------|
| Bad Block | ch, chip, die, plane, block | Mark specified block as bad; subsequent erases fail |
| Read Error | ch, chip, die, plane, block, page | Inject uncorrectable read error on specified page |
| Program Error | ch, chip, die, plane, block, page | Inject write failure on specified page |
| Erase Error | ch, chip, die, plane, block | Inject erase failure on specified block |
| Bit Flip | ch, chip, die, plane, block, page, bit_pos | Flip specified bit in specified page |
| Read Disturb Storm | ch, chip, die, block | Rapidly accumulate read_count, simulating read disturb effects |
| Data Retention | ch, chip, die, plane, block, aging_factor | Accelerate data retention degradation |

### 8.1.2 Power Faults

| Fault Type | Description |
|------------|-------------|
| Sudden Power Off | Immediately terminate daemon (simulating power loss), no persistence |
| Power Off During Write | Simulate power loss during Write Buffer flush to NAND |
| Power Off During GC | Simulate power loss during GC execution |

### 8.1.3 Controller Faults

| Fault Type | Description |
|------------|-------------|
| Memory Corruption | Inject random errors in L2P mapping table |
| Firmware Panic | Trigger firmware Panic, testing Panic handling flow |
| Channel Timeout | Cause all operations on specified Channel to time out |
| NVMe Queue Corruption | Inject CQE errors (Phase Tag flip, etc.) |

## 8.2 Fault Injection Interface

Fault injection is executed via the OOB interface (`POST /fault/inject`) or the `hfsss-ctrl fault inject` CLI tool, supporting:
- **Immediate injection** (immediate=true): Triggers on the next operation;
- **Delayed injection** (delay_ms=N): Triggers after N milliseconds;
- **Probabilistic injection** (probability=0.01): Triggers with specified probability on each operation;
- **Persistent injection** (persistent=true): Remains active until explicitly cleared;
- **One-shot injection** (persistent=false, default): Automatically cleared after triggering once.

---

# Chapter 9: System Reliability and Stability Requirements

## 9.1 MTBF Target

Simulator software Mean Time Between Failures (MTBF) targets:
- Under normal IO workloads (fio mixed read/write, 64 threads, QD=128): MTBF >= 720 hours (30 days continuous operation);
- During fault injection testing: Correctly handle all defined fault types without Panic or data corruption.

## 9.2 Data Integrity Guarantee

- Under normal operation (no fault injection): Read-back data is 100% consistent with written data (md5sum verification);
- After normal shutdown (NVMe Shutdown command) and restart: All data from completed write commands (FUA=1 or post-Flush) must be fully recovered;
- After simulated power loss (SIGKILL) and restart: Data in Write Buffer that was not yet persisted may be lost, but already-persisted data is 100% intact.

## 9.3 Stability Requirements

- Long-term operation stability: No crashes or memory leaks (memory growth < 1 MB/hour) during 72-hour sustained I/O pressure tests;
- Correct handling of extreme scenarios: When NAND capacity is full (no free blocks) or all spare blocks are exhausted, return correct error codes rather than crash;
- Concurrency safety: No data races in multi-threaded concurrent access to shared data structures (verified via Thread Sanitizer).

---

# Chapter 10: Development Constraints and Technology Selection

## 10.1 Development Languages

- **Kernel Module (PCIe/NVMe Emulation Layer)**: C language, following Linux kernel coding style;
- **User-space Daemon (Firmware Emulation Layer)**: C language (main body), minor C++ (STL for efficient data structures such as priority_queue, unordered_map);
- **OOB Management Tool**: Python 3.x (hfsss-ctrl CLI tool, management scripts);
- **Configuration File**: YAML format;
- **Build System**: GNU Make + CMake (user-space parts);
- **Test Framework**: Google Test (C++ unit tests), pytest (Python tests).

## 10.2 Target Operating System

- **Platform**: Debian 12 (Bookworm)
- **Kernel Version**: Linux 6.1.x LTS (Debian 12 default kernel) or Linux 5.15.x LTS
- **Architecture**: x86_64 (amd64)
- **GCC Version**: GCC 12.x (Debian 12 default)
- **Dependencies**:
  - `libyaml`: YAML configuration file parsing
  - `libpthread`: POSIX threads (standard library)
  - `libaio`: Asynchronous I/O (persistence thread)
  - `liburing`: io_uring interface (high-performance I/O for persistence thread)
  - `jansson`: JSON parsing for JSON-RPC interface
  - `libssl`: SHA-256 firmware verification

## 10.3 Code Structure

```
hfsss/
+-- kernel/
|   +-- hfsss_nvme.c/h      # PCIe/NVMe kernel module
+-- daemon/
|   +-- main.c              # Daemon entry point
|   +-- controller/         # Controller thread module
|   +-- firmware/
|   |   +-- hal/            # Hardware Access Layer
|   |   |   +-- nand_drv.c/h
|   |   |   +-- nor_drv.c/h
|   |   |   +-- nvme_hal.c/h
|   |   |   +-- pmic_drv.c/h
|   |   +-- common/         # Common Service Layer
|   |   |   +-- rtos.c/h
|   |   |   +-- scheduler.c/h
|   |   |   +-- memory.c/h
|   |   |   +-- bootloader.c/h
|   |   |   +-- power.c/h
|   |   |   +-- ipc.c/h
|   |   |   +-- watchdog.c/h
|   |   |   +-- panic.c/h
|   |   |   +-- debug.c/h
|   |   |   +-- log.c/h
|   |   +-- app/            # Application Layer
|   |       +-- ftl.c/h
|   |       +-- gc.c/h
|   |       +-- wear_level.c/h
|   |       +-- bad_block.c/h
|   |       +-- ecc.c/h
|   |       +-- qos.c/h
|   |       +-- redundancy.c/h
|   |       +-- error_handler.c/h
|   +-- media/
|   |   +-- nand_media.c/h  # NAND media thread
|   |   +-- nor_media.c/h   # NOR media thread
|   |   +-- timing_model.c/h # Timing model
|   |   +-- persistence.c/h  # Data persistence
|   +-- oob/
|       +-- oob_server.c/h  # OOB Socket service
|       +-- proc_interface.c/h  # /proc interface
+-- tools/
|   +-- hfsss-ctrl          # Python CLI tool
|   +-- scripts/            # Utility scripts
+-- tests/
|   +-- unit/               # Unit tests
|   +-- integration/        # Integration tests
|   +-- fault_injection/    # Fault injection tests
+-- config/
|   +-- hfsss.yaml.example  # Configuration file example
+-- docs/
    +-- PRD.md (this document)
    +-- API.md
```

## 10.4 Performance Engineering Requirements

- **No Dynamic Memory Allocation on Critical Paths**: The critical path for NVMe command processing (from SQ fetch to CQ write-back) must not call `malloc`/`free`;
- **No System Calls on Critical Paths**: Except for necessary `write` (logging) and `mmap` (initialization), the command processing path must not make system calls;
- **Memory Access Locality**: Optimize FTL mapping table access patterns (use 4 KB huge pages to reduce TLB misses, partition L2P table by Channel to improve Cache hit rate);
- **NUMA Awareness**: Media threads, FTL threads, and their operated memory (DRAM storage area, mapping tables) should be on the same NUMA node whenever possible;
- **False Sharing Elimination**: Frequently updated fields in critical shared data structures must be aligned to Cache Line boundaries (64 bytes) to avoid False Sharing.

---

# Chapter 11: Test Strategy

## 11.1 Unit Tests

- FTL address translation logic: L2P query/update correctness for various LBA ranges;
- GC algorithms: Block selection correctness for Greedy, Cost-Benefit, and FIFO algorithms;
- Timing model: EAT calculation correctness for various NAND command types;
- RTOS primitives: Concurrency correctness for mutexes, semaphores, and message queues;
- WAL: Write-replay data consistency.

## 11.2 Integration Tests

- NVMe protocol compatibility: Test using full `nvme-cli` command set;
- fio workload testing: Cover random read/write, sequential read/write, and mixed read/write with various QD and thread counts;
- Data integrity testing: Write known data, read back and compare (using vdbench, blktests, etc.);
- Persistence testing: Post-restart data recovery verification;
- Long-term stability testing: 72-hour sustained IO.

## 11.3 Performance Tests

- IOPS benchmark: fio 4 KB random read/write (QD=1, 4, 8, 16, 32, 64, 128);
- Bandwidth benchmark: fio 128 KB/1 MB sequential read/write;
- Latency testing: fio QD=1 single-thread latency distribution (P50/P90/P99/P99.9);
- Steady-state testing: Sustained writes to 90% disk utilization, then observe IOPS/latency stability;
- GC impact testing: Measure IO latency fluctuations during GC-intensive periods.

## 11.4 Fault Tests

- FTL/GC correct handling after bad block injection;
- Data recovery after simulated power loss (WAL replay);
- Extreme scenarios: NAND capacity exhaustion, all spare blocks depleted;
- Panic recovery: Restart after triggering Panic, verify system can resume normal operation.

---

# Chapter 12: Enterprise SSD Features

## 12.1 Unexpected Power Loss Protection (UPLP)

### 12.1.1 Overview

Enterprise SSDs must guarantee data integrity across unexpected power loss events. HFSSS simulates the supercapacitor-backed power-fail-safe behavior found in enterprise-class drives. This includes modeling the energy budget available from the backup capacitor, implementing an atomic write unit, and defining the precise recovery sequence that executes on the next power-up.

### 12.1.2 Supercapacitor Energy Model (FR-ENT-001)

The simulator models a bank of supercapacitors providing backup energy during power loss.

**Model Parameters**:

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| CAP_TOTAL_ENERGY_MJ | 500 | 100-2000 | Total usable energy in millijoules |
| CAP_VOLTAGE_INITIAL_V | 5.0 | 3.3-12.0 | Initial charged voltage |
| CAP_VOLTAGE_CUTOFF_V | 2.5 | 1.8-4.0 | Minimum operating voltage (below this, flush halts) |
| CAP_ESR_MOHM | 50 | 10-200 | Equivalent series resistance in milliohms |
| FLUSH_POWER_W | 8.0 | 2.0-25.0 | Average power draw during emergency flush |

**Energy Budget Calculation**:
```
Available energy = 0.5 * C * (V_initial^2 - V_cutoff^2)
Available flush time = Available_energy / FLUSH_POWER_W
```

The simulator tracks the remaining energy in real time during a simulated power-fail event. Each NAND program operation during emergency flush deducts energy proportional to its duration and power draw.

### 12.1.3 UPLP State Machine (FR-ENT-002)

The UPLP subsystem operates through a four-state machine:

```
Normal ----[Power-fail detected]----> PowerFail
                                          |
                                  [Begin cap-backed flush]
                                          |
                                          v
                                      CapDrain ----[Flush complete OR energy exhausted]----> SafeState
```

**State Descriptions**:

| State | Description |
|-------|-------------|
| Normal | Power rails stable; all subsystems operating normally. Supercapacitors are charged and monitored. |
| PowerFail | Main power loss detected (simulated via signal or OOB command). All new host I/O is rejected. Controller transitions to emergency flush mode. |
| CapDrain | Supercapacitor energy is being consumed to flush critical metadata and in-flight data. Flush proceeds according to the priority order defined below. |
| SafeState | Either the emergency flush completed successfully, or the energy budget was exhausted. The controller records the flush outcome (complete/partial) in NOR Flash and halts. On next power-up, the recovery sequence uses this record. |

### 12.1.4 Atomic Write Unit Definition (FR-ENT-003)

- The atomic write unit is a single NAND page (default 16 KB).
- Any write that has been programmed to NAND is guaranteed to be fully readable after recovery. Partially programmed pages (torn writes) are detected via ECC failure and discarded.
- The NVMe Identify Controller fields AWUN (Atomic Write Unit Normal) and AWUPF (Atomic Write Unit Power Fail) are set to reflect the configured page size.
- Multi-page host writes that span multiple NAND pages are NOT guaranteed atomic across pages unless the WAL commit covers them.

### 12.1.5 Power-Fail-Safe Metadata Journal (FR-ENT-004)

A dedicated journal ensures metadata consistency across power loss:

- The metadata journal is a circular log stored in a reserved region of NOR Flash (4 MB partition).
- Every L2P mapping table update, BBT modification, and SMART counter update is journaled before the in-memory state is modified.
- Journal entries use CRC-32 checksums for integrity verification.
- Journal replay on recovery restores metadata to the last consistent state.
- Journal write throughput target: >= 50,000 entries/second (sufficient for sustained 200K IOPS write workload).

### 12.1.6 Emergency Flush Priority Order (FR-ENT-005)

During the CapDrain state, the controller flushes data and metadata in the following strict priority order:

1. **L2P Mapping Journal**: Flush the in-memory L2P journal (incremental delta) to NOR Flash. This is highest priority because without a consistent L2P table, all user data is unreachable.
2. **Bad Block Table (BBT)**: Flush updated BBT to NOR Flash. Loss of BBT can lead to data written to known-bad blocks on next boot.
3. **SMART Counters**: Flush accumulated SMART statistics (power cycle count, unsafe shutdown count, write amplification data) to NOR Flash.
4. **WAL Commit Records**: Flush pending WAL commit records for in-flight write operations. This determines which host writes survive power loss.
5. **Write Buffer Dirty Data**: If energy remains, program dirty pages from the Write Buffer to NAND in LBA order (lowest LBA first), maximizing the amount of committed user data.

The flush engine tracks energy consumed after each operation and aborts the sequence when the remaining energy falls below the cost of the next operation.

### 12.1.7 UPLP Recovery Sequence (FR-ENT-006)

On the next power-up after an unexpected power loss:

1. Bootloader reads the SafeState record from NOR Flash to determine flush outcome (complete/partial).
2. Replay the L2P metadata journal from NOR to reconstruct the mapping table to the last consistent checkpoint.
3. Replay the WAL to identify and complete (or discard) partially committed writes.
4. Scan NAND for partially programmed pages (ECC failure on read): Invalidate those pages and update L2P accordingly.
5. Rebuild free block lists and erase count tables.
6. Increment SMART Unsafe Shutdown Count.
7. Resume normal operation.

**Acceptance Criteria**:
- After 1000 random-timing simulated power-loss events during sustained 4KB random write workload, zero data corruption detected (md5sum verification of all successfully completed writes);
- Recovery time after power loss < 10 seconds (configurable).

---

## 12.2 QoS Determinism

### 12.2.1 Overview

Enterprise workloads demand deterministic latency guarantees. HFSSS implements a multi-queue scheduler with per-namespace resource isolation and SLA enforcement, ensuring that background operations (GC, WL) do not cause unpredictable latency spikes.

### 12.2.2 DWRR Multi-Queue Scheduler (FR-ENT-007)

The I/O scheduler implements a Deficit Weighted Round Robin (DWRR) algorithm for fair bandwidth distribution across namespaces and priority classes.

**Scheduler Design**:
- Each namespace is assigned a dedicated scheduling queue.
- Each queue has a configurable weight (default: equal weight across namespaces).
- The DWRR quantum is 16 KB (one NAND page); each round, a queue may dispatch commands consuming up to its deficit + quantum before yielding.
- An Urgent bypass lane exists for latency-critical commands (e.g., NVMe Urgent priority class), which are dispatched immediately regardless of DWRR state.
- The scheduler runs on a dedicated controller core with SCHED_FIFO priority to minimize scheduling jitter.

### 12.2.3 Per-Namespace IOPS and Bandwidth Limits (FR-ENT-008)

Each namespace can be independently configured with:

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| max_read_iops | Unlimited | 1K-2M | Maximum read IOPS for this namespace |
| max_write_iops | Unlimited | 1K-2M | Maximum write IOPS for this namespace |
| max_read_bw_mbps | Unlimited | 50-14000 | Maximum read bandwidth (MB/s) |
| max_write_bw_mbps | Unlimited | 50-14000 | Maximum write bandwidth (MB/s) |

- Limits are enforced via per-namespace token buckets (dual token bucket: one for IOPS, one for bandwidth).
- Token replenishment occurs at microsecond granularity.
- Commands exceeding limits are queued (not rejected), with a configurable maximum queue depth per namespace (default: 1024 commands).
- Limits can be modified at runtime via `Set Features` (Feature ID = vendor-specific) or via the OOB interface without requiring a controller reset.

### 12.2.4 P99/P99.9 Latency SLA Enforcement (FR-ENT-009)

The QoS subsystem continuously monitors per-namespace latency percentiles:

- Latency tracking uses a high-resolution histogram with 1-microsecond bucket granularity up to 100 ms, then 100-microsecond buckets up to 10 seconds.
- SLA violation detection: When observed P99 or P99.9 latency exceeds the configured SLA target for a sliding 1-second window, the QoS controller takes corrective action:
  1. Reduce GC bandwidth allocation by 50%;
  2. Suspend static wear leveling operations;
  3. If SLA still violated for 5 consecutive seconds, temporarily boost the affected namespace's DWRR weight by 2x.
- SLA targets are configurable per namespace via the OOB interface.
- Default SLA targets: P99 read latency <= 500 us, P99.9 read latency <= 2 ms.

### 12.2.5 Deterministic Latency During GC (FR-ENT-010)

GC operations are constrained to limit their impact on foreground IO latency:

- **Maximum latency ratio**: During active GC, the worst-case read latency must not exceed 2x the baseline read latency (baseline = latency measured under the same workload without GC).
- **GC pacing**: The GC engine uses a credit-based pacing mechanism. For every N foreground IO commands completed, the GC engine is allowed to issue one NAND read or program. The ratio N is dynamically adjusted based on current latency percentiles.
- **Program Suspend**: When a foreground read targets a Die currently executing a GC Program, the firmware immediately issues a Program Suspend, services the read, and then issues Program Resume. The suspend/resume overhead (tSUSP + tRESM = 10 us) is included in latency calculations.
- **GC-free windows**: Support for configurable GC-free windows (e.g., no GC during specified time intervals) via the OOB interface, for workloads with known latency-sensitive periods.

### 12.2.6 QoS Policy Hot-Reconfiguration (FR-ENT-011)

All QoS parameters can be modified at runtime without requiring a controller reset or namespace detach:

- DWRR weights, IOPS/bandwidth limits, SLA targets, and GC pacing ratios are all hot-reconfigurable.
- Configuration changes take effect within 100 ms of the command being processed.
- Changes are journaled to NOR Flash so they persist across power cycles.
- The OOB interface provides a `POST /qos/policy` endpoint for batch QoS policy updates.

---

## 12.3 End-to-End Data Protection (T10 DIF/PI)

### 12.3.1 Overview

End-to-end data protection ensures data integrity from the host application through the controller, FTL, and down to the NAND media, and back. HFSSS simulates T10 Data Integrity Field (DIF) / Protection Information (PI) as defined in the NVMe specification.

### 12.3.2 T10 PI Type 1/2/3 Support (FR-ENT-012)

The simulator supports all three T10 Protection Information types:

| PI Type | Guard Tag | Reference Tag | Application Tag | Description |
|---------|-----------|---------------|-----------------|-------------|
| Type 1 | CRC-16 of data | LBA (incrementing) | Application-defined | Full protection; reference tag verified against LBA |
| Type 2 | CRC-16 of data | Application-defined | Application-defined | Reference tag set by application, not tied to LBA |
| Type 3 | CRC-16 of data | Not checked | Application-defined | Guard tag only; no reference tag verification |

PI type is configured per namespace during namespace creation or format.

### 12.3.3 CRC-16 Guard Tag (FR-ENT-013)

- The Guard Tag is a 16-bit CRC computed over each logical block's data (typically 512 bytes or 4096 bytes depending on LBA format).
- CRC polynomial: CRC-16-T10-DIF (x^16 + x^15 + x^11 + x^9 + x^8 + x^7 + x^5 + x^4 + x^2 + x + 1).
- Guard Tag computation is performed by a dedicated firmware thread emulating a hardware CRC engine.
- A Guard Tag value of 0xFFFF indicates that PI checking should be disabled for that block (allowing selective protection).

### 12.3.4 Reference Tag and Application Tag (FR-ENT-014)

- **Reference Tag** (32-bit): For Type 1 PI, the reference tag is initialized to the lower 32 bits of the starting LBA and increments by 1 for each subsequent logical block in a command. For Type 2, the reference tag is application-defined.
- **Application Tag** (16-bit): Freely set by the application. The controller passes the application tag through without modification.
- **Application Tag Mask** (16-bit): Configured via the PRINFO field in NVMe commands. Bits set in the mask cause corresponding application tag bits to be excluded from verification.

### 12.3.5 PI Insertion and Stripping at Controller (FR-ENT-015)

The NVMe controller supports PI insertion and stripping based on the PRACT (Protection Information Action) bit in NVMe commands:

- **PRACT=1 on Write**: Controller generates PI (computes CRC-16 Guard Tag, sets Reference Tag from LBA, copies Application Tag from command) and appends it to each logical block before passing data to FTL.
- **PRACT=1 on Read**: Controller strips PI metadata before returning data to host, verifying Guard and Reference Tags in the process.
- **PRACT=0**: PI is passed through transparently. The host is responsible for generating and verifying PI.

### 12.3.6 PI Verification at Media Layer (FR-ENT-016)

When data is read from NAND media, the HAL layer performs PI verification before returning data to the FTL:

- Guard Tag is recomputed over the read data and compared against the stored Guard Tag.
- Reference Tag (for Type 1) is compared against the expected LBA-derived value.
- On verification failure, the read is retried (Read Retry mechanism). If all retries fail, an End-to-End Guard Check Error (status code 0x82) is returned.
- PI verification failures are logged and counted in the SMART Media and Data Integrity Errors field.

### 12.3.7 PI Metadata Propagation Through FTL (FR-ENT-017)

PI metadata must be preserved through all FTL operations:

- **Write path**: PI data (8 bytes per logical block) is stored alongside user data in the NAND page OOB area.
- **GC path**: When GC migrates valid pages, PI metadata is read along with user data and rewritten to the destination page. The Guard Tag is re-verified during migration; Reference Tags are updated if the LBA-to-PPN mapping changes.
- **Read path**: PI metadata is read from OOB, verified, and optionally stripped before returning to the host.
- **FTL mapping table**: The L2P mapping table does not store PI metadata directly; PI travels with the data in the OOB area.

---

## 12.4 Data-at-Rest Encryption

### 12.4.1 Overview

Enterprise SSDs protect stored data through encryption. HFSSS simulates AES-XTS 256-bit encryption with a hierarchical key management scheme and basic TCG Opal SSC command support. Since this is a simulation, actual cryptographic security is not a goal -- the focus is on correctly modeling the key hierarchy, command flow, and performance impact.

### 12.4.2 AES-XTS 256-bit Simulation (FR-ENT-018)

- All user data written to the emulated NAND media passes through a simulated AES-XTS-256 encryption stage.
- The encryption simulation does not perform actual AES computation by default (to avoid unnecessary CPU overhead). Instead, it XORs data with a deterministic key-derived pattern, which is sufficient for modeling the data flow and verifying key management logic.
- An optional mode (`encryption.real_aes: true` in config) enables actual AES-XTS-256 encryption using OpenSSL/libcrypto, for validating performance impact of real encryption.
- Encryption/decryption latency is modeled as a configurable overhead per page (default: 2 us encrypt, 1 us decrypt), added to the NAND timing model.

### 12.4.3 Key Hierarchy (FR-ENT-019)

The simulator implements a three-level key hierarchy:

```
Master Key (MK)
  |
  +-- Namespace KEK (Key Encryption Key) [one per namespace]
        |
        +-- Data DEK (Data Encryption Key) [one per namespace, wrapped by KEK]
```

- **Master Key (MK)**: A 256-bit key generated at initial drive setup (Format NVM with crypto erase). Stored in a simulated secure key storage area within NOR Flash (encrypted with a hardcoded platform key for simulation purposes).
- **Namespace KEK**: Derived from the Master Key and Namespace ID using HKDF-SHA256. Used to wrap/unwrap the namespace's DEK.
- **Data DEK**: A 256-bit AES key used for actual data encryption. Generated randomly on namespace creation. Stored in NOR Flash wrapped (encrypted) by the corresponding KEK.

### 12.4.4 TCG Opal SSC Basic Command Set (FR-ENT-020)

The simulator implements a subset of the TCG Opal SSC (Security Subsystem Class) protocol:

| Command | Description |
|---------|-------------|
| Discovery0 | Return Level 0 Discovery response with Opal SSC feature descriptor |
| StartSession | Establish a session with the Security Provider (SP) |
| Authenticate | Authenticate using SID (Security Identifier) or Admin credentials |
| Get | Retrieve properties from the SP tables |
| Set | Modify properties in the SP tables (e.g., set password, lock/unlock range) |
| RevertSP | Revert the Security Provider to factory state |
| EndSession | Terminate the current session |

- The Opal SSC simulation includes the Admin SP and Locking SP.
- Locking Ranges correspond to namespace ranges and can be locked/unlocked independently.
- The default SID password is "MSID" (Manufacturer's SID); users must take ownership by changing SID before enabling locking.

### 12.4.5 Crypto Erase (FR-ENT-021)

Crypto erase destroys access to data by deleting the encryption keys rather than erasing NAND media:

- Triggered via NVMe Format NVM command with Secure Erase Setting (SES) = 2 (Cryptographic Erase).
- The controller deletes the namespace's DEK and generates a new random DEK.
- Since the old DEK is destroyed, all previously encrypted data becomes permanently inaccessible.
- Crypto erase completes in < 1 second (key deletion only, no NAND erase required).
- SMART log records the crypto erase event.

### 12.4.6 Secure Erase (FR-ENT-022)

Secure erase performs a full NAND block erase of all user data:

- Triggered via NVMe Format NVM command with SES = 1 (User Data Erase).
- The controller issues Block Erase commands to every block in the target namespace.
- After erase, all blocks are verified to contain 0xFF (simulated verification).
- Secure erase duration is proportional to namespace size (approximately: num_blocks x tERS).
- Progress is reportable via Get Log Page (Format NVM Status).

### 12.4.7 Secure Boot Chain Verification (FR-ENT-023)

The simulator models a simplified secure boot chain:

1. **Root of Trust**: Hardcoded hash of the Bootloader image (stored in a simulated OTP/fuse area).
2. **Bootloader Verification**: On power-up, compute SHA-256 of the Bootloader partition in NOR Flash and compare against the Root of Trust hash. Boot halts on mismatch.
3. **Firmware Verification**: The Bootloader verifies the firmware image (Slot A or B) using SHA-256. The expected hash is stored alongside the firmware image in a signed manifest.
4. **Chain of Trust**: Each layer verifies the next before transferring control.
5. **Failure Handling**: If both firmware slots fail verification, the controller enters a recovery mode (only Admin commands accepted, no IO).

---

## 12.5 Multi-Namespace Management

### 12.5.1 Overview

Enterprise SSDs support multiple namespaces for workload isolation, resource partitioning, and multi-tenant deployment. HFSSS supports up to 32 concurrently active namespaces, each with independent FTL mapping tables and configurable capacity.

### 12.5.2 Namespace Create and Delete (FR-ENT-024)

- Namespaces are created via the NVMe Namespace Management (Admin Opcode 0x0D) command.
- **Create parameters**: NSZE (Namespace Size in logical blocks), NCAP (Namespace Capacity), FLBAS (Formatted LBA Size), DPS (Data Protection Settings), NMIC (Namespace Multi-path and Sharing Capabilities).
- Namespace capacity is carved from the global NAND free block pool. The minimum namespace size is 1 GiB; maximum is the total available capacity.
- Namespace deletion releases all associated NAND blocks back to the global free block pool. Associated L2P mapping table memory is freed. The namespace's DEK is destroyed (crypto erase by default on delete).
- Maximum supported namespaces: 32 (configurable up to 4096 for special testing scenarios).

### 12.5.3 Namespace Attach and Detach (FR-ENT-025)

- Created namespaces must be attached to the controller before they are visible as block devices.
- Attach is performed via the NVMe Namespace Attachment (Admin Opcode 0x14) command with Controller Attachment action.
- Detach removes the namespace from the controller's active namespace list. Detached namespaces retain their data and configuration but are not accessible for IO.
- The host receives an Asynchronous Event Notification (Namespace Attribute Changed) when namespaces are attached or detached.

### 12.5.4 Per-Namespace FTL Mapping Tables (FR-ENT-026)

Each namespace maintains its own independent FTL data structures:

- **L2P Mapping Table**: Separate L2P table per namespace. LPN space starts at 0 for each namespace. Table size is proportional to namespace capacity.
- **Write Block Allocation**: Each namespace has its own Current Write Block pool, ensuring write isolation between namespaces.
- **GC Independence**: GC operates per-namespace. GC in one namespace does not affect the latency or throughput of other namespaces.
- **Wear Leveling**: Wear leveling operates globally across all namespaces to ensure uniform block wear across the entire drive.
- **Memory Allocation**: FTL mapping table memory is allocated from the firmware heap proportionally to namespace size at creation time.

### 12.5.5 Namespace Format (FR-ENT-027)

- The Format NVM command (Admin Opcode 0xC0) can target a specific namespace (by NSID) or all namespaces (NSID = 0xFFFFFFFF).
- Format parameters: LBA Format index (selecting LBA size and PI settings), Secure Erase Setting (none / user data erase / crypto erase), PI Location (first 8 bytes or last 8 bytes of metadata).
- During format, the target namespace is marked as Not Ready (CSTS bit), and IO commands to that namespace return Namespace Not Ready error.
- Format operation erases all data in the namespace, rebuilds the L2P mapping table, and resets SMART counters for that namespace.

### 12.5.6 Namespace Limits (FR-ENT-028)

| Parameter | Value |
|-----------|-------|
| Maximum namespaces | 32 (default), up to 4096 (test mode) |
| Minimum namespace size | 1 GiB |
| Maximum namespace size | Total NAND capacity - OP |
| Namespace granularity | 1 GiB (allocated in whole GiB increments) |
| Maximum attached namespaces | 32 |
| Namespace ID range | 1-4294967294 (0xFFFFFFFE) |

---

## 12.6 Thermal Management and Throttling

### 12.6.1 Overview

Enterprise SSDs must manage thermal conditions to prevent damage and ensure reliable operation. HFSSS simulates composite temperature computation, progressive thermal throttling, and temperature tracking for SMART reporting.

### 12.6.2 Composite Temperature Calculation (FR-ENT-029)

The simulated composite temperature is computed based on workload intensity:

```
T_composite = T_ambient
            + (read_iops * COEFF_READ_IOPS)
            + (write_iops * COEFF_WRITE_IOPS)
            + (gc_bw_mbps * COEFF_GC_BW)
            + (active_channels * COEFF_CHANNEL)
```

**Default Coefficients**:

| Coefficient | Default Value | Description |
|-------------|---------------|-------------|
| T_ambient | 25 C | Ambient temperature (configurable) |
| COEFF_READ_IOPS | 0.00002 C/IOPS | Temperature rise per read IOP |
| COEFF_WRITE_IOPS | 0.00005 C/IOPS | Temperature rise per write IOP (higher due to program power) |
| COEFF_GC_BW | 0.005 C/(MB/s) | Temperature rise per MB/s of GC bandwidth |
| COEFF_CHANNEL | 0.5 C/channel | Temperature rise per active channel |

Temperature is recalculated every 100 ms. The temperature model includes thermal inertia (exponential moving average with configurable time constant, default 10 seconds) to simulate realistic temperature ramp-up and cool-down behavior.

### 12.6.3 Progressive Thermal Throttle (FR-ENT-030)

Three temperature thresholds trigger progressive throttling:

| Threshold | Default | Action |
|-----------|---------|--------|
| Warning (WCTEMP) | 70 C | Set SMART Critical Warning bit 1 (Temperature). Send Asynchronous Event Notification. No performance impact. |
| Throttle (CCTEMP - 5) | 75 C | Reduce maximum IOPS to 50% of baseline. Reduce GC bandwidth allocation. Log throttle event. |
| Critical Shutdown (CCTEMP) | 80 C | Initiate emergency shutdown sequence: flush Write Buffer, persist metadata, set CSTS.CFS. Halt all IO. |

- Throttle thresholds are configurable via NVMe Set Features (Feature ID 0x04: Temperature Threshold).
- Throttle is implemented by reducing the token replenishment rate in the QoS token buckets.
- When temperature drops below the throttle threshold minus a hysteresis margin (default 5 C), throttle is released and full performance is restored.

### 12.6.4 Time-in-Temperature Tracking (FR-ENT-031)

The simulator tracks cumulative time spent in each temperature zone:

- SMART log fields: Warning Composite Temperature Time (bytes 192-195) and Critical Composite Temperature Time (bytes 196-199).
- Additionally, a vendor-specific SMART log page (Log Page ID 0xCA) provides a detailed temperature histogram with 5 C buckets from 0 C to 100 C, recording minutes spent in each bucket.
- Temperature history is persisted to NOR Flash every 10 minutes and on graceful shutdown.

---

## 12.7 Advanced Telemetry and Diagnostics

### 12.7.1 Overview

Enterprise SSDs provide rich telemetry data for proactive monitoring, failure prediction, and root-cause analysis. HFSSS simulates NVMe telemetry log pages, vendor-specific diagnostics, SMART predictive analysis, and asynchronous event notifications.

### 12.7.2 Host-Initiated Telemetry Log Page (FR-ENT-032)

The simulator supports Get Log Page with Log Page Identifier 0x07 (Host-Initiated Telemetry):

- The host triggers telemetry data collection by setting the Create Telemetry Host-Initiated Data bit (CDW10 bit 0) in the Get Log Page command.
- The telemetry log contains three data areas:
  - **Data Area 1** (mandatory, 512 bytes): Telemetry header including Reason Identifier, Data Area sizes, and IEEE OUI.
  - **Data Area 2** (optional, configurable size): Internal state snapshot -- current queue depths, channel utilization, GC state, temperature, power state.
  - **Data Area 3** (optional, configurable size): Historical data -- latency histograms, IOPS time series (last 60 seconds at 1-second granularity), WAF history.
- Maximum telemetry log size: 4 MB (configurable).

### 12.7.3 Controller-Initiated Telemetry Log Page (FR-ENT-033)

The simulator supports Get Log Page with Log Page Identifier 0x08 (Controller-Initiated Telemetry):

- The controller autonomously initiates telemetry collection when it detects notable events:
  - SMART Critical Warning state change;
  - P99.9 latency SLA violation;
  - GC emergency mode activation;
  - Uncorrectable ECC error;
  - Temperature throttle activation.
- The controller sets the Controller-Initiated Telemetry Data Available flag, and sends an Asynchronous Event Notification to the host.
- Log format is identical to host-initiated telemetry.
- At most one controller-initiated telemetry log is retained at a time; a new event overwrites the previous log.

### 12.7.4 Vendor-Specific Log Pages (FR-ENT-034)

The simulator provides the following vendor-specific log pages:

| Log Page ID | Name | Content |
|-------------|------|---------|
| 0xC0 | Internal Statistics | Per-channel IOPS/bandwidth/latency, FTL mapping table hit rate, free block counts per Die |
| 0xC1 | GC Statistics | GC trigger count, average valid pages per victim block, WAF (1-minute/1-hour/lifetime), GC suspend count |
| 0xC2 | Wear Leveling Statistics | Min/max/average erase count, erase count histogram (10 buckets), static WL migration count |
| 0xC3 | Error Statistics | Correctable ECC error count per channel, Read Retry histogram, bad block growth history |
| 0xCA | Temperature Histogram | Time-in-temperature distribution (5 C buckets, 0-100 C) |
| 0xCB | Power State History | Cumulative time per power state, power state transition count |

All vendor-specific log pages are accessible via `nvme get-log /dev/nvme0 --log-id=0xC0 --log-len=4096`.

### 12.7.5 SMART Predictive Analysis (FR-ENT-035)

The simulator performs basic predictive analysis based on SMART data trends:

- **Remaining Lifetime Estimation**: Based on current average P/E cycle count and PE_CYCLE_LIMIT, estimate remaining write endurance as a percentage.
- **Spare Block Depletion Prediction**: Based on bad block growth rate (blocks marked bad per week), predict when Available Spare will reach the threshold.
- **WAF Trend Analysis**: Track WAF over time; detect and alert on WAF degradation trends that may indicate suboptimal workload patterns.
- Predictions are exposed via vendor-specific SMART log page (Log Page ID 0xCB) and via the OOB `GET /smart/predictions` endpoint.

### 12.7.6 Asynchronous Event Notifications (FR-ENT-036)

The simulator generates Asynchronous Event Notifications (AEN) for the following enterprise-relevant events:

| Event Type | Event Info | Description |
|------------|-----------|-------------|
| Smart/Health (0x01) | 0x00 | SMART Health Status change (Critical Warning bits changed) |
| Smart/Health (0x01) | 0x01 | Temperature above Warning threshold |
| Smart/Health (0x01) | 0x02 | Available Spare below threshold |
| Notice (0x02) | 0x00 | Namespace Attribute Changed (NS created/deleted/formatted) |
| Notice (0x02) | 0x01 | Firmware Activation Starting |
| Notice (0x02) | 0x03 | Telemetry Log Changed (controller-initiated telemetry available) |
| Vendor-Specific (0x06) | 0x01 | QoS SLA Violation (P99.9 latency exceeded target) |
| Vendor-Specific (0x06) | 0x02 | Thermal Throttle Engaged/Released |
| Vendor-Specific (0x06) | 0x03 | GC Emergency Mode Activated |

- AEN delivery uses the NVMe Async Event Request mechanism (the host pre-submits AER commands; the controller completes them when events occur).
- Maximum outstanding AER commands: 4 (NVMe specification limit).
- Events are queued if no AER commands are available; queued events are delivered when the host submits new AER commands.

---

# Appendix A: Reference Configuration Examples

## A.1 128-Core Server Recommended Configuration

```yaml
# 128-core / 256 GB DRAM Debian server configuration example
memory:
  nand_base_addr: "0x2000000000"  # 128 GB offset
  nand_size_gb: 192               # 192 GB NAND storage
  write_buffer_gb: 4

cpu:
  nvme_dispatcher_cores: [120, 121]
  nvme_worker_cores: [122, 123, 124, 125, 126, 127]
  controller_cores: [100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111]
  ftl_cores: [80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95]
  nand_channel_cores:
    ch00: [0, 1, 2]
    ch01: [3, 4, 5]
    # ... and so on, 16 Channels x 3 cores = 48 cores

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
      size_gib: 8192    # 8 TB namespace
```

## A.2 GRUB Memory Reservation Configuration

Add the following to `/etc/default/grub` (256 GB DRAM, reserving 192 GB from 128 GB offset):
```
GRUB_CMDLINE_LINUX="memmap=192G\$128G isolcpus=0-127 nohz_full=0-127 rcu_nocbs=0-127 nosoftlockup"
```
- `isolcpus`: Isolate CPU cores used by the simulator from the kernel scheduler;
- `nohz_full`: Disable periodic timer interrupts on isolated cores (Tickless mode), reducing scheduling noise;
- `rcu_nocbs`: Disable RCU callbacks on isolated cores, reducing kernel interference;
- `nosoftlockup`: Disable kernel softlockup detection (avoiding false reports from long-running spins in the simulation).

---

# Appendix B: Performance Reference Data

## B.1 Typical TLC NAND Flash Parameters (Reference: Micron 176L TLC)

| Parameter | Value |
|-----------|-------|
| Read Latency (LSB) | 35 us |
| Read Latency (CSB) | 65 us |
| Read Latency (MSB) | 85 us |
| Page Program Latency (SLC) | 100 us |
| Page Program Latency (TLC, with fine-tuning) | 900 us |
| Block Erase Latency | 2.5 ms |
| ONFI Interface Rate | ONFI 4.2: 1600 MT/s |
| Page Size (Data + OOB) | 16 KB + 384 B |
| Block Size (pages) | 512 pages/block (8 MB/block) |
| Blocks per Die | 4096 blocks/Die |
| P/E Lifetime (TLC) | 1000-3000 cycles |

## B.2 Existing Simulator Performance Comparison

| Simulator | Random Read IOPS (4 KB, QD=32) | Emulation Accuracy | Platform |
|-----------|-------------------------------|-------------------|----------|
| MQSim | N/A (pure software simulation) | Medium | Bare-metal Linux |
| FEMU | 400K-1M (within VM) | Medium | QEMU/KVM |
| NVMeVirt | 1M+ (bare-metal) | Basic (latency injection) | Bare-metal Linux kernel module |
| **HFSSS (target)** | **600K-1M (with ECC)** | **High (complete timing model)** | **Bare-metal Debian** |

---

# Appendix C: Technical Implementation Recommendations and PRD Review Summary

## C.1 PRD Review Summary

**Review Date**: 2026-03-08
**Review Version**: V1.0

### C.1.1 Document Completeness Assessment

| Assessment Item | Status | Description |
|----------------|--------|-------------|
| Document Structure | Complete | Contains 11 main chapters + 3 appendices, clear structure |
| Functional Requirements | Detailed | 30+ FR requirement items, comprehensive coverage |
| Performance Requirements | Clear | Multi-dimensional definition: IOPS, bandwidth, latency, accuracy |
| Interface Definition | Complete | Host interface, management interface, configuration interface, persistence format |
| Technology Selection | Reasonable | C language kernel module + C user-space daemon |
| Test Strategy | Comprehensive | Unit tests, integration tests, performance tests, fault tests |

**Overall Assessment**: The PRD document is structurally complete and substantively detailed, exceeding 50,000 words in the original Chinese, covering all dimensions from product definition to implementation constraints. It is a high-quality requirements document.

### C.1.2 Survey Completeness Summary

This review supplemented the following open-source project updates:

1. **FEMU v10.1**:
   - Supports BBSSD/OCSSD/ZNS/NoSSD four modes
   - Ubuntu 20.04/22.04/24.04 support
   - CI/CD automated testing

2. **NVMeVirt Code Structure**:
   - admin.c/io.c/pci.c/ssd.c core modules
   - simple_ftl.c/conv_ftl.c/zns_ftl.c/kv_ftl.c multiple FTL implementations
   - Can serve as core reference for PCIe/NVMe emulation layer

3. **OpenSSD Jasmine**:
   - Real hardware platform firmware reference
   - Can serve as three-layer firmware architecture design reference

---

## C.2 Technical Implementation Recommendations

### C.2.1 Version Planning Adjustment Recommendations

| Original Version | Original Goal | Recommended Adjustment | Rationale |
|-----------------|---------------|----------------------|-----------|
| V1.0 | Alpha | No change | PCIe/NVMe basic emulation, DRAM backend, basic FTL |
| V1.5 | Beta | No change | 16-channel NAND accurate timing, NOR Flash, multi-core firmware threads |
| V2.0 | GA | Add basic ZNS support | FEMU/NVMeVirt already have references, can be implemented earlier |
| V2.5 | Enterprise | No change | Complete ZNS, KV-SSD, advanced fault injection |

### C.2.2 Key Technical Reference Recommendations

#### C.2.2.1 PCIe/NVMe Emulation Layer (Reference: NVMeVirt)

Recommended to reuse the following NVMeVirt designs:
- PCI configuration space emulation mechanism
- BAR mapping and MMIO handling
- SQ/CQ management and Doorbell handling
- Kernel-user space shared memory communication

**Implementation Path**:
1. Clone NVMeVirt source code as reference
2. Strip its simple FTL, retain PCIe/NVMe emulation framework
3. Implement communication interface with user-space daemon

#### C.2.2.2 NAND Timing Model (Reference: MQSim + FEMU)

Recommended references:
- MQSim's complete NAND hierarchy (Channel -> Chip -> Die -> Plane -> Block -> Page)
- FEMU's configurable timing parameter design
- TLC LSB/CSB/MSB differentiated latency

**Timing Parameter Recommendations**:
```c
// Reference: Micron 176L TLC NAND
#define NAND_T_R_LSB_US    35   // LSB page read latency
#define NAND_T_R_CSB_US    65   // CSB page read latency
#define NAND_T_R_MSB_US    85   // MSB page read latency
#define NAND_T_PROG_US     900  // Page program latency
#define NAND_T_ERS_MS      2.5  // Block erase latency
```

#### C.2.2.3 ZNS Support Implementation Path

ZNS (Zoned Namespace) is an important NVMe feature. Recommendations:

1. **V2.0 Basic ZNS Implementation**:
   - Zone type: Sequential Write Required
   - Zone capacity: Configurable (default 256 MB)
   - Zone state management: Empty -> Open -> Closed -> Full
   - Basic Zone commands: Zone Append, Zone Reset, Zone Report

2. **V2.5 Enhanced ZNS**:
   - Zone Active/Open resource limits
   - Zone Descriptor extensions
   - Zone Copy support

Reference: NVMeVirt's zns_ftl.c implementation

#### C.2.2.4 KV-SSD Support (Optional, V2.5)

Key-Value SSD is another important direction:
- Reference: NVMeVirt's kv_ftl.c
- Implement KV command set: Put, Get, Delete, Iterate
- LSM-Tree or Hash Table as two optional KV organization methods

---

## C.3 Development Priority Recommendations

### C.3.1 V1.0 (Alpha) Development Priorities

**P0 (Must Implement)**:
1. PCIe configuration space emulation (lspci recognizable)
2. NVMe controller register emulation (CAP/VS/CC/CSTS, etc.)
3. Basic Admin Queue support (Identify command)
4. I/O Queue creation and management
5. Simple DRAM backend (no NAND timing, direct memory read/write)
6. Basic page-level FTL (L2P mapping table)
7. Read/Write command processing
8. /dev/nvme0n1 block device recognizable

**P1 (Important)**:
1. Shared memory Ring Buffer (kernel-user space communication)
2. Flush command support
3. Basic error handling
4. Simple OOB Socket interface

**P2 (Optional)**:
1. Write Buffer
2. Basic GC (free block management only)

### C.3.2 V1.5 (Beta) Development Priorities

**P0 (Must Implement)**:
1. 16 Channel NAND hierarchy
2. NAND timing model (tR/tPROG/tERS)
3. Complete page-level FTL + GC (Greedy algorithm)
4. Wear leveling (Dynamic WL)
5. Multi-threaded media threads (independent thread per Channel)
6. CPU binding and NUMA optimization
7. NOR Flash emulation
8. Multi-core firmware CPU threads

**P1 (Important)**:
1. TLC LSB/CSB/MSB differentiated timing
2. Static wear leveling
3. Read cache (LRU)
4. Bad Block Management (BBM)
5. Data persistence (file system)

### C.3.3 V2.0 (GA) Development Priorities

**P0 (Must Implement)**:
1. Complete three-layer firmware architecture
2. RTOS task scheduling
3. ECC/LDPC emulation
4. QoS support
5. Complete OOB management implementation
6. Complete /proc interface implementation
7. hfsss-ctrl CLI tool
8. WAL and Checkpoint persistence

**P1 (Important)**:
1. Basic ZNS support
2. Fault injection framework
3. Performance statistics and observability
4. Panic/Assert handling

---

## C.4 Recommended Code Repository Structure

```
hfsss/
+-- docs/
|   +-- PRD.md                    # This document
|   +-- PRD_REVIEW_REPORT.md      # Review report
|   +-- Requirements_Matrix.md    # Requirements matrix (next step)
|   +-- HLD.md                    # High-Level Design (next step)
|   +-- LLD_*.md                  # Low-Level Designs (next step)
|   +-- API.md
+-- kernel/                       # Kernel module
|   +-- Makefile
|   +-- hfsss_nvme.h
|   +-- hfsss_nvme.c             # Main module entry
|   +-- pci.c                     # PCIe emulation
|   +-- admin.c                   # Admin commands
|   +-- io.c                      # I/O commands
|   +-- sq_cq.c                   # SQ/CQ management
|   +-- dma.c                     # DMA engine
|   +-- msix.c                    # MSI-X interrupts
|   +-- shmem.c                   # Shared memory
+-- daemon/                       # User-space daemon
|   +-- Makefile
|   +-- CMakeLists.txt
|   +-- main.c
|   +-- controller/               # Controller thread
|   |   +-- controller.h
|   |   +-- controller.c
|   |   +-- scheduler.c           # I/O scheduler
|   |   +-- resource.c            # Resource management
|   |   +-- flow_control.c        # Flow control
|   +-- firmware/
|   |   +-- hal/                  # Hardware Access Layer
|   |   |   +-- nand_drv.c/h
|   |   |   +-- nor_drv.c/h
|   |   |   +-- nvme_hal.c/h
|   |   |   +-- pmic_drv.c/h
|   |   +-- common/               # Common Service Layer
|   |   |   +-- rtos.c/h
|   |   |   +-- scheduler.c/h
|   |   |   +-- memory.c/h
|   |   |   +-- bootloader.c/h
|   |   |   +-- power.c/h
|   |   |   +-- ipc.c/h
|   |   |   +-- watchdog.c/h
|   |   |   +-- panic.c/h
|   |   |   +-- debug.c/h
|   |   |   +-- log.c/h
|   |   +-- app/                  # Application Layer
|   |       +-- ftl.c/h
|   |       +-- gc.c/h
|   |       +-- wear_level.c/h
|   |       +-- bad_block.c/h
|   |       +-- ecc.c/h
|   |       +-- qos.c/h
|   |       +-- redundancy.c/h
|   |       +-- error_handler.c/h
|   +-- media/                    # Media threads
|   |   +-- nand_media.c/h        # NAND media emulation
|   |   +-- nor_media.c/h         # NOR media emulation
|   |   +-- timing_model.c/h      # Timing model
|   |   +-- persistence.c/h       # Data persistence
|   +-- oob/                      # OOB management
|       +-- oob_server.c/h        # Socket service
|       +-- rest_api.c/h          # REST API (optional)
|       +-- proc_interface.c/h    # /proc interface
+-- tools/                        # Tools
|   +-- hfsss-ctrl/               # Python CLI tool
|   |   +-- hfsss_ctrl.py
|   |   +-- commands/
|   |   +-- requirements.txt
|   +-- scripts/                  # Utility scripts
|       +-- build.sh
|       +-- install.sh
|       +-- setup_grub.sh
|       +-- run_fio_tests.sh
+-- tests/                        # Tests
|   +-- unit/                     # Unit tests
|   |   +-- test_ftl.cpp
|   |   +-- test_gc.cpp
|   |   +-- test_timing.cpp
|   |   +-- CMakeLists.txt
|   +-- integration/              # Integration tests
|   |   +-- test_nvme_cli.sh
|   |   +-- test_fio.sh
|   |   +-- test_persistence.sh
|   +-- fault_injection/          # Fault injection tests
|       +-- test_bad_block.py
|       +-- test_power_loss.py
+-- config/                       # Configuration
|   +-- hfsss.yaml.example
|   +-- hfsss_64core.yaml
|   +-- hfsss_128core.yaml
|   +-- hfsss_256core.yaml
+-- third_party/                  # Third-party dependencies
|   +-- jansson/                  # JSON parsing
|   +-- libyaml/                  # YAML parsing
+-- README.md
```

---

## C.5 Next Steps Recommendations

### C.5.1 Immediate Next Steps (1-2 weeks)

1. **Generate Requirements Matrix**:
   - Decompose each FR requirement into testable sub-requirements
   - Identify requirement priorities (P0/P1/P2)
   - Associate requirements with test cases

2. **Write High-Level Design Document (HLD)**:
   - Inter-module interface definitions
   - Core data structure design
   - Key algorithm flow design
   - Thread model and synchronization mechanism design

3. **Set Up Development Environment**:
   - Prepare Debian 12 VM/server
   - Install compilation toolchain
   - Install kernel headers
   - Initialize code repository

### C.5.2 Medium-term Work (3-8 weeks)

1. **Write Low-Level Design Documents (LLD)**:
   - Detailed design for each module
   - API interface definitions
   - Error handling design

2. **V1.0 Implementation**:
   - Kernel module PCIe/NVMe basic emulation
   - User-space daemon basic framework
   - Basic FTL implementation

### C.5.3 Long-term Work (2-6 months)

1. V1.5 development
2. V2.0 development
3. Performance optimization
4. Documentation refinement

---

## C.6 Risks and Considerations

### C.6.1 Technical Risks

| Risk | Impact | Probability | Mitigation |
|------|--------|------------|------------|
| Kernel module stability | High | Medium | Thorough testing, reference mature NVMeVirt implementation |
| Performance not meeting targets | High | Medium | Early performance validation, NUMA optimization, lock-free design |
| Timing model accuracy insufficient | Medium | Low | Reference MQSim/FEMU, configurable parameters |
| Multi-threaded concurrency bugs | High | Medium | Thread Sanitizer testing, clear lock strategy |

### C.6.2 Development Considerations

1. **Kernel Module Development**:
   - Follow Linux kernel coding conventions
   - Avoid using user-space libraries
   - Be careful with memory allocation (GFP_ATOMIC/GFP_KERNEL)
   - Thorough testing -- kernel panics cause system reboots

2. **User-space Development**:
   - No malloc/free on critical paths
   - CPU binding using pthread_setaffinity_np
   - SCHED_FIFO real-time priority
   - Avoid False Sharing (Cache Line alignment)

3. **Performance Optimization**:
   - Implement correctly first, then optimize performance
   - Use perf tools to locate bottlenecks
   - Consider NUMA node affinity
   - Huge page memory (HugeTLB) to reduce TLB misses

---

*End of Appendix C*

---

# Appendix D: Enterprise SSD Standards Reference

## D.1 JEDEC JESD218B -- Endurance and Reliability

JEDEC JESD218B defines the standard methodology for measuring and specifying SSD endurance. Key aspects relevant to HFSSS:

- **TBW (Terabytes Written)**: The total amount of data that can be written to the SSD over its lifetime before the drive is expected to exceed its endurance limit. HFSSS tracks cumulative host writes and NAND writes, reporting remaining endurance via the SMART Percentage Used field.
- **DWPD (Drive Writes Per Day)**: Derived from TBW, representing how many times the full drive capacity can be written per day over the warranty period. For a 2 TB enterprise SSD with 5-year warranty: `DWPD = TBW / (capacity_TB x 365 x warranty_years)`.
- **UBER (Uncorrectable Bit Error Rate)**: The maximum acceptable rate of uncorrectable bit errors. Enterprise SSDs typically target UBER < 10^-16. HFSSS's reliability model (P/E degradation + LDPC ECC) can be validated against this target.
- **Functional Failure Requirements**: JEDEC defines conditions under which an SSD is considered to have functionally failed (e.g., inability to complete IO within timeout, data loss, unresponsive controller). HFSSS's fault injection framework enables testing these boundary conditions.

The HFSSS SMART implementation reports JEDEC-aligned metrics including Percentage Used, Available Spare, and Endurance Group information.

## D.2 OCP Cloud SSD Specification

The Open Compute Project (OCP) Cloud SSD Specification defines additional requirements for cloud-scale SSD deployments, beyond the base NVMe specification. Key aspects modeled in HFSSS:

- **Latency Monitoring**: OCP specifies mandatory latency histogram reporting via vendor-specific SMART log pages. HFSSS implements this through its vendor-specific log page 0xC0, which provides per-IO-type latency histograms with bucket granularities aligned to OCP recommendations.
- **SMART Extensions**: OCP defines additional SMART attributes beyond the standard NVMe set, including: Physical Media Units Written (reflecting actual NAND writes, not just host writes), Soft ECC Error Count, Write Amplification Factor, and Thermal Throttle Status. HFSSS reports all of these through vendor-specific log pages.
- **Power Management**: OCP specifies stricter power consumption limits and mandatory support for NVMe power states PS0-PS4. HFSSS's power management emulation (Section 5.5.4) is aligned with OCP power state definitions.
- **Error Reporting**: OCP requires detailed error classification and reporting. HFSSS's error handling framework (Section 5.7.8) and error log implementation provide OCP-compatible error reporting granularity.
- **Firmware Update**: OCP specifies requirements for firmware download and activation, including support for dual firmware slots and activation without reset. HFSSS's firmware management (bootloader dual-slot, firmware commit command) is compatible with OCP requirements.

## D.3 NVMe 2.0 Enterprise Feature Set

The NVMe 2.0 specification includes several enterprise-oriented features that are modeled in HFSSS:

- **Namespace Management**: NVMe 2.0 defines a comprehensive namespace management command set including create, delete, attach, detach, and format operations. HFSSS implements this full command set (Section 12.5), supporting up to 32 namespaces with per-namespace FTL isolation.
- **End-to-End Data Protection (PI)**: NVMe 2.0 specifies Protection Information (PI) Types 1, 2, and 3 with CRC-16 Guard Tag, Reference Tag, and Application Tag. HFSSS implements all three PI types (Section 12.3), with PI propagation through the entire FTL data path.
- **Predictable Latency Mode**: NVMe 2.0 introduces the concept of Deterministic Windows and Non-Deterministic Windows for latency management. HFSSS's QoS Determinism implementation (Section 12.2) provides equivalent functionality through its GC pacing and latency SLA enforcement mechanisms.
- **Asynchronous Event Notifications**: NVMe 2.0 expands the set of asynchronous events that the controller can report to the host. HFSSS supports the full enterprise-relevant AEN set (Section 12.7.6), including vendor-specific events for QoS violations and thermal throttling.
- **Telemetry**: NVMe 2.0 defines host-initiated and controller-initiated telemetry log pages for diagnostic data collection. HFSSS implements both telemetry log page types (Sections 12.7.2 and 12.7.3) with configurable data areas.
- **Sanitize**: NVMe 2.0 defines the Sanitize command for secure data removal. HFSSS supports both Crypto Erase (Section 12.4.5) and Block Erase (Section 12.4.6) as sanitize operations.

---

*End of Appendix D*

---

*End of Document*

**Content Summary**: This document covers Chapters 1 through 12 plus Appendices A through D, containing complete functional requirements (FR-PCIE, FR-NVMe, FR-CTRL, FR-MEDIA, FR-HAL, FR-CS, FR-APP, FR-ENT totaling 36+ requirement items), performance requirements, interface definitions, fault injection framework, stability requirements, development constraints, test strategy, and enterprise SSD features (UPLP, QoS Determinism, T10 DIF/PI, Data-at-Rest Encryption, Multi-Namespace Management, Thermal Management, Advanced Telemetry), along with PRD review summary, technical implementation recommendations, and enterprise standards references.
