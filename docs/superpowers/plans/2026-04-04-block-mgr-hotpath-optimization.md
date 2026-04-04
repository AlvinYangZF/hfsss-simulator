# Block Manager Hot-Path Optimization Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** remove the highest-contention block manager operations from the MT FTL write path and then reduce async CQ reply overhead.

**Architecture:** Phase 1 converts hot counters to atomics and removes lock-based reads from the write path. Phase 2 shards free-block allocation by `(channel, plane)`. Phase 3 batches CQ replies. Phase 4 trims lower-value timestamp overhead only if profiling still justifies it.

**Tech Stack:** C11, pthreads, C11 atomics, existing async NBD pipeline, existing MT FTL workers.

---

## File Structure

### Phase 1
| File | Responsibility |
|------|---------------|
| `include/ftl/block.h` | make block counters atomic, add shard metadata declarations |
| `src/ftl/block.c` | lock-free free-count reads, lock-free page invalidation, atomic counter usage |
| `include/ftl/gc.h` | no API change expected unless helper accessors are added |
| `src/ftl/gc.c` | use atomic counter reads where victim scoring depends on page counts |
| `src/ftl/gc_thread.c` | verify GC trigger path no longer relies on locked free-count reads |

### Phase 2
| File | Responsibility |
|------|---------------|
| `include/ftl/block.h` | add per-`(channel, plane)` free-list shard structs |
| `src/ftl/block.c` | initialize shard lists, add `block_alloc_for_shard()` |
| `src/ftl/ftl.c` | route CWB allocation through shard-aware allocator |
| `src/ftl/gc.c` | choose shard-aware destination allocation path |

### Phase 3
| File | Responsibility |
|------|---------------|
| `src/vhost/nbd_async.c` | batch CQ replies with `writev()` / `sendmsg()` |
| `include/vhost/nbd_async.h` | add small batch helper structs if needed |

### Phase 4
| File | Responsibility |
|------|---------------|
| `src/ftl/ftl.c` | reduce redundant timestamp writes |
| `src/ftl/gc.c` | reduce redundant timestamp updates if still hot |

### Tests
| File | Purpose |
|------|---------|
| `tests/test_gc_mt.c` | GC correctness after counter migration and allocator sharding |
| `tests/test_mt_ftl.c` | regression coverage for MT lifecycle and I/O correctness |
| `tests/test_inflight_pool.c` | async inflight pool remains correct after CQ batching work |
| `tests/test_io_queue.c` | sanity check queue behavior if batching helpers reuse queue assumptions |

---

## Phase 1: Atomic Counters and Lock-Free Reads

### Task 1: Convert hot block counters to atomics

**Files:**
- Modify: `include/ftl/block.h`
- Modify: `src/ftl/block.c`

- [ ] **Step 1: Convert global manager counters**

Make `free_blocks`, `open_blocks`, and `closed_blocks` atomic in `struct block_mgr`.

- [ ] **Step 2: Convert per-block page counters**

Make `valid_page_count` and `invalid_page_count` atomic in `struct block_desc`.

- [ ] **Step 3: Audit direct field accesses**

Replace plain increments, decrements, and reads in `src/ftl/block.c`, `src/ftl/gc.c`, and `src/ftl/ftl.c` with atomic operations or helper accessors.

- [ ] **Step 4: Build**

Run the repository build and confirm there are no compile errors from the type migration.

---

### Task 2: Remove lock from free-block read path

**Files:**
- Modify: `src/ftl/block.c`
- Modify: `src/ftl/gc.c`
- Modify: `src/ftl/gc_thread.c`

- [ ] **Step 1: Make `block_get_free_count()` lock-free**

Replace mutex lock/unlock with an atomic load.

- [ ] **Step 2: Make GC threshold checks lock-free**

Update `gc_should_trigger()` callers to use the atomic free-block read path. Keep `gc.threshold` under the GC lock if desired; the free-block input should no longer require `block_mgr.lock`.

- [ ] **Step 3: Verify behavior**

Run `tests/test_gc_mt.c` and confirm GC still triggers and reclaims blocks.

---

### Task 3: Remove lock from page invalidation

**Files:**
- Modify: `src/ftl/block.c`
- Modify: `src/ftl/gc.c`

- [ ] **Step 1: Rewrite `block_mark_page_invalid()`**

Replace the global lock path with atomic updates on the target block:

- decrement valid count if positive
- increment invalid count

- [ ] **Step 2: Update victim scoring reads**

Ensure `block_find_victim()` reads valid/invalid counts atomically.

- [ ] **Step 3: Run overwrite-heavy correctness tests**

Run `tests/test_gc_mt.c` and `tests/test_mt_ftl.c` to confirm overwrites still preserve data and GC still selects reclaimable victims.

---

## Phase 2: `(channel, plane)` Free-List Sharding

### Task 4: Introduce shard-aware free lists

**Files:**
- Modify: `include/ftl/block.h`
- Modify: `src/ftl/block.c`

- [ ] **Step 1: Add shard data structures**

Add free-list shards keyed by:

```text
shard_id = channel * planes_per_die + plane
```

Each shard should have:

- free list head
- lock
- free counter

- [ ] **Step 2: Populate shards at init time**

During `block_mgr_init()`, place each free block into its owning shard instead of the single global free list.

- [ ] **Step 3: Add `block_alloc_for_shard()`**

Implement shard-local allocation for the common write path.

---

### Task 5: Route CWB allocation through shard-aware allocator

**Files:**
- Modify: `src/ftl/ftl.c`
- Modify: `src/ftl/gc.c`

- [ ] **Step 1: Update `ftl_allocate_cwb()`**

Replace global `block_alloc()` calls with `block_alloc_for_shard(channel, plane)`.

- [ ] **Step 2: Update GC destination allocation**

Use a shard-aware policy for `ctx->dst_block` allocation. Prefer the victim block's shard first.

- [ ] **Step 3: Validate no starvation regressions**

Run MT FTL tests and a write-heavy workload. Confirm allocation still succeeds under sustained writes.

---

## Phase 3: CQ Reply Batching

### Task 6: Batch CQ replies with vectored I/O

**Files:**
- Modify: `src/vhost/nbd_async.c`
- Modify: `include/vhost/nbd_async.h` if helper structs are needed

- [ ] **Step 1: Add bounded batching in CQ thread**

Collect several ready completions before issuing socket writes.

- [ ] **Step 2: Use `writev()` or `sendmsg()`**

Send reply headers, and for READs attach payload slices in the same syscall where practical.

- [ ] **Step 3: Preserve slot lifecycle**

Only free inflight slots after the full reply send completes successfully.

- [ ] **Step 4: Validate async path**

Run async NBD smoke tests and confirm READ/WRITE replies still preserve ordering and payload integrity.

---

## Phase 4: Timestamp Trimming

### Task 7: Remove redundant timestamp work if still hot

**Files:**
- Modify: `src/ftl/ftl.c`
- Modify: `src/ftl/gc.c` only if profiles justify it

- [ ] **Step 1: Re-profile after Phases 1-3**

Do not optimize timestamps unless they still show up materially in profiling.

- [ ] **Step 2: Reduce redundant updates**

Keep correctness-relevant timestamps, but remove duplicate writes inside the same page-program path where possible.

---

## Validation Checklist

- [ ] `make` succeeds
- [ ] `tests/test_mt_ftl.c` passes
- [ ] `tests/test_gc_mt.c` passes
- [ ] `tests/test_inflight_pool.c` passes
- [ ] overwrite-heavy workload shows lower lock contention
- [ ] async NBD path remains functionally correct

---

## Recommended Commit Sequence

1. `perf: make free block reads lock-free`
2. `perf: remove global lock from page invalidation`
3. `perf: shard block allocator by channel and plane`
4. `perf: batch async NBD completions with writev`
5. `perf: trim redundant timestamp updates in MT write path`
