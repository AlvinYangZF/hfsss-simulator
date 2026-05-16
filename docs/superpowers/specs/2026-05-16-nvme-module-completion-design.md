# NVMe Module Completion Design

**Date**: 2026-05-16
**Status**: Design
**Scope**: PCIe/NVMe Device Emulation Module (REQ-001 to REQ-022)

## Background

Code audit revealed that `REQUIREMENT_COVERAGE.md` under-reported NVMe module status. Three
requirements marked ❌ or ⚠️ are already implemented:

| REQ | Description | Doc Status | Actual Status |
|-----|------------|-----------|---------------|
| REQ-009 | I/O Queue Dynamic Creation | ❌ | Implemented — CREATE/DELETE IO SQ/CQ fully functional |
| REQ-010 | PRP/SGL Support | ⚠️ | PRP complete; SGL is a stub |
| REQ-019 | PRP Parsing Engine | ❌ | Implemented — `src/pcie/prp.c` handles all three PRP cases |

After correction, the real gaps are:

1. **REQ-014**: Interrupt Coalescing — not implemented at all
2. **REQ-010**: SGL path — stub only (`sgl_walker_next/skip` return `HFSSS_ERR_NOTSUPP`)
3. **Bug**: `prp_walker_next()` in `src/pcie/queue.c` treats PRP2 as a contiguous base address,
   violating NVMe spec for transfers spanning more than two pages
4. **Bookkeeping**: Update `REQUIREMENT_COVERAGE.md` to reflect true status

## Design

### 1. Interrupt Coalescing (REQ-014)

**Reference**: NVMe Base Spec 1.4/2.0, Set Features FID 0x08 (Interrupt Coalescing)

#### Data Model

Add to `struct nvme_cq` in `include/pcie/queue.h`:

```c
u32 coalesce_time_us;       /* Aggregation time window, 0 = disabled */
u32 coalesce_threshold;     /* Aggregation completion count, 0 = disabled */
u32 pending_completions;    /* Completions accumulated since last interrupt */
u64 last_interrupt_ts_ns;   /* Timestamp of last fired interrupt */
```

#### Interrupt Decision

Replace `nvme_cq_needs_interrupt()` logic:

1. Base condition: `interrupt_enabled && cq_head != cq_tail`
2. If `coalesce_threshold > 0` and `pending_completions < coalesce_threshold`: defer
3. If `coalesce_time_us > 0` and elapsed since `last_interrupt_ts_ns < coalesce_time_us`: defer
4. Otherwise: fire, reset `pending_completions = 0`, store `last_interrupt_ts_ns`

`pending_completions` is incremented in `nvme_cq_post_cpl()` after successful CQ entry write.

#### Feature Wire-Up

Extend `nvme_uspace_get_features()` / `nvme_uspace_set_features()` to accept FID 0x08
(Interrupt Coalescing). The value encoding follows NVMe spec:

- cdw0[7:0]   = Aggregation Time (AGRT), unit = 100 µs
- cdw0[15:8]  = Aggregation Threshold (AGRT)

Set Features broadcasts the new parameters to all IO CQs via the queue manager.

#### Validation

- Per-CQ coalescing: create two CQs with different coalescing parameters, verify each fires
  independently
- Time-based: set `coalesce_time_us = 10000` (1ms), post 5 completions in < 1ms, verify exactly
  one interrupt
- Threshold-based: set `coalesce_threshold = 4`, post 3 completions (no interrupt), post 1 more
  (interrupt fires)
- Disabled: verify `coalesce_time_us = 0` and `coalesce_threshold = 0` restores immediate firing

### 2. SGL Support (REQ-010)

**Reference**: NVMe Base Spec section 4.4, Scatter Gather List (SGL)

#### Descriptor Types

| Type | Value | Description |
|------|-------|-------------|
| Data Block | 0x00 | Points to a data buffer |
| Bit Bucket | 0x01 | Discard on write, zero-fill on read |
| Segment | 0x02 | Points to next SGL segment |
| Last Segment | 0x03 | Points to last SGL segment |

#### Walker Implementation

`struct sgl_segment` (already defined, 16 bytes):

```c
struct sgl_segment {
    u8  type;      /* SGL descriptor type */
    u8  subtype;   /* Address subtype (offset 0x00) */
    u8  flags;
    u8  rsvd;
    u32 length;    /* Data length or segment length in bytes */
    u64 address;   /* Data buffer address or next-segment address */
};
```

`sgl_walker_next()` state machine:

1. If `bytes_left == 0`, return `HFSSS_ERR_NOENT`
2. Read current descriptor at `sgl_base + seg_offset`
3. Branch on type:
   - **Data Block**: `*addr = desc.address`, `*len = MIN(desc.length, bytes_left)`;
     advance `seg_offset` by 16, decrement `bytes_left`
   - **Bit Bucket**: advance `seg_offset` by 16 (skip length bytes logically,
     no data transfer needed); caller sees `*len = min(desc.length, bytes_left)`
     with `*addr = SGL_BIT_BUCKET_SENTINEL`
   - **Segment / Last Segment**: advance `seg_offset` by 16, track descriptor
     count; `sgl_base` switches when entering a new segment. If type is Last,
     mark walker as terminal after this segment
4. If `seg_offset >= sgl_len` (segment exhausted) and Last Segment,
   return `HFSSS_ERR_NOENT`

`sgl_walker_skip(len)`:
- Advance over `len` bytes by repeatedly consuming descriptors via the same
  state machine but without returning addresses

#### Data-Path Integration

`prp_copy_from_host()` / `prp_copy_to_host()` in `src/pcie/prp.c` gain SGL path awareness via a
unified `data_walker` interface or explicit SGL branch. When the command descriptor uses
`dp.sgl` instead of `dp.prp`, the copy functions invoke the SGL walker instead of the PRP walker.

### 3. PRP Walker Bug Fix

#### Bug

`prp_walker_next()` line 249:

```c
*addr = walker->prp2 + (walker->current_page - 1) * walker->page_size;
```

This treats PRP2 as the base address of a contiguous memory region. The NVMe spec requires:

- Transfer <= 1 page: PRP2 unused
- Transfer > 1 page, fits in 2 pages: PRP2 = second data page address
- Transfer > 2 pages: PRP2 = pointer to a PRP list (array of page addresses)

#### Fix

1. Add `u64 prp_list_addr` and `u64 *prp_list_cache` fields to `struct prp_walker`
2. In `prp_walker_init()`:
   - Compute pages needed from `length` and `page_size`
   - If pages == 1: PRP2 ignored
   - If pages == 2: cache PRP2 as page[1] address
   - If pages > 2: treat PRP2 as pointer to PRP list; read list entries on demand
3. In `prp_walker_next()`:
   - Page 0: return PRP1 with offset
   - Page 1..N: look up from PRP list (or cached page array), NOT computed from PRP2 as base

### 4. REQUIREMENT_COVERAGE.md Update

Update PCIe/NVMe table rows:
- REQ-009: ❌ → ✅ ("I/O Queue Dynamic Creation — CREATE/DELETE IO SQ/CQ via `nvme_uspace_create_io_sq/cq`, `nvme_uspace_delete_io_sq/cq`; covered by `tests/test_nvme_uspace.c`")
- REQ-010: ⚠️ → ⚠️ (refine note: "PRP fully implemented in `src/pcie/prp.c` and `src/pcie/queue.c`; SGL stub only — `sgl_walker_next/skip` return `HFSSS_ERR_NOTSUPP`")
- REQ-019: ❌ → ✅ ("PRP Parsing Engine — `src/pcie/prp.c` handles single-page, two-page, and PRP-list cases; `prp_copy_from_host/to_host`; `tests/test_prp.c`")

Recalculate coverage percentages accordingly.

## Files Changed

| File | Change |
|------|--------|
| `include/pcie/queue.h` | Add coalescing fields to `nvme_cq`; add `prp_list_cache` to `prp_walker` |
| `src/pcie/queue.c` | Rewrite `nvme_cq_needs_interrupt`; modify `nvme_cq_post_cpl`; implement `sgl_walker_next/skip`; fix `prp_walker_next` |
| `src/pcie/prp.c` | SGL-aware data copy path |
| `src/pcie/nvme_uspace.c` | Wire FID 0x08 in get/set features |
| `include/pcie/nvme_uspace.h` | Add coalescing config to init struct (optional) |
| `tests/test_pcie_nvme.c` | New test cases for coalescing, SGL walker, PRP walker fix |
| `docs/REQUIREMENT_COVERAGE.md` | Correct REQ-009/010/019 status |

## Test Plan

| # | Test | Covers |
|---|------|--------|
| 1 | `test_intr_coalesce_time` | Time-based coalescing: post N completions within window, verify 1 interrupt |
| 2 | `test_intr_coalesce_threshold` | Threshold-based: verify interrupt fires exactly at threshold |
| 3 | `test_intr_coalesce_disabled` | Both params 0 → immediate fire per completion |
| 4 | `test_intr_coalesce_per_cq` | Two CQs with different params, independent firing |
| 5 | `test_intr_coalesce_set_features` | Get/Set Features FID 0x08 round-trip |
| 6 | `test_sgl_walker_data_block` | Single Data Block descriptor → correct addr/len |
| 7 | `test_sgl_walker_bit_bucket` | Bit Bucket descriptor → skip with sentinel |
| 8 | `test_sgl_walker_multi_segment` | Segment chain → walk across segments |
| 9 | `test_sgl_walker_last_segment` | Last Segment terminates correctly |
| 10 | `test_sgl_walker_partial_consume` | Partial consumption within a descriptor |
| 11 | `test_prp_walker_two_page` | Transfer fitting in exactly 2 pages |
| 12 | `test_prp_walker_prp_list` | Transfer requiring PRP list walk |
| 13 | `test_prp_walker_single_page` | Transfer <= 1 page, PRP2 ignored |
| 14 | `test_prp_walker_boundary` | Page-size, page-size+1, page-size*2 boundary cases |

## Non-Goals (Explicitly Out of Scope)

- Kernel-level interrupt delivery (REQ-013) — user-space limitation
- IOMMU support (REQ-021)
- DMA data copy path (REQ-020)
- Kernel-User space communication (REQ-022)
- Controller initialization via real kernel driver (REQ-006)
