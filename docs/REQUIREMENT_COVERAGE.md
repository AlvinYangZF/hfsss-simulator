# HFSSS Requirement Coverage Analysis

**Document Version**: V3.0
**Date**: 2026-04-19

---

## Overview

This document analyzes the coverage of the 178 requirements from the Requirements Matrix against the current HFSSS implementation. Requirements REQ-001 through REQ-138 cover core functionality; REQ-139 through REQ-178 cover enterprise SSD features added in PRD V2.0 (Chapter 12).

**V3.0 Update (2026-04-19)**: End-to-end code audit against current source tree. Phase 4 (boot/NOR/FTL reliability/trace) and most of Phase 5/6 (OOB/hfsss-ctrl/YAML/perf framework/fault-inject) plus the Enterprise V3.0 groups (UPLP, Multi-NS, Security, Thermal/Telemetry) are now landed. Coverage rises from 38.8% to **65.2%** fully implemented (86.5% counting partials). Post-review corrections: REQ-132 (NAND fault wiring pending), REQ-161 (TCG Opal command parsing pending), REQ-164 (`secure_boot_verify` not invoked from boot flow), REQ-165 (key table persists to arbitrary file, not NOR), REQ-175/176 (telemetry ring exists but NVMe Log Page 07h/08h dispatch not wired) all demoted from ✅ to ⚠️. REQ-163 (sanitize action modes) had been flagged as partial in this note but was a test-expectation bug, not an implementation gap — the underlying `nvme_uspace_sanitize` already dispatches all four NVMe §5.22 SANACT codes. `systest_nvme_compliance::NC-013` and `systest_data_integrity::DI-007` now assert the simulator-modeled post-state for each code (EXIT_FAILURE: no-op; BLOCK_ERASE / CRYPTO_ERASE: L2P dropped → NOENT; OVERWRITE: zero-fill pattern). NVMe §5.22 leaves the exact deallocated / overwritten / crypto-erased content loose; the tests pin the simulator's concrete observables that satisfy the spec's "old data unrecoverable" / "host-pattern written" guarantees.

**V2.0 Update**: Added 40 enterprise requirements (REQ-139 through REQ-178) covering UPLP, QoS Determinism, T10 DIF/PI, Security, Multi-Namespace Management, Thermal Management, and Advanced Telemetry.

**V1.2 Update**: Reflects Phase 1 (FTL/Media), Phase 2 (Controller/HAL), Phase 3 (User-space NVMe) completions.

### Status Definitions
- ✅ **Implemented**: Requirement is fully implemented with at least one passing test
- ⚠️ **Partial**: Requirement is partially implemented; feature set or test coverage not complete
- ❌ **Not Implemented**: Requirement is not implemented
- 🔧 **Stub**: Only placeholder/stub implementation exists

---

## Summary by Module

| Module | Total | ✅ | ⚠️ | ❌ | 🔧 | Coverage % | Change |
|--------|------:|---:|---:|---:|---:|-----------:|--------|
| PCIe/NVMe Device Emulation | 22 | 13 | 1 | 8 | 0 | 59.1% | ↑ REQ-002 PCIe cap linked-list walk flipped after REQ-069 config space landed |
| Controller Thread | 15 | 12 | 1 | 2 | 0 | 80.0% | -- |
| Media Threads | 20 | 17 | 3 | 0 | 0 | 85.0% | ↑ REQ-042 multi-plane concurrency + REQ-044 per-channel worker runtime; REQ-045 scaffolding landed (Partial — full lock-free CQ outstanding) |
| HAL | 12 | 12 | 0 | 0 | 0 | 100% | ↑ REQ-063 AER + REQ-064 PCIe link state + REQ-069 byte-level config space (LLD_13) |
| Common Services | 24 | 20 | 2 | 2 | 0 | 83.3% | ↑ REQ-085 SPSC ring + REQ-087 system resource monitor on top of boot/power/OOB/SMART/trace |
| Algorithm Task Layer (FTL) | 22 | 19 | 1 | 2 | 0 | 86.4% | ↑ +6 (cmd state machine, retries, flow ctl, wear monitor, Error Log Page) |
| Performance Requirements | 8 | 7 | 0 | 1 | 0 | 87.5% (87.5% partial) | ↑ REQ-116..120 + REQ-122 Amdahl scalability + REQ-123 bursty-load CPU probe gated by regression suite |
| Product Interfaces | 8 | 5 | 2 | 1 | 0 | 62.5% (87.5% partial) | ↑ REQ-131 LLD_15 reconciled with shipping code (V1.1); REQ-125/126 retain ⚠️ pending LLD_16 host-path evidence |
| Fault Injection | 3 | 3 | 0 | 0 | 0 | 100% | ↑ REQ-134 controller fault hooks (pool exhaustion + panic + timeout storm) wired into resource_mgr + arbiter; verified by `tests/test_fault_inject.c::test_controller_*` |
| System Reliability | 4 | 3 | 0 | 1 | 0 | 75.0% | ↑ REQ-088 P99.9 latency anomaly detector |
| **Core Subtotal** | **138** | **111** | **10** | **17** | **0** | **80.4%** (87.7% partial) | ↑ from 50.0% |
| Enterprise: UPLP | 8 | 8 | 0 | 0 | 0 | 100% | ↑ implemented |
| Enterprise: QoS Determinism | 7 | 6 | 1 | 0 | 0 | 85.7% (100% partial) | ↑ DWRR + per-NS IOPS/BW caps + SLA rollback + hot-reconfig |
| Enterprise: T10 DIF/PI | 5 | 5 | 0 | 0 | 0 | 100% | ↑ Type 1/2/3 CRC-16 + GC-path PI propagation |
| Enterprise: Security | 7 | 7 | 0 | 0 | 0 | 100% | ↑ AES-XTS sim, keys, crypto erase, sanitize action modes, secure-boot-verify, NOR-backed dual-copy UPLP-safe key table, TCG-Opal lock/unlock |
| Enterprise: Multi-Namespace | 5 | 5 | 0 | 0 | 0 | 100% | ↑ implemented |
| Enterprise: Thermal/Telemetry | 8 | 8 | 0 | 0 | 0 | 100% | ↑ throttle + SMART predict + NVMe Log Page 07h/08h/0xC0 dispatch + AER notifier helpers + REQ-178 runtime producer (smart_monitor) |
| **Enterprise Subtotal** | **40** | **39** | **1** | **0** | **0** | **97.5%** (100% partial) | ↑ from 0% |
| **Grand Total** | **178** | **150** | **11** | **17** | **0** | **84.3%** (90.4% partial) | ↑ from 38.8% |

> **Note**: Figures above count individual requirement rows. Related roadmap group-level coverage tracks the same reality from a different angle. All changes since V2.0 have been verified against current source code; see notes column on each row for file-level evidence.

---

## Detailed Requirement Coverage

### 1. PCIe/NVMe Device Emulation Module (REQ-001 to REQ-022)

| ID | Requirement Description | Status | Notes |
|----|------------------------|--------|-------|
| REQ-001 | PCIe Config Space Emulation - Standard PCI Type 0 Config Header | ✅ | Basic config header structures in `pci.h` |
| REQ-002 | PCIe Config Space Emulation - PCIe Capabilities Linked List | ✅ | Four standard caps (PM 0x01 @ 0x40, MSI 0x05 @ 0x50, MSI-X 0x11 @ 0x70, PCIe Express 0x10 @ 0x90) seeded in `hal_pci_cfg_init`. `hal_pci_capability_find` walks the chain via `cfg_read8` on each entry's `{cap_id, next}` header with a 96-hop safety bound against corrupted pointers. Covered by `tests/test_hal.c::test_hal_pci_cfg_cap_chain_layout` / `test_hal_pci_cfg_capability_find` (15 assertions, incl. cycle-detection). |
| REQ-003 | PCIe Config Space Emulation - BAR Register Configuration | ✅ | BAR constants and structures defined |
| REQ-004 | NVMe Controller Register Emulation - CAP Register Configuration | ✅ | NVMe controller registers in `nvme.h` |
| REQ-005 | NVMe Controller Register Emulation - VS Register Configuration | ✅ | VS register (NVMe 2.0) defined |
| REQ-006 | NVMe Controller Register Emulation - Controller Initialization | ❌ | No real kernel initialization |
| REQ-007 | NVMe Controller Register Emulation - Doorbell Registers | ✅ | Doorbell processing implemented (Phase 3) |
| REQ-008 | NVMe Queue Management - Admin Queue | ✅ | Admin Queue implemented (Phase 3) |
| REQ-009 | NVMe Queue Management - I/O Queue Dynamic Creation | ❌ | No dynamic queue creation |
| REQ-010 | NVMe Queue Management - PRP/SGL Support | ⚠️ | DMA structures defined but not fully implemented |
| REQ-011 | NVMe Queue Management - Completion Processing | ✅ | CQ processing implemented (Phase 3) |
| REQ-012 | MSI-X Interrupt Emulation - MSI-X Table | ✅ | MSI-X table structures defined |
| REQ-013 | MSI-X Interrupt Emulation - Interrupt Delivery | ❌ | No real interrupt delivery (user-space limitation) |
| REQ-014 | MSI-X Interrupt Emulation - Interrupt Coalescing | ❌ | Not implemented |
| REQ-015 | NVMe Admin Command Set | ✅ | Admin command processing implemented (Phase 3) |
| REQ-016 | NVMe I/O Command Set | ✅ | I/O command processing implemented (Phase 3) |
| REQ-017 | NVMe I/O Command Set - Read/Write Detailed Parameters | ✅ | Implemented (Phase 3) |
| REQ-018 | NVMe I/O Command Set - Dataset Management (Trim) | ✅ | `NVME_NVM_DATASET_MANAGEMENT` handler in `nvme_uspace.c` routes to FTL trim; `tests/test_dsm.c` |
| REQ-019 | NVMe DMA Data Transfer - PRP Parsing Engine | ❌ | Not implemented |
| REQ-020 | NVMe DMA Data Transfer - Data Copy Path | ❌ | No kernel-level DMA |
| REQ-021 | NVMe DMA Data Transfer - IOMMU Support | ❌ | Not implemented |
| REQ-022 | Kernel-User Space Communication | ❌ | No kernel module, so no this layer |

### 2. Controller Thread Module (REQ-023 to REQ-037)

| ID | Requirement Description | Status | Notes |
|----|------------------------|--------|-------|
| REQ-023 | Command Receive and Dispatch - Kernel-User Space Communication | ❌ | No kernel module |
| REQ-024 | Command Receive and Dispatch - Command Arbitration Policy | ✅ | Arbiter implemented in `arbiter.h/c` |
| REQ-025 | Command Receive and Dispatch - Command Dispatch | ⚠️ | Basic structures, but no full state machine |
| REQ-026 | Command Receive and Dispatch - Command Timeout Management | ✅ | Command timeout management implemented (Phase 2) |
| REQ-027 | I/O Scheduler - Scheduling Algorithm | ✅ | Scheduler implemented in `scheduler.h/c` |
| REQ-028 | I/O Scheduler - Write Buffer Management | ✅ | Write Buffer implemented in `write_buffer.h/c` |
| REQ-029 | I/O Scheduler - Read Cache | ✅ | Read Cache implemented in `read_cache.h/c` |
| REQ-030 | I/O Scheduler - Channel Load Balancing | ✅ | Channel manager implemented in `channel.h/c` |
| REQ-031 | Resource Manager - Free Block Management | ✅ | Idle block pool management implemented (Phase 2) |
| REQ-032 | Resource Manager - Command Slot Management | ❌ | Not implemented |
| REQ-033 | Flow Control - Token Bucket Rate Limiter | ✅ | Flow control implemented in `flow_control.h/c` |
| REQ-034 | Flow Control - Back-Pressure Mechanism | ✅ | Full backpressure mechanism implemented (Phase 2) |
| REQ-035 | Flow Control - QoS Guarantee | ✅ | QoS guarantees implemented (Phase 2) |
| REQ-036 | Flow Control - GC Traffic Control | ✅ | GC traffic control implemented (Phase 2) |
| REQ-037 | Controller Thread Module - Main Controller | ✅ | Controller implemented in `controller.h/c` |

### 3. Media Threads Module (REQ-038 to REQ-057)

| ID | Requirement Description | Status | Notes |
|----|------------------------|--------|-------|
| REQ-038 | NAND Flash Hierarchy - Hierarchy Definition | ✅ | Complete hierarchy in `nand.h/c` |
| REQ-039 | NAND Flash Hierarchy - Total Capacity Calculation | ✅ | Capacity calculation implemented |
| REQ-040 | NAND Media Timing Model - Basic Timing Parameters | ✅ | Timing model in `timing.h/c` |
| REQ-041 | NAND Media Timing Model - Timing Model Implementation | ✅ | EAT engine in `eat.h/c` |
| REQ-042 | NAND Media Timing Model - Multi-Plane Concurrency | ✅ | Per-plane EAT iteration in `cmd_engine.c` runs N planes in parallel; `tests/test_media_multi_plane_concurrency.c` proves 2/4-plane ops land within 1.5x single-plane wall-clock and per-plane EAT independence |
| REQ-043 | NAND Media Command Execution Engine - NAND Command Support | ⚠️ | Basic read/program/erase, not full ONFI command set |
| REQ-044 | NAND Media Command Execution Engine - Command Queue Design | ✅ | `channel_worker` runtime (`include/controller/channel_worker.h`, `src/controller/channel_worker.c`) provides one pthread per channel with an SPSC request ring; `tests/test_channel_worker.c` exercises single/multi-cmd submit, back-pressure and isolation across channels |
| REQ-045 | NAND Media Command Execution Engine - Completion Notification | ⚠️ | Async scaffolding landed via `channel_worker`: callback + atomic `done` flag + `channel_cmd_wait` poll barrier. Lock-free completion queue with per-command timestamp / actual latency / batch drain (the PRD wording) is a follow-up |
| REQ-046 | NAND Reliability Modeling - P/E Cycle Degradation Model | ✅ | Reliability model in `reliability.h/c` |
| REQ-047 | NAND Reliability Modeling - Read Disturb Model | ✅ | Read disturb implemented |
| REQ-048 | NAND Reliability Modeling - Data Retention Model | ⚠️ | Basic model, no time acceleration |
| REQ-049 | NAND Reliability Modeling - Bad Block Management | ✅ | BBT in `bbt.h/c` |
| REQ-050 | NAND Data Storage - DRAM Storage Layout | ✅ | DRAM storage in `nand.c` |
| REQ-051 | NAND Data Storage - Persistence Strategy | ✅ | Incremental checkpointing implemented (Phase 4 partial) |
| REQ-052 | NAND Data Storage - Recovery Mechanism | ✅ | Recovery from checkpoint implemented (Phase 4 partial) |
| REQ-053 | NOR Flash Media Emulation - NOR Flash Specification | ✅ | Full NOR device in `src/media/nor_flash.c` (256 MB, 512 B page, 64 KB sector, P/E budget); `tests/test_nor_flash.c` |
| REQ-054 | NOR Flash Media Emulation - Storage Partitions | ✅ | 7-partition table in `nor_flash.c`: `NOR_PART_BOOTLOADER`/`FW_SLOT_A`/`FW_SLOT_B`/`CONFIG`/`BBT`/`EVENT_LOG`/`SYSINFO` |
| REQ-055 | NOR Flash Media Emulation - Operation Commands | ✅ | `nor_read`/`nor_program`/`nor_sector_erase`/`nor_chip_erase`/`nor_read_status` in `nor_flash.c` |
| REQ-056 | NOR Flash Media Emulation - Data Persistence | ✅ | `mmap(MAP_SHARED)` + `msync()` persistence in `nor_dev_init`/`nor_sync` |
| REQ-057 | Media Thread Module - Main Interface | ✅ | Media interface in `media.h/c` |

### 4. HAL Module (REQ-058 to REQ-069)

| ID | Requirement Description | Status | Notes |
|----|------------------------|--------|-------|
| REQ-058 | NAND Driver Module - NAND Driver API | ✅ | Complete HAL NAND API in `hal_nand.h/c` |
| REQ-059 | NAND Driver Module - Driver Internal Implementation | ✅ | Implemented |
| REQ-060 | NOR Driver Module - NOR Driver API | ✅ | NOR driver fully implemented (Phase 2) |
| REQ-061 | NOR Driver Module - Driver Internal Implementation | ✅ | Implemented (Phase 2) |
| REQ-062 | NVMe/PCIe Module Management - Command Completion Submission | ✅ | Command completion submission implemented (Phase 2) |
| REQ-063 | NVMe/PCIe Module Management - Async Event Management | ✅ | `struct hal_aer_ctx` (pending event ring + outstanding CID queue) in `src/hal/hal_aer.c`; embedded in `nvme_uspace_dev`. Admin dispatch (`NVME_ADMIN_ASYNC_EVENT`) routes through `hal_aer_submit_request`; controller-side callers post events via `nvme_uspace_aer_post_event()`. DW0 encoding per NVMe spec §5.2, AER LIMIT EXCEEDED returned with SCT=CMD_SPEC/SC=0x05 when the outstanding queue overflows. Covered by unit tests in `test_hal.c` and admin-path integration tests in `test_nvme_uspace.c`. |
| REQ-064 | NVMe/PCIe Module Management - PCIe Link State Management | ✅ | `struct pcie_link_ctx` in `src/hal/hal_pcie_link.c` implements the LLD_13 §6.2 six-state machine (L0 / L0s / L1 / L2 / RESET / FLR) backed by an adjacency matrix. Hot reset is legal from any L* state; FLR is only legal from L0. ASPM policy setter accepts DISABLED / L0s / L1 / L0s+L1. Tests in `test_hal.c::test_pcie_link_*` cover legal transitions, rejected edges (L0s->L2, L1->L0s, FLR from L1), hot reset from each L* state, and the ASPM policy setter. |
| REQ-065 | NVMe/PCIe Module Management - Namespace Management Interface | ✅ | Namespace management implemented (Phase 2) |
| REQ-066 | Power Management IC Driver - NVMe Power State Emulation | ✅ | Power state management implemented (Phase 2) |
| REQ-067 | Power Management IC Driver - Functional Requirements | ✅ | Implemented (Phase 2) |
| REQ-068 | HAL Module - Main Interface | ✅ | HAL interface in `hal.h/c` |
| REQ-069 | PCI Management - Interface | ✅ | Byte-level config space (256B standard + 4KB extended) implemented in `hal_pci.h/c` via `hal_pci_cfg_init/read8/16/32/write8/16/32`. PCIe unresponsive semantics enforced — out-of-range + unaligned reads return all-ones; writes beyond 4KB or unaligned rejected with `HFSSS_ERR_INVAL`. Standard Type-0 header + status capability bit + caps ptr stamped at init. Covered by `tests/test_hal.c::test_hal_pci_cfg_*` (27 assertions). |

### 5. Common Services Module (REQ-070 to REQ-093)

| ID | Requirement Description | Status | Notes |
|----|------------------------|--------|-------|
| REQ-070 | RTOS Emulation - RTOS Primitive Implementation | ✅ | Task/Queue/Semaphore/Mutex in common/ |
| REQ-071 | RTOS Emulation - Message Queue | ✅ | Message queue in `msgqueue.h/c` |
| REQ-072 | RTOS Emulation - Semaphore/Mutex/Event Group | ✅ | Semaphore in `semaphore.h/c`, Mutex in `mutex.h/c` |
| REQ-073 | RTOS Emulation - Software Timer/Memory Pool | ✅ | Memory pool in `mempool.h/c` |
| REQ-074 | Task Scheduling - Static Task Binding | ⚠️ | `pthread_setaffinity_np` wired in `src/common/rt_services.c` (Linux path); macOS gracefully degrades with a warning per LLD_12 |
| REQ-075 | Task Scheduling - Priority Scheduling/Load Balancing | ❌ | SCHED_FIFO/SCHED_RR hook not present in `rt_services.c`; LLD_12 designed |
| REQ-076 | Memory Management - Memory Partition Planning | ❌ | No explicit memory partitioning (mmap/malloc used directly) |
| REQ-077 | Memory Management - Memory Management Strategy | ✅ | mmap/hugetlb memory management implemented (Phase 2) |
| REQ-078 | Bootloader - Boot Sequence | ✅ | 6-stage boot (`BOOT_PHASE_0_HW_INIT`..`BOOT_PHASE_5_READY`) in `src/common/boot.c`; `tests/test_boot.c` |
| REQ-079 | Bootloader - Bootloader Features | ✅ | Dual NOR firmware slot (SlotA/SlotB) selection with CRC in `boot.c`; `boot_select_firmware_slot`/`boot_swap_firmware_slot` |
| REQ-080 | Power-On/Off Service - Power-On Service | ✅ | Boot type detection (FIRST/NORMAL/RECOVERY) via SysInfo markers in `boot.c`; `tests/test_power_cycle.c` |
| REQ-081 | Power-On/Off Service - Power-Off Service | ✅ | `power_mgmt_init`/`normal_shutdown`/`abnormal_shutdown` in `boot.c`; persists markers to NOR SysInfo |
| REQ-082 | Out-of-Band Management - Interface Type | ✅ | JSON-RPC over Unix socket in `src/common/oob.c`; `tests/test_oob.c` |
| REQ-083 | Out-of-Band Management - OOB Management Functions | ✅ | OOB command handlers (device_info / health / stats / reset_counters) in `oob.c` |
| REQ-084 | Out-of-Band Management - SMART Information | ✅ | SMART_INFO handler in `oob.c` (P/E counts, spare, temp, throttle); consumed by `hfsss-ctrl` |
| REQ-085 | Inter-Core Communication - Communication Mechanism | ✅ | Lock-free single-producer single-consumer ring buffer in `include/common/spsc_ring.h` + `src/common/spsc_ring.c`. Power-of-two capacity with masked indices, `atomic_uint` head/tail using release-store on publish and acquire-load on cross-side read, wait-free `tryput` / `tryget` returning `NOSPC` / `AGAIN` on boundary conditions. Covered by `tests/test_common.c::test_spsc_ring_basic` (FIFO + capacity + wrap-around) and `test_spsc_ring_contention` (100K items shipped producer→consumer across two threads, no drops or reorders). |
| REQ-086 | System Stability Monitoring - Watchdog | ✅ | Basic watchdog implemented (Phase 2) |
| REQ-087 | System Stability Monitoring - System Resource Monitoring | ✅ | `include/common/system_monitor.h` + `src/common/system_monitor.c` expose a callback-driven periodic sampler. CPU utilization comes from `(cpu_delta / wall_delta) * 100` between polls; memory + thread count pass through caller-supplied callbacks. Default POSIX implementations back `get_cpu_time_ns` and `get_mem_bytes` with `getrusage(RUSAGE_SELF)` (ru_maxrss conversion handled per-platform). Synchronous `system_monitor_poll_once` for tests + `start/stop` daemon-thread path for production. Covered by `tests/test_common.c::test_system_monitor_basic` and `::test_system_monitor_thread_lifecycle`. |
| REQ-088 | System Stability Monitoring - Performance Anomaly Detection/Thermal Emulation | ✅ | Thermal model in `src/common/thermal.c` (REQ-171..173) + P99.9 latency anomaly detector in `src/controller/latency_monitor.c`. `lat_monitor_set_p999_anomaly` installs threshold + optional callback; `lat_monitor_check_p999_anomaly` computes P99.9 from the existing histogram, bumps `p999_anomalies`, and fires the callback on breach. Covered by `tests/test_qos.c::test_latency_p999_anomaly_detector` (healthy workload doesn't fire, injected outliers do, counter persists across breaches, detector disable/enable, NULL safety). |
| REQ-089 | Panic/Assert Handling - Assert Mechanism | ✅ | Basic ASSERT in `common.h` |
| REQ-090 | Panic/Assert Handling - Panic Flow | ✅ | Panic flow implemented (Phase 1) |
| REQ-091 | System Debug Mechanism - Debug Functions | ✅ | Per-thread lockless trace ring in `src/common/trace.c` (compile-gated behind `HFSSS_DEBUG_TRACE`, `TRACE=1` build variant); `tests/test_trace.c`; `scripts/qemu_blackbox/phase_a/analyze_trace.py` consumes dumps |
| REQ-092 | System Event Log Mechanism - Event Levels | ✅ | Log system in `log.h/c` |
| REQ-093 | System Event Log Mechanism - Log Storage | ✅ | Log persistence to NOR flash implemented (Phase 1) |

### 6. Algorithm Task Layer (FTL) Module (REQ-094 to REQ-115)

| ID | Requirement Description | Status | Notes |
|----|------------------------|--------|-------|
| REQ-094 | Address Mapping Management - Address Mapping Architecture | ✅ | Page-level mapping in `mapping.h/c` |
| REQ-095 | Address Mapping Management - Mapping Table Design | ✅ | L2P/P2L mapping tables |
| REQ-096 | Address Mapping Management - Over-Provisioning | ⚠️ | Basic OP, not configurable via Format NVM |
| REQ-097 | Address Mapping Management - Write Operation Flow | ✅ | FTL write in `ftl.c` |
| REQ-098 | Address Mapping Management - Read Operation Flow | ✅ | FTL read in `ftl.c` |
| REQ-099 | Address Mapping Management - Striping Strategy | ❌ | No striping across channels |
| REQ-100 | NAND Block Address Management - Block State Machine | ✅ | Block state machine in `block.h/c` |
| REQ-101 | NAND Block Address Management - Current Write Block Management | ✅ | CWB management |
| REQ-102 | NAND Block Address Management - Free Block Pool Management | ✅ | Free block pool |
| REQ-103 | Garbage Collection - GC Trigger Strategy | ✅ | GC in `gc.h/c` |
| REQ-104 | Garbage Collection - Victim Block Selection Algorithm | ✅ | Cost-Benefit GC algorithm implemented (Phase 1) |
| REQ-105 | Garbage Collection - GC Execution Flow | ✅ | GC execution implemented |
| REQ-106 | Garbage Collection - GC Concurrency Optimization/WAF Analysis | ✅ | WAF calculation and monitoring implemented (Phase 1) |
| REQ-107 | Wear Leveling - Dynamic Wear Leveling | ✅ | Dynamic wear leveling with erase-count-based free block prioritization |
| REQ-108 | Wear Leveling - Static Wear Leveling | ✅ | Static wear leveling implemented (Phase 1) |
| REQ-109 | Wear Leveling - Wear Monitoring and Alerting | ✅ | `ftl_rel_check_health` in `src/ftl/ftl_reliability.c` reports GOOD/DEGRADED/CRITICAL/FAILED; `tests/test_ftl_reliability.c` |
| REQ-110 | Read/Write/Erase Command Management - Command State Machine | ✅ | `CMD_STATE_FREE`/`RECEIVED`/`ARBITRATED`/`SCHEDULED`/`IN_FLIGHT`/`COMPLETED`/`TIMEOUT` in `src/controller/arbiter.c`; die-level state machine (`DIE_READ_SETUP`..`DIE_SUSPENDED_*`) in `include/media/cmd_state.h`; `tests/test_cmd_integration_*.c`, `tests/systest_phase7_integration.c` |
| REQ-111 | Read/Write/Erase Command Management - Read Retry Mechanism | ✅ | `READ_RETRY_VOLTAGE_OFFSETS` loop in `src/ftl/ftl.c` with `error_read_retry_attempt`/`success` counters; integrates with reliability error path |
| REQ-112 | Read/Write/Erase Command Management - Write Retry/Write Verify | ✅ | `max_write_retries` loop in `src/ftl/ftl.c`; spare-block fallback through `ftl_rel_consume_spare` in `ftl_reliability.c` |
| REQ-113 | IO Flow Control - Multi-Level Flow Control | ✅ | `token_bucket` per-flow + per-QoS array in `src/controller/flow_control.c` (`FLOW_MAX` tiers + `QOS_MAX` classes); paired with backpressure (REQ-034) |
| REQ-114 | Data Redundancy Backup - RAID-Like Data Protection | ❌ | Die-level XOR parity / dual-copy L2P not implemented; BBT mirror in NOR partitions exists via REQ-054 |
| REQ-115 | Command Error Handling - NVMe Error Status Codes/Error Handling Flow | ✅ | Error Log Page (LID=0x01) population via `nvme_uspace_report_error()`; UCE/CE entries flow into the 64-entry ring in `nvme_uspace_dev`; SCT/SC carried in `status_field` per NVMe spec §5.14.1.1 |

### 7. Performance Requirements (REQ-116 to REQ-123)

| ID | Requirement Description | Status | Notes |
|----|------------------------|--------|-------|
| REQ-116 | IOPS Performance - Random Read IOPS | ✅ | `perf_validation_run_all` builds a `REQ-116` row (PRD-aligned target >= 1M IOPS at 4KB/QD=32) via `run_iops(BENCH_RAND_READ, …)`. The regression gate in `tests/test_perf_validation.c::test_validation_run_all` asserts the row is present, `passed==true`, and `target == 1000000.0`; a separate `report.failed == 0` invariant ensures any single-REQ regression fails CI instead of quietly reporting "warning". |
| REQ-117 | IOPS Performance - Random Write IOPS | ✅ | Same shape as REQ-116 — `REQ-117` row for random write (4KB/QD=32, PRD target >= 300K IOPS) asserted passed in the run_all report with a pinned target value. |
| REQ-118 | IOPS Performance - Mixed Read/Write IOPS | ✅ | `REQ-118` row for mixed 70/30 workload at 4KB/QD=32 (total >= 250K IOPS) asserted passed in the report. |
| REQ-119 | Bandwidth Performance - Sequential Read/Write | ✅ | Two rows (`REQ-119-RD` >= 6500 MB/s, `REQ-119-WR` >= 3500 MB/s) at 128KB block size — both asserted passed in the report. |
| REQ-120 | Latency Performance - Random Read/Write Latency | ✅ | Three rows at QD=1: `REQ-120-P50` <= 100 µs, `REQ-120-P99` <= 150 µs, `REQ-120-P999` <= 500 µs. Each asserted passed in the report; histogram invariants (sum == total_ops, ordering P50 <= P99 <= P99.9) covered by Test 9. |
| REQ-121 | Simulation Accuracy - NAND Latency Error | ❌ | No formal accuracy verification against reference device data |
| REQ-122 | Scalability - Channel/Namespace/CPU | ✅ | `perf_validation_run_all` now runs a **measured** scalability probe: back-to-back `bench_run` invocations at nt=1 and nt=8 compute `iops(N) / (N * iops(1))`. A real regression in the bench worker path lowers the ratio and fails the 70% gate. The Amdahl model (`perf_scalability_efficiency`, SIM_SERIAL_FRACTION=0.02) remains as a design-time upper bound. Multi-thread I/O-path scaling against real NAND is still validated externally via QEMU+fio reference runs. Pinned by `tests/test_perf_validation.c::test_validation_run_all` via `r122->passed` + `r122->target == 70.0`. |
| REQ-123 | Resource Utilization Target - CPU/DRAM | ✅ | `bench_run` brackets the workload with a `system_monitor` sampler so `cpu_util_pct` reflects real `getrusage(RUSAGE_SELF)` deltas. `perf_validation_run_all` runs a dedicated REQ-123 probe (bench burst + equal-duration idle window) to model the controller's bursty dispatch profile — CPU vs wall time lands near 50% when the bench saturates. Split into two rows: `REQ-123` asserts CPU <= 50% and `REQ-123-DRAM` asserts process RSS <= 1024 MB under the same probe, covering the PRD's CPU/DRAM budget in one pass. Asserted via `r123->passed` + `r123dram->passed` + pinned targets. |

### 8. Product Interfaces (REQ-124 to REQ-131)

| ID | Requirement Description | Status | Notes |
|----|------------------------|--------|-------|
| REQ-124 | Host Interface - Block Device Node | ❌ | Requires kernel module; covered by NBD bridge + QEMU (indirect). Phase 7 deferral |
| REQ-125 | nvme-cli Compatibility | ⚠️ | Complementary uspace admin evidence in `tests/systest_nvme_cli_compat.c` mirrors `nvme-cli` subcommands (`id-ctrl`, `id-ns`, `smart-log`, `error-log`, `format`, `sanitize`, `fw-download`+`fw-commit`, `get-feature`) against the uspace entry points the CLI would reach through the guest driver. `fw-log` (LID=0x03) is **not** included because the handler returns NOTSUPP — that would be an unimplemented subcommand, not wire-compat. The PRD's traced definition (LLD_16) calls for real `nvme-cli` execution on `/dev/nvmeXnY` via the kernel / NBD host path; that evidence still lives in the QEMU blackbox under `scripts/qemu_blackbox/cases/nvme/` and is not yet a gating systest here. REQ-125 stays Partial until both surfaces meet in one place. |
| REQ-126 | fio Test Tool Compatibility | ⚠️ | Complementary workload-shape evidence in `tests/systest_fio_compat.c` covers fio's canonical shapes (seq-write, seq-read-verify, rand-write, rand-read-verify, 70/30 randrw mix, 128 KiB large-block) with payload-fidelity checks on the verify paths. This is a direct test of the simulator's uspace read/write API, not of the `fio` binary itself. The PRD's traced definition (LLD_16) calls for actual `fio` invocation on `/dev/nvmeXnY` with `io_uring`, `direct=1`, `iodepth=128`, `numjobs=32`; that evidence lives in `scripts/qemu_blackbox/cases/fio/` and is not yet a gating systest. REQ-126 stays Partial pending the host-path integration. |
| REQ-127 | OOB Socket Interface | ✅ | Same JSON-RPC Unix socket as REQ-082 |
| REQ-128 | /proc Filesystem Interface | ✅ | `src/common/proc_interface.c` emits `proc_write_status`/`proc_write_perf_counters`/`proc_write_ftl_stats`; `tests/test_proc_interface.c` |
| REQ-129 | Command Line Interface - hfsss-ctrl | ✅ | `src/tools/hfsss_ctrl.c` CLI speaking to OOB socket |
| REQ-130 | Configuration File Interface - YAML | ✅ | `src/common/hfsss_config.c` YAML loader; `tests/test_config.c` |
| REQ-131 | Persistence Data Format Interface | ✅ | Format spec `docs/LLD_15_PERSISTENCE_FORMAT_EN.md` / `.md` (V1.1, 2026-04-22) reconciled with shipping on-disk layouts: Media checkpoint (`struct media_file_header`, `MEDIA_FILE_MAGIC = 0x48465353`, V2 with FULL / INCREMENTAL flags, single `checkpoint.bin`), Superblock L2P checkpoint + journal (`SB_*_MAGIC`, `sb_page_header`, `ckpt_entry`, `journal_entry` in `include/ftl/superblock.h`), and WAL (`WAL_RECORD_MAGIC = 0x12345678`, `WAL_COMMIT_MARKER = 0xDEADBEEF`, 64-byte `struct wal_record` in `include/ftl/wal.h`). Panic-dump file format moved to §3.5 "Future Work" — current boot flow emits `[PANIC]` via the standard log sink. Persistence paths covered by `tests/test_superblock.c` (superblock / checkpoint / journal) and `tests/test_power_cycle.c` (end-to-end crash recovery). |

### 9. Fault Injection Framework (REQ-132 to REQ-134)

| ID | Requirement Description | Status | Notes |
|----|------------------------|--------|-------|
| REQ-132 | NAND Media Fault Injection | ✅ | Fault registry in `src/common/fault_inject.c`; `struct media_ctx.faults` + `media_attach_fault_registry()` plug it into the NAND I/O path. `media_nand_read/program/erase` consult `fault_check()` early and surface hits as `HFSSS_ERR_IO` (FAULT_READ_ERROR / FAULT_PROGRAM_ERROR / FAULT_ERASE_ERROR). Per-address targeting + wildcards validated by `tests/test_media.c::test_fault_injection_nand_path`. |
| REQ-133 | Power Fault Injection | ✅ | UPLP test hooks `uplp_inject_power_fail`/`uplp_inject_at_phase` in `src/common/uplp.c`; `tests/test_uplp.c` |
| REQ-134 | Controller Fault Injection | ✅ | Three injectable controller-fault surfaces wired into the `fault_inject.c` registry: (1) **pool exhaustion** — `struct resource_mgr.faults` + `resource_mgr_attach_faults()` and `struct arbiter_ctx.faults` + `arbiter_attach_faults()`; `FAULT_POOL_EXHAUST` consulted in `resource_alloc`, `idle_block_alloc`, and `arbiter_alloc_cmd` (all three return NULL on hit). (2) **Soft-panic surrogate** — `FAULT_PANIC` check in `arbiter_enqueue` rejects with `HFSSS_ERR` and sets `cmd->state = CMD_STATE_ERROR`. Not a true process-terminating panic (`fault_controller_panic()` still `abort()`s); this is an injectable error-path that exercises the same rejection semantics clients hit on real controller failures. (3) **Mass-timeout detection** — `FAULT_TIMEOUT` check in `arbiter_check_timeouts` forces every in-flight command to `CMD_STATE_TIMEOUT` on the current tick (sticky fault re-slams every tick; one-shot or probability-gated faults model a transient event). All three exercised by `tests/test_fault_inject.c::test_controller_*` (REQ-134.1 through REQ-134.5). `struct fault_registry` now carries a `struct mutex lock` so `fault_check` / `fault_inject_add` / `fault_inject_remove` / `fault_inject_clear_all` are thread-safe — required because controller hot paths are contended, unlike the REQ-132 NAND-side use that was effectively single-writer under `media_ctx`. |

### 10. System Reliability & Stability (REQ-135 to REQ-138)

| ID | Requirement Description | Status | Notes |
|----|------------------------|--------|-------|
| REQ-135 | MTBF Target | ❌ | No MTBF testing |
| REQ-136 | Data Integrity Guarantee | ✅ | Basic data integrity (md5sum verified in tests, fio `verify=crc32c` in blackbox cases) |
| REQ-137 | Stability Requirement - Long-Running Operation | ⚠️ | `tests/stress_stability.c` provides long-run soak harness; no published multi-day run |
| REQ-138 | Stability Requirement - Memory Leak/Concurrency Safety | ✅ | Clean ASAN (PR #86) and TSAN (PR #85) runs across default + TSAN+ASAN sanitizer builds |

### 11. Enterprise: UPLP - Unexpected Power Loss Protection (REQ-139 to REQ-146)

| ID | Requirement Description | Status | Notes |
|----|------------------------|--------|-------|
| REQ-139 | Supercapacitor energy model (1-10F) | ✅ | `supercap_model` + RC discharge in `src/common/uplp.c`; `tests/test_uplp.c` |
| REQ-140 | UPLP state machine (Normal→PowerFail→CapDrain→SafeState) | ✅ | 6-state `enum uplp_state` (NORMAL/POWER_FAIL/CAP_DRAINING/EMERGENCY_FLUSH/SAFE_STATE/RECOVERY) in `uplp.c` |
| REQ-141 | Atomic write unit (4KB power-safe) | ✅ | `write_unit_header` (magic/sequence/CRC32) in `uplp.c` |
| REQ-142 | Power-fail-safe metadata journal | ✅ | `flush_progress` bitmask across 6 steps (INFLIGHT_NAND / L2P_JOURNAL / BBT / SMART / WAL_COMMIT / SYSINFO) in `uplp.c` |
| REQ-143 | Write buffer emergency flush (priority order) | ✅ | `uplp_emergency_flush` with per-step `flush_step_energy[]` budget |
| REQ-144 | UPLP recovery sequence (<5s for 1TB) | ✅ | Recovery path in `uplp.c`; drain time derived from supercap model |
| REQ-145 | UPLP test mode (injectable power-fail) | ✅ | `uplp_inject_power_fail`/`uplp_set_cap_drain_time`/`uplp_inject_at_phase` hooks |
| REQ-146 | Unsafe shutdown counter (SMART) | ✅ | `sysinfo.unsafe_shutdown_count` incremented on RECOVERY boot, persisted to NOR |

### 12. Enterprise: QoS Determinism (REQ-147 to REQ-153)

| ID | Requirement Description | Status | Notes |
|----|------------------------|--------|-------|
| REQ-147 | DWRR multi-queue scheduler | ✅ | `src/controller/dwrr_scheduler.c` with per-NS queue create/delete + weighted dispatch; `tests/test_qos.c` |
| REQ-148 | Per-namespace IOPS limits (1K-2M) | ✅ | Per-NS `ns_qos_ctx` table embedded in `struct nvme_uspace_dev`; `nvme_uspace_read/write` refill + acquire tokens on every LBA-bearing command, returning `HFSSS_ERR_BUSY` on exhaustion. Dispatcher maps to CQE `SC=NAMESPACE_NOT_READY` so hosts treat it as retryable. Setter `nvme_uspace_dev_set_qos_policy` accepts the full 1K..2M IOPS range. Covered by `tests/test_nvme_uspace.c::test_qos_per_ns_iops_cap_engages` (throttle observation on the read path + CQE status + boundary accept). |
| REQ-149 | Per-namespace bandwidth limits (50MB/s-14GB/s) | ✅ | Same acquire-tokens path — the BW bucket consumes `count * lba_size` bytes per request. Setter accepts the full 50 MB/s..14 GB/s span (50..14000 MB/s). Covered by `tests/test_nvme_uspace.c::test_qos_per_ns_bw_cap_engages` (100K reads against a 50 MB/s cap drive the bucket into throttle; spec lower + upper bounds accepted cleanly). |
| REQ-150 | Latency SLA enforcement (P99) | ✅ | `nvme_uspace_dev_set_sla_rollback(nsid, target_us, trigger_count, cb, ctx)` arms a P99 SLA target + rollback callback; `nvme_uspace_dev_check_sla` folds `lat_monitor_check_sla`'s breach detection on top of the trigger count and fires the callback on sustained breaches, then resets consecutive_violations so each window is evaluated independently. Covered by `tests/test_nvme_uspace.c::test_qos_sla_rollback` (healthy P99 silent, 3-in-a-row breaches fire exactly once, disabled trigger stays silent, second window re-arms). |
| REQ-151 | QoS policy hot-reconfiguration | ✅ | `nvme_uspace_dev_set_qos_policy` rebuilds the token buckets in place and the next `qos_acquire_tokens` call sees the new caps without stopping traffic or draining the I/O path. Works for cap changes AND for `enforced` flag flips. Covered by `tests/test_nvme_uspace.c::test_qos_hot_reconfigure_live` (tight -> unenforced mid-traffic stops throttling immediately; subsequent reconfigure to 2M IOPS still passes all commands). |
| REQ-152 | GC/WL background priority yield | ✅ | GC bandwidth cap (REQ-036) + QoS-aware dispatcher yields foreground reads |
| REQ-153 | Deterministic latency window (duty cycle) | ⚠️ | `det_window.c` provides windowed metrics; duty-cycle enforcement partial |

### 13. Enterprise: T10 DIF/PI - Data Integrity (REQ-154 to REQ-158)

| ID | Requirement Description | Status | Notes |
|----|------------------------|--------|-------|
| REQ-154 | T10 PI Type 1/2/3 support (per namespace) | ✅ | `src/ftl/t10_pi.c` implements Type 1/2/3 CRC-16; `tests/test_t10_pi.c` |
| REQ-155 | CRC-16 guard tag (write generate, read verify) | ✅ | CRC-16 guard generate/verify in `t10_pi.c` |
| REQ-156 | Reference and application tag processing | ✅ | Reference + application tag handling in `t10_pi.c` |
| REQ-157 | PI metadata propagation through FTL/GC | ✅ | GC read/program path passes spare_buf through HAL; `src/media/media.c` copies spare back on read so PI tuples survive the migrate. End-to-end covered by `tests/test_t10_pi.c::test_pi_gc_preservation` |
| REQ-158 | E2E data integrity error reporting (NVMe status) | ✅ | PI errors map to NVMe status (SCT=2, SC=0x81/0x82/0x83); populated into Error Log Page ring via `nvme_uspace_report_error()`; host observable via `Get Log Page LID=0x01` |

### 14. Enterprise: Security / Data-at-Rest Encryption (REQ-159 to REQ-165)

| ID | Requirement Description | Status | Notes |
|----|------------------------|--------|-------|
| REQ-159 | AES-XTS 256-bit simulation (XOR placeholder) | ✅ | `crypto_xts_encrypt`/`crypto_xts_decrypt` in `src/controller/security.c`; `tests/test_security.c` |
| REQ-160 | Key hierarchy (MK→KEK→DEK, per-NS isolation) | ✅ | `sec_hkdf_derive` + per-NS `key_entry` in `security.c` |
| REQ-161 | TCG Opal SSC basic commands (lock/unlock) | ✅ | `opal_lock_ns` / `opal_unlock_ns` in `src/controller/security.c` drive the existing ACTIVE<->SUSPENDED state transitions. Per-NS auth tokens derived from the master key via `opal_derive_auth` (HKDF with a domain-separation tag XORed into MK). Wrong auth returns `HFSSS_ERR_AUTH` and leaves the namespace locked. Covered by five `tests/test_security.c::test_opal_*` cases including deterministic+NS-unique derivation and cross-NSID auth rejection. |
| REQ-162 | Crypto erase (destroy DEK) | ✅ | `crypto_erase_ns` in `security.c` |
| REQ-163 | Secure erase (block erase all user data) | ✅ | `nvme_uspace_sanitize` dispatches all four SANACT modes (EXIT_FAILURE/BLOCK_ERASE/OVERWRITE/CRYPTO_ERASE); OVERWRITE performs explicit zero-fill on every LBA |
| REQ-164 | Secure boot chain verification (ROM→BL→FW) | ✅ | `secure_boot_verify()` invoked during `BOOT_PHASE_1_POST` in `src/common/boot.c`; tampered image or wrong magic → `HFSSS_ERR_AUTH` abort |
| REQ-165 | Key storage in NOR (dual-copy, UPLP-safe) | ✅ | The canonical `key_table_save(kt, nor)` / `key_table_load(kt, nor)` API in `src/controller/security.c` persists through `NOR_PART_KEYS` with dual-copy slots at rel-offsets 0 and 64 KB. There is no file-backed fallback — the file-path overloads were removed so no persistence path bypasses NOR. Each slot is stamped with a monotonic generation + outer CRC; save writes slot B first then slot A so an interrupted update leaves at least one valid copy. Load returns the slot with the highest valid generation. Covered by `tests/test_security.c::test_key_table_init_fields` + six `test_key_table_nor_*` cases including single-slot corruption and mid-update crash recovery. |

### 15. Enterprise: Multi-Namespace Management (REQ-166 to REQ-170)

| ID | Requirement Description | Status | Notes |
|----|------------------------|--------|-------|
| REQ-166 | Namespace create (allocate from global pool) | ✅ | `ns_mapping_create` in `src/ftl/ns_mapping.c`; `tests/test_multi_ns.c` |
| REQ-167 | Namespace delete (reclaim blocks, free L2P) | ✅ | `ns_mapping_delete` reclaims blocks + clears L2P |
| REQ-168 | Namespace attach/detach (preserve data) | ✅ | `ns_mapping_attach`/detach preserves NS state across |
| REQ-169 | Per-namespace FTL tables (L2P isolation) | ✅ | Each `ns_mapping_ctx` has isolated L2P in `ns_mapping.c` |
| REQ-170 | Namespace format (per-NS LBA size change) | ✅ | `ns_mapping_format` changes per-NS LBA block size |

### 16. Enterprise: Thermal Management & Telemetry (REQ-171 to REQ-178)

| ID | Requirement Description | Status | Notes |
|----|------------------------|--------|-------|
| REQ-171 | Composite temperature (weighted per-die average) | ✅ | `src/common/thermal.c` computes composite level (NONE/LIGHT/MODERATE/HEAVY/SHUTDOWN) with hysteresis; `tests/test_thermal_telemetry.c` |
| REQ-172 | Progressive thermal throttle (75C/80C/85C) | ✅ | Factor table 1.0/0.80/0.50/0.20/0.0 at 75/80/85/90 °C thresholds in `thermal.c` |
| REQ-173 | Thermal shutdown (90C threshold) | ✅ | `thermal_is_shutdown`/`THERMAL_LEVEL_SHUTDOWN` at ≥90 °C |
| REQ-174 | Host-initiated telemetry (Log Page 07h) | ✅ | `nvme_uspace_get_log_page(LID=0x07)` serializes `dev->telemetry` into the NVMe Telemetry Log layout (512-byte header + Data Area 1 events, newest-first); `host_gen_number` advances on every read per spec. **Caveat**: the underlying `telemetry_record()` has no production caller yet, so a live system returns header-only pages. Producers are wired in the follow-up Tier-C `nvme_uspace_aer_notify_*` bridges. |
| REQ-175 | Controller-initiated telemetry (Log Page 08h) | ✅ | Same serializer as LID 0x07 but `ctrl_data_available` tracks ring non-emptiness and `ctrl_gen_number` only advances when new events appear since the last poll. **Caveat**: same producer gap as REQ-174 until Tier-C notifiers land. |
| REQ-176 | Vendor-specific log page (internal counters) | ✅ | LID 0xC0 returns `struct nvme_vendor_log_counters` (magic/total_events/events_in_ring + per-type counts over `enum tel_event_type`) for `hfsss-ctrl` and test introspection. **Caveat**: counters reflect whatever is recorded in `dev->telemetry`; in production they stay zero until Tier-C wires the AER-notify producers. |
| REQ-177 | SMART remaining life prediction (PE+WAF trend) | ✅ | `smart_predict_life` computes `remaining_life_pct`/`waf`/`avg_erase_count` in `telemetry.c` |
| REQ-178 | Async event notification (temp/spare/reliability AER) | ✅ | Helper bridges `nvme_uspace_aer_notify_thermal/wear/spare()` wire telemetry + AER posting per NVMe §5.2. Runtime producer lives in `src/pcie/smart_monitor.c`: a callback-driven polling loop (configurable interval) detects thermal level changes, wear bucket increases (10% granularity), and spare bucket decreases, then calls the matching notifier bridge. Callers inject their own thermal/wear/spare data sources via `smart_monitor_config` callbacks. Tests: `test_smart_monitor_poll_fires_aer_on_threshold_cross` (deterministic poll_once path) and `test_smart_monitor_thread_lifecycle` (background thread path). |

---

## Key Observations

### What's Working Well

Phases 0 through 6 are substantially landed, plus the Enterprise V3.0 UPLP / Multi-NS / Security / Thermal-Telemetry groups. Module-level state:

- **HAL Layer**: 75% (NAND + NOR drivers, completion submission, namespace mgmt, power states; AER is stub pending LLD_13)
- **Media Layer**: 75% (NAND hierarchy + timing + reliability + BBT + full NOR with 7-partition layout and mmap persistence)
- **Controller Thread**: 80% (timeout mgmt, backpressure, QoS buckets, GC traffic control, DWRR + latency monitor)
- **FTL Layer**: 81.8% (Cost-Benefit GC + wear leveling + WAF + command state machine + read/write retry with voltage offsets + multi-level flow control + T10 PI CRC-16)
- **PCIe/NVMe User-Space**: 54.5% (admin / IO / DSM / identify / doorbell / CQ; kernel-side DMA/MSI-X/IOMMU remain Phase 7)
- **Common Services**: 75% (mutex/semaphore/msgqueue/mempool, boot 6-stage + dual-slot firmware, power mgmt, OOB JSON-RPC, SMART, trace ring, watchdog, UPLP, thermal, telemetry, secure boot, security key table)
- **Product Interfaces**: 62.5% — `/proc`, `hfsss-ctrl`, YAML config, LLD_15 persistence format landed; REQ-124 (block device) remains Phase 7; REQ-125/126 retain ⚠️ pending LLD_16 host-path evidence
- **Fault Injection**: 66.7% (NAND registry + power-fail hooks; controller-wide hooks partial)
- **Enterprise UPLP / Multi-NS / Security / Thermal-Telemetry**: 100% / 100% / 85.7% / 75%

Clean ASAN and TSAN runs on the default + sanitizer builds; `make test` reports 0 failures across ~2,759 assertions as of PR #86.

### Architecture Decision: User-Space vs. Kernel Module

The PRD and HLD/LLD documents describe a Linux **kernel module** (`hfsss_nvme.ko`) as the host interface. The current implementation stays in user-space and reaches the guest via a **QEMU-NVMe → NBD → `hfsss-nbd-server`** bridge:

- Phases 0–6 build the core SSD simulation + sanity/perf/reliability harnesses in user-space (complete)
- Phase 7 (optional) adds the kernel module for real `/dev/nvme` block device support — currently deferred
- See `ARCHITECTURE.md` and `docs/QEMU_BLACKBOX_TESTING.md`

### Remaining Major Gaps

1. **Phase 7 kernel module** (REQ-006/009/013/014/019/020/021/022/023/124/064) — intentional deferral; user-space simulator cannot provide kernel-side DMA, MSI-X, IOMMU, or `/dev/nvmeXnY` directly.
2. **Performance gates** — REQ-121 NAND timing accuracy, REQ-122 measured scalability (bench_run nt=1 vs nt=8), and REQ-123 CPU + DRAM budgets all gated by `perf_validation_run_all` with target values pinned in the regression suite.
3. **Deep QoS features** (REQ-148..151, 153) — per-NS IOPS/BW caps, strict P99 SLA rollback, and hot-reconfiguration are partial against the LLD_18 target.
4. **Deep QoS features beyond DWRR** (REQ-148..151, 153) — per-NS IOPS/BW caps, strict P99 SLA rollback, and hot-reconfiguration are partial against the LLD_18 target.
5. **RAID-like data protection** (REQ-114) — die-level XOR parity and dual-copy L2P not implemented; BBT dual mirror exists via NOR partitions.
6. **IPC ring + resource sampling** (REQ-085, 087) — SPSC ring and periodic CPU/memory/thread-pool sampling not implemented; watchdog covers hang detection only.
7. **Striping / memory partitioning** (REQ-099, 076) — single-channel mapping and unpartitioned mmap/malloc remain as designed for the current build.
8. **RT scheduling** (REQ-075) — `SCHED_FIFO`/`SCHED_RR` hook absent; `pthread_setaffinity_np` side (REQ-074) is wired under Linux.
9. **Long-haul stability report** (REQ-137) — `tests/stress_stability.c` provides the harness, but no published multi-day run.

### LLD Implementation Status

| Document | Scope | Status |
|----------|-------|--------|
| LLD_07_OOB_MANAGEMENT.md | REQ-082..084, REQ-127..130 | Implemented |
| LLD_08_FAULT_INJECTION.md | REQ-132..134 | Mostly implemented; controller hooks partial |
| LLD_09_BOOTLOADER.md | REQ-078..081 | Implemented |
| LLD_10_PERFORMANCE_VALIDATION.md | REQ-116..123 | All perf targets asserted in `perf_validation_run_all` + CI gate. REQ-122 uses a measured nt=1/nt=8 ratio; REQ-123 splits into CPU-utilization + DRAM-RSS rows. |
| LLD_11_FTL_RELIABILITY.md | REQ-110..115, REQ-154..158 | Implemented except RAID-XOR (REQ-114) |
| LLD_12_REALTIME_SERVICES.md | REQ-074, REQ-085..088, REQ-171..178 | Implemented except IPC ring + resource sampling + P99 anomaly alert |
| LLD_13_HAL_ADVANCED.md | REQ-063, REQ-064, REQ-069 | AER + PCIe link state + PCI config space (byte-level 256B/4KB) all implemented |
| LLD_14_NOR_FLASH.md | REQ-053..056 | Implemented |
| LLD_15_PERSISTENCE_FORMAT.md | REQ-131 | Spec published (EN + CN) V1.1 — reconciled with shipping Media / Superblock / WAL layouts; panic-dump deferred to future work |
| LLD_17_POWER_LOSS_PROTECTION.md | REQ-139..146 | Implemented |
| LLD_18_QOS_DETERMINISM.md | REQ-147..153 | DWRR + latency monitor landed; per-NS caps + SLA enforcement partial |
| LLD_19_SECURITY_ENCRYPTION.md | REQ-159..165 | Implemented (all 7) |

---

## Next Steps

Current position: **core + enterprise features largely landed; polish and gap-closure in progress.**

Near-term priorities:
1. Core gaps: REQ-010 PRP/SGL, REQ-025 dispatch FSM, REQ-096 Format NVM OP%, REQ-048 data retention time acceleration, REQ-074 task binding.
2. Wire **real producers** for `system_monitor` (REQ-087) and the QoS hot-reconfig entry point into the NBD / vhost / exporter mains — hooks exist, callers still need to attach real sensors + config surfaces.
3. Retarget FTL / GC on top of the new `channel_worker` runtime to realise the async NAND-completion benefit in the hot path (scaffold and tests already in place).

Mid-term:
4. Broaden QoS coverage — per-NS IOPS/BW limits (REQ-148/149), P99 SLA enforcement (REQ-150), hot-reconfigure (REQ-151).
5. Implement SPSC IPC ring + periodic resource sampling (REQ-085, 087).
6. Schedule a published multi-day soak run (REQ-137) off the existing `stress_stability` harness.

Long-term:
7. **Phase 7 kernel module** — optional path to real `/dev/nvme`; gated on Phases 0–6 stability (now satisfied).

See `IMPLEMENTATION_ROADMAP.md` for the phase-indexed plan.
