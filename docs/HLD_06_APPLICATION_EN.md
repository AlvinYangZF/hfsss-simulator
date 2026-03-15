# HFSSS High-Level Design Document

**Document Name**: Application Layer (FTL) HLD
**Document Version**: V1.0
**Date**: 2026-03-14
**Design Phase**: V1.0 (Alpha)

---

## Implementation Status

**Design Document**: Describes a comprehensive FTL with advanced features
**Actual Implementation**: Partial implementation with core FTL features

**Coverage Status**: 7/22 requirements implemented for this module (31.8%)

See [REQUIREMENT_COVERAGE.md](./REQUIREMENT_COVERAGE.md) for complete details.

---

## Revision History

| Version | Date | Author | Description |
|---------|------|--------|-------------|
| V0.1 | 2026-03-08 | Architecture Team | Initial draft |
| V1.0 | 2026-03-08 | Architecture Team | Official release |
| EN-V1.0 | 2026-03-14 | Translation Agent | English translation with implementation notes |

---

## Table of Contents

1. [Module Overview](#1-module-overview)
2. [Requirements Review](#2-requirements-review)
3. [System Architecture](#3-system-architecture)
4. [Detailed Design](#4-detailed-design)
5. [Interface Design](#5-interface-design)
6. [Data Structures](#6-data-structures)
7. [Flow Diagrams](#7-flow-diagrams)
8. [Performance Design](#8-performance-design)
9. [Error Handling](#9-error-handling)
10. [Test Design](#10-test-design)

---

## 1. Module Overview

### 1.1 Module Positioning

The Application Layer is the core layer of SSD firmware algorithms, including address mapping management, NAND block address organization management, read/program/erase command management, garbage collection, IO flow control, data redundancy backup, and command error handling. This ensures the firmware can run stably under any host traffic and IO pattern.

### 1.2 Module Responsibilities

This module is responsible for:
- **Flash Translation Layer** — Address mapping management (address mapping architecture, mapping table design, over-provisioning OP, write operation flow, read operation flow, striping strategy)
- **NAND Block Address Organization Management** (Block state machine, Block metadata, Current Write Block management, free block pool management)
- **Garbage Collection** (GC trigger strategy, Victim Block selection algorithm, GC execution flow, GC concurrency optimization, write amplification analysis)
- **Wear Leveling** (dynamic wear leveling, static wear leveling, wear monitoring and alerts)
- **Read/Program/Erase Command Management** (command state machine, Read Retry mechanism, Write Retry mechanism, Write Verify)
- **IO Flow Control** (multi-level flow control, host IO rate limiting, GC/WL bandwidth quota, NAND channel-level flow control, write buffer flow control)
- **Data Redundancy Backup** (LDPC ECC, cross-Die parity, critical metadata redundancy, Write Buffer power-loss protection)
- **Command Error Handling** (NVMe error status codes, error handling flow, recoverable errors, unrecoverable data errors, NAND device errors, command timeout handling, firmware internal errors)

---

## 2. Requirements Review

### 2.1 Requirements Traceability Matrix

| Requirement ID | Description | Priority | Version | Implementation Status |
|----------------|-------------|----------|---------|----------------------|
| REQ-094 | L2P Address Mapping | P0 | V1.0 | ✅ Implemented |
| REQ-095 | P2L Reverse Mapping | P0 | V1.0 | ✅ Implemented |
| REQ-096 | Page-Level Mapping | P0 | V1.0 | ✅ Implemented |
| REQ-097 | Mapping Table Caching | P1 | V1.5 | ❌ Not Implemented |
| REQ-098 | Over-Provisioning (OP) | P0 | V1.0 | ✅ Implemented |
| REQ-099 | Striping Across Channels | P0 | V1.0 | ❌ Not Implemented |
| REQ-100 | Block State Machine | P0 | V1.0 | ✅ Implemented |
| REQ-101 | Current Write Block (CWB) | P0 | V1.0 | ✅ Implemented |
| REQ-102 | Free Block Pool | P0 | V1.0 | ✅ Implemented |
| REQ-103 | Garbage Collection (GC) | P0 | V1.0 | ✅ Implemented |
| REQ-104 | Cost-Benefit GC Algorithm | P0 | V1.0 | ❌ Not Implemented |
| REQ-105 | Victim Block Selection | P0 | V1.0 | ✅ Implemented |
| REQ-106 | Write Amplification (WAF) Calculation | P0 | V1.0 | ❌ Not Implemented |
| REQ-107 | Read Retry Mechanism | P1 | V2.0 | ❌ Not Implemented |
| REQ-108 | Static Wear Leveling | P0 | V1.0 | ❌ Not Implemented |
| REQ-109 | Wear Monitoring & Alerts | P1 | V2.0 | ❌ Not Implemented |
| REQ-110 | ECC Error Handling | P1 | V2.0 | ✅ Implemented |
| REQ-111 | NVMe Error Handling | P0 | V1.0 | ❌ Not Implemented |
| REQ-112 | Multi-Level Flow Control | P1 | V2.0 | ❌ Not Implemented |
| REQ-113 | QoS Guarantees | P0 | V1.0 | ❌ Not Implemented |
| REQ-114 | Metadata Redundancy | P1 | V2.0 | ❌ Not Implemented |
| REQ-115 | Power-Loss Protection | P0 | V1.0 | ❌ Not Implemented |

### 2.2 Key Performance Requirements

| Metric | Target | Description |
|--------|--------|-------------|
| L2P Lookup Latency | < 100ns | Page-level mapping lookup |
| GC Trigger Latency | < 1ms | From trigger to start of collection |
| Wear Leveling Accuracy | < 5% | Block wear variance |
| Max Mapping Table | 2GB | Supports 2TB capacity |
| Write Amplification Factor | ≤ 3 | TLC with 20% OP |

---

## 3. System Architecture

### 3.1 Module Layer Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│              Application Layer (FTL)                            │
│                                                                 │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │  FTL Core (ftl.c)                                        │ │
│  │  ┌──────────────────────┐  ┌───────────────────────┐  │ │
│  │  │  Mapping (mapping.c) │  │  Block Mgmt (block.c) │  │ │
│  │  │  - L2P Table         │  │  - Block State Machine│  │ │
│  │  │  - P2L Table         │  │  - CWB Management     │  │ │
│  │  │  - PPN Encode/Decode │  │  - Free Block Pool    │  │ │
│  │  └──────────────────────┘  └───────────────────────┘  │ │
│  │                                                             │ │
│  │  ┌──────────────────────┐  ┌───────────────────────┐  │ │
│  │  │  GC (gc.c)          │  │  Wear Leveling (wl.c)│  │ │
│  │  │  - Trigger Strategy  │  │  - Dynamic WL         │  │ │
│  │  │  - Victim Selection  │  │  - Static WL          │  │ │
│  │  │  - GC Execution Flow │  │  - Wear Monitoring    │  │ │
│  │  └──────────────────────┘  └───────────────────────┘  │ │
│  │                                                             │ │
│  │  ┌──────────────────────┐  ┌───────────────────────┐  │ │
│  │  │  ECC (ecc.c)        │  │  Error Handling (err.c)│ │ │
│  │  │  - LDPC              │  │  - Read Retry         │  │ │
│  │  │  - Cross-Die Parity  │  │  - Write Retry        │  │ │
│  │  └──────────────────────┘  └───────────────────────┘  │ │
│  └───────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

**Actual Implementation**:

The actual code provides implementations in `include/ftl/` and `src/ftl/`:

```
┌─────────────────────────────────────────────────────────────────┐
│          User-Space Implementation (Actual)                    │
│                                                                 │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │  FTL Structures (Implemented)                             │ │
│  │  - ftl.h: Top-level FTL context                          │ │
│  │  - mapping.h/c: L2P/P2L mapping (page-level)            │ │
│  │  - block.h/c: Block management (state machine, CWB)      │ │
│  │  - gc.h/c: Garbage Collection (Greedy algorithm)         │ │
│  │  - wear_level.h/c: Basic wear leveling (dynamic only)    │ │
│  │  - ecc.h/c: ECC structures                                │ │
│  │  - error.h/c: Error handling structures                   │ │
│  └───────────────────────────────────────────────────────────┘ │
│                                                                 │
│  Key features implemented:                                      │
│  - Page-level L2P/P2L mapping                                 │
│  - Block state machine (FREE/OPEN/CLOSED/GC/BAD)             │
│  - Current Write Block (CWB) management                       │
│  - Greedy GC algorithm                                         │
│  - Basic dynamic wear leveling                                 │
│                                                                 │
│  Key features NOT implemented:                                 │
│  - Cost-Benefit GC algorithm                                   │
│  - Static wear leveling                                        │
│  - Read Retry / Write Retry                                   │
│  - Striping across channels                                    │
│  - WAF calculation/monitoring                                  │
│  - Multi-level flow control                                    │
│  - Metadata redundancy / power-loss protection                │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 Component Decomposition

#### 3.2.1 Address Mapping (mapping.c)

**Responsibilities**:
- L2P (Logical-to-Physical) address mapping
- P2L (Physical-to-Logical) reverse mapping
- PPN (Physical Page Number) encoding/decoding
- Mapping table entry management

**Key Components** (from header files):
- `struct mapping_ctx`: Mapping context (see [include/ftl/mapping.h](../include/ftl/mapping.h))
- `union ppn`: Physical page number encoding with channel/chip/die/plane/block/page fields

**Implementation Status**: ✅ Implemented

#### 3.2.2 Block Management (block.c)

**Responsibilities**:
- Block state machine (FREE → OPEN → CLOSED → GC → FREE)
- Current Write Block (CWB) management
- Free block pool management
- Block metadata tracking (erase count, valid pages, etc.)

**Key Components** (from header files):
- `struct block_mgr`: Block manager context (see [include/ftl/block.h](../include/ftl/block.h))
- `struct block_desc`: Block descriptor
- `enum block_state`: Block state enumeration

**Implementation Status**: ✅ Implemented

#### 3.2.3 Garbage Collection (gc.c)

**Responsibilities**:
- GC trigger strategy (based on free block count)
- Victim block selection (Greedy algorithm in implementation)
- GC execution flow (valid page migration, block erase)
- Write amplification analysis (not implemented)

**Key Components** (from header files):
- `struct gc_ctx`: GC context (see [include/ftl/gc.h](../include/ftl/gc.h))

**Implementation Status**: ✅ Implemented (Greedy only, no Cost-Benefit)

#### 3.2.4 Wear Leveling (wear_level.c)

**Responsibilities**:
- Dynamic wear leveling
- Static wear leveling (not implemented)
- Wear monitoring and alerts (not implemented)

**Key Components** (from header files):
- `struct wear_level_ctx`: Wear leveling context (see [include/ftl/wear_level.h](../include/ftl/wear_level.h))

**Implementation Status**: ✅ Basic dynamic WL implemented, ❌ Static WL not implemented

---

## 4. Detailed Design

### 4.1 FTL Context

**From Actual Implementation** ([include/ftl/ftl.h](../include/ftl/ftl.h)):

```c
/* FTL Configuration */
struct ftl_config {
    u64 total_lbas;
    u32 page_size;
    u32 pages_per_block;
    u32 blocks_per_plane;
    u32 planes_per_die;
    u32 dies_per_chip;
    u32 chips_per_channel;
    u32 channel_count;
    u32 op_ratio;         /* Over-provisioning ratio (percentage) */
    enum gc_policy gc_policy;
    u32 gc_threshold;
    u32 gc_hiwater;
    u32 gc_lowater;
};

/* FTL Statistics */
struct ftl_stats {
    u64 read_count;
    u64 write_count;
    u64 trim_count;
    u64 read_bytes;
    u64 write_bytes;
    u64 gc_count;
    u64 moved_pages;
    u64 reclaimed_blocks;
};

/* FTL Context */
struct ftl_ctx {
    struct ftl_config config;
    struct mapping_ctx mapping;
    struct block_mgr block_mgr;
    struct cwb *cwbs;
    u32 cwb_count;
    struct gc_ctx gc;
    struct wear_level_ctx wl;
    struct ecc_ctx ecc;
    struct error_ctx error;
    struct hal_ctx *hal;
    struct ftl_stats stats;
    struct mutex lock;
    bool initialized;
};

/* Function Prototypes */
int ftl_init(struct ftl_ctx *ctx, struct ftl_config *config, struct hal_ctx *hal);
void ftl_cleanup(struct ftl_ctx *ctx);
int ftl_read(struct ftl_ctx *ctx, u64 lba, u32 len, void *data);
int ftl_write(struct ftl_ctx *ctx, u64 lba, u32 len, const void *data);
int ftl_trim(struct ftl_ctx *ctx, u64 lba, u32 len);
int ftl_flush(struct ftl_ctx *ctx);
void ftl_get_stats(struct ftl_ctx *ctx, struct ftl_stats *stats);
void ftl_reset_stats(struct ftl_ctx *ctx);
```

### 4.2 PPN Encoding

**From Design Document / Actual Implementation**:

```c
/* PPN Encoding */
union ppn {
    u64 raw;
    struct {
        u64 channel : 6;
        u64 chip : 4;
        u64 die : 3;
        u64 plane : 2;
        u64 block : 12;
        u64 page : 10;
        u64 reserved : 27;
    } bits;
};
```

### 4.3 Block States

**From Design Document / Actual Implementation**:

```c
/* Block State */
enum block_state {
    BLOCK_FREE = 0,
    BLOCK_OPEN = 1,
    BLOCK_CLOSED = 2,
    BLOCK_GC = 3,
    BLOCK_BAD = 4,
};
```

---

## 5. Interface Design

### 5.1 Top-Level Interface

**From [include/ftl/ftl.h](../include/ftl/ftl.h)**:

```c
int ftl_init(struct ftl_ctx *ctx, struct ftl_config *config, struct hal_ctx *hal);
void ftl_cleanup(struct ftl_ctx *ctx);
int ftl_read(struct ftl_ctx *ctx, u64 lba, u32 len, void *data);
int ftl_write(struct ftl_ctx *ctx, u64 lba, u32 len, const void *data);
int ftl_trim(struct ftl_ctx *ctx, u64 lba, u32 len);
int ftl_flush(struct ftl_ctx *ctx);
void ftl_get_stats(struct ftl_ctx *ctx, struct ftl_stats *stats);
void ftl_reset_stats(struct ftl_ctx *ctx);
```

### 5.2 Integration with Top-Level API

The FTL integrates with the top-level `sssim.h` API:
- `sssim_write()` → FTL write → HAL → Media
- `sssim_read()` → FTL read → HAL → Media

---

## 6. Data Structures

All data structures are defined in the header files under `include/ftl/`:

- [include/ftl/ftl.h](../include/ftl/ftl.h) - Top-level FTL context
- [include/ftl/mapping.h](../include/ftl/mapping.h) - Mapping structures
- [include/ftl/block.h](../include/ftl/block.h) - Block management structures
- [include/ftl/gc.h](../include/ftl/gc.h) - GC structures
- [include/ftl/wear_level.h](../include/ftl/wear_level.h) - Wear leveling structures
- [include/ftl/ecc.h](../include/ftl/ecc.h) - ECC structures
- [include/ftl/error.h](../include/ftl/error.h) - Error handling structures

---

## 7. Flow Diagrams

The design document includes detailed flow diagrams for:
- Host write command FTL processing flow
- Host read command FTL processing flow
- GC complete execution flow
- Static wear leveling trigger flow
- Read Retry and error handling flow

**Implementation Note**: Some flows (static wear leveling, Read Retry) are not implemented in the actual codebase.

---

## 8. Performance Design

See Section 2.2 for performance requirements. Core features are implemented and tested.

---

## 9. Error Handling

The design document includes comprehensive error handling for:
- NVMe error status codes
- Error handling flow
- Recoverable errors
- Unrecoverable data errors
- NAND device errors
- Command timeout handling
- Firmware internal errors

**Implementation Note**: Basic error handling structures are defined, but many advanced error handling features (Read Retry, Write Retry) are not implemented.

---

## 10. Test Design

Tests for the FTL module can be found in the `tests/` directory. The FTL components are well-tested as part of the 362 passing tests.

---

## Summary of Differences

| Aspect | Design Document | Actual Implementation |
|--------|-----------------|---------------------|
| GC Algorithm | Cost-Benefit + Greedy | Greedy only |
| Wear Leveling | Dynamic + Static | Dynamic only |
| Read Retry | Full Read Retry mechanism | Not implemented |
| Write Retry/Verify | Write Retry + Write Verify | Not implemented |
| Striping | Striping across channels | Not implemented |
| WAF Monitoring | WAF calculation/monitoring | Not implemented |
| Flow Control | Multi-level flow control | Not implemented |
| QoS | Full QoS guarantees | Not implemented |
| Metadata Redundancy | Cross-Die parity, redundancy | Not implemented |
| Power-Loss Protection | Write Buffer protection | Not implemented |
| Requirements Coverage | 22/22 designed | 7/22 implemented (31.8%) |

---

## References

1. [ARCHITECTURE.md](./ARCHITECTURE.md) - Actual system architecture
2. [REQUIREMENT_COVERAGE.md](./REQUIREMENT_COVERAGE.md) - Requirement coverage analysis
3. [IMPLEMENTATION_ROADMAP.md](./IMPLEMENTATION_ROADMAP.md) - Implementation roadmap
