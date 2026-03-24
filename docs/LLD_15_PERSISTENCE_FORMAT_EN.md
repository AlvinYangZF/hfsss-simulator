# Persistence Format Interface Low-Level Design

## Revision History

| Version | Date       | Author | Description     |
|---------|------------|--------|-----------------|
| V1.0    | 2026-03-15 | HFSSS  | Initial release |

## Table of Contents

1. [Module Overview](#1-module-overview)
2. [Requirements Traceability](#2-requirements-traceability)
3. [Binary Format Design](#3-binary-format-design)
4. [Header File Design](#4-header-file-design)
5. [Function Interface Design](#5-function-interface-design)
6. [Flow Diagrams](#6-flow-diagrams)
7. [Capacity and Performance Analysis](#7-capacity-and-performance-analysis)
8. [Test Plan](#8-test-plan)

---

## 1. Module Overview

The Persistence Format module defines the on-disk binary layout for all HFSSS persistent state. This document is a pure format/protocol specification without runtime logic, focusing on binary field layout, magic numbers, version numbers, and compatibility rules.

Persistent state consists of five file categories:

1. **NAND Data Files**: Raw NAND page data, one file per plane;
2. **L2P Checkpoint Files**: Complete snapshots of the logical-to-physical address mapping table;
3. **WAL Files (Write-Ahead Log)**: Incremental mapping updates for crash recovery;
4. **NOR Image File**: NOR Flash binary image (covered by LLD_14, referenced here);
5. **Panic Dump Files**: Crash scene state dumps for post-mortem debugging.

All files use little-endian byte order. All headers include CRC verification fields that must be validated before reading data regions.

**Requirements Coverage**: REQ-131 (persistence format specification), implicit dependency on REQ-051 (persistent write), REQ-052 (crash recovery).

---

## 2. Requirements Traceability

| Req ID  | Description | Priority | Target |
|---------|-------------|----------|--------|
| REQ-131 | Persistence format specification: binary layout, magic numbers, versioning | P0 | V1.5 |
| REQ-051 | Persistent write: L2P checkpoint and WAL file atomic write semantics | P0 | V1.5 |
| REQ-052 | Crash recovery: load from latest valid checkpoint, replay WAL increments | P0 | V1.5 |

---

## 3. Binary Format Design

### 3.1 NAND Data File Format

File naming: `<checkpoint_dir>/nand_ch<CH>_chip<CHIP>_die<DIE>_plane<PLANE>.dat`

Header (80 bytes): magic("HFSSS_ND"), format_version, flags, channel/chip/die/plane IDs, blocks_per_plane, pages_per_block, page_size, oob_size, timestamps, total_file_size, header_crc32.

Data region: blocks x pages x (page_size + oob_size) bytes. Unwritten pages filled with 0xFF.

OOB per page (384 bytes): LPN (0xFFFF...=unwritten), write_timestamp, ECC syndrome, OOB CRC32.

### 3.2 L2P Checkpoint File Format

File naming: `<checkpoint_dir>/l2p_checkpoint_<SEQ>.dat`

Header (64 bytes): magic("HFSSS_L2"), format_version, flags, total_lpn_count, checkpoint_seq, timestamp, host_bytes_written, data_crc64, header_crc32.

Data region: uint64_t[total_lpn_count], indexed by LPN, value = PPN (0xFFFF...=unmapped).

Retention: keep latest 3 generations; delete oldest after new checkpoint.

### 3.3 WAL File Format

File path: `<wal_dir>/hfsss.wal`. Sequential append; truncated to header-only after successful checkpoint.

Header (64 bytes): magic("HFSSS_WL"), format_version, flags, associated_checkpoint_seq, timestamp, record_count, header_crc32.

WAL Record (fixed 64 bytes): seq, record_type, payload_len (<=40), payload[40], record_crc32, end_marker (0xDEADBEEF).

```c
enum wal_record_type {
    WAL_REC_L2P_UPDATE       = 0x0001,
    WAL_REC_BLOCK_STATE      = 0x0002,
    WAL_REC_ERASE_COUNT      = 0x0003,
    WAL_REC_BAD_BLOCK        = 0x0004,
    WAL_REC_CHECKPOINT_BEGIN = 0x0010,
    WAL_REC_CHECKPOINT_END   = 0x0011,
    WAL_REC_SHUTDOWN_CLEAN   = 0x0020,
    WAL_REC_SHUTDOWN_PANIC   = 0x0021,
};

struct wal_payload_l2p {
    uint64_t lpn, old_ppn, new_ppn, timestamp;
    uint8_t reserved[8];
};  /* 40 bytes */
```

### 3.4 Panic Dump File

Path: `/var/hfsss/panic_dump_<timestamp>.bin`

Header (400 bytes): magic("HFSSS_PD"), format_version, firmware_build_hash, panic_timestamp, reason_code, thread_id, message[256], stack_frames[8], last_checkpoint_seq, last_wal_seq. Followed by partial L2P summary (first/last 1000 entries).

### 3.5 Version Compatibility Rules

- `current_version < min_reader_version`: Reject, require migration
- `writer_version > current_version`: Open with warning (forward compatible)
- Magic mismatch: Reject immediately
- Header CRC mismatch: Reject, do not attempt data read

---

## 4. Header File Design

```c
/* include/common/persistence_fmt.h */
#define HFSSS_FMT_VERSION_CURRENT  1u
#define HFSSS_WAL_END_MARKER  0xDEADBEEFu
#define HFSSS_UNMAPPED        UINT64_C(0xFFFFFFFFFFFFFFFF)

struct nand_file_header { /* 80 bytes, packed */ };
struct nand_oob_entry { /* 384 bytes, packed */ };
struct l2p_checkpoint_header { /* 64 bytes, packed */ };
struct wal_file_header { /* 64 bytes, packed */ };
struct wal_record { /* 64 bytes, packed */ };
struct panic_dump_header { /* 400 bytes, packed */ };

/* static_assert on all struct sizes */
```

---

## 5. Function Interface Design

### 5.1 nand_file_open / nand_file_write_page / nand_file_read_page

Open/create NAND data file, verify magic and CRC. Write/read page data + OOB at calculated offset.

### 5.2 l2p_checkpoint_write

Write to temp file, stream mapping data with CRC64 accumulation, backfill header, fsync, atomic rename. Delete oldest if more than 3 generations.

### 5.3 l2p_checkpoint_read

Enumerate checkpoint files by descending seq, try each: verify magic -> header CRC -> read data -> verify data CRC64 -> load mapping. Return -ENODATA if all fail.

### 5.4 wal_open / wal_append / wal_replay / wal_truncate

- **wal_append**: Construct 64-byte record, compute CRC32, pwrite at sequential offset, fdatasync.
- **wal_replay**: Read records sequentially, verify end_marker (0xDEADBEEF) and CRC32, apply L2P updates. Stop on incomplete or corrupt record.
- **wal_truncate**: ftruncate to 64 bytes (header only), reset record_count and seq counter.

### 5.5 panic_dump_write

Signal-safe: write header + partial L2P summary, fsync. Each dump gets unique timestamp filename.

---

## 6. Flow Diagrams

### 6.1 Checkpoint Write Flow

```
l2p_checkpoint_write()
  -> Open temp file .tmp
  -> Stream mapping data with CRC64
  -> Backfill header (data_crc64, header_crc32)
  -> fsync + close
  -> rename(.tmp -> .dat)  // atomic
  -> Delete oldest if > 3 generations
```

### 6.2 Crash Recovery Flow

```
Startup:
  -> l2p_checkpoint_read() // find newest valid checkpoint
  -> wal_open() // open WAL if exists
  -> wal_replay() // apply records until incomplete/corrupt
  -> Recovery complete
```

### 6.3 WAL Record Integrity Check

```
Read 64 bytes -> check end_marker == 0xDEADBEEF?
  No -> WARNING "incomplete record", stop replay
  Yes -> compute CRC32(bytes 0-55) == record_crc32?
    No -> WARNING "CRC mismatch", stop replay
    Yes -> apply record by type
```

---

## 7. Capacity and Performance Analysis

| Metric | Calculation | Result |
|--------|-------------|--------|
| L2P checkpoint size (2TB) | 512M LPN x 8B | 4 GB/file |
| L2P write time | 4 GB / 1 GB/s | ~4 seconds |
| 3-generation storage | 4 GB x 3 | 12 GB |
| WAL record size | Fixed | 64 bytes |
| WAL max file size | 256K records x 64B | 16 MB |
| Single wal_append latency | Including fdatasync | ~100 us |
| Panic dump size | Header + 2000 entries | ~16 KB |

---

## 8. Test Plan

| Test ID | Description | Verification Point |
|---------|-------------|-------------------|
| PF-001 | L2P checkpoint write then read | All LPN->PPN mappings match |
| PF-002 | Header CRC32 corruption | Returns -EILSEQ, rejects file |
| PF-003 | Data CRC64 corruption | Falls back to previous generation |
| PF-004 | WAL append 1M records then replay | All records applied correctly |
| PF-005 | WAL last record truncated (<64 bytes) | Replay stops cleanly |
| PF-006 | WAL record CRC error | Replay stops at N-1, WARNING |
| PF-007 | WAL end_marker invalid | Treated as incomplete, stop |
| PF-008 | NAND data file: write page, reopen, read | Data + OOB match |
| PF-009 | Unwritten page OOB LPN | lpn == HFSSS_UNMAPPED |
| PF-010 | NAND file header magic mismatch | Returns -ENOTSUP |
| PF-011 | Panic dump: trigger then verify | File exists, magic correct |
| PF-012 | Version forward compatibility | Opens with WARNING |
| PF-013 | Version requires migration | Rejects with error |
| PF-014 | Checkpoint rotation: 4 writes | Only 3 files remain |
| PF-015 | Full crash recovery flow | checkpoint -> WAL -> consistent state |
| PF-016 | WAL truncate verification | File size == 64 bytes |

---

**Document Statistics**:
- Requirements covered: REQ-131 (primary), REQ-051, REQ-052 (implicit)
- Header file: `include/common/persistence_fmt.h`
- Function interfaces: 20+
- Test cases: 16

## Appendix: Cross-References

| Reference | Document |
|-----------|----------|
| Bootloader recovery paths | LLD_09_BOOTLOADER |
| L2P checkpoint triggers | LLD_11_FTL_RELIABILITY |
| NOR Flash image format | LLD_14_NOR_FLASH |
| Power loss and WAL | LLD_17_POWER_LOSS_PROTECTION |
