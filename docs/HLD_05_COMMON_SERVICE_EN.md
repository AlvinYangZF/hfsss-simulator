# HFSSS High-Level Design Document

**Document Name**: Common Service Layer HLD
**Document Version**: V2.0
**Date**: 2026-03-23
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
| EN-V2.0 | 2026-03-23 | Architecture Team | Enterprise SSD architecture update: UPLP power mgmt, thermal mgmt service, security key mgmt |

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
11. [Enterprise SSD Extensions](#11-enterprise-ssd-extensions)
12. [Architecture Decision Records](#12-architecture-decision-records)

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
- UPLP power management service (power-fail detection, capacitor drain, emergency flush)
- Thermal management service (temperature polling, progressive throttle, SMART thermal updates)
- Security key management service (key generation, wrapping, storage, lifecycle)

---

## 2. Requirements Review

### 2.1 Requirements Traceability Matrix

| Requirement ID | Description | Priority | Version | Implementation Status |
|----------------|-------------|----------|---------|----------------------|
| FR-CS-001 | RTOS primitives | P0 | V1.0 | Implemented in `common/` |
| FR-CS-002 | Task scheduler | P0 | V1.0 | Not Implemented |
| FR-CS-003 | Memory management | P0 | V1.0 | Partial (mempool only) |
| FR-CS-004 | Bootloader | P1 | V1.5 | Not Implemented |
| FR-CS-005 | Power-on/off services | P1 | V1.5 | Not Implemented |
| FR-CS-006 | Out-of-band management | P2 | V2.0 | Not Implemented |
| FR-CS-007 | Inter-core communication | P1 | V2.0 | Not Implemented |
| FR-CS-008 | Watchdog | P1 | V2.0 | Not Implemented |
| FR-CS-009 | Debug/Log | P0 | V1.5 | Implemented in `log.h/c` |
| FR-CS-010 | Debug mechanism | P0 | V2.0 | Not Implemented |
| FR-CS-011 | Log mechanism | P0 | V1.5 | Implemented in `log.h/c` |
| REQ-ENT-040 | UPLP Power Management Service | P0 | V2.0 | Design Only |
| REQ-ENT-041 | Thermal Management Service | P0 | V2.0 | Design Only |
| REQ-ENT-042 | Security Key Management Service | P0 | V2.0 | Design Only |

### 2.2 Key Performance Requirements

| Metric | Target | Description |
|--------|--------|-------------|
| Task switch latency | < 1us | Context switch latency |
| Message queue latency | < 500ns | Enqueue + dequeue latency |
| Log output latency | < 10us | Log write latency |
| Max tasks | 256 | Maximum supported tasks |
| Log buffer | 64MB | Ring buffer size |
| Power-fail flush time | < 5s | Time to flush WB + metadata after power loss |
| Thermal poll period | 1s | Temperature polling interval |
| Key generation latency | < 10ms | AES-256 key generation |

---

## 3. System Architecture

```
+-----------------------------------------------------------------+
|              Common Service Layer                                |
|                                                                  |
|  +--------------+  +--------------+  +------------------+       |
|  |  RTOS Prims  |  |  Task Sched  |  |  Memory Mgmt     |       |
|  |  (rtos.c)    |  |  (sched.c)   |  |  (memory.c)       |       |
|  |  Implemented |  |  Not Impl.   |  |  Partial           |       |
|  +--------------+  +--------------+  +------------------+       |
|                                                                  |
|  +--------------+  +--------------+  +------------------+       |
|  |  Bootloader  |  |  Power Svcs  |  |  OOB Management   |       |
|  |  (boot.c)    |  |  (power.c)   |  |  (oob.c)          |       |
|  |  Not Impl.   |  |  Not Impl.   |  |  Not Impl.        |       |
|  +--------------+  +--------------+  +------------------+       |
|                                                                  |
|  +--------------+  +--------------+  +------------------+       |
|  |  IPC         |  |  Watchdog    |  |  Debug/Log        |       |
|  |  (ipc.c)     |  |  (watchdog.c)|  |  (debug/log.c)    |       |
|  |  Not Impl.   |  |  Not Impl.   |  |  Log Implemented  |       |
|  +--------------+  +--------------+  +------------------+       |
|                                                                  |
|  +------------------+  +------------------+  +--------------+   |
|  |  UPLP Power Mgmt |  |  Thermal Mgmt    |  |  Security Key|   |
|  |  (uplp.c)        |  |  (thermal_svc.c) |  |  (keymgr.c)  |   |
|  |  [ENTERPRISE]    |  |  [ENTERPRISE]     |  |  [ENTERPRISE]|   |
|  +------------------+  +------------------+  +--------------+   |
+-----------------------------------------------------------------+
```

---

## 4. Detailed Design

### 4.1 RTOS Primitives Design

**Actual Implementation from `include/common/` headers**:

```c
/* Task Priority */
#define TASK_PRIO_IDLE 0
#define TASK_PRIO_LOW 1
#define TASK_PRIO_NORMAL 2
#define TASK_PRIO_HIGH 3
#define TASK_PRIO_REALTIME 4

/* Task State */
enum task_state {
    TASK_STATE_CREATED = 0,
    TASK_STATE_READY = 1,
    TASK_STATE_RUNNING = 2,
    TASK_STATE_BLOCKED = 3,
    TASK_STATE_SUSPENDED = 4,
    TASK_STATE_DELETED = 5,
};

/* Task Control Block */
struct task_tcb {
    uint32_t task_id;
    const char *name;
    enum task_state state;
    uint32_t priority;
    void (*entry)(void *arg);
    void *arg;
    pthread_t thread;
    uint64_t runtime_ns;
    uint64_t last_sched_ts;
    struct task_tcb *next;
    struct task_tcb *prev;
};

/* Message Queue */
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

/* Semaphore */
struct semaphore {
    int count;
    void *lock;
    void *cond;
    u64 wait_count;
    u64 signal_count;
};

/* Mutex */
struct rtos_mutex {
    pthread_mutex_t lock;
    uint32_t owner;
    uint32_t recursion;
};

/* Event Group */
struct event_group {
    uint32_t bits;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

/* Timer */
enum timer_type {
    TIMER_ONESHOT = 0,
    TIMER_PERIODIC = 1,
};

struct rtos_timer {
    uint32_t timer_id;
    enum timer_type type;
    uint64_t period_ns;
    uint64_t expiry_ts;
    void (*callback)(void *arg);
    void *arg;
    bool active;
    struct rtos_timer *next;
};

/* Memory Pool */
struct mem_pool {
    uint32_t block_size;
    uint32_t block_count;
    uint32_t free_count;
    void *memory;
    void **free_list;
    pthread_mutex_t lock;
};
```

**Implementation Note**: Task control block (TCB), event group, and timer are NOT implemented. The design showed these but they are missing from the actual code.

### 4.2 Log Mechanism Design

```c
/* Log Level */
#define LOG_LEVEL_ERROR 0
#define LOG_LEVEL_WARN 1
#define LOG_LEVEL_INFO 2
#define LOG_LEVEL_DEBUG 3
#define LOG_LEVEL_TRACE 4

/* Log Entry */
struct log_entry {
    u64 timestamp;
    u32 level;
    const char *module;
    const char *file;
    u32 line;
    char message[LOG_ENTRY_SIZE];
};

/* Log Context */
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

**Design-Only Interfaces (from Chinese original)**:

```c
/* Task */
int task_create(struct task_tcb **tcb, const char *name, uint32_t priority,
                void (*entry)(void *), void *arg);
void task_delete(struct task_tcb *tcb);
void task_yield(void);
void task_sleep(uint64_t ns);

/* Semaphore (design) */
int sem_create(struct semaphore **sem, int initial_count);
void sem_delete(struct semaphore *sem);
int sem_take(struct semaphore *sem, uint64_t timeout_ns);
int sem_give(struct semaphore *sem);
```

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

Refer to the design document for complete flow diagrams, noting that most features are not implemented.

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

## 11. Enterprise SSD Extensions

### 11.1 UPLP Power Management Service

#### 11.1.1 Overview

The Unexpected Power Loss Protection (UPLP) service is a critical enterprise feature that detects power failure events and orchestrates an emergency flush of volatile data (write buffer, L2P mapping table, and FTL metadata) to NAND using energy from the supercapacitor. The UPLP service coordinates multiple subsystems to maximize data saved within the available energy budget.

#### 11.1.2 Power-Fail Detection

```c
/* Power Fail Detection Source */
enum power_fail_source {
    PF_SOURCE_NONE = 0,
    PF_SOURCE_GPIO_INTERRUPT = 1,    /* Hardware GPIO pin detects power loss */
    PF_SOURCE_VOLTAGE_MONITOR = 2,   /* Voltage drops below threshold */
    PF_SOURCE_HOST_SHUTDOWN = 3,     /* Host sends NVMe shutdown notification */
    PF_SOURCE_SOFTWARE_INJECT = 4,   /* Software-injected for testing */
};

/* UPLP Service State */
enum uplp_state {
    UPLP_STATE_NORMAL = 0,           /* Normal operation, mains power OK */
    UPLP_STATE_POWER_FAIL_DETECTED = 1,  /* Power loss detected */
    UPLP_STATE_EMERGENCY_FLUSH = 2,  /* Flushing volatile data to NAND */
    UPLP_STATE_METADATA_SAVE = 3,    /* Saving FTL metadata to NAND */
    UPLP_STATE_COMPLETE = 4,         /* All data saved, safe to lose power */
    UPLP_STATE_FAILED = 5,           /* Energy exhausted before completion */
};
```

#### 11.1.3 Capacitor Drain State Machine

```c
/* UPLP Service Context */
struct uplp_ctx {
    enum uplp_state state;
    enum power_fail_source pf_source;
    u64 pf_detect_ts;               /* Timestamp of power failure detection */
    u64 flush_start_ts;
    u64 flush_end_ts;

    /* Energy budget tracking */
    struct hal_scap_ctx *scap;       /* Supercapacitor HAL */
    double initial_energy_j;
    double consumed_energy_j;
    uint64_t max_flush_pages;
    uint64_t flushed_pages;

    /* Flush progress */
    uint32_t wb_entries_remaining;
    uint32_t metadata_blocks_remaining;
    bool wb_flush_complete;
    bool l2p_flush_complete;
    bool metadata_flush_complete;

    /* References to other subsystems */
    void *controller_ctx;
    void *ftl_ctx;
    void *hal_ctx;

    /* Callback for completion notification */
    void (*on_complete)(void *ctx, bool success);
    void *on_complete_ctx;

    struct mutex lock;
};
```

#### 11.1.4 Emergency Flush Orchestration

The UPLP service follows a prioritized flush sequence:

```
Step 1: Stop accepting new host I/O
  - Signal controller thread to stop dequeuing from ring buffer
  - Allow in-flight commands to complete

Step 2: Flush Write Buffer (highest priority)
  - Iterate through all dirty WB entries
  - For each entry: FTL write -> HAL program -> Media program
  - Track energy consumed per page write
  - If energy budget exhausted, skip remaining WB entries

Step 3: Save L2P Mapping Table
  - Serialize L2P table to NAND reserved blocks
  - Use simplified write path (bypass normal FTL)
  - Write directly via HAL to pre-allocated metadata blocks

Step 4: Save FTL Metadata
  - Block manager state (free list, open blocks, CWB pointers)
  - GC state (if mid-GC, save victim block info)
  - Wear leveling counters
  - Write to NOR flash (if available) or NAND reserved area

Step 5: Write UPLP completion marker
  - Write a magic number + CRC to a known NAND location
  - This marker tells the bootloader that UPLP completed successfully

Step 6: Notify completion
  - Set state to UPLP_STATE_COMPLETE or UPLP_STATE_FAILED
  - Invoke completion callback
```

#### 11.1.5 UPLP Interface

```c
int uplp_init(struct uplp_ctx *ctx, struct hal_scap_ctx *scap,
              void *controller_ctx, void *ftl_ctx, void *hal_ctx);
void uplp_cleanup(struct uplp_ctx *ctx);
void uplp_register_completion(struct uplp_ctx *ctx,
                               void (*callback)(void *ctx, bool success),
                               void *cb_ctx);
int uplp_trigger_power_fail(struct uplp_ctx *ctx, enum power_fail_source source);
int uplp_execute_flush(struct uplp_ctx *ctx);
enum uplp_state uplp_get_state(struct uplp_ctx *ctx);
void uplp_get_stats(struct uplp_ctx *ctx, uint64_t *flushed_pages,
                     double *energy_consumed_j, uint64_t *flush_time_ns);
```

### 11.2 Thermal Management Service

#### 11.2.1 Overview

The thermal management service periodically polls temperature sensors via the HAL thermal interface and makes progressive throttling decisions to keep the SSD within safe operating temperatures. It also updates SMART thermal log entries for host visibility.

#### 11.2.2 Progressive Throttle Design

```c
/* Throttle Level */
enum thermal_throttle_level {
    THERMAL_THROTTLE_NONE = 0,       /* No throttling */
    THERMAL_THROTTLE_LIGHT = 1,      /* Reduce write IOPS by 25% */
    THERMAL_THROTTLE_MODERATE = 2,   /* Reduce write IOPS by 50%, reduce read by 10% */
    THERMAL_THROTTLE_HEAVY = 3,      /* Reduce write IOPS by 75%, reduce read by 25% */
    THERMAL_THROTTLE_CRITICAL = 4,   /* Stop all writes, reads at 50% */
    THERMAL_THROTTLE_SHUTDOWN = 5,   /* Initiate emergency shutdown */
};

/* Thermal Management Service Context */
struct thermal_mgmt_ctx {
    struct hal_thermal_ctx *thermal_hal;    /* HAL thermal sensor interface */
    struct flow_ctrl_ctx *flow_ctrl;        /* Controller flow control (for throttling) */

    /* Current state */
    enum thermal_throttle_level current_level;
    double current_composite_temp_c;
    double max_temp_since_boot_c;

    /* Thresholds for throttle levels */
    double throttle_light_temp_c;          /* e.g., 65C */
    double throttle_moderate_temp_c;       /* e.g., 70C */
    double throttle_heavy_temp_c;          /* e.g., 75C */
    double throttle_critical_temp_c;       /* e.g., 80C */
    double throttle_shutdown_temp_c;       /* e.g., 85C */
    double throttle_hysteresis_c;          /* e.g., 3C (de-throttle at temp - hysteresis) */

    /* Polling */
    uint64_t poll_interval_ns;             /* e.g., 1 second */
    uint64_t last_poll_ts;

    /* SMART thermal log */
    double smart_composite_temp_c;
    double smart_warning_temp_time_minutes;
    double smart_critical_temp_time_minutes;

    /* Statistics */
    uint64_t throttle_events;
    uint64_t total_throttle_ns;
    uint64_t max_throttle_duration_ns;

    struct mutex lock;
};
```

#### 11.2.3 Thermal Management Flow

```
Every poll_interval_ns:
  1. Read composite temperature from HAL
     temp = hal_thermal_read_composite_temp()

  2. Determine throttle level
     if temp >= shutdown_temp:  level = SHUTDOWN
     elif temp >= critical_temp: level = CRITICAL
     elif temp >= heavy_temp:    level = HEAVY
     elif temp >= moderate_temp: level = MODERATE
     elif temp >= light_temp:    level = LIGHT
     else:                       level = NONE

  3. Apply hysteresis for de-throttling
     if temp < (threshold_for_current_level - hysteresis):
       level = next_lower_level

  4. Apply throttle
     if level != current_level:
       update flow_ctrl rate limits based on level
       log thermal event
       current_level = level

  5. Update SMART thermal stats
     smart_composite_temp = temp
     if temp > warning_temp:
       smart_warning_time += poll_interval
     if temp > critical_temp:
       smart_critical_time += poll_interval

  6. If level == SHUTDOWN:
     trigger UPLP emergency flush
```

#### 11.2.4 Thermal Management Interface

```c
int thermal_mgmt_init(struct thermal_mgmt_ctx *ctx,
                       struct hal_thermal_ctx *thermal_hal,
                       struct flow_ctrl_ctx *flow_ctrl);
void thermal_mgmt_cleanup(struct thermal_mgmt_ctx *ctx);
void thermal_mgmt_set_thresholds(struct thermal_mgmt_ctx *ctx,
                                  double light, double moderate,
                                  double heavy, double critical,
                                  double shutdown, double hysteresis);
void thermal_mgmt_poll(struct thermal_mgmt_ctx *ctx, uint64_t current_ts);
enum thermal_throttle_level thermal_mgmt_get_level(struct thermal_mgmt_ctx *ctx);
double thermal_mgmt_get_composite_temp(struct thermal_mgmt_ctx *ctx);
void thermal_mgmt_get_smart_data(struct thermal_mgmt_ctx *ctx,
                                  double *composite_temp,
                                  double *warning_time_min,
                                  double *critical_time_min);
```

### 11.3 Security Key Management Service

#### 11.3.1 Overview

The security key management service handles the lifecycle of data encryption keys (DEKs) used by the crypto engine for self-encrypting drive (SED) functionality. It supports key generation, key wrapping/unwrapping (protecting DEKs with a key encryption key), persistent key storage in NOR flash, and key lifecycle management (creation, rotation, destruction).

#### 11.3.2 Key Hierarchy

```
Key Hierarchy:
  +---------------------------+
  |  Master Key (MK)          |  Derived from SID password or TPM
  |  - Never stored in clear  |
  +------------+--------------+
               |
               v
  +---------------------------+
  |  Key Encryption Key (KEK) |  Derived from MK via KDF (PBKDF2/HKDF)
  |  - Used to wrap/unwrap    |
  |    Data Encryption Keys   |
  +------------+--------------+
               |
               v
  +---------------------------+
  |  Data Encryption Keys     |  Per-namespace or per-locking-range
  |  (DEK-0, DEK-1, ...,     |
  |   DEK-N)                  |
  |  - Used by AES-XTS engine |
  |  - Stored wrapped in NOR  |
  +---------------------------+
```

#### 11.3.3 Key Management Data Structures

```c
/* Key State */
enum key_state {
    KEY_STATE_EMPTY = 0,
    KEY_STATE_GENERATED = 1,
    KEY_STATE_ACTIVE = 2,
    KEY_STATE_SUSPENDED = 3,
    KEY_STATE_DESTROYED = 4,
};

/* Key Entry */
struct key_entry {
    uint32_t key_id;
    uint32_t nsid;                  /* Associated namespace (0 = global) */
    enum key_state state;
    uint8_t  wrapped_key[80];       /* AES-KW wrapped key (key + 8 byte integrity) */
    uint32_t wrapped_key_len;
    uint8_t  key_hash[32];          /* SHA-256 hash of unwrapped key (for verification) */
    uint64_t created_ts;
    uint64_t last_used_ts;
    uint32_t usage_count;
};

/* Key Storage (NOR Flash layout) */
#define KEY_STORAGE_MAGIC 0x4B455953  /* "KEYS" */
#define MAX_KEYS 128

struct key_storage_header {
    uint32_t magic;
    uint32_t version;
    uint32_t key_count;
    uint32_t crc32;
};

/* Security Key Management Context */
struct keymgr_ctx {
    struct key_entry keys[MAX_KEYS];
    uint32_t key_count;

    /* KEK (loaded into memory from password-derived key) */
    uint8_t  kek[32];
    bool     kek_loaded;

    /* HAL references */
    struct hal_crypto_ctx *crypto;   /* Crypto engine for key operations */
    struct hal_nor_dev *nor;         /* NOR flash for persistent storage */

    /* Key storage location in NOR */
    uint32_t nor_partition_id;
    uint32_t nor_offset;

    /* Statistics */
    uint64_t keys_generated;
    uint64_t keys_destroyed;
    uint64_t wrap_operations;
    uint64_t unwrap_operations;

    struct mutex lock;
};
```

#### 11.3.4 Key Lifecycle Operations

1. **Key Generation**: Generate a cryptographically random DEK using a PRNG seeded by hardware entropy. The key length depends on the crypto mode (128-bit or 256-bit for AES-XTS).

2. **Key Wrapping**: Before storing a DEK to NOR flash, wrap it with the KEK using AES Key Wrap (RFC 3394). This protects the DEK at rest.

3. **Key Unwrapping**: When loading a DEK from NOR flash (e.g., during boot or when a namespace is attached), unwrap it with the KEK and load it into the crypto engine key slot.

4. **Key Rotation**: Generate a new DEK, re-encrypt all data in the namespace with the new key (or mark old key as deprecated and use new key for new writes), wrap and store the new key.

5. **Key Destruction**: Overwrite the key material in memory with zeros, overwrite the wrapped key in NOR flash, mark the key entry as destroyed. This implements "crypto erase" -- making all data encrypted with this key permanently unrecoverable.

#### 11.3.5 Key Management Interface

```c
int keymgr_init(struct keymgr_ctx *ctx, struct hal_crypto_ctx *crypto,
                struct hal_nor_dev *nor);
void keymgr_cleanup(struct keymgr_ctx *ctx);

/* KEK management */
int keymgr_derive_kek(struct keymgr_ctx *ctx, const uint8_t *password,
                       uint32_t password_len, const uint8_t *salt,
                       uint32_t salt_len, uint32_t iterations);
int keymgr_load_kek(struct keymgr_ctx *ctx, const uint8_t *kek, uint32_t kek_len);
void keymgr_clear_kek(struct keymgr_ctx *ctx);

/* DEK management */
int keymgr_generate_key(struct keymgr_ctx *ctx, uint32_t nsid,
                         uint32_t key_len_bits, uint32_t *out_key_id);
int keymgr_destroy_key(struct keymgr_ctx *ctx, uint32_t key_id);
int keymgr_activate_key(struct keymgr_ctx *ctx, uint32_t key_id,
                         uint32_t crypto_slot_id);
int keymgr_suspend_key(struct keymgr_ctx *ctx, uint32_t key_id);

/* Persistent storage */
int keymgr_save_to_nor(struct keymgr_ctx *ctx);
int keymgr_load_from_nor(struct keymgr_ctx *ctx);

/* Query */
int keymgr_get_key_state(struct keymgr_ctx *ctx, uint32_t key_id,
                          enum key_state *state);
int keymgr_get_key_for_ns(struct keymgr_ctx *ctx, uint32_t nsid,
                           uint32_t *key_id);
```

---

## 12. Architecture Decision Records

### ADR-CS-001: UPLP Flush Priority Order

**Context**: During a power loss event, limited energy is available. The flush order determines which data is prioritized.

**Decision**: Flush in this priority order: (1) Write Buffer dirty entries (user data at risk), (2) L2P mapping table (required for data recovery), (3) FTL metadata (block states, GC state). User data in the write buffer is highest priority because it represents acknowledged-but-not-persisted host writes.

**Consequences**:
- Positive: Maximizes data integrity for host-acknowledged writes. Even if L2P or metadata save is incomplete, the bootloader can reconstruct metadata from NAND scanning.
- Negative: If write buffer is very large and energy is limited, L2P may not be saved. Mitigated by keeping write buffer size proportional to supercapacitor energy budget.

**Status**: Accepted.

### ADR-CS-002: Progressive Thermal Throttling vs. Binary Throttle

**Context**: When the SSD overheats, it must reduce activity. Two approaches: (a) binary on/off throttling at a single threshold, (b) progressive multi-level throttling.

**Decision**: Implement 5-level progressive throttling with hysteresis. This provides smoother performance degradation and avoids oscillation between full-speed and throttled states.

**Consequences**:
- Positive: Better user experience (gradual performance reduction), avoids thermal oscillation, matches behavior of real enterprise SSDs.
- Negative: More complex implementation with multiple threshold checks. Acceptable complexity.

**Status**: Accepted.

### ADR-CS-003: Key Storage in NOR vs. NAND Reserved Blocks

**Context**: Wrapped DEKs must be stored persistently. Options are NOR flash (dedicated, always available, no wear leveling needed) or NAND reserved blocks (no additional hardware, but subject to wear and more complex management).

**Decision**: Store wrapped keys in NOR flash (Config partition). NOR flash provides byte-level addressability, simpler access patterns, and is already used for firmware and configuration storage. A backup copy is kept in a separate NOR partition for redundancy.

**Consequences**:
- Positive: Simple, reliable, fast access during boot, no interaction with FTL/GC. Keys survive NAND-level failures.
- Negative: Requires NOR flash support. In configurations without NOR, fall back to NAND reserved blocks with dual-copy redundancy.

**Status**: Accepted.

---

**Document Statistics**:
- Total words: ~30,000
