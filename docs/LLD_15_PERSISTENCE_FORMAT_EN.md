# Persistence Format Interface Low-Level Design

## Revision History

| Version | Date       | Author | Description                                                              |
|---------|------------|--------|--------------------------------------------------------------------------|
| V1.0    | 2026-03-15 | HFSSS  | Initial release (aspirational; diverged from shipping code)              |
| V1.1    | 2026-04-22 | HFSSS  | Reconciled with shipping on-disk layouts; headers corrected; panic-dump moved to future work |

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

The Persistence Format module defines the on-disk and on-NAND binary layout for all HFSSS persistent state. This document is a format/protocol specification without runtime logic, focusing on binary field layout, magic numbers, version numbers, and compatibility rules.

Persistent state consists of four active artefacts:

1. **Media checkpoint file** (`checkpoint.bin`): complete or incremental snapshot of NAND page data + per-page metadata + BBT. Produced by the media layer.
2. **Superblock-resident checkpoint**: L2P mapping snapshot + journal pages, written into reserved NAND superblocks. Produced by the FTL.
3. **WAL (Write-Ahead Log)**: in-memory ring of 64-byte records for crash recovery. Optional on-disk persistence via `wal_save` / `wal_load`.
4. **NOR image file**: NOR Flash binary image, specified in LLD_14_NOR_FLASH.

Panic-dump files are reserved for a future release (see §3.5) — the current boot flow only emits log-level panic records via the standard log sink.

All multi-byte fields use host byte order (little-endian on all supported targets). All headers include a magic number and, where applicable, a CRC field that must be validated before trusting the payload region.

**Requirements Coverage**: REQ-131 (persistence format specification), implicit dependency on REQ-051 (persistent write), REQ-052 (crash recovery).

---

## 2. Requirements Traceability

| Req ID  | Description                                                                     | Priority | Target |
|---------|---------------------------------------------------------------------------------|----------|--------|
| REQ-131 | Persistence format specification: binary layout, magic numbers, versioning      | P0       | V1.5   |
| REQ-051 | Persistent write: media + superblock checkpoint + WAL atomic write semantics    | P0       | V1.5   |
| REQ-052 | Crash recovery: load from latest valid checkpoint, replay journal / WAL deltas  | P0       | V1.5   |

---

## 3. Binary Format Design

### 3.1 Media Checkpoint File (`checkpoint.bin`)

A single file produced by `media_save` / `media_save_incremental` (see `src/media/media.c`). File path is provided by the caller; the default convention is `<checkpoint_dir>/checkpoint.bin`.

**File header** (internal to the media layer, not exported):

```c
#define MEDIA_FILE_MAGIC   0x48465353u  /* "HFSS" little-endian */
#define MEDIA_FILE_VERSION 2u           /* incremental-checkpoint support */

#define MEDIA_FILE_FLAG_FULL        0x00000000u
#define MEDIA_FILE_FLAG_INCREMENTAL 0x00000001u

struct media_file_header {
    u32                  magic;            /* MEDIA_FILE_MAGIC */
    u32                  version;          /* MEDIA_FILE_VERSION */
    u32                  flags;            /* FULL | INCREMENTAL */
    struct media_config  config;           /* channel / chip / die / plane / block / page geometry */
    struct media_stats   stats;            /* aggregate counters */
    u64                  nand_data_offset; /* start of NAND data region */
    u64                  bbt_offset;       /* start of BBT region */
};
```

A V1 reader-compat shim (`struct media_file_header_v1`) retains the V1 layout (no `flags` field) so V2 code can load pre-V2 checkpoints and re-save them as V2. Newer writers never emit V1.

**NAND data region**: enumerated in channel / chip / die / plane / block / page order.

For each block: `dirty` (bool) + `state` (`enum block_state`, u32) + `pages_written` (u32). If `flags & INCREMENTAL` and the block is clean, only the `dirty=false` byte is written and per-page data is skipped.

For each page: `dirty` (bool) + `struct page_metadata` + `page_size` bytes of data + `spare_size` bytes of OOB.

```c
struct page_metadata {
    enum page_state state;         /* erased / programmed / error */
    u64             program_ts;
    u32             erase_count;
    u32             bit_errors;
    u32             read_count;
};
```

**BBT region**: appended after NAND data via `bbt_save()`; see `src/media/bbt.c`.

**Incremental-mode caveat.** `write_file_header` computes `bbt_offset` from the *full* NAND footprint (block + page metadata + page data + OOB) regardless of the `flags` value. When `flags & INCREMENTAL`, clean blocks and pages are written as a single `false` byte, so fewer bytes land between `nand_data_offset` and the actual BBT. `bbt_offset` in the header therefore does **not** point at the true BBT start in an incremental file — it is only accurate for `FULL` files. The shipping code compensates by sequentially reading the NAND data region (which self-describes its size from the config geometry) and ignoring `bbt_offset` during load. `tests/systest_persistence.c` documents this as a known limitation; callers that want the header offsets to be authoritative on reload should use `media_save` rather than `media_save_incremental`.

### 3.2 Superblock / FTL L2P Checkpoint (on-NAND)

The FTL keeps its L2P checkpoint and operation journal in reserved NAND superblocks rather than in a separate host-side file. Layouts and magic numbers are in `include/ftl/superblock.h`.

```c
#define SB_HEADER_MAGIC 0x53425F48u  /* "SB_H" */
#define SB_CKPT_MAGIC   0x434B5054u  /* "CKPT" */
#define SB_JRNL_MAGIC   0x4A524E4Cu  /* "JRNL" */

enum sb_page_type {
    SB_PAGE_HEADER    = 0,
    SB_PAGE_CKPT_DATA = 1,
    SB_PAGE_JRNL_DATA = 2,
    SB_PAGE_CKPT_END  = 3,
};

/* 32-byte header at the start of every superblock page */
struct sb_page_header {
    u32 magic;        /* SB_HEADER_MAGIC | SB_CKPT_MAGIC | SB_JRNL_MAGIC */
    u32 page_type;    /* enum sb_page_type */
    u64 sequence;     /* monotonic sequence number */
    u32 page_index;   /* offset within the logical segment */
    u32 total_pages;  /* total pages in this checkpoint/journal segment */
    u32 crc32;        /* CRC over the data portion after this header */
    u32 reserved;
};
```

**Checkpoint payload** (`SB_PAGE_CKPT_DATA`): streamed as an array of `ckpt_entry`:

```c
struct ckpt_entry {
    u64 lba;
    u64 ppn_raw;  /* HFSSS_UNMAPPED if the LBA has no mapping */
};
```

**Journal payload** (`SB_PAGE_JRNL_DATA`): streamed as an array of `journal_entry`:

```c
enum journal_op { JRNL_OP_WRITE = 1, JRNL_OP_TRIM = 2 };

struct journal_entry {
    u32 op;
    u32 reserved;
    u64 lba;
    u64 ppn_raw;   /* new PPN for WRITE; 0 for TRIM */
    u64 sequence;  /* monotonic within the journal */
};
```

### 3.3 WAL (Write-Ahead Log)

Layouts and magic numbers are in `include/ftl/wal.h`. The WAL context lives in memory as a ring of 64-byte records; `wal_save` / `wal_load` provide optional file-backed persistence for clean power cycles.

```c
#define WAL_RECORD_MAGIC  0x12345678u
#define WAL_COMMIT_MARKER 0xDEADBEEFu
#define WAL_RECORD_SIZE   64
#define WAL_MAX_RECORDS   (16 * 1024)

enum wal_rec_type {
    WAL_REC_L2P_UPDATE   = 1,
    WAL_REC_TRIM         = 2,
    WAL_REC_BBT_UPDATE   = 3,
    WAL_REC_SMART_UPDATE = 4,
    WAL_REC_COMMIT       = 0xFF,
};

/* 64-byte record */
struct wal_record {
    u32 magic;       /* WAL_RECORD_MAGIC */
    u32 type;        /* enum wal_rec_type */
    u64 sequence;    /* monotonic within the WAL */
    u64 lba;
    u64 ppn;
    u8  meta[16];    /* type-specific metadata */
    u32 crc32;       /* CRC-32 over bytes [0..55] */
    u32 end_marker;  /* 0 for data records, WAL_COMMIT_MARKER for COMMIT */
};
```

`WAL_REC_COMMIT` records carry `end_marker = WAL_COMMIT_MARKER` and advance the "last durably-committed sequence" counter maintained by `wal_get_committed_seq`. That counter is a helper exposed for callers that want to reason about the last safe point; the replay path itself does **not** consult it.

`wal_replay` iterates the in-memory ring in order, validates each record's `magic` + `crc32`, stops at the first bad record, skips `WAL_REC_COMMIT` entries (control markers, not state deltas), and forwards every other valid record to the caller-supplied callback. There is no "records past the last COMMIT are dropped" behavior in the shipped implementation — every valid record ahead of the first corrupt slot is replayed.

The on-disk WAL file (when used) is a flat append of `struct wal_record` entries with **no file preamble**: `wal_save` writes `count × sizeof(struct wal_record)` bytes directly, and `wal_load` reads records one at a time, stopping on bad magic, CRC mismatch, or ring-capacity exhaustion.

### 3.4 NOR Image

See LLD_14_NOR_FLASH for the full NOR partition table and on-disk `mmap(MAP_SHARED)` layout (bootloader / fw-slot-a / fw-slot-b / config / bbt / event-log / sysinfo / keys).

### 3.5 Panic Dump (Future Work)

The initial V1.0 draft of this document specified a `panic_dump_<timestamp>.bin` file format with a 400-byte header. That design is not shipped: `hfsss_panic()` in `src/common/log.c` prints `HFSSS PANIC` directly to `stderr` (bypassing the structured log sink) and the current boot flow relies on the superblock checkpoint + WAL for post-mortem state. A dedicated panic-dump file format is reserved for a future release; the V1.0 draft remains on file as the starting point for that work.

### 3.6 Version Compatibility Rules

- `current_version < min_reader_version`: reject; require explicit migration.
- `writer_version > current_version`: open with warning (forward-compatible read where the trailing new fields are ignored).
- Magic mismatch: reject immediately.
- CRC mismatch: reject the specific artefact and return an I/O error to the caller. The shipped implementations do **not** fall back to older generations — `sb_checkpoint_read` picks the single highest-sequence superblock header and gives up if its data pages fail CRC, and the media checkpoint is a single file, so a CRC failure there has nothing to fall back to.

For the media checkpoint, V1 → V2 is handled automatically via `struct media_file_header_v1`; newer writers never emit V1.

---

## 4. Header File Design

The persistence types are split across three shipping headers; there is no single `include/common/persistence_fmt.h`.

| Header                       | Types                                                                                       |
|------------------------------|---------------------------------------------------------------------------------------------|
| `include/ftl/superblock.h`   | `SB_*_MAGIC`, `enum sb_page_type`, `struct sb_page_header`, `struct ckpt_entry`, `struct journal_entry`, `enum journal_op` |
| `include/ftl/wal.h`          | `WAL_RECORD_MAGIC`, `WAL_COMMIT_MARKER`, `enum wal_rec_type`, `struct wal_record`, `struct wal_ctx` |
| `src/media/media.c` (static) | `MEDIA_FILE_MAGIC`, `MEDIA_FILE_VERSION`, `struct media_file_header`, `struct media_file_header_v1`, `struct page_metadata` |

Media-layer types are deliberately kept internal because the checkpoint file is consumed only by `media_save` / `media_load`. If an external consumer (e.g. an offline tool) is added in the future, the relevant structs will move to a public header at that time.

---

## 5. Function Interface Design

### 5.1 Media checkpoint

```c
int media_save(struct media_ctx *ctx, const char *filepath);             /* full */
int media_save_incremental(struct media_ctx *ctx, const char *filepath); /* delta */
int media_load(struct media_ctx *ctx, const char *filepath);
```

- `media_save` writes a V2 `struct media_file_header` with `flags = MEDIA_FILE_FLAG_FULL`, followed by the full NAND data region and the BBT.
- `media_save_incremental` writes the same header layout with `flags = MEDIA_FILE_FLAG_INCREMENTAL`; clean blocks and pages are emitted as a single boolean and skipped.
- `media_load` reads the header, detects V1 via the shim, and streams the NAND data + BBT back into the in-memory media context.

### 5.2 Superblock / L2P checkpoint

```c
int  sb_checkpoint_write(struct superblock_ctx *sb, struct mapping_ctx *mapping);
int  sb_checkpoint_read (struct superblock_ctx *sb, struct mapping_ctx *mapping);
int  sb_journal_append  (struct superblock_ctx *sb, enum journal_op op, u64 lba, u64 ppn_raw);
int  sb_journal_flush   (struct superblock_ctx *sb);
int  sb_journal_replay  (struct superblock_ctx *sb, struct mapping_ctx *mapping);
int  sb_recover         (struct superblock_ctx *sb, struct mapping_ctx *mapping, struct block_mgr *mgr);
bool sb_has_valid_checkpoint(struct superblock_ctx *sb);
```

Checkpoint pages are streamed with an `SB_HEADER_MAGIC` header, followed by `SB_CKPT_MAGIC` data pages, terminated by an `SB_PAGE_CKPT_END` page. Each page carries its own `sb_page_header` + `crc32`. Journals grow with `sb_journal_append`; `sb_journal_flush` forces the active page out to NAND. Recovery applies the newest valid checkpoint and then replays the journal.

### 5.3 WAL

```c
int  wal_init    (struct wal_ctx *ctx, u32 max_records);
void wal_cleanup (struct wal_ctx *ctx);

int  wal_append  (struct wal_ctx *ctx, enum wal_rec_type type,
                  u64 lba, u64 ppn, const void *meta, u32 meta_len);
int  wal_commit  (struct wal_ctx *ctx);
int  wal_replay  (const struct wal_ctx *ctx, wal_replay_cb cb, void *user_data);

u64  wal_get_committed_seq(const struct wal_ctx *ctx);
int  wal_truncate(struct wal_ctx *ctx, u64 up_to_seq);
u32  wal_get_count (const struct wal_ctx *ctx);
void wal_reset     (struct wal_ctx *ctx);

int  wal_save    (const struct wal_ctx *ctx, const char *filepath);
int  wal_load    (struct wal_ctx *ctx, const char *filepath);
```

Each `wal_append` stamps a new record with `WAL_RECORD_MAGIC`, monotonic sequence, type, LBA, PPN, type-specific metadata, and a CRC-32 over bytes [0..55]. `wal_commit` appends a `WAL_REC_COMMIT` entry with `end_marker = WAL_COMMIT_MARKER`.

`wal_replay` validates each record's magic + CRC in order, stops at the first bad slot, skips COMMIT records (they are control markers), and forwards every other valid record to the callback. It does **not** consult `wal_get_committed_seq`, so there is no "drop anything past the last COMMIT" behavior — every valid record ahead of the first corrupt slot is replayed. `wal_get_committed_seq` remains available as a separate helper for callers that want to reason about the last durable commit point.

Note that `wal_replay` is currently an exported API with no production caller: the shipped recovery path is entirely superblock-driven (see `sb_recover` below). WAL coverage lives in unit tests only (`tests/test_foundation.c`).

---

## 6. Flow Diagrams

### 6.1 Full Checkpoint Write Flow

```
media_save(ctx, "checkpoint.bin")
  -> fopen(filepath, "wb")
  -> write_file_header(flags = FULL)
  -> write_nand_data(full)          # every block + page + metadata
  -> bbt_save()                     # BBT region
  -> fclose()
```

### 6.2 Incremental Checkpoint Write Flow

```
media_save_incremental(ctx, "checkpoint.bin")
  -> fopen(filepath, "wb")
  -> write_file_header(flags = INCREMENTAL)
  -> write_nand_data(incremental)   # clean blocks/pages = single false byte
  -> bbt_save()
  -> fclose()
```

### 6.3 Crash Recovery Flow (FTL side)

```
startup
  -> sb_recover(sb, mapping, mgr)
       -> sb_has_valid_checkpoint?
            no -> cold start
            yes -> sb_checkpoint_read
                   -> sb_journal_replay (apply WRITE / TRIM deltas in seq order)
  -> recovery complete
```

`wal_replay` is not wired into the production recovery path (`sssim_init` / `ftl_init` only call `sb_recover`). The WAL API is available for callers that want to layer their own replay on top of the in-memory ring; `tests/test_foundation.c` exercises it at the unit level.

### 6.4 Superblock Page Integrity Check

```
read sb_page_header at page offset
  magic in { SB_HEADER_MAGIC, SB_CKPT_MAGIC, SB_JRNL_MAGIC } ?
    no  -> reject page, return HFSSS_ERR_IO (no older-generation fallback)
    yes -> recompute CRC over the data region
           matches header.crc32 ?
             no  -> reject page, return HFSSS_ERR_IO
             yes -> apply by page_type
```

`sb_checkpoint_read` scans page 0 of every reserved superblock, picks the one with the highest `sequence` in its header, and reads the rest of the checkpoint from that block. If any data page in the chosen block fails CRC, the read aborts with `HFSSS_ERR_IO` — the shipped code does **not** retry against older-generation superblocks.

### 6.5 WAL Record Integrity Check

```
for each 64-byte slot in the WAL ring:
  magic == WAL_RECORD_MAGIC ?
    no  -> stop replay (empty / corrupt tail)
    yes -> recompute CRC over bytes [0..55]
           matches record.crc32 ?
             no  -> stop replay (corrupt record)
             yes -> if type == WAL_REC_COMMIT
                        -> skip (control marker, not forwarded to callback)
                    else
                        -> forward to callback(rec, user_data)
```

`wal_get_committed_seq` runs an independent pass over the ring to find the highest-sequence COMMIT; it is not part of replay. Production code that cares about the last durable commit point calls that helper directly.

---

## 7. Capacity and Performance Analysis

Figures below are for a 2 TB geometry (512 M LPN, 4 KiB page) unless noted.

| Metric                                        | Calculation                          | Result      |
|-----------------------------------------------|--------------------------------------|-------------|
| Media checkpoint size (full, 2 TB NAND data)  | `total_pages × (page_size + spare_size)` dominant term | ≈ data size + metadata (~1%) |
| Media checkpoint size (incremental, idle)     | one bool per block + BBT             | ~ total_blocks bytes + BBT |
| Superblock L2P checkpoint size                | `512 M × 16 B (ckpt_entry)`          | 8 GB spread across superblock pages |
| WAL in-memory ring                            | `WAL_MAX_RECORDS × 64 B`             | 1 MB        |
| WAL on-disk file (when persisted)             | `count × 64 B` (no preamble)         | ≤ 1 MB       |
| Superblock page header overhead               | `sizeof(struct sb_page_header)`       | 32 B / page |
| `wal_append` latency (in-memory only)          | memcpy + CRC                         | < 1 µs      |
| `wal_save` latency (flush to disk)             | one `fwrite` + `fclose`              | ~100 µs     |

---

## 8. Test Plan

Shipping persistence tests:

| Test ID     | Source                                           | Covers                                           |
|-------------|--------------------------------------------------|--------------------------------------------------|
| PF-SB-*     | `tests/test_superblock.c`                        | Superblock header / checkpoint / journal round-trips, magic & CRC rejection, recovery |
| PF-PWR-*    | `tests/test_power_cycle.c`                       | End-to-end crash recovery via `media_save` + reboot + `media_load` + journal replay |
| PF-MEDIA-*  | `tests/test_media.c::test_persistence`           | Media checkpoint write/read (full-only) at the `media_save` / `media_load` level |
| PF-PER-*    | `tests/systest_persistence.c`                    | Full-vs-incremental checkpoint behaviour, including the `bbt_offset` limitation noted in §3.1 |
| PF-WAL-*    | `tests/test_foundation.c` (WAL unit sections)    | `wal_init` / `wal_append` / `wal_commit` / `wal_replay` / `wal_save` / `wal_load` round-trips, magic + CRC rejection |

Representative test points (verified by the shipping suites):

| Case ID | Description                                                             | Verification Point                                      |
|---------|-------------------------------------------------------------------------|---------------------------------------------------------|
| PF-001  | Superblock checkpoint write then read                                   | All LBA → PPN mappings round-trip                       |
| PF-002  | Corrupt `sb_page_header.crc32`                                           | `sb_checkpoint_read` returns `HFSSS_ERR_IO`; no older-generation retry |
| PF-003  | Corrupt checkpoint entry region                                         | CRC mismatch → reject with `HFSSS_ERR_IO`                |
| PF-004  | WAL append N records, `wal_replay` with callback                        | Every valid non-COMMIT record forwarded to callback; COMMIT records skipped |
| PF-005  | Interrupted WAL (final slot has `magic = 0`)                             | Replay stops at the bad slot; records before it are still forwarded |
| PF-006  | WAL record CRC error                                                    | Replay stops at record N-1; no later records forwarded  |
| PF-007  | Magic mismatch on any persisted artefact                                | Reader rejects, does not attempt to parse payload       |
| PF-008  | Media checkpoint: save (full) → reopen → load → verify page data + OOB  | Page data + metadata match                              |
| PF-009  | Media checkpoint: save (incremental) after partial dirty                | Clean blocks skipped; dirty blocks round-trip; `bbt_offset` divergence tolerated by sequential load |
| PF-010  | `MEDIA_FILE_MAGIC` mismatch                                             | `media_load` returns `HFSSS_ERR_INVAL`                  |
| PF-011  | Media V1 → V2 upgrade path                                              | V1 file loads cleanly via the `_v1` shim                |
| PF-012  | Cold boot (no checkpoint, no journal)                                   | FTL starts with empty mapping, no errors                |
| PF-013  | Warm boot with valid checkpoint + empty journal                         | Mapping matches pre-shutdown state                      |
| PF-014  | Warm boot with valid checkpoint + non-empty journal                     | Checkpoint applied, then journal deltas in seq order    |
| PF-015  | Full crash recovery flow (superblock checkpoint + journal; `wal_replay` exercised independently as unit test only) | End state consistent with last durably-committed write  |

---

**Document Statistics**:
- Requirements covered: REQ-131 (primary), REQ-051, REQ-052 (implicit)
- Header files: `include/ftl/superblock.h`, `include/ftl/wal.h`, media-internal `src/media/media.c`
- Function interfaces: media 3, superblock 7, WAL 11
- Test cases: 15 (15 verified by shipping suites)

## Appendix: Cross-References

| Reference                        | Document                          |
|----------------------------------|-----------------------------------|
| Bootloader recovery paths        | LLD_09_BOOTLOADER                 |
| L2P checkpoint triggers          | LLD_11_FTL_RELIABILITY            |
| NOR Flash image format           | LLD_14_NOR_FLASH                  |
| Power loss and WAL interaction   | LLD_17_POWER_LOSS_PROTECTION      |
