# Multi-Threaded HFSSS Architecture — Design Spec

**Date:** 2026-04-04
**Model:** Marvell Bravera SC5 heterogeneous (10-core enterprise SSD controller)
**Target:** 7 threads — 1 NBD dispatch, 4 FTL workers, 1 GC, 1 WL/RD

---

## Problem

The HFSSS simulator is single-threaded. All I/O serializes through one global FTL mutex. On a Mac Studio with 20 cores, only 1 core is utilized. Throughput caps at ~3 MB/s regardless of queue depth or job count.

## Research Basis

Enterprise SSD controllers use 4-10 ARM cores with heterogeneous role assignment:

| Controller | Cores | Architecture |
|---|---|---|
| Marvell Bravera SC5 | 10 | Cortex-R8 + M7 + M3 heterogeneous |
| Silicon Motion SM8366 | 8 | Cortex-A55 symmetric |
| Samsung Elpis | 5 | Cortex-R5 |
| Phison E26 | 2 + 3 HW accel | Cortex-R5 + CoXProcessor |

Marvell's architecture assigns dedicated cores to host interface, FTL, flash management, background tasks, and security. We model this with pthreads.

Marvell's TAA (Table Access Accelerator) hardware block handles concurrent L2P lookups with conflict arbitration in silicon. We approximate this with per-LBA-range sharded mutexes.

---

## Thread Architecture

```
                    +---------------------+
                    |  NBD Dispatch Thread |  (1 thread, pinned to P-core)
                    |  - Accept NBD reqs   |
                    |  - Parse commands     |
                    |  - Route to FTL queue |
                    +--------+------------+
                             | SPSC ring buffer (1 per worker)
                    +--------v------------+
                    |  FTL Worker Threads  |  (4 fixed threads)
                    |  Worker 0: ch 0      |
                    |  Worker 1: ch 1      |
                    |  Worker 2: ch 2      |
                    |  Worker 3: ch 3      |
                    |                      |
                    |  Each worker:        |
                    |  - TAA shard lookup   |
                    |  - CWB write buffer  |
                    |  - NAND page r/w     |
                    |  - Completion notify  |
                    +--+----------+--------+
                       |          |
          +------------v+    +---v---------------+
          |  GC Thread   |    |  WL/RD Thread      |
          |  (1 thread)  |    |  (1 thread)        |
          |  - Victim    |    |  - Wear leveling   |
          |    selection |    |  - Read disturb    |
          |  - Page move |    |  - Static data     |
          |  - Block     |    |    migration       |
          |    erase     |    |  - Wakes every 10s |
          +--------------+    +--------------------+
```

### Thread Count: 7 (fixed)

| Role | Count | Priority | CPU Affinity |
|---|---|---|---|
| NBD Dispatch | 1 | High (SCHED_FIFO) | P-core |
| FTL Worker | 4 (fixed) | Normal | P-cores |
| GC | 1 | Low | E-core |
| WL/Read Disturb | 1 | Low | E-core |

---

## TAA (Table Access Accelerator) — Software Approximation

Replaces the single global `mapping_ctx` + mutex with sharded L2P/P2L tables.

### Structure

```c
struct taa_ctx {
    uint32_t          num_shards;      /* 256 default */
    uint64_t          lbas_per_shard;  /* total_lbas / num_shards */
    struct taa_shard  *shards;
};

struct taa_shard {
    struct l2p_entry  *l2p;
    struct p2l_entry  *p2l;
    struct mutex       lock;           /* Per-shard lock */
    uint64_t           base_lba;
    uint64_t           lba_count;
    uint64_t           p2l_count;
    uint64_t           lookup_count;   /* Stats */
    uint64_t           conflict_count; /* Stats: lock contention */
};
```

### Shard Selection

```
shard_id = lba / lbas_per_shard
```

With 256 shards on 16M LBAs: ~64K LBAs per shard. Random 4K I/O across 4 workers has <2% collision probability.

### API

```c
int  taa_init(struct taa_ctx *ctx, uint64_t total_lbas, uint64_t total_pages, uint32_t num_shards);
void taa_cleanup(struct taa_ctx *ctx);
int  taa_lookup(struct taa_ctx *ctx, uint64_t lba, union ppn *out);
int  taa_update(struct taa_ctx *ctx, uint64_t lba, union ppn ppn);
int  taa_invalidate(struct taa_ctx *ctx, uint64_t lba);
int  taa_reverse_lookup(struct taa_ctx *ctx, union ppn ppn, uint64_t *lba_out);
void taa_get_stats(struct taa_ctx *ctx, uint64_t *lookups, uint64_t *conflicts);
```

---

## Inter-Thread Communication

### NBD Dispatch -> FTL Workers

SPSC (Single Producer Single Consumer) lock-free ring buffer, one per worker.

```c
struct io_request {
    uint64_t  lba;
    uint32_t  count;
    uint8_t  *data;          /* Pointer to NBD buffer */
    uint64_t  nbd_handle;    /* For NBD reply */
    enum      { IO_READ, IO_WRITE, IO_TRIM, IO_FLUSH } opcode;
};

struct io_ring {
    struct io_request  *slots;
    uint32_t            capacity;        /* Power of 2 */
    volatile uint32_t   producer_idx;    /* Written by producer */
    volatile uint32_t   consumer_idx;    /* Written by consumer */
};
```

**Routing:** `worker_id = lba % 4` (channel-based). Multi-page I/O spanning channels is split into per-channel sub-requests by dispatch thread.

### FTL Workers -> NBD Dispatch

MPSC (Multi Producer Single Consumer) completion ring. Workers push completions, dispatch thread drains and sends NBD replies.

```c
struct io_completion {
    uint64_t  nbd_handle;
    int       status;        /* 0 = success, else error */
};
```

Uses atomic `__sync_fetch_and_add` for producer index.

### FTL Workers -> GC Thread

- Workers check `free_blocks[channel]` after each write
- When `free_blocks < gc_threshold`: `pthread_cond_signal(&gc_wakeup)`
- GC thread blocks on `pthread_cond_wait(&gc_wakeup)`

### GC -> FTL Workers

- GC adds freed blocks to per-channel lock-free free lists
- Workers pull from free list during block allocation (no contention with GC)

---

## GC Thread Design

Current: GC runs inline during `ftl_write` when free blocks are low.
New: dedicated background thread.

```
GC Thread Loop:
  1. pthread_cond_wait(&gc_wakeup)
  2. Select victim block (greedy/cost-benefit policy)
  3. For each valid page in victim:
     a. Lock TAA shard for that LBA
     b. Read page data from NAND
     c. Write to new block (using per-channel CWB)
     d. Update L2P mapping
     e. Unlock TAA shard
  4. Erase victim block
  5. Add freed block to per-channel free list
  6. Update stats (gc_count, moved_pages, reclaimed_blocks)
  7. If free_blocks still < hiwater: goto 2
  8. goto 1
```

**Backpressure:** If all free lists are empty and GC hasn't reclaimed yet, FTL workers spin-wait (with exponential backoff) on the free list. This is the only case where I/O stalls — same as real hardware.

---

## WL/Read Disturb Thread

```
WL Thread Loop:
  1. sleep(10 seconds)
  2. Scan block erase counts
  3. If max_erase - min_erase > WL_THRESHOLD (default 100):
     a. Select coldest block (lowest erase count with valid data)
     b. Migrate data to hottest free block
     c. This forces the cold block to get erased eventually
  4. Scan read counts per block
  5. If any block read_count > RD_THRESHOLD (default 100K):
     a. Re-read and rewrite all valid pages (refresh)
     b. Reset read count
  6. goto 1
```

---

## NBD Server Changes

The NBD server main loop becomes the dispatch thread:

```c
/* Current: synchronous */
nbd_serve(client_fd, dev, lba_size);  /* blocks on every I/O */

/* New: async dispatch */
while (running) {
    read NBD request from socket
    split multi-page request by channel
    for each sub-request:
        ring = &worker_rings[lba % NUM_FTL_WORKERS]
        io_ring_push(ring, &request)
    
    /* Drain completions and send NBD replies */
    while (io_completion_ring_pop(&completion)) {
        send_nbd_reply(client_fd, completion.handle, completion.status)
    }
}
```

For FLUSH: broadcast to all 4 workers, wait for all completions before replying.

---

## Files

| Action | Path | Description |
|--------|------|-------------|
| Create | `include/ftl/taa.h` | TAA shard structure + API |
| Create | `src/ftl/taa.c` | TAA init/lookup/update |
| Create | `include/ftl/io_queue.h` | Lock-free SPSC/MPSC ring buffers |
| Create | `src/ftl/io_queue.c` | Ring buffer implementation |
| Create | `src/ftl/ftl_worker.c` | FTL worker thread main loop |
| Create | `src/ftl/gc_thread.c` | GC background thread |
| Create | `src/ftl/wl_thread.c` | Wear leveling + read disturb thread |
| Modify | `src/ftl/ftl.c` | Remove global lock, integrate TAA, add worker dispatch |
| Modify | `src/ftl/mapping.c` | Replace with TAA-backed sharded init |
| Modify | `src/ftl/gc.c` | Refactor for thread-safe operation |
| Modify | `src/vhost/hfsss_nbd_server.c` | Async dispatch + completion |
| Modify | `Makefile` | New source files |
| Create | `tests/test_taa.c` | TAA shard unit tests |
| Create | `tests/test_mt_ftl.c` | Multi-threaded FTL integration test |
| Create | `tests/test_io_queue.c` | Ring buffer unit tests |

---

## Testing

1. **TAA unit tests**: init, shard lookup, concurrent 4-thread access, conflict stats
2. **IO queue tests**: SPSC push/pop, MPSC concurrent producers, overflow/empty
3. **MT FTL integration**: 4 threads random R/W 16 GB, verify data integrity
4. **GC background test**: fill device, verify GC runs in thread, I/O doesn't stall
5. **Regression**: all existing single-threaded tests pass unchanged
6. **QEMU+fio**: throughput comparison vs single-threaded baseline (target: 4x improvement)

## Success Criteria

- All existing tests pass (zero regressions)
- 4K random write IOPS >= 3000 (vs ~750 single-threaded) — ~4x improvement
- fio data verification passes at 16 GB with 4 jobs
- GC runs in background without blocking I/O path
- No deadlocks under 2-hour stress test
