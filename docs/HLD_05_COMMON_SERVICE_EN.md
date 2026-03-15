# HFSSS High-Level Design Document

**Document Name**: Common Service Layer HLD
**Document Version**: V1.0
**Date**: 2026-03-14
**Design Phase**: V2.0 (GA)

---

## Implementation Status

**Design Document**: Describes a comprehensive common service layer with RTOS primitives, task scheduling, memory management, bootloader, power services, OOB management, IPC, watchdog, debug, and logging.

**Actual Implementation**: Partial implementation with core RTOS primitives (task/queue/semaphore/mutex/mempool), log system, and basic asserts. No task scheduler, bootloader, power services, OOB, IPC, watchdog.

**Coverage Status**: 7/24 requirements implemented for this module (29.2%)

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

The Common Service Layer is the infrastructure layer for firmware simulation, providing RTOS primitives, task scheduling, memory management, bootloader, power-on/off services, out-of-band management, inter-core communication, system stability monitoring, panic/assert handling, system exception handling, system debug mechanisms, and system event logging mechanisms.

### 1.2 Module Responsibilities

This module is responsible for the following core functions:
- Real-time Operating System (RTOS) simulation (Task/Message Queue/Semaphore/Mutex/Event Group/Timer/Memory Pool)
- Task scheduling (static task binding, priority scheduling, load balancing, scheduling statistics)
- Memory management (memory partition planning, static pre-allocation, memory pools, memory protection, memory pressure management)
- Bootloader (boot sequence, dual-image redundancy, secure boot verification, boot log)
- Power-on/off services (power-on service, normal power-off, abnormal power-off handling)
- Out-of-band management (Unix Domain Socket/JSON-RPC, /proc filesystem interface, REST API, SMART information)
- Inter-core communication (message passing, shared memory, inter-core signals, global locks)
- System stability monitoring (Watchdog, system resource monitoring, performance anomaly detection, temperature simulation)
- Panic/Assert handling (Assert mechanism, Panic flow, Coredump)
- System Debug mechanism (command trace, NAND operation trace, FTL operation trace, GDB support, performance counters)
- System event Log mechanism (event levels, Log storage, Log entry format)

---

## 2. Requirements Review

### 2.1 Requirements Traceability Matrix

| Requirement ID | Description | Priority | Version | Implementation Status |
|----------------|-------------|----------|---------|----------------------|
| FR-CS-001 | RTOS primitives | P0 | V1.0 | ✅ Implemented in `common/` |
| FR-CS-002 | Task scheduler | P0 | V1.0 | ❌ Not Implemented |
| FR-CS-003 | Memory management | P0 | V1.0 | ⚠️ Partial (mempool only) |
| FR-CS-004 | Bootloader | P1 | V1.5 | ❌ Not Implemented |
| FR-CS-005 | Power-on/off services | P1 | V1.5 | ❌ Not Implemented |
| FR-CS-006 | Out-of-band management | P2 | V2.0 | ❌ Not Implemented |
| FR-CS-007 | Inter-core communication | P1 | V2.0 | ❌ Not Implemented |
| FR-CS-008 | Watchdog | P1 | V2.0 | ❌ Not Implemented |
| FR-CS-009 | Debug/Log | P0 | V1.5 | ✅ Implemented in `log.h/c` |
| FR-CS-010 | Debug mechanism | P0 | V2.0 | ❌ Not Implemented |
| FR-CS-011 | Log mechanism | P0 | V1.5 | ✅ Implemented in `log.h/c` |

### 2.2 Key Performance Requirements

| Metric | Target | Description |
|--------|--------|-------------|
| Task switch latency | < 1μs | Context switch latency |
| Message queue latency | < 500ns | Enqueue + dequeue latency |
| Log output latency | < 10μs | Log write latency |
| Max tasks | 256 | Maximum supported tasks |
| Log buffer | 64MB | Ring buffer size |

---

## 3. System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│              Common Service Layer                                │
│                                                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐   │
│  │  RTOS Primitives│  │  Task Scheduler│  │  Memory Management│   │
│  │  (rtos.c)    │  │  (scheduler.c)│  │  (memory.c)      │   │
│  │  ✅ Implemented│  │  ❌ Not Implem. │  │  ⚠️ Partial       │   │
│  └──────────────┘  └──────────────┘  └──────────────────┘   │
│                                                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐   │
│  │  Bootloader  │  │  Power Svcs  │  │  OOB Management  │   │
│  │  (boot.c)    │  │  (power.c)   │  │  (oob.c)         │   │
│  │  ❌ Not Implem. │  │  ❌ Not Implem. │  │  ❌ Not Implem.   │   │
│  └──────────────┘  └──────────────┘  └──────────────────┘   │
│                                                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐   │
│  │  IPC         │  │  Watchdog    │  │  Debug/Log       │   │
│  │  (ipc.c)     │  │  (watchdog.c)│  │  (debug/log.c)   │   │
│  │  ❌ Not Implem. │  │  ❌ Not Implem. │  │  ✅ Log Implemented│  │
│  └──────────────┘  └──────────────┘  └──────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

---

## 4. Detailed Design

### 4.1 RTOS Primitives Design

**Actual Implementation from `include/common/` headers**:

```c
/* From common.h - Log Levels */
#define LOG_LEVEL_ERROR    0
#define LOG_LEVEL_WARN     1
#define LOG_LEVEL_INFO     2
#define LOG_LEVEL_DEBUG    3
#define LOG_LEVEL_TRACE    4

/* From msgqueue.h - Message Queue */
struct msg_queue {
    u32 msg_size;
    u32 queue_len;
    u32 count;
    u32 head;
    u32 tail;
    u8 *buffer;
    void *lock;
    void *not_empty;
    void *not_full;
    u64 send_count;
    u64 recv_count;
};

/* From semaphore.h - Semaphore */
struct semaphore {
    int count;
    void *lock;
    void *cond;
    u64 wait_count;
    u64 signal_count;
};

/* From mutex.h - Mutex */
/* (See include/common/mutex.h for details) */

/* From mempool.h - Memory Pool */
/* (See include/common/mempool.h for details) */

/* From log.h - Log System */
struct log_entry {
    u64 timestamp;
    u32 level;
    const char *module;
    const char *file;
    u32 line;
    char message[LOG_ENTRY_SIZE];
};

struct log_ctx {
    struct log_entry *buffer;
    u32 buffer_size;
    u32 head;
    u32 tail;
    u32 count;
    u32 level;
    FILE *output_file;
    int use_stdout;
    void *lock;
};
```

**Implementation Note**: Task control block (TCB), event group, and timer are NOT implemented. The design showed these but they are missing from the actual code.

### 4.2 Log Mechanism Design

**Fully implemented** - see `include/common/log.h` and `src/common/log.c`.

---

## 5. Interface Design

**Actual Implemented Interfaces**:

```c
/* Message Queue (msgqueue.h) */
int msg_queue_init(struct msg_queue *mq, u32 msg_size, u32 queue_len);
void msg_queue_cleanup(struct msg_queue *mq);
int msg_queue_send(struct msg_queue *mq, const void *msg, u64 timeout_ns);
int msg_queue_recv(struct msg_queue *mq, void *msg, u64 timeout_ns);
int msg_queue_trysend(struct msg_queue *mq, const void *msg);
int msg_queue_tryrecv(struct msg_queue *mq, void *msg);
u32 msg_queue_count(struct msg_queue *mq);
void msg_queue_stats(struct msg_queue *mq, u64 *send_count, u64 *recv_count);

/* Semaphore (semaphore.h) */
int semaphore_init(struct semaphore *sem, int initial_count);
void semaphore_cleanup(struct semaphore *sem);
int semaphore_take(struct semaphore *sem, u64 timeout_ns);
int semaphore_trytake(struct semaphore *sem);
int semaphore_give(struct semaphore *sem);
int semaphore_get_count(struct semaphore *sem);
void semaphore_stats(struct semaphore *sem, u64 *wait_count, u64 *signal_count);

/* Log (log.h) */
int log_init(struct log_ctx *ctx, u32 buffer_size, u32 level);
void log_cleanup(struct log_ctx *ctx);
void log_set_output_file(struct log_ctx *ctx, const char *filename);
void log_set_level(struct log_ctx *ctx, u32 level);
void log_printf(struct log_ctx *ctx, u32 level, const char *module,
                const char *file, u32 line, const char *fmt, ...);

/* Convenience macros */
#define log_error(ctx, module, ...)
#define log_warn(ctx, module, ...)
#define log_info(ctx, module, ...)
#define log_debug(ctx, module, ...)
#define log_trace(ctx, module, ...)
```

**Missing Interfaces from Design**:
- Task: `task_create()`, `task_delete()`, `task_yield()`, `task_sleep()`
- Mutex: Wrapped in `common/mutex.h` but simplified
- Event Group: Not implemented
- Timer: Not implemented

---

## 6. Data Structures

See Section 4 "Detailed Design" for implemented data structures.

The design document showed many additional data structures that are NOT implemented:
- Task Control Block (TCB) with states, priorities, threads
- Event Group with bits
- Timer with oneshot/periodic types
- Bootloader structures
- Power service structures
- OOB management structures
- IPC structures
- Watchdog structures
- Debug trace structures

---

## 7. Flow Diagrams

Sections 7-10 are abbreviated in the original Chinese document. Refer to the design document for the complete flow diagrams, noting that most features are not implemented.

---

## 8. Performance Design

Most performance design features are NOT implemented:
- No task scheduler, so no task switching
- No NUMA awareness
- No memory partitioning
- No watchdog monitoring

---

## 9. Error Handling Design

- **Assert mechanism**: Basic ASSERT macro implemented in `common.h`
- **Panic handling**: NOT implemented
- **Coredump**: NOT implemented

---

## 10. Test Design

Refer to the original design document for test cases. Only basic tests for implemented features exist.

---

**Document Statistics**:
- Total words: ~30,000
