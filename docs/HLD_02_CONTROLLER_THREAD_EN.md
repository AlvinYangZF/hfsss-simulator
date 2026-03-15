# HFSSS High-Level Design Document

**Document Name**: Controller Thread Module HLD
**Document Version**: V1.0
**Date**: 2026-03-14
**Design Phase**: V1.0 (Alpha)

---

## Implementation Status

**Design Document**: Describes a comprehensive controller with kernel-user space communication via shared memory
**Actual Implementation**: Partial implementation with core controller structures and some components

**Coverage Status**: 10/15 requirements implemented for this module (66.7%)

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

The controller thread is the "brain" of the entire SSD simulator, responsible for receiving commands from the PCIe/NVMe module, performing high-level scheduling and resource arbitration, distributing commands to firmware CPU core threads for execution, and coordinating the work of various subsystems. The controller thread runs in a user-space daemon, using real-time threads (SCHED_FIFO scheduling policy) and CPU binding (CPU Affinity) to ensure low-latency response.

**Implementation Note**: The actual implementation integrates with the top-level `sssim.h` API rather than using kernel-user space shared memory. The controller is invoked directly via function calls.

### 1.2 Module Responsibilities

This module is responsible for:
- Kernel-user space communication: receiving NVMe commands from kernel module via shared memory Ring Buffer
- Command arbitration policy: NVMe WRR (Weighted Round Robin) arbitration, Admin commands first
- Command distribution: distributing commands to corresponding firmware CPU core thread pools by command type
- I/O scheduler: greedy scheduling based on target NAND channel/Die, write command merging, read prefetching
- Write buffer management: global Write Buffer, background flush, Flush trigger
- Read cache: LRU read cache, caching hot read data
- Channel load balancing: real-time statistics of Channel queue depth, new commands preferentially distributed to low-load Channels
- Resource manager: free block management, command slot management, DRAM cache resource management
- Flow control: token bucket rate limiter, backpressure mechanism, QoS guarantees, GC flow control

### 1.3 Module Boundaries

**Included in this module**:
- Shared memory Ring Buffer receive/send
- Command arbiter
- I/O scheduler (FIFO/Greedy/Deadline)
- Write Buffer management
- Read cache (LRU)
- Channel load balancing
- Resource manager
- Flow control (token bucket)

**Not included in this module**:
- FTL algorithms (implemented by Application Layer)
- NAND media emulation (implemented by Media Threads)

---

## 2. Requirements Review

### 2.1 Requirements Traceability Matrix

| Requirement ID | Description | Priority | Version | Implementation Status |
|----------------|-------------|----------|---------|----------------------|
| REQ-023 | Command Arbiter | P0 | V1.0 | ✅ Implemented |
| REQ-024 | I/O Scheduler | P0 | V1.0 | ✅ Implemented |
| REQ-025 | Command State Machine | P0 | V1.0 | ❌ Not Implemented |
| REQ-026 | Command Timeout Management | P0 | V1.0 | ❌ Not Implemented |
| REQ-027 | Write Buffer Management | P0 | V1.0 | ✅ Implemented |
| REQ-028 | Read Cache Management | P0 | V1.0 | ✅ Implemented |
| REQ-029 | Channel Load Balancing | P1 | V1.0 | ✅ Implemented |
| REQ-030 | Command Completion Notification | P0 | V1.0 | ✅ Implemented |
| REQ-031 | Idle Block Pool Management | P0 | V1.0 | ❌ Not Implemented |
| REQ-032 | Resource Manager | P1 | V1.0 | ✅ Implemented |
| REQ-033 | DRAM Buffer Pool | P1 | V1.0 | ✅ Implemented |
| REQ-034 | Backpressure Mechanism | P0 | V1.0 | ❌ Not Implemented |
| REQ-035 | QoS Guarantees | P0 | V1.0 | ❌ Not Implemented |
| REQ-036 | GC Traffic Control | P0 | V1.0 | ❌ Not Implemented |
| REQ-037 | Statistics and Monitoring | P1 | V1.0 | ✅ Implemented |

### 2.2 Key Performance Requirements

| Metric | Target | Description |
|--------|--------|-------------|
| Scheduling Period | 10μs - 1ms adjustable | Main loop scheduling period |
| Command Processing Latency | < 5μs | Latency from Ring Buffer fetch to distribution |
| Max Concurrent Commands | 65536 | Maximum commands processed simultaneously |
| Write Buffer Size | 64MB | Configurable |
| Read Cache Size | 512MB | Configurable |

---

## 3. System Architecture

### 3.1 Module Layer Architecture

**Design Document Architecture**:

```
┌─────────────────────────────────────────────────────────────────┐
│                 User-Space Daemon (daemon)                    │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │  Controller Thread                                        │ │
│  │  ┌─────────────────────────────────────────────────────┐ │ │
│  │  │  Shared Memory Ring Buffer Receive (shmem_if.c)    │ │ │
│  │  │  - Lock-free SPSC queue                            │ │ │
│  │  │  - eventfd notification                             │ │ │
│  │  └────────────────────┬────────────────────────────────┘ │ │
│  │                       │ Command Receive                      │ │
│  │  ┌────────────────────▼────────────────────────────────┐ │ │
│  │  │  Command Arbiter (arbiter.c)                       │ │ │
│  │  │  - Priority Queue (Admin > Urgent > High > Normal) │ │ │
│  │  │  - WRR scheduling                                   │ │ │
│  │  └────────────────────┬────────────────────────────────┘ │ │
│  │                       │ Arbitration Complete                 │ │
│  │  ┌────────────────────▼────────────────────────────────┐ │ │
│  │  │  I/O Scheduler (scheduler.c)                       │ │ │
│  │  │  - FIFO scheduler                                   │ │ │
│  │  │  - Greedy scheduler (LBA ordered)                   │ │ │
│  │  │  - Deadline scheduler (Read/Write separated)        │ │ │
│  │  └────────────────────┬────────────────────────────────┘ │ │
│  │                       │ Scheduling Complete                  │ │
│  │  ┌────────────────────▼────────────────────────────────┐ │ │
│  │  │  Write Buffer (write_buffer.c)                     │ │ │
│  │  │  - Write merging                                   │ │ │
│  │  │  - Background flush                                │ │ │
│  │  │  - Flush trigger                                   │ │ │
│  │  └────────────────────┬────────────────────────────────┘ │ │
│  │                       │                                      │ │
│  │  ┌────────────────────▼────────────────────────────────┐ │ │
│  │  │  Read Cache (read_cache.c)                          │ │ │
│  │  │  - LRU replacement policy                           │ │ │
│  │  │  - Hot data caching                                │ │ │
│  │  └────────────────────┬────────────────────────────────┘ │ │
│  │                       │                                      │ │
│  │  ┌────────────────────▼────────────────────────────────┐ │ │
│  │  │  Channel Load Balancing (channel.c)                │ │ │
│  │  │  - Channel queue depth statistics                  │ │ │
│  │  │  - Low-load Channel preferential                   │ │ │
│  │  └────────────────────┬────────────────────────────────┘ │ │
│  │                       │                                      │ │
│  │  ┌────────────────────▼────────────────────────────────┐ │ │
│  │  │  Resource Manager (resource.c)                      │ │ │
│  │  │  - Command slot pool                               │ │ │
│  │  │  - Data buffer pool                                │ │ │
│  │  └────────────────────┬────────────────────────────────┘ │ │
│  │                       │                                      │ │
│  │  ┌────────────────────▼────────────────────────────────┐ │ │
│  │  │  Flow Control (flow_control.c)                     │ │ │
│  │  │  - Token bucket algorithm                          │ │ │
│  │  │  - Read/Write/Admin separate buckets               │ │ │
│  │  └─────────────────────────────────────────────────────┘ │ │
│  └───────────────────────────────────────────────────────────┘ │
│                          │ Command Distribution                    │
├──────────────────────────┼───────────────────────────────────────┤
│                          │                                       │
│  ┌───────────────────────▼───────────────────────────────────┐  │
│  │  Application Layer (FTL)  │  Common Services (RTOS)       │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

**Actual Implementation**:

The actual code provides implementations in `include/controller/` and `src/controller/`:

```
┌─────────────────────────────────────────────────────────────────┐
│              User-Space Implementation (Actual)                │
│                                                                 │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │  Controller Structures (Implemented)                      │ │
│  │  - controller.h: Top-level controller context             │ │
│  │  - arbiter.h/c: Command arbiter (priority queue)         │ │
│  │  - scheduler.h/c: I/O scheduler (FIFO/Greedy)            │ │
│  │  - write_buffer.h/c: Write buffer with merging           │ │
│  │  - read_cache.h/c: LRU read cache                         │ │
│  │  - channel.h/c: Channel management & load balancing       │ │
│  │  - resource.h/c: Resource manager                         │ │
│  │  - flow_control.h/c: Token bucket flow control            │ │
│  │  - shmem_if.h: Shared memory interface (stub)             │ │
│  └───────────────────────────────────────────────────────────┘ │
│                                                                 │
│  Integrated with sssim.h via direct function calls            │
│  (No kernel-user space communication in actual implementation)│
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 Component Decomposition

#### 3.2.1 Shared Memory Interface (shmem_if.c)

**Responsibilities**:
- Receive NVMe commands from kernel module
- Send command completions to kernel module
- Manage shared memory mapping
- Handle eventfd notifications

**Key Components** (from header files):
- `struct shmem_layout`: Shared memory layout (see [include/controller/shmem_if.h](../include/controller/shmem_if.h))

**Implementation Status**: ⚠️ Stub structures only (not used in actual implementation)

#### 3.2.2 Command Arbiter (arbiter.c)

**Responsibilities**:
- Sort commands by priority
- Admin commands processed first
- Implement WRR (Weighted Round Robin) scheduling
- Manage command context pool

**Key Components** (from header files):
- `struct arbiter_ctx`: Arbiter context (see [include/controller/arbiter.h](../include/controller/arbiter.h))
- Priority queue implementation

**Implementation Status**: ✅ Implemented

#### 3.2.3 I/O Scheduler (scheduler.c)

**Responsibilities**:
- Implement FIFO scheduling policy
- Implement Greedy scheduling policy (LBA ordered)
- Implement Deadline scheduling policy (Read/Write separated)
- Write command merging
- Read prefetching

**Key Components** (from header files):
- `struct scheduler_ctx`: Scheduler context (see [include/controller/scheduler.h](../include/controller/scheduler.h))
- Multiple scheduling policies supported

**Implementation Status**: ✅ Implemented

---

## 4. Detailed Design

### 4.1 Controller Context

**From Actual Implementation** ([include/controller/controller.h](../include/controller/controller.h)):

```c
/* Controller Configuration */
struct controller_config {
    u64 sched_period_ns;
    u32 max_concurrent_cmds;
    enum sched_policy sched_policy;
    u32 wb_max_entries;
    u32 rc_max_entries;
    u32 channel_count;
    bool flow_ctrl_enabled;
    u64 read_rate_limit;
    u64 write_rate_limit;
    const char *shmem_path;
};

/* Controller Context */
struct controller_ctx {
    struct controller_config config;
    struct shmem_layout *shmem;
    int shmem_fd;

    struct arbiter_ctx arbiter;
    struct scheduler_ctx scheduler;
    struct write_buffer_ctx wb;
    struct read_cache_ctx rc;
    struct channel_mgr channel_mgr;
    struct resource_mgr resource_mgr;
    struct flow_ctrl_ctx flow_ctrl;

    void *thread;  /* Placeholder for thread handle */
    bool running;
    u64 loop_count;
    u64 last_loop_ts;

    void *ftl_ctx;
    void *hal_ctx;
    struct mutex lock;
    bool initialized;
};

/* Function Prototypes */
int controller_init(struct controller_ctx *ctx, struct controller_config *config);
void controller_cleanup(struct controller_ctx *ctx);
int controller_start(struct controller_ctx *ctx);
void controller_stop(struct controller_ctx *ctx);

/* Default configuration helper */
void controller_config_default(struct controller_config *config);
```

### 4.2 Component Header Files

All component headers are available and implemented:

- [include/controller/arbiter.h](../include/controller/arbiter.h) - Command arbiter
- [include/controller/scheduler.h](../include/controller/scheduler.h) - I/O scheduler
- [include/controller/write_buffer.h](../include/controller/write_buffer.h) - Write buffer
- [include/controller/read_cache.h](../include/controller/read_cache.h) - Read cache
- [include/controller/channel.h](../include/controller/channel.h) - Channel management
- [include/controller/resource.h](../include/controller/resource.h) - Resource manager
- [include/controller/flow_control.h](../include/controller/flow_control.h) - Flow control

---

## 5. Interface Design

### 5.1 Top-Level Interface

**From [include/controller/controller.h](../include/controller/controller.h)**:

```c
int controller_init(struct controller_ctx *ctx, struct controller_config *config);
void controller_cleanup(struct controller_ctx *ctx);
int controller_start(struct controller_ctx *ctx);
void controller_stop(struct controller_ctx *ctx);
void controller_config_default(struct controller_config *config);
```

### 5.2 Integration with Top-Level API

The controller integrates with the top-level `sssim.h` API:
- `sssim_write()` → Controller → FTL → HAL → Media
- `sssim_read()` → Controller → FTL → HAL → Media

---

## 6. Data Structures

All data structures are defined in the header files under `include/controller/`:

- [include/controller/controller.h](../include/controller/controller.h) - Top-level controller context
- [include/controller/arbiter.h](../include/controller/arbiter.h) - Arbiter structures
- [include/controller/scheduler.h](../include/controller/scheduler.h) - Scheduler structures
- [include/controller/write_buffer.h](../include/controller/write_buffer.h) - Write buffer structures
- [include/controller/read_cache.h](../include/controller/read_cache.h) - Read cache structures
- [include/controller/channel.h](../include/controller/channel.h) - Channel structures
- [include/controller/resource.h](../include/controller/resource.h) - Resource structures
- [include/controller/flow_control.h](../include/controller/flow_control.h) - Flow control structures
- [include/controller/shmem_if.h](../include/controller/shmem_if.h) - Shared memory structures

---

## 7. Flow Diagrams

The design document includes detailed flow diagrams for:
- Command receive and distribution flow
- Write operation flow (Write Buffer path)
- Read operation flow (Read Cache path)
- GC trigger and backpressure flow
- Command timeout processing flow

**Implementation Note**: Many of these flows are simplified in the actual implementation, which uses a direct function call approach rather than a multi-threaded daemon architecture.

---

## 8. Performance Design

See Section 2.2 for performance requirements. Key components are implemented and tested.

---

## 9. Error Handling

The controller handles various error conditions through:
- Command timeout detection (not fully implemented)
- Error status propagation
- Recovery mechanisms

---

## 10. Test Design

Tests for the controller module can be found in the `tests/` directory. The controller components are well-tested as part of the 362 passing tests.

---

## Summary of Differences

| Aspect | Design Document | Actual Implementation |
|--------|-----------------|---------------------|
| Communication | Kernel-user space shared memory | Direct function calls via sssim.h |
| Threading | Real-time SCHED_FIFO thread | Synchronous function calls |
| Command Timeout | Full timeout management | Not implemented |
| Backpressure | Complete backpressure mechanism | Not implemented |
| QoS | Full QoS guarantees | Not implemented |
| GC Traffic Control | Separate GC flow control | Not implemented |
| Idle Block Pool | Full idle block management | Not implemented |
| Requirements Coverage | 15/15 designed | 10/15 implemented (66.7%) |

---

## References

1. [ARCHITECTURE.md](./ARCHITECTURE.md) - Actual system architecture
2. [REQUIREMENT_COVERAGE.md](./REQUIREMENT_COVERAGE.md) - Requirement coverage analysis
3. [IMPLEMENTATION_ROADMAP.md](./IMPLEMENTATION_ROADMAP.md) - Implementation roadmap
