# Bootloader and Power Management Low-Level Design

## Revision History

| Version | Date       | Author | Description     |
|---------|------------|--------|-----------------|
| V1.0    | 2026-03-15 | HFSSS  | Initial release |

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

### 1.1 Module Positioning and Responsibilities

The Bootloader and Power Management module is responsible for simulating the complete lifecycle of SSD firmware: from power-on boot to system ready, and from normal/abnormal power-down to state persistence and consistency guarantees between the next power-up.

This module spans two functional domains:

1. **Bootloader Simulation** (REQ-073, REQ-074): Reproduces in software the six phases of real SSD firmware startup, with configurable timing delays (total 3-8 seconds), implements dual NOR image (Slot A / Slot B) firmware selection and integrity verification, and writes boot logs to the NOR Log partition;
2. **Power-Up/Down Services** (REQ-075, REQ-076): On power-up, detects the previous power-down type (normal/abnormal/first format), executes the corresponding recovery path (checkpoint load, WAL replay, full scan); on power-down, safely saves SSD state through normal/abnormal paths and records power events in SMART counters.

### 1.2 Relationships with Other Modules

| Direction | Related Module | Interaction |
|-----------|---------------|-------------|
| Boot phase call | Media threads (NOR/NAND) | nand_device_init, nor_init, BBT loading |
| Boot phase call | FTL (address mapping/GC/WL) | L2P checkpoint load, WAL replay, block pool init |
| Boot phase call | Controller threads (arbitration/scheduler/WB/read cache) | Sub-module initialization |
| Boot phase call | NVMe/PCIe emulation layer | Queue structure init, CSTS.RDY assertion |
| Full lifecycle call | OOB management module | Start OOB listener thread, SMART updates |
| Power-down write | NOR Flash (SysInfo partition) | clean_shutdown_marker / crash_marker |
| Power-down write | WAL file | Incremental log entries |

### 1.3 NOR Flash Partition Layout

This module directly depends on the NOR Flash partition structure (REQ-052):

```
NOR Flash 256MB
+----------------------------------+
|  Bootloader      4MB  [0x000_0000 - 0x03F_FFFF] |
+----------------------------------+
|  Firmware Slot A 64MB [0x040_0000 - 0x43F_FFFF] |
+----------------------------------+
|  Firmware Slot B 64MB [0x440_0000 - 0x83F_FFFF] |
+----------------------------------+
|  Config Area     8MB  [0x840_0000 - 0x8BF_FFFF] |
+----------------------------------+
|  BBT             8MB  [0x8C0_0000 - 0x93F_FFFF] |
+----------------------------------+
|  Log Area        16MB [0x940_0000 - 0xA3F_FFFF] |
+----------------------------------+
|  SysInfo         4MB  [0xA40_0000 - 0xA7F_FFFF] |
+----------------------------------+
```

The SysInfo partition is the core persistent storage area for power management, storing clean shutdown marker, crash marker, power-on count, last power-down type, and other metadata.

### 1.4 Design Constraints

- The entire boot sequence executes sequentially in a dedicated simulation thread and does not accept host I/O until Phase 5 completes and CSTS.RDY=1 is set;
- All phase delays are implemented via `nanosleep` or high-precision timers to ensure measurability;
- The abnormal power-down path must not call heap allocation functions (to avoid undefined behavior in signal handler context);
- NOR Flash read/write operations are completed through the HAL layer `nor_partition_read / nor_partition_write`, not through direct memory access.

---

## 2. Requirements Traceability

| Req ID  | Description | Priority | Target Version |
|---------|-------------|----------|----------------|
| REQ-073 | Six-phase boot sequence simulation: Phase 0 hardware init, Phase 1 POST self-test, Phase 2 metadata load, Phase 3 controller init, Phase 4 NVMe init, Phase 5 system ready; total boot delay 3-8s configurable | P0 | V1.5 |
| REQ-074 | Bootloader features: boot time simulation, dual-image redundant Slot A/B switching, secure boot CRC verification, boot log written to NOR Log partition | P0 | V2.0 |
| REQ-075 | Power-up service: detect last power-down type (normal/abnormal/first format), execute corresponding recovery path, POST self-test, SMART records power-on count and abnormal shutdown count | P0 | V1.5 |
| REQ-076 | Power-down service: normal power-down (NVMe Shutdown notification) seven-step sequence + CSTS.SHST=0x02; abnormal power-down via atexit/SIGTERM/SIGINT signal handling for best-effort persistence, maintain WAL log | P0 | V1.5 |

---

## 3. Data Structure Design

### 3.1 Boot Phase Enumeration and Boot Type

```c
enum boot_phase {
    BOOT_PHASE_0_HW_INIT    = 0,  /* hardware init: NAND/NOR init, memory pools */
    BOOT_PHASE_1_POST       = 1,  /* POST self-test: scan NOR slots, verify CRC */
    BOOT_PHASE_2_META_LOAD  = 2,  /* metadata load: BBT, L2P checkpoint, WAL replay */
    BOOT_PHASE_3_CTRL_INIT  = 3,  /* controller init: arbitration, WB, cache, LB */
    BOOT_PHASE_4_NVME_INIT  = 4,  /* NVMe init: queue structs, CSTS.RDY=1 */
    BOOT_PHASE_5_READY      = 5,  /* system ready: OOB listener, monitoring, I/O */
};

enum boot_type {
    BOOT_FIRST    = 0,  /* all-0xFF SysInfo: first power-on, format required */
    BOOT_NORMAL   = 1,  /* clean shutdown marker present: minimal WAL replay */
    BOOT_RECOVERY = 2,  /* crash marker or missing marker: full WAL replay */
    BOOT_DEGRADED = 3,  /* WAL replay succeeded but data integrity warnings exist */
};

enum shutdown_type {
    SHUTDOWN_NORMAL      = 0,  /* NVMe shutdown notification, CSTS.SHST=0x02 reached */
    SHUTDOWN_ABNORMAL    = 1,  /* SIGTERM/SIGINT caught, best-effort persist executed */
    SHUTDOWN_POWER_LOSS  = 2,  /* SIGKILL / sudden power loss, WAL-only recovery */
};
```

### 3.2 NOR Firmware Image Slot Descriptor

```c
#define NOR_SLOT_MAGIC      0x48465353U  /* "HFSS" */

struct nor_firmware_slot {
    uint32_t magic;
    uint32_t version;
    uint32_t crc32;
    uint8_t  active;
    uint8_t  reserved0[3];
    uint32_t image_size;
    uint32_t build_timestamp;
    uint8_t  reserved1[40];
} __attribute__((packed));     /* 64 bytes */
```

### 3.3 SysInfo Partition

```c
#define SYSINFO_MAGIC              0x53594E46U
#define SYSINFO_CLEAN_MARKER       0xC1EA4E00U
#define SYSINFO_CRASH_MARKER       0xCCA5DEADU

struct sysinfo_partition {
    uint32_t magic;
    uint32_t boot_count;
    uint32_t unsafe_shutdown_count;
    uint8_t  last_shutdown_type;
    uint8_t  clean_shutdown_marker_valid;
    uint8_t  crash_marker_valid;
    uint8_t  reserved0;
    uint32_t clean_shutdown_marker;
    uint32_t crash_marker;
    uint64_t last_shutdown_ns;
    uint64_t last_boot_ns;
    uint64_t total_power_on_ns;
    uint32_t wal_sequence_at_shutdown;
    uint32_t checkpoint_seq_at_shutdown;
    uint8_t  active_slot;
    uint8_t  boot_type_last;
    uint8_t  reserved1[50];
    uint32_t crc32;
} __attribute__((packed));     /* 128 bytes */
```

### 3.4 Boot Context

```c
#define BOOT_PHASE_COUNT    6
#define BOOT_LOG_ENTRY_MAX  64
#define BOOT_LOG_MSG_LEN    128

struct boot_log_entry {
    uint64_t timestamp_ns;
    uint8_t  phase;
    uint8_t  level;    /* 0=INFO 1=WARN 2=ERROR */
    char     msg[BOOT_LOG_MSG_LEN];
};

struct boot_ctx {
    enum boot_phase  phase;
    enum boot_type   boot_type;
    enum shutdown_type last_shutdown;
    uint64_t boot_start_ns;
    uint64_t phase_start_ns[BOOT_PHASE_COUNT];
    uint64_t phase_duration_ns[BOOT_PHASE_COUNT];
    uint64_t total_boot_duration_ns;
    uint8_t  active_slot;
    bool     recovery_mode;
    bool     degraded_mode;
    struct boot_log_entry log[BOOT_LOG_ENTRY_MAX];
    uint32_t log_count;
    void *sssim_ctx;
};
```

### 3.5 Power Management Context

```c
#define POWER_DOWN_IO_DRAIN_TIMEOUT_S  30
#define POWER_DOWN_WB_FLUSH_TIMEOUT_S  60

struct power_mgmt_ctx {
    enum shutdown_type shutdown_type;
    uint64_t shutdown_start_ns;
    uint64_t io_drain_end_ns;
    uint64_t wb_flush_end_ns;
    uint64_t l2p_persist_end_ns;
    uint64_t nor_update_end_ns;
    bool     io_drain_ok;
    bool     wb_flush_ok;
    bool     l2p_persist_ok;
    bool     nor_update_ok;
    uint32_t inflight_io_at_start;
    uint32_t wal_entries_flushed;
    void *sssim_ctx;
};
```

---

## 4. Header File Design

### 4.1 include/common/boot.h

```c
#ifndef __HFSSS_BOOT_H
#define __HFSSS_BOOT_H

int boot_init(struct boot_ctx *ctx, struct sssim_ctx *sssim);
enum boot_phase boot_get_phase(const struct boot_ctx *ctx);
int boot_select_firmware_slot(struct nor_ctx *nor_ctx,
                              struct nor_firmware_slot *slot_out);
bool boot_verify_slot_crc(const struct nor_firmware_slot *slot);
int boot_log_flush_to_nor(struct boot_ctx *ctx, struct nor_ctx *nor_ctx);
uint64_t boot_get_phase_duration_ms(const struct boot_ctx *ctx, enum boot_phase phase);
int boot_firmware_update(struct nor_ctx *nor_ctx,
                         const uint8_t *new_image, uint32_t image_size,
                         uint32_t new_version);

#endif /* __HFSSS_BOOT_H */
```

### 4.2 include/common/power_mgmt.h

```c
#ifndef __HFSSS_POWER_MGMT_H
#define __HFSSS_POWER_MGMT_H

int power_down_normal(struct power_mgmt_ctx *ctx);
int power_down_abnormal(struct power_mgmt_ctx *ctx);
enum shutdown_type power_detect_last_shutdown(const struct sysinfo_partition *sysinfo_buf);
int power_recovery(struct power_mgmt_ctx *ctx, enum shutdown_type stype);
int power_recovery_first_boot(struct power_mgmt_ctx *ctx);
void boot_update_smart_on_poweron(struct nvme_smart_log *smart_log, enum shutdown_type stype);
int sysinfo_read(struct nor_ctx *nor_ctx, struct sysinfo_partition *out);
int sysinfo_write(struct nor_ctx *nor_ctx, const struct sysinfo_partition *sysinfo);
void power_install_signal_handlers(struct sssim_ctx *sssim);
void power_install_atexit(struct sssim_ctx *sssim);

#endif /* __HFSSS_POWER_MGMT_H */
```

---

## 5. Function Interface Design

### 5.1 boot_init - Six-Phase Boot Main Sequence

`boot_init(ctx, sssim)` is the top-level entry for the boot module, executing all six phases sequentially:

```
boot_init(ctx, sssim):
  ctx->boot_start_ns = clock_monotonic_ns()

  [Phase 0] boot_phase0_hw_init(ctx)        // ~50ms delay
    +-- nand_device_init, nor_init, mempool_init

  [Phase 1] boot_phase1_post(ctx)           // ~100ms delay
    +-- sysinfo_read -> detect boot type
    +-- boot_select_firmware_slot -> select active slot
    +-- boot_verify_slot_crc

  [Phase 2] boot_phase2_meta_load(ctx)      // ~500ms-2000ms delay
    +-- power_recovery(based on boot type)
    +-- bbt_load_from_nor, ftl_block_pool_init

  [Phase 3] boot_phase3_ctrl_init(ctx)      // ~200ms delay
    +-- arb_init, sched_init, write_buffer_init, read_cache_init

  [Phase 4] boot_phase4_nvme_init(ctx)      // ~50ms delay
    +-- nvme_ctrl_init, nvme_queues_init, CSTS.RDY=1
    +-- boot_update_smart_on_poweron

  [Phase 5] boot_phase5_ready(ctx)
    +-- oob_init, monitoring_thread_start, boot_log_flush_to_nor
    +-- ctx->phase = BOOT_PHASE_5_READY (host I/O accepted)

  return 0
```

### 5.2 boot_select_firmware_slot - Dual Image Selection

```
boot_select_firmware_slot(nor_ctx, slot_out):
  Read Slot A and Slot B headers from NOR
  valid_a = boot_verify_slot_crc(&slot_a)
  valid_b = boot_verify_slot_crc(&slot_b)

  Both valid -> pick higher version (Slot A preferred when equal)
  Only one valid -> use that slot
  Neither valid -> return -1 (recovery mode)
```

### 5.3 power_down_normal - Normal Power-Down Seven-Step Sequence

```
power_down_normal(ctx):
  Step 1: nvme_set_accept_io(false)           // Stop accepting new I/O
  Step 2: Wait for in-flight I/O drain (30s timeout)
  Step 3: write_buffer_flush_all()            // Flush Write Buffer to NAND
  Step 4: l2p_checkpoint_write()              // Update L2P checkpoint
  Step 5: bbt_persist_to_nor() + pe_table_persist_to_nor()  // Update NOR
  Step 6: sysinfo_write(clean_shutdown_marker) // Write clean marker
  Step 7: nvme_set_csts_shst(0x02)            // Notify host Shutdown Complete
```

### 5.4 power_down_abnormal - Abnormal Power-Down Emergency Persistence

```
power_down_abnormal(ctx):
  // Signal handler context: no malloc/free, no pthread locks
  1. Write crash_marker to SysInfo (using pre-allocated static buffer)
  2. wal_flush_pending() -> flush all pending WAL entries
  return 0 or -EIO
```

### 5.5 power_detect_last_shutdown

```
power_detect_last_shutdown(sysinfo_buf):
  magic == 0xFFFFFFFF -> SHUTDOWN_NORMAL (treat as BOOT_FIRST)
  clean_marker valid  -> SHUTDOWN_NORMAL
  crash_marker set    -> SHUTDOWN_ABNORMAL
  neither flag valid  -> SHUTDOWN_POWER_LOSS
```

### 5.6 power_recovery - Classified Recovery Path

```
power_recovery(ctx, stype):
  SHUTDOWN_NORMAL:
    l2p_checkpoint_load() + wal_replay_delta()
  SHUTDOWN_ABNORMAL / SHUTDOWN_POWER_LOSS:
    l2p_checkpoint_load() (or empty init on failure)
    wal_replay_full()
    oob_integrity_spot_check(512 random pages)
  BOOT_FIRST:
    power_recovery_first_boot() -> format NAND, init BBT, zero L2P
```

### 5.7 boot_firmware_update - Online Firmware Upgrade

```
boot_firmware_update(nor_ctx, new_image, image_size, new_version):
  1. Determine inactive slot (opposite of active_slot)
  2. Erase target slot partition
  3. Write new firmware image + header with CRC
  4. Verify written CRC
  5. Atomic flip: activate new slot, deactivate old slot
  6. Update SysInfo active_slot
```

---

## 6. Flow Diagrams

### 6.1 Six-Phase Boot Main Flow

```
boot_init()
    |
    v
[Phase 0] Hardware Init (~50ms)
    +-- nand_device_init, nor_init, mempool_init
    |
    v
[Phase 1] POST Self-Test (~100ms)
    +-- sysinfo_read -> CRC check
    |   +- CRC fail ---------> BOOT_RECOVERY
    |   +- magic=0xFFFFFFFF --> BOOT_FIRST
    +-- power_detect_last_shutdown
    +-- boot_select_firmware_slot
    |   +- Both slots corrupt -> recovery_mode, return -ENODEV
    |
    v
[Phase 2] Metadata Load (~500ms-2000ms)
    +- BOOT_FIRST ----> format_nand + bbt_init + l2p_init_empty
    +- BOOT_NORMAL ---> checkpoint_load + wal_replay_delta
    +- BOOT_RECOVERY -> checkpoint_load + wal_replay_full + spot_check
    |
    v
[Phase 3] Controller Init (~200ms)
    +-- arb, sched, write_buffer, read_cache, channel_lb
    |
    v
[Phase 4] NVMe Init (~50ms)
    +-- nvme_ctrl_init, queues, CSTS.RDY=1
    +-- boot_update_smart_on_poweron
    |
    v
[Phase 5] System Ready
    +-- oob_init, monitoring, boot_log_flush_to_nor
    -> Host I/O accepted
```

### 6.2 Normal Power-Down Flow

```
power_down_normal()
  -> Step 1: Stop accepting new I/O
  -> Step 2: Drain in-flight I/O (30s timeout)
  -> Step 3: Flush Write Buffer
  -> Step 4: L2P checkpoint write
  -> Step 5: BBT + PE tables to NOR
  -> Step 6: Write clean_shutdown_marker
  -> Step 7: CSTS.SHST = 0x02
```

### 6.3 Abnormal Power-Down Flow

```
SIGTERM/SIGINT -> signal_handler()
  -> power_down_abnormal()
     +-- Write crash_marker (signal-safe, no heap alloc)
     +-- wal_flush_pending()

atexit() path:
  -> Check CSTS.SHST == 0x02?
     +- Yes -> skip (already cleanly shutdown)
     +- No  -> power_down_abnormal()
```

---

## 7. Test Plan

| Test ID | Description | Verification Point |
|---------|-------------|-------------------|
| BL-001 | Cold boot (first format) full flow | boot_type==BOOT_FIRST, NAND formatted, L2P zero-init, boot time 3-8s |
| BL-002 | Cold boot (clean shutdown) full flow | boot_type==BOOT_NORMAL, checkpoint loaded, delta WAL replay, CSTS.RDY=1 |
| BL-003 | Abnormal restart (crash_marker present) | boot_type==BOOT_RECOVERY, full WAL replay, unsafe_shutdowns incremented |
| BL-004 | Both NOR slots valid, select higher version | Slot A v10 / Slot B v15 -> active_slot=1 (Slot B) |
| BL-005 | Slot A CRC corrupt, fallback to Slot B | active_slot=1, boot succeeds |
| BL-006 | Both slots corrupt -> recovery_mode | boot_init returns -ENODEV |
| BL-007 | Phase durations measurable | Phase 0 ~50ms, Phase 1 ~100ms, <10% error |
| BL-008 | Boot log written to NOR Log partition | NOR Log contains phase INFO entries with timestamps |
| BL-009 | Normal power-down seven-step completeness | CSTS.SHST=0x02, clean_marker_valid=1, L2P checkpoint updated |
| BL-010 | I/O drain timeout triggers degraded shutdown | After 30s timeout, shutdown_type becomes ABNORMAL |
| BL-011 | Data integrity after normal power-down | Write 1000 LBAs, shutdown, restart, md5sum 100% match |
| BL-012 | SIGTERM triggers abnormal path | crash_marker written, WAL entries flushed |
| BL-013 | SIGKILL + WAL replay recovery | Full WAL replay, BOOT_RECOVERY, data intact |
| BL-014 | atexit guard | exit() without clean shutdown triggers power_down_abnormal |
| BL-015 | SMART power_cycles incremented each boot | 5 boots -> power_cycles==5 |
| BL-016 | SMART unsafe_shutdowns only on abnormal | 1 normal + 2 SIGTERM -> unsafe_shutdowns==2 |
| BL-017 | SysInfo CRC failure triggers BOOT_RECOVERY | sysinfo_read returns -EBADMSG |
| BL-018 | Firmware update uses new slot on restart | Slot B v100, restart -> active_slot=1 |
| BL-019 | Firmware update failure keeps old slot | NOR write error -> -EIO, active_slot unchanged |
| BL-020 | WB flush failure marks ABNORMAL shutdown | crash_marker_valid=1 |
| BL-021 | OOB thread not online until Phase 5 | Socket connection refused before Phase 5, accepted after |
| BL-022 | Phase 2 delay proportional to SSD capacity | 1TB ~500ms, 4TB ~2000ms, <15% error |
| BL-023 | BBT loaded from NOR matches NAND OOB | Pre-written NOR BBT entries reflected in ftl_ctx |
| BL-024 | SysInfo all-0xFF triggers BOOT_FIRST | boot_type==BOOT_FIRST, NAND format executed |
| BL-025 | Degraded mode OOB query | OOB status.get shows "degraded":true |

---

**Document Statistics**:
- Requirements covered: 4 (REQ-073, REQ-074, REQ-075, REQ-076)
- New header files: `include/common/boot.h`, `include/common/power_mgmt.h`
- New source files: `src/common/boot.c`, `src/common/power_mgmt.c`, `src/common/sysinfo.c`
- Function interfaces: 20+
- Test cases: 25

## Appendix: Cross-References

| Reference | Document |
|-----------|----------|
| NOR Flash partition layout | LLD_14_NOR_FLASH |
| WAL format and replay | LLD_15_PERSISTENCE_FORMAT |
| Fault injection power loss | LLD_08_FAULT_INJECTION |
| OOB initialization | LLD_07_OOB_MANAGEMENT |
| Power loss protection | LLD_17_POWER_LOSS_PROTECTION |
