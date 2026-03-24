# High-Fidelity Full-Stack SSD Simulator (HFSSS) Low-Level Design

**Document Name**: Fault Injection Framework Low-Level Design
**Document Version**: V1.0
**Date**: 2026-03-15
**Design Phase**: V2.5 (Enterprise)
**Classification**: Internal

---

## Revision History

| Version | Date       | Author | Description     |
|---------|------------|--------|-----------------|
| V1.0    | 2026-03-15 | HFSSS  | Initial release |

---

## Table of Contents

1. [Module Overview](#1-module-overview)
2. [Requirements Traceability](#2-requirements-traceability)
3. [Data Structure Design](#3-data-structure-design)
4. [Header File Design](#4-header-file-design)
5. [Function Interface Design](#5-function-interface-design)
6. [Flow Diagrams](#6-flow-diagrams)
7. [Test Plan](#7-test-plan)

---

## 1. Module Overview

The Fault Injection Framework is a core testability component of the HFSSS simulator, providing controlled failure capabilities for the NAND media layer, power subsystem, and controller internal state, enabling the upper-level test suite to systematically verify the simulator's behavioral correctness under various abnormal conditions.

The framework is divided into four sub-domains:

1. **NAND Media Faults** (REQ-128): Inject bad blocks, read errors, write errors, erase errors, bit flips, read disturb storms, and data retention degradation at page/block/die granularity;
2. **Power Faults** (REQ-129): Simulate power loss scenarios at different timing points, working with the WAL recovery mechanism to verify the data persistence path;
3. **Controller Faults** (REQ-130): Inject firmware panics, memory pool exhaustion, command timeout storms, and L2P mapping table corruption;
4. **Fault Control Interface** (REQ-131): Unified management of fault registration, scheduling, persistence, and clearing through the OOB JSON-RPC interface.

**Design Constraints**:

- The `fault_check_*` family of functions is on the **hot path** of NAND operations and must guarantee O(log N) or better query complexity; single check overhead must not exceed 200 ns (N <= 1024 active faults).
- Fault registration and clearing are low-frequency operations (management path), which may use write locks with no complexity restrictions.
- The module is decoupled from the OOB module (LLD_07): the OOB layer handles JSON-RPC serialization/deserialization and passes parsed parameters to this module; this module is unaware of the transport protocol.
- All public functions must be thread-safe.

**Requirements Coverage**: REQ-128, REQ-129, REQ-130, REQ-131.

---

## 2. Requirements Traceability

| Req ID  | Description | Priority | Target Version |
|---------|-------------|----------|----------------|
| REQ-128 | NAND media fault injection: bad block, read/write/erase errors, bit flip, read disturb storm, data retention degradation | P0 | V2.5 |
| REQ-129 | Power fault injection: immediate power loss, power loss during write, power loss during GC, power loss during checkpoint | P0 | V2.5 |
| REQ-130 | Controller fault injection: firmware panic, memory pool exhaustion, command timeout storm, L2P table corruption | P0 | V2.5 |
| REQ-131 | Unified fault injection interface: OOB JSON-RPC `fault.inject`, immediate/deferred scheduling, one-shot/persistent modes | P0 | V2.5 |

### 2.1 REQ-128 NAND Media Fault Breakdown

| Sub-fault Type | Injection Granularity | Trigger Mechanism | Effect |
|---------------|----------------------|-------------------|--------|
| `bad_block` | ch/chip/die/plane/block | At media layer erase call | BBT marks as bad block, FTL retires the block |
| `read_error` | ch/chip/die/plane/block/page | At media layer read call | Returns uncorrectable ECC error (UECC) |
| `program_error` | ch/chip/die/plane/block/page | At media layer program call | Returns program failure status |
| `erase_error` | ch/chip/die/plane/block | At media layer erase call | Returns erase failure status |
| `bit_flip` | ch/chip/die/plane/block/page/bit_pos | Before media layer read return | XOR specified bit in page data buffer |
| `read_disturb_storm` | ch/chip/die/block | Background counter | Forces the block's read_count to accumulate to threshold, sharply increasing probabilistic read error rate |
| `data_retention` | ch/chip/die/plane/block/aging_factor | Background clock advancement | Accelerates Vt drift by aging_factor multiplier, producing soft errors earlier |

### 2.2 REQ-129 Power Fault Scenarios

| Scenario | Trigger Timing | Simulation Method |
|----------|---------------|-------------------|
| `power_loss_idle` | Immediate power loss in idle state | Write crash_marker then call `_exit(1)` |
| `power_loss_during_write` | During Write Buffer flush to NAND | Trigger at next nand_program call |
| `power_loss_during_gc` | During GC data migration | Trigger at next gc_move_page call |
| `power_loss_during_checkpoint` | During checkpoint write to disk | Trigger at next checkpoint_write call |

### 2.3 REQ-130 Controller Fault Breakdown

| Sub-fault Type | Trigger Mechanism | Effect |
|---------------|-------------------|--------|
| `firmware_panic` | Immediate execution | Invoke internal `HFSSS_PANIC()` macro, trigger assertion failure path |
| `mem_pool_exhaust` | On next allocation from specified pool | Force allocation function to return NULL |
| `cmd_timeout_storm` | Immediate effect, lasting N ms | All command completion callbacks delayed to `now + storm_duration_ms` |
| `l2p_corrupt` | Immediate execution | Write random PPN values to mapping table entries for specified LBA range |

---

## 3. Data Structure Design

### 3.1 Fault Type Enumeration

```c
enum fault_type {
    FAULT_NAND_BAD_BLOCK         = 0,
    FAULT_NAND_READ_ERROR        = 1,
    FAULT_NAND_PROGRAM_ERROR     = 2,
    FAULT_NAND_ERASE_ERROR       = 3,
    FAULT_NAND_BIT_FLIP          = 4,
    FAULT_NAND_READ_DISTURB      = 5,
    FAULT_NAND_DATA_RETENTION    = 6,
    FAULT_POWER_LOSS_IDLE        = 10,
    FAULT_POWER_LOSS_WRITE       = 11,
    FAULT_POWER_LOSS_GC          = 12,
    FAULT_POWER_LOSS_CHECKPOINT  = 13,
    FAULT_CTRL_FIRMWARE_PANIC    = 20,
    FAULT_CTRL_MEM_POOL_EXHAUST  = 21,
    FAULT_CTRL_CMD_TIMEOUT_STORM = 22,
    FAULT_CTRL_L2P_CORRUPT       = 23,
    FAULT_TYPE_MAX               = 32,
};
```

### 3.2 Fault Target Address

```c
#define FAULT_TARGET_ANY_U8    0xFFu
#define FAULT_TARGET_ANY_U16   0xFFFFu
#define FAULT_TARGET_ANY_U32   0xFFFFFFFFu

struct fault_target {
    uint8_t  channel;
    uint8_t  chip;
    uint8_t  die;
    uint8_t  plane;
    uint32_t block;
    uint32_t page;
    uint32_t bit_pos;
    uint32_t pool_id;
    uint64_t lba_start;
    uint64_t lba_count;
    uint32_t storm_duration_ms;
    double   aging_factor;
};
```

### 3.3 Trigger Mode and Persistence Mode

```c
enum fault_trigger_mode {
    FAULT_TRIGGER_IMMEDIATE = 0,
    FAULT_TRIGGER_DEFERRED  = 1,
};

enum fault_persist_mode {
    FAULT_PERSIST_ONE_SHOT = 0,
    FAULT_PERSIST_STICKY   = 1,
};
```

### 3.4 Fault Entry

```c
struct fault_entry {
    uint32_t              id;
    enum fault_type       type;
    struct fault_target   target;
    enum fault_trigger_mode trigger_mode;
    enum fault_persist_mode persist_mode;
    uint32_t              trigger_count;
    uint64_t              hit_count;
    bool                  active;
    uint8_t               _pad[7];
} __attribute__((aligned(64)));
```

### 3.5 Fault Registry

```c
#define FAULT_REGISTRY_MAX_ENTRIES  1024

struct fault_registry {
    struct fault_entry   entries[FAULT_REGISTRY_MAX_ENTRIES];
    uint32_t             count;
    uint32_t             next_id;
    uint16_t             sorted_idx[FAULT_REGISTRY_MAX_ENTRIES];
    uint32_t             sorted_count;
    uint32_t             type_present;
    pthread_rwlock_t     lock;
    char                 crash_marker_path[256];
};
```

### 3.6 Power Loss Scenario State

```c
enum power_loss_scenario {
    PLS_IDLE        = 0,
    PLS_WRITE       = 1,
    PLS_GC          = 2,
    PLS_CHECKPOINT  = 3,
};

struct crash_marker {
    uint32_t magic;
    uint32_t version;
    uint32_t scenario;
    uint32_t reserved;
    uint64_t crash_time_ns;
    uint64_t last_wal_seq;
    uint8_t  padding[96];
} __attribute__((packed));

#define CRASH_MARKER_MAGIC  0x43524153u
#define CRASH_MARKER_PATH_DEFAULT  "/var/lib/hfsss/wal/crash_marker"
```

---

## 4. Header File Design

```c
/* include/common/fault_inject.h */
#ifndef __HFSSS_FAULT_INJECT_H
#define __HFSSS_FAULT_INJECT_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

struct sssim_ctx;

int fault_inject_init(struct fault_registry *reg,
                      const char *crash_marker_path,
                      struct crash_marker *marker_out);
void fault_inject_cleanup(struct fault_registry *reg);

int fault_register(struct fault_registry *reg,
                   enum fault_type type,
                   const struct fault_target *target,
                   enum fault_trigger_mode trigger_mode,
                   enum fault_persist_mode persist_mode,
                   uint32_t trigger_count);
int fault_clear(struct fault_registry *reg, uint32_t fault_id);
void fault_clear_all(struct fault_registry *reg);

bool fault_check_nand_read(struct fault_registry *reg,
                           uint8_t ch, uint8_t chip, uint8_t die,
                           uint8_t plane, uint32_t block, uint32_t page);
bool fault_check_nand_program(struct fault_registry *reg,
                              uint8_t ch, uint8_t chip, uint8_t die,
                              uint8_t plane, uint32_t block, uint32_t page);
bool fault_check_nand_erase(struct fault_registry *reg,
                            uint8_t ch, uint8_t chip, uint8_t die,
                            uint8_t plane, uint32_t block);
uint64_t fault_get_bit_flip_mask(struct fault_registry *reg,
                                 uint8_t ch, uint8_t chip, uint8_t die,
                                 uint8_t plane, uint32_t block, uint32_t page);

void fault_inject_power_loss(struct fault_registry *reg,
                             enum power_loss_scenario scenario,
                             uint64_t last_wal_seq) __attribute__((noreturn));

bool fault_check_cmd_timeout(struct fault_registry *reg,
                             uint32_t *storm_duration_ms_out);
bool fault_check_mem_pool_exhaust(struct fault_registry *reg, uint32_t pool_id);
void fault_trigger_firmware_panic(struct fault_registry *reg);

int fault_list_active(const struct fault_registry *reg,
                      char *out_buf, size_t buf_len);
const char *fault_type_to_str(enum fault_type type);
int fault_type_from_str(const char *name);

#endif /* __HFSSS_FAULT_INJECT_H */
```

---

## 5. Function Interface Design

### 5.1 fault_inject_init

```
fault_inject_init(reg, crash_marker_path, marker_out):
  1. memset(reg, 0, sizeof(*reg))
  2. pthread_rwlock_init(&reg->lock)
  3. reg->next_id = 1
  4. Set reg->crash_marker_path (default CRASH_MARKER_PATH_DEFAULT)
  5. Attempt to open crash_marker_path:
       +- File does not exist -> normal startup, return 0
       +- File exists:
             a. Read 128 bytes into crash_marker struct
             b. Verify magic == CRASH_MARKER_MAGIC
             c. If magic matches -> copy contents to *marker_out (if non-NULL)
             d. Delete crash_marker_path file
             e. return 0
             f. If magic mismatch -> log WARN, delete file, return 0
```

### 5.2 fault_register

```
fault_register(reg, type, target, trigger_mode, persist_mode, trigger_count):
  1. Validate: type < FAULT_TYPE_MAX, target in range, count < MAX_ENTRIES
  2. Acquire write lock
  3. Find empty slot (active==false)
  4. Fill entry fields, set active=true
  5. reg->count++, reg->next_id++
  6. Rebuild sorted_idx[]
  7. Atomically set type_present bit
  8. Release write lock
  9. return fault_id
```

### 5.3 Hot-Path Query - fault_check_nand_read

Two-level optimization:

**Level 1 (lock-free)**: Atomic read of `type_present` bitmask. If the bit for the relevant type is zero, return false immediately.

**Level 2 (read lock)**: Binary search on `sorted_idx[]` sorted by (type, channel, chip, die, plane, block, page) for O(log N) complexity.

```
fault_check_nand_read(reg, ch, chip, die, plane, block, page):
  1. if !(atomic_load(&reg->type_present) & (1u << FAULT_NAND_READ_ERROR)):
         return false
  2. pthread_rwlock_rdlock(&reg->lock)
  3. Binary search sorted_idx[] for FAULT_NAND_READ_ERROR range
  4. For each matching active entry:
       - Check fault_target_match
       - Handle deferred trigger (decrement trigger_count)
       - On fire: increment hit_count, handle ONE_SHOT deactivation
       - return true
  5. pthread_rwlock_unlock(&reg->lock)
  6. return false
```

### 5.4 fault_inject_power_loss

```
fault_inject_power_loss(reg, scenario, last_wal_seq):
  // Does not return
  1. Construct crash_marker: magic, version=1, scenario, crash_time_ns, last_wal_seq
  2. open(crash_marker_path, O_WRONLY|O_CREAT|O_TRUNC)
  3. write + fsync + close
  4. _exit(1)
```

### 5.5 fault_target_match (Internal Helper)

```c
static inline bool fault_target_match(const struct fault_target *t,
                                      uint8_t ch, uint8_t chip, uint8_t die,
                                      uint8_t plane, uint32_t block, uint32_t page)
{
    if (t->channel != FAULT_TARGET_ANY_U8   && t->channel != ch)   return false;
    if (t->chip    != FAULT_TARGET_ANY_U8   && t->chip    != chip)  return false;
    if (t->die     != FAULT_TARGET_ANY_U8   && t->die     != die)   return false;
    if (t->plane   != FAULT_TARGET_ANY_U8   && t->plane   != plane) return false;
    if (t->block   != FAULT_TARGET_ANY_U32  && t->block   != block) return false;
    if (t->page    != FAULT_TARGET_ANY_U32  && t->page    != page)  return false;
    return true;
}
```

### 5.6 Media Layer Integration Points

```c
int nand_read_page(uint8_t ch, uint8_t chip, uint8_t die, uint8_t plane,
                  uint32_t block, uint32_t page, uint8_t *buf)
{
    if (fault_check_nand_read(g_fault_reg, ch, chip, die, plane, block, page))
        return NAND_ERR_UECC;
    memcpy(buf, nand_get_page_ptr(ch, chip, die, plane, block, page),
           g_nand_cfg.page_size);
    uint64_t flip_mask = fault_get_bit_flip_mask(
        g_fault_reg, ch, chip, die, plane, block, page);
    if (flip_mask) {
        uint64_t *w = (uint64_t *)buf;
        uint32_t word_off = g_active_bit_pos / 64;
        w[word_off] ^= flip_mask;
    }
    return NAND_OK;
}
```

---

## 6. Flow Diagrams

### 6.1 Fault Registration Flow

```
fault_register() -> validate params -> acquire write lock -> find empty slot
    -> fill entry -> rebuild sorted_idx[] -> set type_present bit
    -> release lock -> return fault_id
```

### 6.2 Hot-Path Fault Check Flow

```
fault_check_nand_read()
    -> [lock-free] check type_present bit -> 0? return false (ns-level)
    -> acquire read lock -> binary search sorted_idx[]
    -> match? -> handle trigger/one-shot -> return true
    -> no match -> release lock -> return false
```

### 6.3 Power Fault Injection and WAL Recovery Flow

```
Test script: fault.inject(type=power_loss_during_gc)
    -> fault_register(FAULT_POWER_LOSS_GC)
    -> GC thread: gc_move_page()
    -> fault_check_power_loss_gc() == true
    -> fault_inject_power_loss(PLS_GC, last_wal_seq)
    -> write crash_marker -> fsync -> _exit(1)
    [Process terminated]
    [Restart simulator]
    -> fault_inject_init() finds crash_marker
    -> read + verify magic -> return marker to caller
    -> sssim_init() -> wal_replay(last_wal_seq)
    -> normal operation
```

---

## 7. Test Plan

| Test ID | Description | Verification Point |
|---------|-------------|-------------------|
| FI-001 | BBT update after bad block injection | BBT records bad block; FTL stops allocating that block |
| FI-002 | GC skips bad block | GC migration skips bad block; bad_block_skipped count increases |
| FI-003 | Read error immediate trigger | First read returns UECC; media_errors +1 |
| FI-004 | Read error one-shot auto-clear | Second read succeeds; fault.list shows active=false |
| FI-005 | Read error sticky mode | 10 consecutive reads return UECC; hit_count==10 |
| FI-006 | Deferred trigger (trigger_count=3) | First 3 reads succeed, 4th returns UECC |
| FI-007 | Write error injection | FTL receives Program Fail, triggers block retirement |
| FI-008 | Erase error injection | GC erase fails; block marked bad in BBT |
| FI-009 | Bit flip precise position | Bit 42 flipped; ECC reports 1-bit soft error |
| FI-010 | Bit flip multi-bit mask | Two bits flipped; mask contains two set bits |
| FI-011 | FAULT_TARGET_ANY wildcard | All reads on channel 0 return UECC |
| FI-012 | Read disturb storm | read_count reaches threshold after 1 read |
| FI-013 | Data retention degradation | Soft error rate increases with aging_factor |
| FI-014 | Immediate power loss (idle) | Process exits; crash_marker exists with PLS_IDLE |
| FI-015 | Power loss during write + WAL recovery | Committed data readable after restart |
| FI-016 | Power loss during GC + WAL recovery | Filesystem consistency verified |
| FI-017 | Power loss during checkpoint | Recovery from last checkpoint + WAL |
| FI-018 | Firmware panic path | Process exits with non-zero; panic log recorded |
| FI-019 | Memory pool exhaustion | Allocation returns NULL; controller enters DEGRADED |
| FI-020 | Command timeout storm | Completions delayed >=500ms during storm |
| FI-021 | L2P corruption injection | Reads return wrong data; integrity check fails |
| FI-022 | fault_clear precise removal | One fault cleared; others still effective |
| FI-023 | fault_clear_all | type_present==0; all operations normal |
| FI-024 | Registry capacity limit | 1025th register returns -ENOMEM |
| FI-025 | Hot-path performance (no faults) | <10 ns average |
| FI-026 | Hot-path performance (1024 faults) | <200 ns average |
| FI-027 | Concurrent thread safety | Zero data races under TSan |
| FI-028 | Corrupt crash_marker tolerance | WARN logged; normal startup |

---

**Document Statistics**:
- Requirements covered: 4 (REQ-128, REQ-129, REQ-130, REQ-131)
- New header files: `include/common/fault_inject.h`
- New source files: `src/common/fault_inject.c`, `src/common/fault_power.c`
- Function interfaces: 18 public + 4 internal
- Test cases: 28 (FI-001 through FI-028)

## Appendix: Cross-References

| Reference | Document |
|-----------|----------|
| OOB JSON-RPC integration | LLD_07_OOB_MANAGEMENT |
| WAL recovery path | LLD_09_BOOTLOADER |
| Power loss protection | LLD_17_POWER_LOSS_PROTECTION |
| Persistence format | LLD_15_PERSISTENCE_FORMAT |
