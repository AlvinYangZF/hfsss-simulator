# HFSSS Implementation Roadmap

**Version**: V2.0
**Date**: 2026-03-15
**Based on**: PRD V1.0, Requirements Matrix (134 requirements), REQUIREMENT_COVERAGE.md V1.2

---

## Overview

This roadmap tracks implementation of all 134 requirements across 8 phases.
All 134 requirements are classified as **P0** in the Requirements Matrix.
This document adds a practical **tier classification** (Critical / High / Medium / Optional)
based on dependency depth, user-visible impact, and version target.

| Tier | Criteria | Count |
|------|----------|-------|
| **Critical** | Core correctness, must ship for V2.0 | ~48 |
| **High** | Major features, V2.0 quality | ~35 |
| **Medium** | Validation & tooling, V2.0 polish | ~28 |
| **Optional** | Kernel module, V2.5 enterprise features | ~23 |

---

## Requirement Coverage Snapshot (as of 2026-04-19)

| Phase | Scope | Coverage (core ✅) | Coverage (incl. ⚠️) | Status |
|-------|-------|-------------------:|--------------------:|--------|
| Phase 0 – Foundation | REQ groups for NAND hierarchy, timing, EAT, BBT, RTOS primitives, basic FTL/Controller/HAL | 46/138 | — | ✅ Complete |
| Phase 1 – Core FTL | Cost-Benefit GC, wear leveling, WAF, incremental checkpoint + WAL, panic/assert | 60/138 | — | ✅ Complete |
| Phase 2 – HAL & Controller | Timeout mgmt, backpressure, QoS buckets, GC traffic ctl, NOR driver, namespace mgmt, PM states | 74/138 | — | ✅ Complete |
| Phase 3 – User-Space NVMe | Admin/IO/DSM command processing, doorbell, CQ, identify, DSM trim | 87/138 | — | ✅ Complete |
| Phase 4 – Boot/NOR/FTL Reliability/Trace | 6-stage boot, dual NOR slots, power mgmt, full NOR partitions, read/write retry, cmd state machine, flow control, trace ring | ~101/138 | — | ✅ Complete |
| Phase 5 – OOB & Tools | JSON-RPC Unix socket, /proc interface, `hfsss-ctrl` CLI, YAML config, latency monitor | ~114/138 | — | ✅ Complete |
| Phase 6 – Perf & Fault Inj. | `perf_validation` benchmark harness, `fault_inject` registry, DWRR, UPLP, thermal throttle, telemetry, security, multi-NS | **92/138 core + 30/40 enterprise** | 122/178 (✅) / 153/178 (✅+⚠️) | ✅ Complete (perf target enforcement remains ⚠️) |
| Phase 7 – Kernel Module | `/dev/nvme` block device, real MSI-X / DMA / IOMMU, nvme-cli / fio on raw NVMe | 0/12 (intentional) | — | 🔲 Optional (deferred) |

> **Note**: Core counts the 138 REQs from REQ-001..REQ-138; partials are visible in `REQUIREMENT_COVERAGE.md`. Phase 6 also covers the 40 Enterprise V3.0 REQs (UPLP / QoS / T10 PI / Security / Multi-NS / Thermal+Telemetry), most of which are implemented or partially implemented. Phase 7 is the only remaining scope, and is explicitly optional per the user-space-first architecture decision.

---

## Version Target Reference

| Version | Commitment | Scope | Status |
|---------|-----------|-------|--------|
| V1.0 | Alpha — core simulation | Foundation + core FTL + HAL + Controller + user-space NVMe (REQ-001..115 core subset) | ✅ Complete |
| V1.5 | Beta — full FTL + persistence + boot/power | L2P checkpoint + WAL + UPLP + dual-slot firmware + recovery | ✅ Complete |
| V2.0 | Release — OOB, reliability, fault-inj, perf harness | JSON-RPC + `hfsss-ctrl` + YAML + fault-inject + `perf_validation` + thermal/telemetry | ✅ Complete (target enforcement ⚠️) |
| V3.0 | Enterprise | UPLP / Multi-NS / Security / Thermal / Telemetry / DWRR QoS (REQ-139..178) | ✅ Mostly complete (30/40 ✅, 9/40 ⚠️) |
| V2.5 (optional) | Kernel module | `hfsss_nvme.ko`, real `/dev/nvme`, nvme-cli / fio on raw NVMe | 🔲 Deferred (Phase 7) |

---

## Dependency Map

```
Foundation (P0) ─────────────────────────────────────────────┐
  NAND Hierarchy  Timing Model  Memory Mgmt  RTOS Primitives  │
       │               │              │             │           │
       └───────────────┴──────────────┴─────────────┘           │
                             │                                   │
              ┌──────────────┴──────────────┐                   │
        FTL Layer (P1)               HAL Layer (P2)             │
  Mapping / GC / WL / ECC       NAND/NOR Drivers               │
              │                         │                       │
              └─────────┬───────────────┘                       │
                        │                                       │
              Controller Thread (P2)                           │
          Arbiter / Scheduler / FlowCtrl                       │
                        │                                       │
              User-Space NVMe (P3)                             │
          Admin/IO Cmds / CQ / Doorbell                        │
                        │                                       │
         ┌──────────────┴──────────────────┐                   │
   Boot & Reliability (P4)       OOB & Tools (P5)              │
Bootloader / Power / Retry    Socket / CLI / Config            │
         │                              │                       │
         └──────────────┬───────────────┘                       │
                        │                                       │
         Perf & Fault Injection (P6)                           │
    Benchmarks / FaultInj / Stability                          │
                        │                                       │
         ┌──────────────┘                                       │
   Kernel Module (P7, Optional)                                │
  Real /dev/nvme / nvme-cli / fio  ◄──────────────────────────┘
```

---

## Completed Phases

### Phase 0: Foundation ✅

**Requirements**: REQ-038, REQ-039, REQ-040, REQ-041, REQ-050, REQ-057, REQ-058, REQ-059, REQ-068, REQ-070, REQ-071, REQ-072, REQ-073, REQ-076, REQ-077, REQ-089, REQ-092, REQ-094, REQ-095, REQ-097, REQ-098, REQ-100, REQ-101, REQ-102, REQ-103, REQ-105, REQ-136, REQ-138 *(+ 18 structure/interface reqs)*

**Deliverables**:
- Common services: log, mempool, msgqueue, semaphore, mutex
- Media layer: NAND hierarchy, timing model, EAT engine, BBT, reliability model
- HAL: NAND driver (sync), NOR/PCI/power stubs
- FTL: L2P/P2L mapping, block state machine, Greedy GC, basic wear leveling
- Controller: arbiter, scheduler, write buffer, read cache, channel, token-bucket flow control
- PCIe/NVMe: register structures (stub)
- **362 tests, 0 failures**

**Coverage**: 46/134 (34.3%)

---

### Phase 1: Core FTL & Media ✅

**Requirements added**: REQ-051, REQ-052, REQ-090, REQ-093, REQ-104, REQ-106, REQ-107, REQ-108 *(~14 req-rows)*

**Deliverables**:
- Cost-Benefit GC victim selection algorithm (REQ-104)
- Static wear leveling with periodic block shuffling (REQ-108)
- WAF (Write Amplification Factor) calculation and monitoring (REQ-106)
- Dynamic wear leveling with erase-count-based prioritization (REQ-107)
- Incremental checkpoint + WAL persistence to host filesystem (REQ-051)
- Recovery from checkpoint with WAL replay (REQ-052)
- Log persistence to NOR Flash (REQ-093)
- Comprehensive ASSERT/Panic handling with dump file (REQ-090)

**Coverage**: 60/134 (44.8%)

---

### Phase 2: HAL & Controller ✅

**Requirements added**: REQ-025, REQ-026, REQ-028, REQ-029, REQ-032, REQ-033, REQ-034, REQ-035, REQ-060, REQ-061, REQ-062, REQ-065, REQ-066, REQ-067, REQ-086 *(~14 req-rows)*

**Deliverables**:
- Command timeout management with per-command timer (REQ-025)
- Deadline I/O scheduler (REQ-026)
- Full backpressure mechanism (write buffer >90% → halt new writes) (REQ-033)
- QoS latency guarantees for Urgent/High/Medium/Low priorities (REQ-034)
- GC traffic control (max 30% bandwidth to GC) (REQ-035)
- NOR driver full implementation (REQ-060, REQ-061)
- NVMe command completion submission via HAL (REQ-062)
- Namespace management (dynamic NSID allocation) (REQ-065)
- NVMe power state transitions PS0–PS4 (REQ-066, REQ-067)
- Memory management with mmap + hugetlb (REQ-032)
- Basic watchdog (per-task feed, Hang detection) (REQ-086)

**Coverage**: 74/134 (55.2%)

---

### Phase 3: User-Space NVMe Interface ✅

**Requirements added**: REQ-007, REQ-008, REQ-011, REQ-015, REQ-016, REQ-017 *(~13 req-rows)*

**Deliverables**:
- NVMe doorbell register processing (REQ-007)
- Admin Queue (QID=0) creation and management (REQ-008)
- Completion Queue (CQ) processing and CQE construction (REQ-011)
- Admin command set: Identify, Create/Delete SQ/CQ, Get Log Page, Set/Get Features (REQ-015)
- I/O command set: Read, Write, Flush (REQ-016, REQ-017)
- User-space NVMe library with direct function-call integration
- **431+ tests, 0 failures**

**Architecture note**: Phases 0–3 implement a user-space-only library. The host Linux NVMe driver cannot detect this device directly. Phase 7 (optional) adds the kernel module for real `/dev/nvme` support.

**Coverage**: 87/134 (64.9%)

---

## Phase 4–7 Scope & Status

---

### Phase 4: Boot, Power & Core Reliability

**Goal**: Complete the firmware lifecycle (boot → run → shutdown → recover) and close critical reliability gaps
**Status**: ✅ Complete — all four tracks landed (boot/power, full NOR, FTL reliability, common-service trace)
**Design References**: LLD_09_BOOTLOADER.md, LLD_06_APPLICATION.md

#### Track A — Bootloader & Power Services (Critical, V1.5)
*Can run in parallel with Track B*

| Task | REQ-ID | Tier | Description |
|------|--------|------|-------------|
| 6-stage boot sequence | REQ-078 | Critical | Phase 0–5: HW init → POST → metadata load → ctrl init → NVMe init → ready; simulate 3–8s delay |
| Dual firmware slot (NOR Slot A/B) | REQ-079 | Critical | Select slot with valid CRC; atomic slot swap on firmware update |
| Boot type detection | REQ-080 | Critical | Detect first-boot / normal / abnormal from SysInfo partition |
| Normal power-down service | REQ-081 | Critical | Stop I/O → flush write buffer → update L2P checkpoint → set CSTS.SHST=0x02 |
| Abnormal power-down (SIGTERM/WAL) | REQ-081 | Critical | atexit handler → write crash marker → flush WAL |
| WAL recovery on abnormal restart | REQ-080 | Critical | Replay WAL entries; verify data integrity after abnormal shutdown |
| SMART power cycle tracking | REQ-080 | High | Increment power_cycles and unsafe_shutdowns on each boot |

#### Track B — NOR Flash Full Implementation (High, V1.5)
*Can run in parallel with Track A*

| Task | REQ-ID | Tier | Description |
|------|--------|------|-------------|
| NOR command set | REQ-053, REQ-055 | High | Read, Program, Erase operations on NOR image file |
| NOR partition layout | REQ-054 | High | Bootloader(4MB) / SlotA(64MB) / SlotB(64MB) / Config(8MB) / BBT(8MB) / Log(16MB) / SysInfo(4MB) |
| NOR data persistence | REQ-056 | High | Persist BBT, P/E counts, firmware metadata to NOR image on disk |

#### Track C — FTL Reliability (Critical, V1.5–V2.0)
*Depends on Track A (power-down ensures WAL is available for retry context)*

| Task | REQ-ID | Tier | Description |
|------|--------|------|-------------|
| Command state machine | REQ-110 | Critical | RECEIVED → PARSING → L2P_LOOKUP → NAND_QUEUED → EXECUTING → ECC_CHECK → COMPLETE |
| Read Retry with voltage offset | REQ-111 | Critical | Adjust Vread bias; up to 15 retries; soft-decision LDPC before retry |
| Write Retry + Write Verify | REQ-112 | Critical | Retry failed program; allocate backup block on repeated failure |
| Wear monitoring & SMART alerts | REQ-109 | High | SMART Available Spare low-warning when max P/E > 80% of PE_CYCLE_LIMIT |
| NVMe error codes (complete) | REQ-115 | High | DNR bit, Error Log Page population, SMART media error counter |

#### Track D — Common Services (High, V2.0)
*Can run in parallel with Tracks A–C*

| Task | REQ-ID | Tier | Description |
|------|--------|------|-------------|
| System resource monitoring | REQ-087 | High | CPU usage per thread, memory partition usage, NAND channel queue depth |
| Performance anomaly detection | REQ-088 | High | P99.9 latency alert; temperature model T = T_ambient + IOPS×c_i + BW×c_b |
| Debug trace ring buffer | REQ-091 | High | 100K-entry lock-free command trace; JSON Lines export; per-channel NAND trace |

**Phase 4 Exit Criteria**:
- [x] `hfsss_init()` executes all 6 boot phases with logged timing per phase (`src/common/boot.c`)
- [x] Normal shutdown: L2P checkpoint written, CSTS.SHST=0x02 confirmed in test (`tests/test_power_cycle.c`)
- [x] Abnormal shutdown: WAL file written, crash marker set; next boot replays WAL correctly
- [x] Read Retry: injected ECC error → up to 15 retries (`READ_RETRY_VOLTAGE_OFFSETS` loop in `src/ftl/ftl.c`)
- [x] SMART Available Spare drops below threshold → critical_warning bit set (`ftl_rel_check_health`)
- [x] All new tests pass (regression at 0 failures as of PR #86)

**Residual follow-ups**: REQ-115 Error Log Page population and REQ-087 periodic resource sampling remain ⚠️/❌; see `REQUIREMENT_COVERAGE.md` for the gap list.

---

### Phase 5: OOB Management & Tools

**Goal**: Expose full observability and control interfaces; complete the product tool chain
**Status**: ✅ Complete — JSON-RPC socket, `/proc` interface, `hfsss-ctrl` CLI, YAML config, latency monitor all landed
**Design References**: LLD_07_OOB_MANAGEMENT.md

#### Track A — OOB Backend (Critical, V2.0)
*Prerequisite for Tracks B and C*

| Task | REQ-ID | Tier | Description |
|------|--------|------|-------------|
| Unix Domain Socket JSON-RPC 2.0 server | REQ-082 | Critical | Listen on `/var/run/hfsss/hfsss.sock`; max 16 clients; epoll-based |
| OOB management functions | REQ-083 | Critical | status.get / smart.get / perf.get / channel.get / die.get / config.set / gc.trigger / snapshot.save / log.get |
| SMART/Health Log Page (0x02) | REQ-084 | Critical | All 16 fields; temperature model integration; warn/crit threshold triggers |
| Latency histogram (P50/P99/P99.9) | REQ-088 | High | Exponential buckets; per-command-type breakdown |

#### Track B — Product Interfaces (High, V2.0)
*Depends on Track A (JSON-RPC backend)*

| Task | REQ-ID | Tier | Description |
|------|--------|------|-------------|
| /proc/hfsss/ filesystem interface | REQ-128 | High | status / config / perf_counters / channel_stats / ftl_stats / latency_hist / version (Linux only) |
| hfsss-ctrl CLI tool | REQ-129 | High | Thin wrapper over JSON-RPC socket; all commands listed in LLD_07 §8 |
| YAML configuration file | REQ-130 | High | Parse `/etc/hfsss/hfsss.yaml`; validate all sections; default values |
| Persistence format documentation | REQ-131 | Medium | NAND data file header spec; L2P checkpoint format; WAL record format |

#### Track C — Debug & IPC (Medium, V2.0)
*Can run in parallel with Track B*

| Task | REQ-ID | Tier | Description |
|------|--------|------|-------------|
| Debug trace export via OOB | REQ-091 | High | `trace.enable` / `trace.dump` JSON Lines via OOB (backend from Phase 4 Track D) |
| Inter-core communication (IPC) | REQ-085 | Medium | SPSC Ring Buffer between firmware threads; eventfd notification; shared-memory large payloads |
| Channel striping (Round-Robin) | REQ-099 | Medium | LPN → channel assignment via round-robin or hash; modifiable via `config.set` |
| Dataset Management / Trim | REQ-018 | Medium | NVMe Deallocate command → FTL invalidate LPN range |

**Phase 5 Exit Criteria**:
- [ ] `echo '{"jsonrpc":"2.0","method":"status.get","params":{},"id":1}' | nc -U /var/run/hfsss/hfsss.sock` returns valid JSON
- [x] `hfsss-ctrl smart` displays SMART fields including temperature (via OOB socket)
- [x] Trace dump via `HFSSS_TRACE_DUMP` env variable (TRACE=1 build); JSON / analyze path in `scripts/qemu_blackbox/phase_a/analyze_trace.py`
- [x] YAML config file loads on startup (`src/common/hfsss_config.c`)
- [x] Test count well past 600 (regression ~2,759 assertions as of PR #86)

**Residual follow-ups**: REQ-126 (direct `/dev/nvme` for fio) requires Phase 7 kernel module; currently satisfied indirectly via QEMU → NBD.

---

### Phase 6: Performance Validation & Fault Injection

**Goal**: Verify all performance targets; implement fault injection framework; achieve production stability
**Status**: ✅ Complete — harness + framework landed; strict target enforcement remaining (see below)
**Design References**: LLD_08_FAULT_INJECTION.md, LLD_10_PERFORMANCE_VALIDATION.md

#### Track A — Performance Validation (Critical, V2.0)
*Can run in parallel with Tracks B and C*

| Task | REQ-ID | Tier | Description |
|------|--------|------|-------------|
| Built-in benchmark engine | REQ-116, REQ-117 | Critical | Sequential/random/mixed/Zipfian load generator; QD 1–128; collect latency histogram |
| Random read IOPS target (600K–1M) | REQ-116 | Critical | Validate at QD=32, 4KB; report pass/fail vs. target |
| Random write IOPS target (150K–300K) | REQ-117 | Critical | Validate at QD=32, 4KB |
| Mixed R/W IOPS target (250K, 70/30) | REQ-118 | Critical | Validate at QD=32 |
| Sequential bandwidth (6.5 GB/s read, 3.5 GB/s write) | REQ-119 | Critical | Validate at 128KB block size |
| Latency targets (P50 ≤100µs, P99 ≤150µs, P99.9 ≤500µs) | REQ-120 | Critical | Validate at QD=1 |
| NAND timing accuracy (<5% error vs. reference) | REQ-121 | High | tR / tPROG / tERS vs. ONFI reference; N=1000 samples per operation |
| Scalability (≥70% parallel efficiency at 16 threads) | REQ-122 | High | Amdahl serial fraction ≤ 6% |
| CPU ≤ 50%, DRAM within provisioned budget | REQ-123 | Medium | Resource utilization profiling under peak load |

#### Track B — Fault Injection Framework (High, V2.5)
*Depends on OOB `fault.inject` interface from Phase 5*

| Task | REQ-ID | Tier | Description |
|------|--------|------|-------------|
| NAND fault registry (O(log N) hot-path) | REQ-132 | High | `fault_registry` with sorted index; `type_present` bitmask for lock-free fast exit |
| Bad block / read / program / erase error injection | REQ-132 | High | Per (ch, chip, die, plane, block, page) targeting; wildcard support |
| Bit flip injection | REQ-132 | High | XOR mask applied to page buffer in media layer |
| Read disturb storm simulation | REQ-132 | High | Probability amplifier per block after threshold read count |
| Data retention aging acceleration | REQ-132 | Medium | Aging factor multiplier on retention decay model |
| Power fault injection (idle / mid-write / mid-GC / mid-checkpoint) | REQ-133 | High | `_exit(1)` after writing crash marker; verify WAL recovery |
| Controller fault injection (Panic / pool exhaustion / timeout storm) | REQ-134 | Medium | Trigger existing Panic flow or exhaust named mempool |
| One-shot vs. sticky fault modes | REQ-132 | Medium | `FAULT_PERSIST_ONE_SHOT` auto-clears after first hit |
| Active fault registry list/clear via OOB | REQ-132 | Medium | `fault.list` and `fault.clear` JSON-RPC methods |

#### Track C — System Reliability (High, V2.0)
*Can run in parallel with Tracks A and B*

| Task | REQ-ID | Tier | Description |
|------|--------|------|-------------|
| Multi-level flow control | REQ-113 | High | Per-namespace token bucket + NAND channel-level queuing depth limit |
| RAID-like metadata redundancy | REQ-114 | Medium | Dual-copy L2P (DRAM + file); BBT dual-mirror in NOR; consistency check on boot |
| CPU thread affinity + SCHED_FIFO | REQ-074, REQ-075 | Medium | `pthread_setaffinity_np` binding; real-time priority for NVMe dispatch threads |
| Async NAND completion notifications | REQ-045 | Medium | Lock-free completion queue; batch completion processing |
| Data integrity verification | REQ-136 | Critical | End-to-end md5sum: write N×4KB → read back → verify match; 100% pass rate required |
| 72-hour stability test | REQ-137 | High | Continuous 50%-load R/W; no crashes, no data corruption, no memory leaks |
| ThreadSanitizer clean run | REQ-138 | High | `-fsanitize=thread`; zero reported data races |
| AddressSanitizer clean run | REQ-135 | High | `-fsanitize=address`; zero heap errors; MTBF proxy test |
| `perf_validation_run_all` report | all perf reqs | Critical | Structured JSON + text report; non-zero exit if any Critical requirement fails |

**Phase 6 Exit Criteria**:
- [ ] `perf_validation_run_all` exits 0 — harness present, strict pass/fail thresholds for REQ-116..120 not yet asserted
- [x] `fault_inject` registry with NAND + power-fail hooks (`src/common/fault_inject.c`, `tests/test_fault_inject.c`)
- [x] Power fault injection → WAL replay verified (`src/common/uplp.c` + `tests/test_uplp.c`)
- [ ] Published 72-hour stability run — `tests/stress_stability.c` harness exists; long-form run not published
- [x] ThreadSanitizer clean (PR #85)
- [x] AddressSanitizer clean (PR #86)

**Residual follow-ups**: perf target enforcement (`perf_validation_run_all` pass/fail wiring), long-haul stability publish, REQ-114 die-level XOR parity, REQ-075 SCHED_FIFO, REQ-045 async completion notifications, REQ-087/088 periodic resource + P99.9 anomaly alert.

---

### Phase 7: Kernel Module (Optional)

**Goal**: Present a real `/dev/nvme` block device to the host Linux kernel, enabling nvme-cli and fio
**Status**: 🔲 Optional — gates on Phases 0–6 stability
**Estimated Duration**: 4–6 weeks
**Prerequisite**: Phases 0–6 complete and stable; Linux kernel ≥ 5.15 build environment

#### Track A — Kernel PCIe/NVMe Driver

| Task | REQ-ID | Tier | Description |
|------|--------|------|-------------|
| Linux PCI endpoint function driver skeleton | REQ-022 | Optional | `hfsss_nvme.ko`; PCI probe/remove hooks |
| PCI config space emulation | REQ-001, REQ-002 | Optional | Full Type 0 header; PCIe Capabilities chain (PM, MSI-X, PCIe) |
| BAR0 MMIO mapping | REQ-003 | Optional | Map NVMe register space into kernel address space |
| NVMe CAP/VS/CC/CSTS registers | REQ-004, REQ-005, REQ-006 | Optional | Real MMIO with read/write handlers; CSTS.RDY transitions |
| I/O Queue dynamic creation | REQ-009 | Optional | Create/Delete SQ/CQ via Admin commands in kernel context |
| MSI-X Table + interrupt delivery | REQ-012, REQ-013 | Optional | `pci_alloc_irq_vectors`; per-CQ MSI-X vector |
| Interrupt coalescing | REQ-014 | Optional | Aggregate completions before raising interrupt |

#### Track B — DMA & Data Path

| Task | REQ-ID | Tier | Description |
|------|--------|------|-------------|
| PRP list parsing engine | REQ-019 | Optional | PRP1 / PRP2 / PRP list traversal in kernel |
| Data copy via DMA | REQ-020 | Optional | `dma_map_page` / `dma_unmap_page` for host↔simulator transfers |
| IOMMU support | REQ-021 | Optional | `iommu_domain_alloc`; DMA remapping |
| Kernel-user space Ring Buffer | REQ-022 | Optional | mmap shared memory between `hfsss_nvme.ko` and user-space simulator |

#### Track C — Host Tool Integration

| Task | REQ-ID | Tier | Description |
|------|--------|------|-------------|
| `/dev/nvme0n1` block device | REQ-124 | Optional | Visible to `lsblk`, `fdisk`, `mkfs` |
| NVMe Trim (Dataset Management) | REQ-018 | Optional | Kernel Deallocate → user-space FTL trim |
| nvme-cli full compatibility | REQ-125 | Optional | `nvme id-ctrl`, `nvme smart-log`, `nvme format`, etc. |
| fio with io_uring / direct I/O | REQ-126 | Optional | `ioengine=io_uring`, `direct=1`, `iodepth=128`, `numjobs=32` |

**Phase 7 Exit Criteria**:
- [ ] `lspci | grep -i nvme` shows HFSSS device
- [ ] `nvme list` lists `/dev/nvme0n1`
- [ ] `nvme smart-log /dev/nvme0` returns valid SMART data
- [ ] `fio --ioengine=io_uring --direct=1 --rw=randread --bs=4k --iodepth=32 --filename=/dev/nvme0n1` completes without error

**Coverage target**: 134/134 (100%)

---

## Parallelization Guide

### Phase 4 Parallel Tracks

```
Week 1-2:
  ├── Track A: Bootloader (REQ-078, REQ-079)
  ├── Track B: NOR Flash full (REQ-053–056)
  └── Track D: Debug trace ring (REQ-091) ← independent

Week 2-3:
  ├── Track A: Power services (REQ-080, REQ-081)  ← needs WAL from Phase 1
  ├── Track C: Read Retry (REQ-111)  ← needs command state machine (REQ-110) first
  └── Track D: Resource monitoring (REQ-087, REQ-088)

Week 3-4:
  ├── Track C: Write Retry (REQ-112)
  ├── Track C: Wear monitoring (REQ-109)
  └── Integration + new tests
```

### Phase 5 Parallel Tracks

```
Week 1:
  └── Track A: OOB socket server + status/smart/perf handlers (REQ-082, REQ-083, REQ-084)

Week 2:
  ├── Track B: /proc interface (REQ-128)  ← needs Track A
  ├── Track B: YAML config (REQ-130)  ← independent
  └── Track C: IPC Ring Buffer (REQ-085)  ← independent

Week 3:
  ├── Track B: hfsss-ctrl CLI (REQ-129)  ← needs OOB socket
  ├── Track C: Channel striping (REQ-099)  ← independent
  └── Track C: Trim/Deallocate (REQ-018)  ← independent

Week 4:
  └── Integration + persistence format doc (REQ-131) + new tests
```

### Phase 6 Parallel Tracks

```
Weeks 1-3:
  ├── Track A: Benchmark engine + IOPS/BW targets (REQ-116–119)
  ├── Track B: Fault registry + NAND fault injection (REQ-132)
  └── Track C: Multi-level flow control (REQ-113)

Weeks 3-5:
  ├── Track A: Latency targets + accuracy + scalability (REQ-120–122)
  ├── Track B: Power + controller fault injection (REQ-133, REQ-134)
  └── Track C: TSan/ASan runs + 72h stability setup (REQ-137, REQ-138)

Weeks 5-6:
  ├── Track A: Resource utilization profiling (REQ-123) + report generation
  ├── Track B: OOB fault.list / fault.clear integration
  └── Track C: MTBF proxy test + final data integrity sweep
```

---

## Open Items (Deferred or Needs Clarification)

| REQ-ID | Description | Current State | Decision |
|--------|-------------|---------------|----------|
| REQ-006 | NVMe controller initialization (real MMIO) | ❌ | Deferred to Phase 7 (kernel only) |
| REQ-009 | I/O Queue dynamic creation (kernel) | ❌ | Deferred to Phase 7 |
| REQ-010 | PRP/SGL support | ⚠️ Partial | Full PRP parsing deferred to Phase 7; user-space uses buffer pointers |
| REQ-013 | MSI-X interrupt delivery | ❌ | User-space limitation; deferred to Phase 7 |
| REQ-014 | Interrupt coalescing | ❌ | Deferred to Phase 7 |
| REQ-021 | IOMMU support | ❌ | Deferred to Phase 7 |
| REQ-022 | Kernel-user space Ring Buffer | ❌ | Deferred to Phase 7 |
| REQ-024 | Command dispatch (full state machine) | ⚠️ Partial | Command state machine (REQ-110) in Phase 4 fills this gap |
| REQ-031 | Command slot management (65535 in-flight limit) | ❌ | Can add in Phase 5 or 6; low risk until kernel integration |
| REQ-042 | Multi-plane concurrency (full) | ⚠️ Partial | EAT tracks per-plane; full ZNS-compatible multi-plane in Phase 6 |
| REQ-043 | Full ONFI command set | ⚠️ Partial | Cache Read, Suspend/Resume, Reset, Read ID — Phase 6 Track C |
| REQ-044 | Per-channel command queue threads | ⚠️ Partial | Full threading deferred to Phase 6 Track C |
| REQ-048 | Data retention time acceleration | ⚠️ Partial | 1000:1 time ratio — Phase 6 alongside fault injection |
| REQ-063 | Async event management (ASYNC EVENT) | ❌ | Needs NVMe Admin command → controller wakeup path; Phase 6 |
| REQ-064 | PCIe link state (L0/L1/L2, hot-reset, FLR) | ❌ | Deferred to Phase 7 (kernel only) |
| REQ-096 | OP configurable via Format NVM | ⚠️ Partial | Format NVM command not yet wired to FTL OP ratio; Phase 5 |
| REQ-114 | Cross-Die XOR parity (RAID-5) | ❌ | Medium priority; Phase 6 Track C; only metadata redundancy in V2.0 |

---

## Risk Register

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Performance targets not met (REQ-116–120) | Medium | High | Add multi-threading in Phase 6 before benchmarking; identify bottleneck with profiling |
| WAL recovery correctness | Low | Critical | Property-based tests: random crash points, verify invariants on recovery |
| Kernel module complexity (Phase 7) | High | Medium | Phases 0–6 are complete without kernel; Phase 7 is optional |
| ThreadSanitizer data races | Medium | High | Run TSan in CI from Phase 4 onwards; fix races incrementally |
| Test coverage drift (REQ-ID → test mapping) | Medium | Medium | Each Phase exit criterion lists explicit test names; enforce in CI |
| NOR Flash partition layout changes | Low | Medium | All partition offsets defined in a single header; single point of change |

---

## Requirement → Phase Cross-Reference

| REQ-ID | Phase | REQ-ID | Phase | REQ-ID | Phase |
|--------|-------|--------|-------|--------|-------|
| REQ-001–005 | 0 ✅ | REQ-046–049 | 0 ✅ | REQ-091 | 4 |
| REQ-006 | 7 | REQ-050 | 0 ✅ | REQ-092–093 | 0/1 ✅ |
| REQ-007–008 | 3 ✅ | REQ-051–052 | 1 ✅ | REQ-094–098 | 0 ✅ |
| REQ-009 | 7 | REQ-053–056 | 4 | REQ-099 | 5 |
| REQ-010 | 7 (partial) | REQ-057–059 | 0 ✅ | REQ-100–103 | 0 ✅ |
| REQ-011 | 3 ✅ | REQ-060–062 | 2 ✅ | REQ-104–108 | 1 ✅ |
| REQ-012 | 0 ✅ | REQ-063 | 6 | REQ-109–112 | 4 |
| REQ-013–014 | 7 | REQ-064 | 7 | REQ-113 | 6 |
| REQ-015–017 | 3 ✅ | REQ-065–067 | 2 ✅ | REQ-114 | 6 |
| REQ-018 | 5 | REQ-068–069 | 0 ✅ | REQ-115 | 4 |
| REQ-019–022 | 7 | REQ-070–073 | 0 ✅ | REQ-116–123 | 6 |
| REQ-023–030 | 0–2 ✅ | REQ-074–075 | 6 | REQ-124–126 | 7 |
| REQ-031 | 6 | REQ-076–077 | 0/2 ✅ | REQ-127–131 | 5 |
| REQ-032–036 | 2 ✅ | REQ-078–081 | 4 | REQ-132–134 | 6 |
| REQ-037 | 0 ✅ | REQ-082–084 | 5 | REQ-135–138 | 6 |
| REQ-038–045 | 0 (partial) | REQ-085 | 5 | | |
| | | REQ-086–090 | 2/1 ✅ | | |
