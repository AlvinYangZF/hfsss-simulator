# Enlarge HFSSS Disk Capacity — Design Spec

**Date:** 2026-04-04
**Target:** 16-64 GB configurable capacity (up from 4 GB L2P limit)
**Approach:** Raise static limits + NBD auto-sizing + config file driven with runtime override

---

## Problem

The simulator's addressable capacity is capped at ~4 GB due to:
- L2P mapping table: 1M entries (`L2P_TABLE_SIZE = 1ULL << 20`)
- P2L mapping table: 16M entries
- NAND MAX_* constants too conservative for enterprise geometries
- NBD server hardcodes a small NAND geometry regardless of `-s` flag

## Solution

### 1. Raise Static Limits

| Constant | Current | New | File |
|----------|---------|-----|------|
| `L2P_TABLE_SIZE` | `1ULL << 20` (1M) | `1ULL << 24` (16M) | `include/ftl/mapping.h` |
| `P2L_TABLE_SIZE` | `1ULL << 24` (16M) | `1ULL << 25` (32M) | `include/ftl/mapping.h` |
| `MAX_CHANNELS` | 32 | 64 | `include/media/eat.h`, `include/media/bbt.h` |
| `MAX_CHIPS_PER_CHANNEL` | 8 | 16 | same |
| `MAX_DIES_PER_CHIP` | 4 | 8 | same |
| `MAX_PLANES_PER_DIE` | 2 | 4 | same |
| `MAX_BLOCKS_PER_PLANE` | 2048 | 4096 | `include/media/nand.h`, `include/media/bbt.h` |
| `MAX_PAGES_PER_BLOCK` | 512 | 1024 | `include/media/nand.h` |

PPN bitfield (64-bit union in `mapping.h`): block=12 bits (max 4096), page=10 bits (max 1024), channel=6 bits (max 64), chip=4 bits (max 16), die=3 bits (max 8), plane=2 bits (max 4). All new MAX values fit — no bitfield changes needed.

### 2. NAND Validation Update

`nand_device_init()` in `src/media/nand.c` validates geometry against MAX_* constants. Update bounds to match new MAX values.

### 3. NBD Server Auto-Sizing

Current NBD server hardcodes `4ch/4chip/2die/2plane/512blk/256pg/4096B` regardless of `-s`. Fix: auto-compute NAND geometry from requested export size.

Algorithm:
```
page_size = 4096
pages_needed = ceil(target_bytes / page_size)
Start with base geometry: 4ch, 4chip, 2die, 2plane, 512blk, 256pg
Scale up blocks_per_plane first (up to 4096)
Then pages_per_block (up to 1024)
Then channels (up to 16)
Then chips_per_channel (up to 8)
Until raw_pages >= pages_needed * (100 + op_pct) / 100
```

### 4. Config Hierarchy

1. YAML config file sets default geometry (existing mechanism)
2. NBD `-s` flag overrides: if specified size exceeds current geometry capacity, auto-scale geometry up
3. Existing default config (4TB enterprise QLC) already has large geometry — only the mapping table sizes gate the capacity

### 5. Testing

- **Existing tests**: unchanged (small geometries, fast)
- **New test**: `tests/test_large_capacity.c` — init 16 GB geometry, verify L2P can map all LBAs, write+read boundary LBAs (first, last, random middle)
- **fio validation**: run fio_verify_suite.sh with `-s 16384` (16 GB)

## Memory Budget

At 16 GB with 4K pages = 4M LBAs:
- L2P table: 4M * 16 bytes = 64 MB
- P2L table: ~4M * 9 bytes = 36 MB
- Block descriptors: ~64K blocks * ~64 bytes = 4 MB
- NAND page data: 4M pages * 4096 bytes = **16 GB** (this IS the simulated data)
- NAND page spare: 4M pages * 64 bytes = 256 MB

Total host RAM: ~16.4 GB for 16 GB capacity. For 64 GB capacity: ~65.5 GB host RAM.

This is acceptable for a dev machine (Mac Studio with 64-192 GB RAM). The NAND page data is the dominant cost and is inherent to a full-fidelity simulator.

## Files Modified

| Action | Path |
|--------|------|
| Modify | `include/ftl/mapping.h` |
| Modify | `include/media/eat.h` |
| Modify | `include/media/bbt.h` |
| Modify | `include/media/nand.h` |
| Modify | `src/media/nand.c` (validation bounds) |
| Modify | `src/vhost/hfsss_nbd_server.c` (auto-sizing) |
| Create | `tests/test_large_capacity.c` |
| Modify | `Makefile` (new test target) |
