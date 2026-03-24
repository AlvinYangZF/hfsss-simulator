# NOR Flash Simulation Low-Level Design

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
7. [Integration Notes](#7-integration-notes)
8. [Test Plan](#8-test-plan)

---

## 1. Module Overview

The NOR Flash simulation module provides complete NOR Flash device behavior simulation for HFSSS, covering device specifications, fixed partition layout, operation command set, and data persistence.

The module targets a 256MB NOR device, using mmap to map a host filesystem image file into the process address space for low-overhead fast access; critical partition writes use `fsync` to ensure data survival across host crashes. Two interface sets are provided:

1. **Raw Device Interface** (`nor_flash.h`): Exposes Read / Program / Erase / ReadStatus physical command semantics for the HAL layer;
2. **Partition-Level Interface** (`hal_nor_full.h`): Addresses by partition ID, hiding sector alignment and erase cycle management details for FTL and firmware components.

Thread safety is guaranteed by internal `pthread_mutex_t`; callers need not add additional locking.

**Requirements Coverage**: REQ-053, REQ-054, REQ-055, REQ-056.

---

## 2. Requirements Traceability

| Req ID  | Description | Priority | Target |
|---------|-------------|----------|--------|
| REQ-053 | 256MB NOR device; 512B page; 64KB erase sector; 100K P/E cycle limit | P0 | V1.5 |
| REQ-054 | 7 fixed partitions: Bootloader(4MB), FwSlotA(64MB), FwSlotB(64MB), Config(8MB), BBT(8MB), EventLog(16MB), SysInfo(4MB) | P0 | V1.5 |
| REQ-055 | Commands: Read (byte granularity), Program (512B page), Erase (64KB sector), ReadStatus, Reset, ReadID | P0 | V1.5 |
| REQ-056 | Persistence: NOR image stored as host file; mmap fast access; fsync after critical writes | P0 | V1.5 |

---

## 3. Data Structure Design

```c
#define NOR_TOTAL_SIZE_MB      256
#define NOR_PAGE_SIZE          512
#define NOR_SECTOR_SIZE        (64 * 1024)
#define NOR_TOTAL_SECTORS      (NOR_TOTAL_SIZE_MB * 1024 * 1024 / NOR_SECTOR_SIZE)
#define NOR_PE_CYCLE_LIMIT     100000
#define NOR_VENDOR_ID          0xEF
#define NOR_DEVICE_ID          0x4019

enum nor_partition_id {
    NOR_PART_BOOTLOADER = 0, NOR_PART_FW_SLOT_A, NOR_PART_FW_SLOT_B,
    NOR_PART_CONFIG, NOR_PART_BBT, NOR_PART_EVENT_LOG, NOR_PART_SYSINFO,
    NOR_PART_COUNT
};

struct nor_partition {
    enum nor_partition_id id;
    const char *name;
    uint32_t offset;
    uint32_t size;
    bool read_only_at_runtime;
};

struct nor_sector_meta {
    uint32_t erase_count;
    bool bad;
};

struct nor_dev {
    uint8_t *image;
    size_t image_size;
    char image_path[256];
    int image_fd;
    bool use_mmap;
    struct nor_sector_meta sectors[NOR_TOTAL_SECTORS];
    uint8_t status_reg;
    bool write_enabled;
    uint32_t pe_count_total;
    pthread_mutex_t lock;
};
```

**Partition Layout**:

| Partition | Name | Offset | Size | Runtime Read-Only |
|-----------|------|--------|------|-------------------|
| NOR_PART_BOOTLOADER | bootloader | 0 MB | 4 MB | Yes |
| NOR_PART_FW_SLOT_A | fw_slot_a | 4 MB | 64 MB | No |
| NOR_PART_FW_SLOT_B | fw_slot_b | 68 MB | 64 MB | No |
| NOR_PART_CONFIG | config | 132 MB | 8 MB | No |
| NOR_PART_BBT | bbt | 140 MB | 8 MB | No |
| NOR_PART_EVENT_LOG | event_log | 148 MB | 16 MB | No |
| NOR_PART_SYSINFO | sysinfo | 164 MB | 4 MB | No |

Total used: 168 MB; 88 MB reserved for future expansion.

---

## 4. Header File Design

Key functions in `include/media/nor_flash.h`:

```c
int nor_dev_init(struct nor_dev *dev, const char *image_path);
int nor_dev_format(struct nor_dev *dev);
void nor_dev_cleanup(struct nor_dev *dev);
int nor_read(struct nor_dev *dev, uint32_t offset, void *buf, size_t len);
int nor_page_program(struct nor_dev *dev, uint32_t offset, const void *buf, size_t len);
int nor_sector_erase(struct nor_dev *dev, uint32_t sector_idx);
int nor_chip_erase(struct nor_dev *dev);
uint8_t nor_read_status(struct nor_dev *dev);
void nor_write_enable(struct nor_dev *dev);
void nor_write_disable(struct nor_dev *dev);
void nor_read_id(struct nor_dev *dev, uint8_t buf[3]);
int nor_fsync_critical(struct nor_dev *dev, enum nor_partition_id part_id);
```

Key functions in `include/hal/hal_nor_full.h`:

```c
int nor_partition_read(struct nor_dev *dev, enum nor_partition_id part_id,
                       uint32_t offset_in_part, void *buf, size_t len);
int nor_partition_write(struct nor_dev *dev, enum nor_partition_id part_id,
                        uint32_t offset_in_part, const void *buf, size_t len);
int nor_partition_erase(struct nor_dev *dev, enum nor_partition_id part_id);
const struct nor_partition *nor_partition_info(enum nor_partition_id part_id);
```

---

## 5. Function Interface Design

### 5.1 nor_dev_init

Opens or creates the NOR image file, truncates/extends to 256MB, mmaps it, verifies or writes magic header, initializes sector metadata.

### 5.2 nor_page_program

NOR AND semantics: bits can only be cleared (`image[i] &= buf[i]`). Requires WEL; clears WEL after completion. Calls msync for critical partitions (BBT, SysInfo).

### 5.3 nor_sector_erase

Sets all bytes in sector to 0xFF, increments erase_count, marks sector bad if count exceeds PE_CYCLE_LIMIT. Requires WEL.

### 5.4 nor_partition_write

Handles the full read-modify-erase-reprogram cycle across all affected 64KB sectors. For critical partitions (BBT, SysInfo) calls nor_fsync_critical after the final reprogram.

---

## 6. Flow Diagrams

### 6.1 Page Program Flow

```
nor_page_program() -> WEL set? -> aligned? -> sector bad?
  -> image[i] &= buf[i] (AND semantics)
  -> critical partition? -> msync
  -> clear WEL -> return 0
```

### 6.2 Partition Write (Read-Modify-Erase-Write)

```
nor_partition_write() -> read-only check -> range check
  -> for each affected sector:
     1. Read sector to tmp[]
     2. Merge new data into tmp
     3. Erase sector
     4. Program sector page by page from tmp
  -> critical partition? -> fsync
```

---

## 7. Integration Notes

| Module | Integration |
|--------|-------------|
| hal_nor.c | Delegates all operations to nor_dev |
| LLD_09_BOOTLOADER | Reads firmware images and SysInfo via nor_partition_read |
| LLD_11_FTL_RELIABILITY | BBT dual-write via nor_partition_write with fsync |
| log.c | Event log append to NOR_PART_EVENT_LOG |

---

## 8. Test Plan

| Test ID | Description | Expected Result |
|---------|-------------|----------------|
| NF-001 | nor_dev_init creates new image | File created, 256MB, magic written |
| NF-002 | nor_dev_init opens existing image | Magic verified, no format |
| NF-003 | nor_dev_format then full read | All bytes 0xFF; all erase_count 0 |
| NF-004 | nor_read out of range | Returns -ERANGE |
| NF-005 | nor_page_program AND semantics | Readback = original & written |
| NF-006 | nor_page_program without WEL | Returns -EPERM |
| NF-007 | nor_page_program unaligned | Returns -EINVAL |
| NF-008 | nor_sector_erase | All 0xFF; erase_count +1 |
| NF-009 | Erase count exceeds PE limit | sector.bad = true; next erase returns -EIO |
| NF-010 | nor_partition_write to Bootloader | Returns -EACCES |
| NF-011 | nor_partition_write cross-sector | read-modify-erase-reprogram; readback matches |
| NF-012 | BBT partition write triggers fsync | nor_fsync_critical called |
| NF-013 | Data persistence: write, cleanup, reinit, read | Data matches original |
| NF-014 | nor_read_id values | 0xEF, 0x40, 0x19 |
| NF-015 | Multi-thread concurrent read + write | No data races; consistent reads |

---

**Document Statistics**:
- Requirements covered: 4 (REQ-053, REQ-054, REQ-055, REQ-056)
- Header files: `include/media/nor_flash.h`, `include/hal/hal_nor_full.h`
- Function interfaces: 20+
- Test cases: 15

## Appendix: Cross-References

| Reference | Document |
|-----------|----------|
| Bootloader NOR access | LLD_09_BOOTLOADER |
| BBT dual-mirror write | LLD_11_FTL_RELIABILITY |
| SysInfo power management | LLD_09_BOOTLOADER |
| Key storage in NOR | LLD_19_SECURITY_ENCRYPTION |
