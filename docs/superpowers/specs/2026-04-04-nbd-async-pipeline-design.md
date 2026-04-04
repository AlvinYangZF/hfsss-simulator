# NBD Async Pipeline & Performance Optimization Design

## Goal

Eliminate the serial NBD request processing bottleneck in HFSSS simulator by implementing an SPDK-style polling architecture with a two-thread SQ/CQ (Submission Queue / Completion Queue) model. Target: 10-50x throughput improvement over current ~750 IOPS / 3 MB/s baseline.

## Background

The HFSSS simulator exposes its FTL/NAND stack as a live NVMe block device via:

```
fio → guest NVMe → QEMU → NBD TCP → hfsss-nbd-server → FTL workers → DRAM NAND
```

The current NBD server processes requests **serially**: read one request, block until FTL completes, send reply, read next request. With 4 FTL worker threads ready but only 1 I/O in-flight at any time, the pipeline is starved. Workers burn 98% CPU in `sched_yield()` spin-wait loops doing nothing.

### Current Bottleneck Analysis

| Layer | Problem | Impact |
|-------|---------|--------|
| NBD serve loop | Serial: read → process → reply → next. Max 1 in-flight. | ~750 IOPS ceiling |
| Write path | `malloc(4096)` + write-verify read-back per page = 2x NAND ops | 50% throughput lost |
| Worker idle spin | `sched_yield()` busy-loop when no work | 4 cores at 98% CPU wasted |
| GC in MT mode | `ftl_gc_trigger()` uses `ctx->mapping` (never updated via TAA) | GC broken → eventual ENOSPC |

### Measured Baseline (16 GB device, 4K randwrite, QEMU+NBD)

- IOPS: ~750
- Throughput: ~3 MB/s
- Latency (avg): ~1300 µs
- Max in-flight: 1
- CPU: 5 cores at 90%+ (4 workers spinning, 1 NBD dispatch)

## Architecture

Two-phase optimization: Phase 1 fixes per-I/O overhead without changing the architecture. Phase 2 replaces the serial NBD loop with an async two-thread pipeline.

### Phase 1: Quick Wins (No Architecture Change)

#### 1.1 Remove Write-Verify Read-Back

`ftl_write_page_mt()` (src/ftl/ftl.c:758-788) performs:
1. NAND program (write)
2. NAND read (verify)
3. `memcmp()` (compare)

In a DRAM-backed simulator, the verify always passes. Remove steps 2-3 from `ftl_write_page_mt()`. Keep it in the single-threaded `ftl_write_page()` path for correctness testing.

**Impact:** 2x write throughput.

#### 1.2 Remove Per-I/O malloc in NBD WRITE Handler

`nbd_serve()` (src/vhost/hfsss_nbd_server.c:394) allocates `malloc(length)` for every write request, copies data, then frees. Replace with pre-allocated buffer or direct read into the iobuf.

**Impact:** Eliminates ~1500 malloc/free calls per second.

#### 1.3 Fix Worker Spin-Wait

FTL worker threads (src/ftl/ftl_worker.c:20) call `sched_yield()` in a tight loop when no requests are available. Replace with exponential backoff: `sched_yield()` → `usleep(1)` → `usleep(10)` after consecutive empty polls. Reset backoff on successful dequeue.

**Impact:** Idle CPU drops from 98% to <5% per worker.

#### 1.4 Fix GC in MT Mode

`ftl_gc_trigger()` (src/ftl/ftl.c:650-657) calls `gc_run()` with `&ctx->mapping`, but in MT mode, L2P updates go through TAA, not the global mapping table. GC reads stale mapping data and cannot identify valid pages to relocate.

Fix: Add `gc_run_mt()` variant that uses `taa_reverse_lookup()` for valid page identification during victim block scanning. Wire this into `ftl_write_page_mt()` at line 836 and `gc_thread_main()`.

**Impact:** Prevents ENOSPC under sustained random write workloads.

#### 1.5 Fix RMW Path in MT Mode

`nbd_serve()` WRITE handler (src/vhost/hfsss_nbd_server.c:407) uses `nvme_uspace_read()` for read-modify-write even when `g_multithread` is enabled. Must use `mt_io(IO_OP_READ, ...)` instead.

**Impact:** Correctness fix for sub-page writes in MT mode.

### Phase 2: Async NBD Pipeline (SPDK-Style Two-Thread SQ/CQ)

Inspired by SPDK reactor pattern and io_uring polling mode. Two dedicated threads replace the single serial `nbd_serve()` loop.

#### 2.1 Thread Model

```
┌──────────────────────┐         ┌──────────────────────┐
│  Submit Thread (SQ)  │         │  Completion Thread   │
│  (core-pinned)       │         │  (CQ, core-pinned)   │
│                      │         │                      │
│  loop:               │         │  loop:               │
│   recv(nbd_req)      │         │   poll worker CQ     │
│   [non-blocking]     │         │   rings (×4)         │
│   parse + validate   │         │   lookup inflight[]  │
│   alloc inflight     │         │   send(nbd_reply)    │
│   slot               │         │   [+ read data for   │
│   submit to worker   │         │    READ commands]    │
│   SQ ring            │         │   free inflight slot │
└──────────┬───────────┘         └──────────▲───────────┘
           │ submit via                     │ completions via
           │ SPSC request rings             │ SPSC completion rings
    ┌──────▼────────────────────────────────┴──────┐
    │          FTL Workers (×4)                     │
    │   poll SQ ring → process → push CQ ring      │
    └──────────────────────────────────────────────┘
```

**Socket ownership:** Submit thread owns `recv()`, completion thread owns `send()`. Concurrent recv/send on a TCP socket is safe without locking (independent kernel buffers). This is the same pattern used by SPDK NVMe-oF TCP transport.

#### 2.2 Inflight Slot Pool

Pre-allocated fixed-size array, zero malloc in the hot path:

```c
#define MAX_INFLIGHT     256
#define SLOT_BUF_SIZE    (64 * 4096)  /* 256 KB max per request */

enum slot_state { SLOT_FREE = 0, SLOT_SUBMITTED, SLOT_COMPLETE };

struct inflight_slot {
    _Atomic int       state;
    uint64_t          nbd_handle;   /* echoed back to client */
    uint16_t          nbd_cmd;      /* NBD_CMD_READ / WRITE / TRIM / FLUSH */
    uint32_t          byte_off;     /* sub-page offset within first page */
    uint32_t          length;       /* original NBD request length */
    uint32_t          slot_id;      /* index in pool, passed through io_request */
    int               status;       /* FTL result code */
    uint8_t           data[SLOT_BUF_SIZE];
};

struct inflight_pool {
    struct inflight_slot  slots[MAX_INFLIGHT];
    _Atomic uint32_t      alloc_cursor;  /* submit thread scans from here */
    _Atomic uint32_t      in_use_count;  /* backpressure gauge */
};
```

**Memory budget:** 256 slots × ~256 KB = **64 MB** fixed at startup.

**Slot lifecycle:** `FREE →[submit thread]→ SUBMITTED →[worker]→ COMPLETE →[CQ thread]→ FREE`

All state transitions use `atomic_store` with release/acquire ordering. No mutex needed.

#### 2.3 Submit Thread (SQ)

```
set_nonblocking(client_fd)
setsockopt(client_fd, TCP_NODELAY, 1)

while (running) {
    /* Non-blocking read of NBD request header */
    n = recv(client_fd, &req_hdr, sizeof(req_hdr), MSG_DONTWAIT)
    if (n == 0) → client disconnected, exit
    if (n < 0 && EAGAIN) → no data, continue polling
    if (n < sizeof(req_hdr)) → partial read, accumulate in state machine

    parse request (magic, type, offset, length, handle)

    slot = alloc_inflight_slot()
    if (!slot) → backpressure: all 256 slots in use, spin briefly

    if (WRITE) {
        /* Payload follows header immediately — blocking recv OK here */
        recv_exact(client_fd, slot->data, length)
    }

    slot->nbd_handle = handle
    slot->nbd_cmd = type
    slot->byte_off = offset % page_size
    slot->length = length

    /* Build io_request with slot_id so completion can find us */
    io_req.nbd_handle = slot->slot_id
    io_req.lba = offset / page_size
    io_req.count = ceil(end_byte / page_size) - lba
    io_req.data = slot->data  /* zero-copy: worker reads/writes directly */

    ftl_mt_submit(mt, &io_req)
    atomic_store(&slot->state, SLOT_SUBMITTED)
}
```

**Partial read handling:** NBD request headers are 28 bytes. A non-blocking `recv()` may return fewer bytes. The submit thread maintains a small accumulation buffer and state machine to handle this (standard non-blocking TCP pattern).

#### 2.4 Completion Thread (CQ)

```
while (running) {
    found_work = false

    for (w = 0; w < NUM_WORKERS; w++) {
        while (io_ring_pop(&workers[w].completion_ring, &cpl)) {
            found_work = true
            slot = &pool.slots[cpl.nbd_handle]  /* slot_id passed through */

            /* Build NBD reply */
            error = (cpl.status == HFSSS_OK) ? 0 : NBD_EIO
            send_nbd_reply(client_fd, slot->nbd_handle, error)

            /* For READ: send data payload after reply header */
            if (slot->nbd_cmd == NBD_CMD_READ && error == 0) {
                send(client_fd, slot->data + slot->byte_off, slot->length)
            }

            atomic_store(&slot->state, SLOT_FREE)
            atomic_fetch_sub(&pool.in_use_count, 1)
        }
    }

    if (!found_work) {
        /* Brief pause to avoid pure busy-spin on CQ side */
        _mm_pause() or usleep(1)
    }
}
```

#### 2.5 io_request / io_completion Changes

The existing `io_request.nbd_handle` field (u64) is repurposed to carry the `slot_id` (u32) through the FTL pipeline. The completion echoes it back so the CQ thread can locate the inflight slot in O(1).

No changes to io_ring or FTL worker internals needed.

#### 2.6 Backpressure

When all 256 inflight slots are in use, the submit thread stalls (spin on `alloc_inflight_slot()`). This naturally applies TCP backpressure: QEMU's NBD send buffer fills up, QEMU's NVMe queue fills up, guest kernel sees full queue, fio blocks. This is the correct flow control behavior.

## File Changes Summary

### Phase 1
| File | Change |
|------|--------|
| `src/ftl/ftl.c` | Remove write-verify in `ftl_write_page_mt()`. Add `ftl_gc_trigger_mt()`. |
| `src/ftl/gc.c` | Add `gc_run_mt()` using TAA for valid page lookup. |
| `include/ftl/gc.h` | Declare `gc_run_mt()`. |
| `src/ftl/gc_thread.c` | Call `gc_run_mt()` with TAA context. |
| `src/ftl/ftl_worker.c` | Replace `sched_yield()` with backoff in worker loop. |
| `src/vhost/hfsss_nbd_server.c` | Remove per-write malloc. Fix RMW to use mt_io in MT mode. |

### Phase 2
| File | Change |
|------|--------|
| `include/vhost/nbd_async.h` | **New.** inflight_pool, slot structs, SQ/CQ thread API. |
| `src/vhost/nbd_async.c` | **New.** inflight_pool_init/alloc/free, nbd_sq_thread, nbd_cq_thread. |
| `src/vhost/hfsss_nbd_server.c` | Add `-a` flag for async mode. Wire async path alongside existing sync path. |
| `src/ftl/ftl_worker.c` | No change (slot_id flows through existing nbd_handle field). |

### Tests
| File | Purpose |
|------|---------|
| `tests/test_inflight_pool.c` | **New.** Slot alloc/free, state transitions, concurrent access. |
| `tests/test_nbd_async.c` | **New.** Integration: mock NBD client → async pipeline → FTL → verify data. |

## Performance Targets

| Metric | Current | Phase 1 | Phase 2 |
|--------|---------|---------|---------|
| IOPS (4K randwrite) | ~750 | ~1500-2000 | 10K-50K |
| Throughput | 3 MB/s | 6-8 MB/s | 40-200 MB/s |
| CPU (idle workers) | 98% spin | <5% | <5% |
| Max in-flight I/Os | 1 | 1 | 256 |
| GC under sustained write | Broken | Fixed | Fixed |
| Per-I/O malloc calls | 2 | 0 | 0 |

## Verification Plan

1. **Phase 1:** Re-run single-job fio 4K randwrite 60s, compare IOPS before/after.
2. **Phase 1 GC:** Run fio randwrite for 30 min on 14G region (triggers GC), verify no ENOSPC.
3. **Phase 2:** Run fio 4K randwrite with `-a` flag, verify >5x IOPS improvement.
4. **Phase 2 data integrity:** fio `--verify=crc32c` write+readback across async pipeline.
5. **Phase 2 stress:** 3-hour mixed-block-size randwrite (same test currently running), verify stability.

## Non-Goals

- Multi-connection NBD (QEMU NVMe multi-queue) — deferred, separate design.
- vhost-user-blk — requires Linux host, not viable on macOS.
- Disk-backed mmap mode — separate capacity enhancement.
