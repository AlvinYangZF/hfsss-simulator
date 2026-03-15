# HFSSS Documentation Index

**Project**: High-Fidelity Full-Stack SSD Simulator
**Version**: V1.0
**Date**: 2026-03-14

---

## Overview

This directory contains all design documents for the HFSSS project. The documentation includes High-Level Design (HLD), Low-Level Design (LLD), and Test Design documents, totaling over 580,000 words in the original Chinese.

For the translation plan and progress, see [TRANSLATION_PLAN.md](./TRANSLATION_PLAN.md).

---

## English Documentation (Already Available)

| Document | Type | Description |
|----------|------|-------------|
| [ARCHITECTURE.md](./ARCHITECTURE.md) | Architecture | Complete system architecture overview with diagrams |
| [USER_GUIDE.md](./USER_GUIDE.md) | User Guide | Practical usage examples and API reference |
| [REQUIREMENT_COVERAGE.md](./REQUIREMENT_COVERAGE.md) | Analysis | Complete requirement coverage analysis (134 requirements) |
| [AGENT_TEAM.md](./AGENT_TEAM.md) | Process | Agent team structure and responsibilities |
| [IMPLEMENTATION_ROADMAP.md](./IMPLEMENTATION_ROADMAP.md) | Process | 7-phase implementation roadmap |
| [TRANSLATION_PLAN.md](./TRANSLATION_PLAN.md) | Process | Documentation translation plan |

---

## High-Level Design (HLD) Documents

| Document | English Title | Priority | Status |
|----------|---------------|----------|--------|
| [HLD_01_PCIE_NVMe_EMULATION.md](./HLD_01_PCIE_NVMe_EMULATION.md) | PCIe/NVMe Device Emulation HLD | P0 | ⚠️ Partial (Chinese) |
| [HLD_02_CONTROLLER_THREAD.md](./HLD_02_CONTROLLER_THREAD.md) | Controller Thread HLD | P0 | ❌ Chinese Only |
| [HLD_03_MEDIA_THREADS.md](./HLD_03_MEDIA_THREADS.md) | Media Threads HLD | P1 | ❌ Chinese Only |
| [HLD_04_HAL.md](./HLD_04_HAL.md) | Hardware Abstraction Layer HLD | P1 | ❌ Chinese Only |
| [HLD_05_COMMON_SERVICE.md](./HLD_05_COMMON_SERVICE.md) | Common Services HLD | P1 | ❌ Chinese Only |
| [HLD_06_APPLICATION.md](./HLD_06_APPLICATION.md) | Application Layer (FTL) HLD | P0 | ❌ Chinese Only |

**Total**: ~160,000 words

---

## Low-Level Design (LLD) Documents

| Document | English Title | Priority | Status |
|----------|---------------|----------|--------|
| [LLD_README.md](./LLD_README.md) | LLD Overview | P1 | ❌ Chinese Only |
| [LLD_01_PCIE_NVMe_EMULATION.md](./LLD_01_PCIE_NVMe_EMULATION.md) | PCIe/NVMe Device Emulation LLD | P0 | ❌ Chinese Only |
| [LLD_02_CONTROLLER_THREAD.md](./LLD_02_CONTROLLER_THREAD.md) | Controller Thread LLD | P0 | ❌ Chinese Only |
| [LLD_03_MEDIA_THREADS.md](./LLD_03_MEDIA_THREADS.md) | Media Threads LLD | P1 | ❌ Chinese Only |
| [LLD_04_HAL.md](./LLD_04_HAL.md) | Hardware Abstraction Layer LLD | P1 | ❌ Chinese Only |
| [LLD_05_COMMON_SERVICE.md](./LLD_05_COMMON_SERVICE.md) | Common Services LLD | P1 | ❌ Chinese Only |
| [LLD_06_APPLICATION.md](./LLD_06_APPLICATION.md) | Application Layer (FTL) LLD | P0 | ❌ Chinese Only |

**Total**: ~178,000 words

---

## Test Design Documents

| Document | English Title | Priority | Status |
|----------|---------------|----------|--------|
| [TEST_README.md](./TEST_README.md) | Test Design Overview | P1 | ❌ Chinese Only |
| [TEST_LLD_01_PCIE_NVMe_EMULATION.md](./TEST_LLD_01_PCIE_NVMe_EMULATION.md) | PCIe/NVMe Test Design | P2 | ❌ Chinese Only |
| [TEST_LLD_02_CONTROLLER_THREAD.md](./TEST_LLD_02_CONTROLLER_THREAD.md) | Controller Thread Test Design | P2 | ❌ Chinese Only |
| [TEST_LLD_03_MEDIA_THREADS.md](./TEST_LLD_03_MEDIA_THREADS.md) | Media Threads Test Design | P2 | ❌ Chinese Only |
| [TEST_LLD_04_HAL.md](./TEST_LLD_04_HAL.md) | HAL Test Design | P2 | ❌ Chinese Only |
| [TEST_LLD_05_COMMON_SERVICE.md](./TEST_LLD_05_COMMON_SERVICE.md) | Common Services Test Design | P2 | ❌ Chinese Only |
| [TEST_LLD_06_APPLICATION.md](./TEST_LLD_06_APPLICATION.md) | Application Layer Test Design | P2 | ❌ Chinese Only |

**Total**: ~196,000 words

---

## Root-Level Documents (Chinese)

| Document | English Title | Priority | Status |
|----------|---------------|----------|--------|
| `../SSD_Simulator_PRD.md` | SSD Simulator PRD | P0 | ❌ Chinese Only |
| `../DESIGN_REVIEW_REPORT.md` | Design Review Report | P2 | ❌ Chinese Only |
| `../PRD_REVIEW_REPORT.md` | PRD Review Report | P2 | ❌ Chinese Only |
| `../PROJECT_SUMMARY.md` | Project Summary | P1 | ❌ Chinese Only |

**Total**: ~75,000 words

---

## System Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        Host Linux OS                                      │
│   NVMe Driver │ blk-mq │ File System │ fio/nvme-cli │ User Apps        │
└────────────────────────────┬────────────────────────────────────────────┘
                             │ PCIe / NVMe Interface
┌────────────────────────────▼────────────────────────────────────────────┐
│                                                                           │
│                      HFSSS User-Space Daemon                             │
│                                                                           │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │           PCIe/NVMe Emulation (User-Space Only)                 │   │
│  │   - NVMe Admin Command Processing                                 │   │
│  │   - NVMe I/O Command Processing                                   │   │
│  │   - Queue Management (SQ/CQ)                                      │   │
│  └───────────────────────────┬─────────────────────────────────────┘   │
│                              │                                           │
│  ┌───────────────────────────▼─────────────────────────────────────┐   │
│  │                    Controller Thread                               │   │
│  │   - Command Arbitration                                           │   │
│  │   - I/O Scheduler                                                 │   │
│  │   - Write Buffer / Read Cache                                     │   │
│  │   - Channel Load Balancing                                        │   │
│  │   - Flow Control / QoS                                            │   │
│  └───────────────────────────┬─────────────────────────────────────┘   │
│                              │                                           │
│  ┌───────────────────────────▼─────────────────────────────────────┐   │
│  │              Application Layer (FTL)                               │   │
│  │   - Address Mapping (L2P/P2L)                                     │   │
│  │   - Block Management                                                │   │
│  │   - Garbage Collection (GC)                                        │   │
│  │   - Wear Leveling (WL)                                             │   │
│  │   - ECC / Error Handling                                           │   │
│  └───────────────────────────┬─────────────────────────────────────┘   │
│                              │                                           │
│  ┌───────────────────────────▼─────────────────────────────────────┐   │
│  │                Hardware Abstraction Layer (HAL)                    │   │
│  │   - NAND Driver                                                     │   │
│  │   - NOR Driver (Stub)                                               │   │
│  │   - PCI Management (Stub)                                           │   │
│  │   - Power Management (Stub)                                         │   │
│  └───────────────────────────┬─────────────────────────────────────┘   │
│                              │                                           │
│  ┌───────────────────────────▼─────────────────────────────────────┐   │
│  │                  Media Layer                                        │   │
│  │   - NAND Hierarchy (Channel→Chip→Die→Plane→Block→Page)          │   │
│  │   - Timing Model                                                    │   │
│  │   - EAT Engine (Earliest Available Time)                          │   │
│  │   - Bad Block Table (BBT)                                          │   │
│  │   - Reliability Modeling                                            │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                              │                                           │
│  ┌───────────────────────────▼─────────────────────────────────────┐   │
│  │                  Common Services                                    │   │
│  │   - Log System                                                      │   │
│  │   - Memory Pool                                                     │   │
│  │   - Message Queue                                                   │   │
│  │   - Semaphore / Mutex                                               │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                                                                           │
└─────────────────────────────────────────────────────────────────────────┘
```

**Note**: The current implementation is user-space only. Kernel module design is documented but deferred to optional Phase 7.

---

## Module Summary

### PCIe/NVMe Emulation Module
- **Status**: Stub structures implemented
- **Files**: `include/pcie/`, `src/pcie/`
- **Requirements**: REQ-001 to REQ-022 (22 requirements)
- **Coverage**: 0/22 (0%)

### Controller Thread Module
- **Status**: Partially implemented
- **Files**: `include/controller/`, `src/controller/`
- **Requirements**: REQ-023 to REQ-037 (15 requirements)
- **Coverage**: 10/15 (66.7%)

### Media Threads Module
- **Status**: Well implemented
- **Files**: `include/media/`, `src/media/`
- **Requirements**: REQ-038 to REQ-057 (20 requirements)
- **Coverage**: 15/20 (75%)

### HAL Module
- **Status**: Partially implemented
- **Files**: `include/hal/`, `src/hal/`
- **Requirements**: REQ-058 to REQ-069 (12 requirements)
- **Coverage**: 6/12 (50%)

### Common Services Module
- **Status**: Well implemented
- **Files**: `include/common/`, `src/common/`
- **Requirements**: REQ-070 to REQ-093 (24 requirements)
- **Coverage**: 13/24 (54.2%)

### Application Layer (FTL) Module
- **Status**: Partially implemented
- **Files**: `include/ftl/`, `src/ftl/`
- **Requirements**: REQ-094 to REQ-115 (22 requirements)
- **Coverage**: 7/22 (31.8%)

### Integration & Test
- **Status**: 362 unit tests passing
- **Files**: `tests/`
- **Requirements**: REQ-116 to REQ-138 (23 requirements)
- **Coverage**: 1/23 (4.3%)

---

## Implementation Status

**Overall Coverage**: 34.3% (46/134 requirements)

See [REQUIREMENT_COVERAGE.md](./REQUIREMENT_COVERAGE.md) for complete details.

---

## Next Steps

1. **Phase 1 (Current)**: Improve existing English documentation
2. **Phase 2**: Translate key HLD documents (HLD_01, HLD_02, HLD_06)
3. **Phase 3**: Translate key LLD documents (LLD_01, LLD_02, LLD_06)
4. **Phase 4**: Translate remaining HLD/LLD documents
5. **Phase 5**: Translate PRD and root-level documents
6. **Phase 6**: Translate TEST documents (optional)

See [IMPLEMENTATION_ROADMAP.md](./IMPLEMENTATION_ROADMAP.md) for the implementation plan.

---

## References

1. NVMe Specification: NVM Express Base Specification Revision 2.0c
2. NVMeVirt Paper: NVMeVirt: A Versatile Software-defined Virtual NVMe Device (USENIX FAST 2023)
3. FEMU Paper: The CASE of FEMU: Cheap, Accurate, Scalable and Extensible Flash Emulator (USENIX FAST 2018)
4. MQSim Paper: MQSim: A Framework for Enabling Realistic Studies of Modern Multi-Queue SSD Devices (USENIX FAST 2018)
