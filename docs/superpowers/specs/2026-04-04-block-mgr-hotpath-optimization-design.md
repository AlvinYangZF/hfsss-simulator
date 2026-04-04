# Block Manager Hot-Path Optimization Design

## Goal

Reduce multi-threaded FTL contention in the host write path by removing the hottest global lock acquisitions and by aligning block allocation with the existing `(channel, plane)` write ownership model.

The target of this design is the current master branch after the multi-threaded FTL and async NBD pipeline landed.

## Background

The current multi-threaded write path looks like:

```text
QEMU/fio -> NBD async SQ -> FTL worker -> ftl_write_page_mt()
                                      -> block_mgr / TAA / HAL
```

The async NBD work removed the front-end serialization bottleneck, but the storage core still pays for a single global `block_mgr.lock` on nearly every write. With 4 workers active, the same mutex is hit repeatedly by:

1. `block_alloc()`
2. `block_mark_page_invalid()`
3. `block_get_free_count()`
4. GC victim selection and state transitions

This design focuses on the hot path first and keeps GC correctness intact.

## Current Bottlenecks

### 1. Global block manager mutex

`struct block_mgr` uses one mutex for:

- free/open/closed list mutation
- free block count reads
- invalid-page accounting
- victim selection scans

This is correct but over-serialized for MT writes.

### 2. Frequent read-only lock acquisitions

The hot path reads `free_blocks` to decide whether to trigger GC. This does not need a full mutex if the count becomes atomic.

### 3. Per-page invalidation under global lock

`block_mark_page_invalid()` is called when overwriting an existing LBA. This is one of the highest-frequency operations in random write workloads. Today it takes the global manager lock even though it only updates the target block's page counters.

### 4. Allocation topology mismatch

The current write path chooses a logical `channel` and `plane`, and CWB ownership is already keyed that way. However, `block_alloc()` still allocates from one global free list. That forces all workers to contend on the same allocator state.

### 5. CQ syscall overhead

The async CQ thread still sends one reply at a time. Once the core lock bottlenecks are reduced, CQ reply overhead will become more visible.

## Design Principles

1. Do not weaken correctness for GC or recovery.
2. Replace lock-based reads with real atomics, not "plain load of a lock-protected field".
3. Align allocator sharding with the existing `(channel, plane)` ownership in the FTL.
4. Keep changes incremental so each phase can be benchmarked independently.
5. Avoid redesigning TAA, HAL, or media timing in this plan.

## Proposed Changes

### Phase 1: Atomic hot counters

#### 1.1 Make block manager counters atomic

Convert these fields in `struct block_mgr` to `_Atomic u64`:

- `free_blocks`
- `open_blocks`
- `closed_blocks`

Convert these per-block fields in `struct block_desc` to atomic integer counters:

- `valid_page_count`
- `invalid_page_count`

This supports lock-free reads in the host path and lock-free per-block invalidation updates.

#### 1.2 Lock-free free block reads

Replace the current mutex-based `block_get_free_count()` with an atomic load.

Use the same atomic read path for:

- `block_get_free_count()`
- GC threshold checks
- any debug or stats code that only needs an approximate instantaneous count

This is safe because the decision is advisory: GC trigger checks do not require a fully serialized snapshot of every list.

#### 1.3 Lock-free page invalidation

Replace `block_mark_page_invalid()`'s global lock with atomic counter updates on the target block:

- decrement `valid_page_count` if positive
- increment `invalid_page_count`

Important: this optimization must update both counters atomically. Updating only `invalid_page_count` is not sufficient because GC victim selection uses both valid and invalid page counts.

### Phase 2: Shard free lists by `(channel, plane)`

#### 2.1 New allocator structure

Replace the single global free list with free-list shards keyed by:

```text
shard_id = channel * planes_per_die + plane
```

Each shard owns:

- its own free list
- its own lock
- its own free block counter

The existing global manager lock remains for:

- open/closed list transitions
- GC victim list management
- initialization / teardown

#### 2.2 Allocation API change

Introduce an allocator API that matches the current CWB ownership:

- `block_alloc_for_shard(mgr, channel, plane)`

Update `ftl_allocate_cwb()` to request blocks from the matching shard instead of the global pool.

This removes allocator contention from the common write path and avoids cross-channel allocation drift.

#### 2.3 GC destination allocation

GC destination allocation should continue to use a controlled path. Two acceptable options:

1. allocate from a shard selected by the victim block's `(channel, plane)`
2. allocate from a small fallback global reserve if shard-local allocation fails

The first implementation should prefer shard-local allocation and only add fallback logic if testing shows starvation.

### Phase 3: CQ batching

Batch completion replies in the async CQ thread using `writev()` or `sendmsg()`.

The batching unit should support:

- reply header only for WRITE / TRIM / FLUSH
- reply header + payload slice for READ

The initial version does not need full socket state machine complexity. A bounded batch of ready completions per CQ polling round is enough.

### Phase 4: Reduce timestamp cost

`get_time_ns()` calls in the write path are lower priority than the lock work, but they still add overhead.

The preferred approach is:

- keep precise timestamps where correctness depends on them
- reduce redundant hot-path updates such as multiple writes of `last_write_ts` in the same page program sequence

This phase should be benchmark-driven, not assumption-driven.

## Non-Goals

This plan does not attempt to solve:

- CPU affinity / NUMA placement
- media busy-wait removal
- large-I/O fanout across multiple workers
- full GC victim-selection redesign
- async partial-write semantic cleanup in NBD

Those are valid future efforts, but they are not the first ROI target.

## Expected Impact

Expected qualitative gains, in order:

1. Atomic free count + lock-free GC threshold checks:
   small but immediate reduction in hot-path lock traffic.
2. Lock-free page invalidation with atomic per-block counters:
   large reduction in overwrite-path contention.
3. `(channel, plane)` free-list sharding:
   major reduction in allocator contention under multi-worker writes.
4. CQ batching:
   moderate reduction in syscall overhead once storage-side contention is lower.

The combined first three phases are the main throughput unlock.

## Risks

### 1. Inconsistent counter semantics

If atomic counter updates are mixed with unlocked plain reads or old mutex-based paths, GC accounting will become harder to reason about. All reads of the migrated counters should be audited.

### 2. Shard starvation

If one `(channel, plane)` shard exhausts its free blocks earlier than others, strict local allocation can underutilize global capacity. This should be measured before adding cross-shard fallback complexity.

### 3. GC interaction

GC touches block state transitions and relies on valid/invalid counts for victim selection. Any counter migration must keep GC's scoring behavior stable enough for regression tests.

## Validation Plan

### Functional

- existing MT FTL tests still pass
- GC relocation test still reclaims blocks and preserves data
- async NBD path still returns zeros for unmapped reads

### Performance

Measure before/after for:

- 4K random write, QD=16
- 4K random read, QD=16
- mixed overwrite workload to stress invalidation counters

Track:

- IOPS
- average and p99 latency
- CPU usage per thread
- lock contention samples in Instruments / profiler

## Recommended Implementation Order

1. Atomic `free_blocks` and lock-free `block_get_free_count()`
2. Lock-free `gc_should_trigger()` using atomic free block reads
3. Atomic per-block valid/invalid counters and lock-free `block_mark_page_invalid()`
4. `(channel, plane)` free-list sharding and allocator API
5. CQ batching with `writev()`
6. timestamp trimming only if profiles still show it matters
