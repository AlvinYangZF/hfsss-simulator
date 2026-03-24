# HFSSS Low-Level Design Document Overview

**Document Version**: V2.0
**Date**: 2026-03-23

---

## Table of Contents

1. [LLD Document Overview](#1-lld-document-overview)
2. [Module Relationship Diagram](#2-module-relationship-diagram)
3. [LLD Document Descriptions](#3-lld-document-descriptions)
4. [Usage Guide](#4-usage-guide)
5. [Future Work](#5-future-work)

---

## 1. LLD Document Overview

This series of Low-Level Design (LLD) documents is based on the PRD and HLD and provides detailed designs ready for direct implementation, including:

- Complete data structure definitions
- Complete header file designs
- Detailed function interface specifications
- Module internal logic
- Flow diagrams
- Debug mechanisms
- Test cases

---

## 2. Module Relationship Diagram

```
+-------------------------------------------------------------+
|                     Host Linux Kernel                        |
|  +--------------+  +--------------+  +------------------+   |
|  |  NVMe Driver |  |  File System |  |  fio / nvme-cli  |   |
|  +------+-------+  +------+-------+  +---------+--------+   |
+---------+---------------------+--------------------+---------+
          |                     |                    |
+---------v---------------------v--------------------v---------+
|         LLD_01: PCIe/NVMe Device Emulation Module            |
|  (Kernel module - interacts with kernel NVMe driver)         |
+-----------------------------+--------------------------------+
                              | Shared Memory / Ring Buffer
+-----------------------------v--------------------------------+
|         LLD_02: Controller Thread Module                     |
|  (Arbitration/Scheduling/Write Buffer/Read Cache/            |
|   Load Balancing/Flow Control)                               |
+----------+----------------------------+----------------------+
           |                            |
+----------v----------+   +------------v--------------+
| LLD_06: Application |   | LLD_05: Common Services   |
| (FTL/GC/WL/ECC)     |   | (RTOS/Memory/Log/Debug)   |
+----------+----------+   +---------------------------+
           |
+----------v----------+
|  LLD_04: HAL Layer   |
|  (NAND/NOR Drivers)  |
+----------+----------+
           |
+----------v----------+
| LLD_03: Media Threads|
| (NAND/NOR Emulation) |
+----------+-----------+
```

---

## 3. LLD Document Descriptions

### LLD_01_PCIE_NVMe_EMULATION_EN.md
- **Content**:
  - PCIe configuration space emulation
  - NVMe controller register emulation
  - Queue management (SQ/CQ)
  - MSI-X interrupts
  - DMA engine
  - Kernel-user space communication
- **Header files**: pci.h, nvme.h, queue.h, msix.h, dma.h, shmem.h
- **Functions**: 80+

### LLD_02_CONTROLLER_THREAD_EN.md
- **Content**:
  - Shared memory Ring Buffer
  - Command arbiter
  - I/O scheduler (FIFO/Greedy/Deadline)
  - Write Buffer management
  - Read cache (LRU)
  - Channel load balancing
  - Resource manager
  - Flow control (token bucket)
- **Header files**: controller.h, shmem_if.h, scheduler.h, write_buffer.h, read_cache.h, resource.h, flow_control.h
- **Functions**: 60+

### LLD_03_MEDIA_THREADS_EN.md
- **Content**:
  - NAND hierarchy (Channel -> Chip -> Die -> Plane -> Block -> Page)
  - Timing model (tR/tPROG/tERS, TLC LSB/CSB/MSB)
  - EAT calculation engine
  - Concurrency control (Multi-Plane/Die Interleaving/Chip Enable)
  - Reliability model
  - Bad block management (BBT)
- **Header files**: nand.h, media.h, timing.h, reliability.h, bbt.h
- **Functions**: 40+

### LLD_04_HAL_EN.md
- **Content**:
  - NAND driver API
  - NOR driver API
  - Power management
  - Command issue queue
- **Header files**: hal_nand.h, hal_nor.h, hal_pci.h, hal_power.h
- **Functions**: 30+

### LLD_05_COMMON_SERVICE_EN.md
- **Content**:
  - RTOS primitives (Task/Message Queue/Semaphore/Mutex/Event Group/Timer/Memory Pool)
  - Task scheduler
  - Memory management
  - Bootloader
  - Power-on/off service
  - Out-of-band management
  - Inter-core communication
  - Watchdog
  - Debug/Log
- **Header files**: rtos.h, scheduler.h, memory.h, boot.h, power_mgmt.h, oob.h, ipc.h, watchdog.h, debug.h, log.h
- **Functions**: 70+

### LLD_06_APPLICATION_EN.md
- **Content**:
  - FTL address mapping (L2P/P2L)
  - PPN encoding/decoding
  - Block state machine
  - Current Write Block
  - Free block pool
  - GC (Greedy/Cost-Benefit/FIFO)
  - Wear leveling
  - Read Retry
  - ECC (LDPC)
  - Error handling
- **Header files**: ftl.h, mapping.h, block.h, gc.h, wear_level.h, ecc.h, error.h
- **Functions**: 60+

### LLD_07_OOB_MANAGEMENT_EN.md
- **Content**:
  - Unix Domain Socket JSON-RPC 2.0 server
  - NVMe SMART/Health Log Page (0x02) emulation
  - Performance counters and latency histograms
  - Temperature model and anomaly alerting
  - Command trace ring buffer
  - /proc/hfsss/ filesystem interface
  - hfsss-ctrl CLI tool
  - YAML configuration file loading
- **Header files**: `include/common/oob.h`
- **Functions**: 25+
- **Covered requirements**: REQ-077 through REQ-079, REQ-083, REQ-086, REQ-123 through REQ-126

### LLD_08_FAULT_INJECTION_EN.md
- **Content**:
  - NAND media fault injection (bad block/read error/write error/erase error/bit flip/read disturb storm/data retention acceleration)
  - Power fault injection (idle/write-in-progress/GC-in-progress/checkpoint-in-progress power loss)
  - Controller fault injection (Panic/memory pool exhaustion/command timeout storm/L2P corruption)
  - Fault registry (binary search O(log N) hot-path check)
  - OOB fault.inject interface integration
  - WAL recovery and crash marker design
- **Header files**: `include/common/fault_inject.h`
- **Functions**: 18+
- **Test cases**: 28 (FI-001 through FI-028)
- **Covered requirements**: REQ-128 through REQ-131

### LLD_09_BOOTLOADER_EN.md
- **Content**:
  - Six-stage boot sequence emulation (Phase 0-5, total 3-8 seconds)
  - NOR Flash dual firmware slot (Slot A/B) hot standby and atomic switchover
  - SysInfo partition design (boot type detection/clean-shutdown marker)
  - Normal power-off service (seven-step orderly shutdown)
  - Abnormal power-off service (signal handling + WAL best-effort persistence)
  - Power-on recovery paths (first boot/normal recovery/abnormal recovery/degraded mode)
- **Header files**: `include/common/boot.h`, `include/common/power_mgmt.h`
- **Functions**: 15+
- **Test cases**: 25 (BL-001 through BL-025)
- **Covered requirements**: REQ-073 through REQ-076

### LLD_10_PERFORMANCE_VALIDATION_EN.md
- **Content**:
  - Built-in benchmark engine (sequential/random/mixed/Zipfian workloads)
  - NAND timing accuracy verification (tR/tPROG/tERS, error <5%)
  - Scalability testing (Amdahl parallel efficiency evaluation)
  - Data integrity verification (md5sum end-to-end checksum)
  - Long-term stability framework (72-hour stress test, TSan/ASan)
  - Validation report generation (JSON + human-readable text)
- **Header files**: `include/common/perf_validator.h`
- **Functions**: 15+
- **Test cases**: 40 (PV-001 through PV-040)
- **Covered requirements**: REQ-112 through REQ-119, REQ-122, REQ-131 through REQ-134

### LLD_11_FTL_RELIABILITY_EN.md
- **Content**:
  - Command state machine (8 states, FTL_CMD_RECEIVED through FTL_CMD_COMPLETE)
  - Read Retry mechanism (up to 15 voltage offset adjustments, soft-decision LDPC)
  - Write Retry/Write Verify (write-after-read verification, ECC check)
  - Multi-level IO flow control (host/FTL/NAND three-tier token bucket)
  - RAID-Like data protection (L2P dual copy, BBT dual mirror, die-level XOR parity)
  - NVMe error status code mapping and Error Log Page
  - T10 DIF/PI integration (PI Type 1/2/3, CRC-16 guard tag, reference/application tag)
- **Header files**: `include/ftl/ftl_reliability.h`
- **Functions**: 35+
- **Test cases**: 24 (FR-001 through FR-024)
- **Covered requirements**: REQ-110 through REQ-115, REQ-154 through REQ-158

### LLD_12_REALTIME_SERVICES_EN.md
- **Content**:
  - CPU affinity binding (pthread_setaffinity_np, macOS graceful degradation)
  - Real-time scheduling priority (SCHED_FIFO/SCHED_RR, macOS graceful degradation)
  - SPSC Ring Buffer IPC (cache-line aligned _Atomic uint64_t, capacity 4096, eventfd wakeup)
  - System resource monitoring (CPU usage, memory pressure, thread state periodic sampling)
  - Performance anomaly detection and thermal emulation (P99.9 latency alert, thermal throttle >=75C)
- **Header files**: `include/common/rt_services.h`
- **Functions**: 30+
- **Test cases**: 25 (RT-001 through RT-025)
- **Covered requirements**: REQ-074, REQ-075, REQ-085, REQ-087, REQ-088

### LLD_13_HAL_ADVANCED_EN.md
- **Content**:
  - NVMe Asynchronous Event Request (AER) management (16 pending queue, epoll-driven delivery)
  - PCIe link state machine (L0/L0s/L1/L2/RESET/FLR, 6 states)
  - PCIe configuration space full emulation (256-byte standard + 4KB extended)
- **Header files**: `include/hal/hal_advanced.h`
- **Functions**: 20+
- **Test cases**: 24 (HA-001 through HA-024)
- **Covered requirements**: REQ-063, REQ-064, REQ-069

### LLD_14_NOR_FLASH_EN.md
- **Content**:
  - NOR Flash specification (256MB, 512B page, 64KB sector, 100K PE cycle endurance)
  - 7-partition layout (Bootloader/SlotA/SlotB/Config/BBT/EventLog/SysInfo)
  - AND-semantics programming (image[i] &= buf[i], can only clear bits to zero)
  - Full operation command set (read/program/sector-erase/chip-erase/status-register read-write)
  - mmap persistence (MAP_SHARED file mapping, power-fail safe)
- **Header files**: `include/media/nor_flash.h`, `include/hal/hal_nor_full.h`
- **Functions**: 20+
- **Test cases**: 22 (NF-001 through NF-022)
- **Covered requirements**: REQ-053 through REQ-056

### LLD_15_PERSISTENCE_FORMAT_EN.md
- **Content**:
  - NAND data file format (80-byte header + data area, magic "HFSSS_ND", CRC32 checksum)
  - OOB area format (384 bytes per page: LPN/timestamp/ECC syndrome/OOB_CRC32)
  - L2P checkpoint format (64-byte header + uint64_t[] array, CRC64 integrity)
  - WAL record format (64-byte fixed-length, 0xDEADBEEF terminator, 8 record types)
  - static_assert size checks, `persistence_fmt.h` centralized format constants
- **Header files**: `include/common/persistence_fmt.h`
- **Functions**: 20+
- **Test cases**: 22 (PF-001 through PF-022)
- **Covered requirements**: REQ-131 (supplements LLD_07 persistence format details)

### LLD_16_KERNEL_MODULE_EN.md
- **Content**:
  - Linux kernel module for PCIe/NVMe device emulation
  - Virtual PCIe device registration
  - NVMe controller register MMIO handling
  - Doorbell register processing
  - Shared memory interface with user-space
- **Header files**: kernel module headers
- **Functions**: 15+
- **Covered requirements**: REQ-001 through REQ-022 (kernel-level implementation)

### LLD_17_POWER_LOSS_PROTECTION_EN.md (Enterprise)
- **Content**:
  - Supercapacitor energy model (1-10F capacitance, RC discharge curve)
  - UPLP state machine (Normal -> PowerFail -> CapDrain -> SafeState)
  - Atomic write unit (4KB power-safe granularity)
  - Power-fail-safe metadata journal
  - Write buffer emergency flush (priority order: metadata > dirty data > clean)
  - UPLP recovery sequence (<5s for 1TB)
  - UPLP test mode (injectable power-fail events)
- **Header files**: `include/common/uplp.h`
- **Functions**: 25+
- **Covered requirements**: REQ-139 through REQ-146

### LLD_18_QOS_DETERMINISM_EN.md (Enterprise)
- **Content**:
  - DWRR multi-queue scheduler (deficit-weighted round robin)
  - Per-namespace IOPS limits (1K-2M range)
  - Per-namespace bandwidth limits (50MB/s-14GB/s range)
  - Latency SLA enforcement (P99/P99.9 targets)
  - QoS policy hot-reconfiguration
  - GC/WL background priority yield
  - Deterministic latency window (duty cycle scheduling)
- **Header files**: `include/controller/qos.h`
- **Functions**: 30+
- **Covered requirements**: REQ-147 through REQ-153

### LLD_19_SECURITY_ENCRYPTION_EN.md (Enterprise)
- **Content**:
  - AES-XTS 256-bit simulation (XOR-based placeholder + optional real AES)
  - Key hierarchy management (MK -> KEK -> DEK, per-namespace isolation)
  - TCG Opal SSC basic commands (Discovery0, StartSession, lock/unlock)
  - Crypto erase (DEK destruction)
  - Secure erase (block-level user data erase)
  - Secure boot chain verification (ROM -> Bootloader -> Firmware)
  - Key storage in NOR Flash (dual-copy, UPLP-safe)
- **Header files**: `include/common/security.h`
- **Functions**: 30+
- **Covered requirements**: REQ-159 through REQ-165

---

## 4. Usage Guide

### Recommended Implementation Order

1. LLD_05: Common Services (foundation)
2. LLD_12: Real-Time Services (CPU affinity/IPC/resource monitoring, depends on LLD_05)
3. LLD_09: Bootloader and Power-On/Off Services (depends on LLD_05)
4. LLD_14: NOR Flash Complete Implementation (depends on LLD_03/LLD_04 basics)
5. LLD_03: Media Threads
6. LLD_04: HAL Layer
7. LLD_13: HAL Advanced Features (AER/PCIe link, depends on LLD_04)
8. LLD_15: Persistence Data Formats (depends on LLD_03/LLD_06)
9. LLD_06: Application Layer (FTL)
10. LLD_11: FTL Reliability (Read/Write Retry/RAID protection, depends on LLD_06)
11. LLD_02: Controller Thread
12. LLD_01: PCIe/NVMe Device Emulation
13. LLD_16: Kernel Module (depends on LLD_01)
14. LLD_07: Out-of-Band Management (depends on LLD_01 through LLD_06)
15. LLD_08: Fault Injection Framework (depends on LLD_03/LLD_06/LLD_07)
16. LLD_17: UPLP (depends on LLD_04/LLD_05/LLD_15)
17. LLD_18: QoS Determinism (depends on LLD_02/LLD_06)
18. LLD_19: Security and Encryption (depends on LLD_04/LLD_05/LLD_14)
19. LLD_10: Performance Validation (last to integrate, depends on full stack)

---

## 5. Future Work

- Implementation (following the recommended order above)
- Unit testing (independent testing per module)
- Integration testing (end-to-end flow verification)
- Performance testing (LLD_10 validation framework)
- Fault injection testing (LLD_08 framework)
- Enterprise feature testing (LLD_17/LLD_18/LLD_19)

---

**Document Statistics**:
- Total LLD documents: 19 + 1 overview
- Header files: 50+
- Function interfaces: 600+
- Test cases (design phase): 250+
