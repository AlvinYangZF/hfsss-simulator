# FTL Die-Busy Wait-Queue Design

## 1. Goal

Replace the FTL-side spin-retry loop (`ftl_busy_backoff_sleep`: `nanosleep(100µs) × 2048`) with a per-die wait queue plus completion-event-driven dispatch. Eliminate the 142 ms mean / 152 ms p99 sequential-write tail observed in the `012_fio_seqwrite_verify` baseline (Apr 2026, master sha `0eb0d83`). Preserve cmd_engine's existing fail-fast contract.

This work covers what was scoped as item **B-1 / model 2 / option (i)** during brainstorming on 2026-04-30:

- **Model 2 fidelity** — per-die wait list + event-driven dispatch (mirrors real SSD controller behavior: R/B# transition wakes a queued task)
- **Option (i)** — synchronous FTL public API; the wait/dispatch is async internally; FTL workers block on a per-request condition variable analogous to RTOS task `sem_wait`

## 2. Background

### 2.1 Observed performance bug

`tools/qemu_blackbox` baseline run on `master@0eb0d83` (2026-04-29, archived at `docs/perf-baselines/2026-04-29-master-0eb0d83/`):

| Case | Read IOPS | Read mean lat | Read p99 | Write IOPS | Write mean lat | Write p99 |
|---|---|---|---|---|---|---|
| 010 randwrite | 5385 | 2.8 ms | 3.1 ms | 3217 | 5.0 ms | 18.5 ms |
| 011 randrw | 5459 | 2.0 ms | 6.7 ms | 2363 | 2.8 ms | 8.4 ms |
| **012 seqwrite** | 1744 | 9.1 ms | 11.7 ms | **112** | **142.0 ms** | **152.0 ms** |
| 013 trim | 5368 | 2.8 ms | 3.1 ms | 1684 | 8.9 ms | 12.8 ms |
| 014 stress | 5814 | 8.5 ms | 13.4 ms | 2581 | 8.2 ms | 15.1 ms |

Sequential 128 KiB writes at iodepth=16 deliver ~28× lower IOPS than random writes. The `142 ms ≈ 2048 × 100 µs` retry budget set by the prior async-FTL race fix accounts cleanly for this latency: every FTL worker that hits a die-busy state in cmd_engine sleeps 100 µs and retries, up to 2048 times.

### 2.2 Why the prior fix landed there

The retry budget was raised in commit `b8b6dae` (PR #115) as a safety net after a cv-based wait-on-busy attempt inside `cmd_engine` was reverted (commit `32e4611`). The cv approach broke multiple regression tests (`tests/test_media.c`'s IS-04, IS-06, and SUSPENDED-state cases) because they assert that `cmd_engine` returns `HFSSS_ERR_BUSY` synchronously when the die's state machine rejects an op. The cv variant silently waited instead of returning. **`cmd_engine`'s fail-fast contract is therefore non-negotiable**: any wait logic must live above it, not inside it.

### 2.3 Real-hardware reference

In a real flash controller, FTL/GC tasks are RTOS-style cooperative state machines, not preemptive worker threads. They submit a NAND command to a per-channel (or per-die) command queue and yield. The channel scheduler picks the next command whose target die is `ready`. When a die finishes its program/erase, the R/B# pin transitions raise an event (interrupt or status-poll hit), and the scheduler wakes the queued task that was waiting for that die. There is no "thread blocked on the die" anywhere. This design implements the same dispatch semantics in user-space pthreads.

## 3. Architecture

### 3.1 Component placement

```
┌────────────────────────────────────────────────┐
│   FTL public API (ftl_*_page_mt — sync shape)  │
│            ftl_mt.c                            │
└──────────────────┬─────────────────────────────┘
                   │ ftl_mt internal: enqueue + cv_wait
                   ▼
┌────────────────────────────────────────────────┐
│         die_dispatcher (NEW)                   │
│   src/ftl/die_dispatcher.c                     │
│   • per-die multi-tier wait list               │
│   • priority + WFQ + EMA host pressure         │
│   • cmd_engine notifier callback installer     │
└──────────────────┬─────────────────────────────┘
                   │ submit / re-submit
                   ▼
┌────────────────────────────────────────────────┐
│    cmd_engine (UNCHANGED contract)             │
│    src/media/cmd_engine.c                      │
│    + dev->die_ready_notifier hook fires when   │
│      die transitions to DIE_IDLE               │
└────────────────────────────────────────────────┘
```

`die_dispatcher` is a pure FTL-layer subsystem; it is the only consumer of cmd_engine's new notifier hook. Other media-layer callers (legacy tests, micro-benchmarks) leave `dev->die_ready_notifier = NULL` and observe zero behavior change.

### 3.2 Invariants (must hold across every change)

1. **Fail-fast preserved**: `cmd_engine` always returns `HFSSS_ERR_BUSY` synchronously when the die state machine rejects an op. No path inside `cmd_engine` ever blocks waiting for state.
2. **No new dispatcher thread**: completion events are propagated by signaling the head waiter's per-request condvar; that waiter resubmits on its own thread (the FTL worker that originally blocked).
3. **Lock order invariant**: `die_lock → waitqueue_lock`. The reverse direction must never occur.
4. **Notification idempotency**: a notifier call on an empty wait list is a no-op. No allocation, no signal, defensive only.
5. **Notification safety**: a wakeup always reaches a waiter eventually; if a wakeup races a competing submit and the waiter re-fails with BUSY, the waiter re-queues and the next IDLE notification covers it.
6. **Existing test parity**: all currently-passing tests (notably `tests/test_media.c`'s SUSPENDED / IS-04 / IS-06 cases, `tests/test_cmd_engine.c`, `tests/test_channel_worker.c`) pass unchanged in behavior and timing characteristics.

## 4. cmd_engine extension

### 4.1 New field on `struct nand_device`

```c
/* Optional callback invoked when any die transitions to DIE_IDLE.
 * Installed by die_dispatcher; default NULL means no observer. */
void (*die_ready_notifier)(struct nand_device *dev, u32 ch, u32 chip, u32 die);
void  *die_ready_ctx;     /* opaque pointer for the observer's own state */
```

The verbose "pass `dev` plus coords" signature is chosen for readability over the curried `notifier(ctx, …)` style; the compiler optimizes both forms identically and the verbose form preserves type information at the call site.

### 4.2 Anchor points in `cmd_engine.c`

There are three points where a die transitions to `DIE_IDLE`. Two of them must fire the notifier; one must not.

| Anchor | Where | Trigger | Notify? | Why |
|---|---|---|---|---|
| **A — Normal completion** | end of `engine_submit`, the "drive to IDLE" branch (after `state = DIE_IDLE` and the `in_flight=false / phase=COMPLETE / cache_active=false` cleanup) | every successful op completion or cache-end | **yes** | Primary completion path; this is the canonical wake point |
| **B — Reset force-clear** | `engine_submit_reset`, after `nand_cmd_state_init(&die->cmd_state)` | external reset abort the in-flight op | **yes** | Reset clears the die; queued waiters get a chance to retry. Any waiter whose op is still illegal (e.g., `RESETTING` lingers) re-fails with BUSY and re-queues — self-correcting |
| **C — Cache-sequence implicit termination** | `engine_submit` entry, the `cache_active && new-non-cache-op` block (state forced to IDLE before the new op proceeds) | a non-cache op preempts a still-open cache sequence | **no** | The IDLE here is a transient inside the same caller's `engine_submit`; the caller immediately re-busies the die with the new op. A wake here would only cause one wasted wakeup-and-requeue cycle |

### 4.3 Call placement and lock discipline

The notifier is invoked **inside** `die_lock`, after the IDLE-state cleanup is complete but before the lock release. Lock order is therefore `die_lock → waitqueue_lock` (the notifier acquires `waitqueue_lock` internally to manipulate the per-die list). Because the woken waiter only resumes execution after the original notifier caller releases `die_lock`, no reverse acquisition occurs.

Holding `die_lock` across `pthread_cond_signal` is permitted by POSIX and is the convention used in `cmd_engine`'s existing path. The added cost per completion is one list-head check, one optional dequeue, and one `pthread_cond_signal` syscall — negligible relative to NAND timing (>= 50 µs program even in the timing-skip tests).

### 4.4 Backward compatibility

When `dev->die_ready_notifier == NULL`, anchors A and B perform a single `if` check and proceed unchanged. All existing callers that construct a `nand_device` without a dispatcher leave the field zeroed by `calloc` / `memset`, observing identical behavior to today.

## 5. die_dispatcher module

### 5.1 Public header (`include/ftl/die_dispatcher.h`)

```c
#ifndef HFSSS_FTL_DIE_DISPATCHER_H
#define HFSSS_FTL_DIE_DISPATCHER_H

#include "common/types.h"
#include "media/nand.h"
#include <pthread.h>
#include <stdatomic.h>

/* Priority class assigned by the caller at submit time.
 * Internally maps to one of four wait-queue tiers. */
typedef enum {
    DIE_PRIO_HOST_READ,    /* T2 share */
    DIE_PRIO_HOST_WRITE,   /* T3 share */
    DIE_PRIO_GC_CRITICAL,  /* T1 — pre-empts host IO */
    DIE_PRIO_GC_FORCE,     /* T2 share — read-disturb / WL trigger */
    DIE_PRIO_GC_NORMAL,    /* T3 share — host-write-driven block release */
    DIE_PRIO_GC_IDLE,      /* T4 — yields under host burst */
    DIE_PRIO_COUNT
} die_priority_t;

/* GC kick reason; carried through the GC pipeline so dispatcher submit
 * can map the right priority class. */
typedef enum {
    GC_TRIGGER_FREE_SB_LOW,    /* → DIE_PRIO_GC_CRITICAL */
    GC_TRIGGER_READ_DISTURB,   /* → DIE_PRIO_GC_FORCE */
    GC_TRIGGER_WEAR_LEVELING,  /* → DIE_PRIO_GC_FORCE */
    GC_TRIGGER_HOST_WRITE,     /* → DIE_PRIO_GC_NORMAL */
    GC_TRIGGER_IDLE,           /* → DIE_PRIO_GC_IDLE */
} gc_trigger_t;

struct die_dispatcher;

/* Lifecycle */
struct die_dispatcher *die_dispatcher_create(struct nand_device *dev);
void                   die_dispatcher_destroy(struct die_dispatcher *d);

/* Caller blocks until the target die is dispatched to it.
 * On return, the caller holds a logical "go ahead" and is expected to
 * immediately invoke the cmd_engine submit. If cmd_engine returns
 * HFSSS_ERR_BUSY, the caller calls die_dispatcher_wait again to
 * re-queue. The dispatcher does not retry on the caller's behalf. */
int die_dispatcher_wait(struct die_dispatcher *d,
                        u32 ch, u32 chip, u32 die,
                        die_priority_t prio,
                        u32 max_wait_ms);  /* 0 = no timeout */

/* Notify hook installed on dev->die_ready_notifier.
 * Internal use; exposed in the header for the install path. */
void die_dispatcher_on_die_ready(struct nand_device *dev,
                                 u32 ch, u32 chip, u32 die);

/* GC trigger → priority class mapping (helper). */
die_priority_t die_dispatcher_prio_for_gc(gc_trigger_t t);

#endif
```

### 5.2 Per-die wait queue

```c
struct die_waiter {
    struct list_head        list;     /* member of one of tier[N] */
    pthread_cond_t          cv;
    pthread_mutex_t         cv_lock;  /* paired with cv */
    bool                    signaled; /* spurious-wake guard */
    die_priority_t          prio;
    /* No payload pointer here — the caller's request lives in its own
     * stack frame, blocked on cv. die_waiter itself is stack-allocated
     * by the caller of die_dispatcher_wait. */
};

struct die_waitqueue {
    pthread_mutex_t  lock;            /* guards tier lists + WFQ state */
    struct list_head tier[4];         /* T1..T4 */
    /* WFQ deficit counters for T2 and T3 (T1 strict, T4 strict-low). */
    int32_t          wfq_deficit_t2[2];  /* [0]=HOST_READ, [1]=GC_FORCE */
    int32_t          wfq_deficit_t3[2];  /* [0]=HOST_WRITE,[1]=GC_NORMAL */
    int32_t          wfq_quantum_t2[2];  /* configurable; default {1,1} */
    int32_t          wfq_quantum_t3[2];
    /* Host pressure (EMA of host IO inter-arrival time) */
    double           ema_iat_ns;
    uint64_t         last_host_ts_ns;
    /* Constants */
    double           ema_alpha;          /* default 0.125 */
    uint64_t         yield_threshold_ns; /* default 1_000_000 (1 ms) */
    uint64_t         burst_grace_ns;     /* default 1_000_000 (1 ms) */
};
```

The dispatcher allocates one `die_waitqueue` per (channel, chip, die) tuple. For the default 4×4×2 device geometry that is 32 wait queues of ~200 bytes each (negligible).

### 5.3 Tier mapping

| Caller priority | Tier | Notes |
|---|---|---|
| `DIE_PRIO_GC_CRITICAL` | T1 | Strict pre-emption above all others |
| `DIE_PRIO_HOST_READ` | T2 (slot 0) | WFQ shared with FORCE |
| `DIE_PRIO_GC_FORCE` | T2 (slot 1) | WFQ shared with HOST_READ; default 1:1 |
| `DIE_PRIO_HOST_WRITE` | T3 (slot 0) | WFQ shared with NORMAL |
| `DIE_PRIO_GC_NORMAL` | T3 (slot 1) | WFQ shared with HOST_WRITE; default 1:1 |
| `DIE_PRIO_GC_IDLE` | T4 | Strict-low; yields under host burst |

The two tiers (T2, T3) that mix two priority classes use deficit-round-robin scheduling per `wfq_quantum_*`. Quantum defaults are `{1,1}` (equal share) and overridable via runtime config (out of scope for this PR — env override only).

### 5.4 Dispatch decision

```
on every "die became idle" event:
    lock(waitqueue.lock)
    pick = NULL
    if !empty(tier[T1]):
        pick = pop_head(tier[T1])
    else if !empty(tier[T2]):
        pick = wfq_pick(tier[T2], wfq_deficit_t2, wfq_quantum_t2)
    else if !empty(tier[T3]):
        pick = wfq_pick(tier[T3], wfq_deficit_t3, wfq_quantum_t3)
    else if !empty(tier[T4]) and not host_bursting():
        pick = pop_head(tier[T4])
    unlock(waitqueue.lock)
    if pick: signal(pick.cv)
```

`wfq_pick` walks the list and selects the first waiter from the slot whose deficit is non-positive (or maximum), then increments its deficit by `quantum`. The non-selected slot's deficit decays toward zero. Implementation detail kept out of this spec — see `die_dispatcher.c` for the full algorithm.

### 5.5 Host pressure detection (EMA)

On every dispatch of a `HOST_READ` or `HOST_WRITE` waiter:

```
now = get_time_ns()
iat = now - last_host_ts_ns
ema_iat_ns = ema_iat_ns * (1 - alpha) + iat * alpha
last_host_ts_ns = now
```

T4 yield decision:

```
host_bursting() :=
    (ema_iat_ns < yield_threshold_ns)  OR
    ((now - last_host_ts_ns) < burst_grace_ns)
```

The OR with raw `now - last_host_ts_ns < burst_grace_ns` is a starvation guard: even when the EMA has decayed below threshold, a fresh host IO within the past 1 ms still counts as bursty. This prevents an idle GC waiter from sneaking in during the gap between two back-to-back host IOs.

### 5.6 GC origin tagging

GC tasks today (`src/ftl/gc_mt.c` and friends) compute their own kick reason locally. This spec adds a `gc_trigger_t` carried on the GC context so that when the GC task submits to `die_dispatcher_wait`, the right priority class is picked. The mapping helper `die_dispatcher_prio_for_gc` is the single source of truth for trigger→priority mapping.

GC kick sites that must thread the trigger through:
- Free-superblock-low watcher (`GC_TRIGGER_FREE_SB_LOW`)
- Read-disturb counter trip (`GC_TRIGGER_READ_DISTURB`)
- Wear-leveling threshold trip (`GC_TRIGGER_WEAR_LEVELING`)
- Host-write back-pressure path (`GC_TRIGGER_HOST_WRITE`)
- Idle-time background sweep (`GC_TRIGGER_IDLE`)

## 6. ftl_mt integration

### 6.1 Current flow (to be removed)

`src/ftl/ftl_mt.c` currently contains the retry loop:

```c
for (retry_count = 0; retry_count < max_retries; retry_count++) {
    rc = cmd_engine_submit(...);
    if (rc != HFSSS_ERR_BUSY) break;
    ftl_busy_backoff_sleep();   /* nanosleep(100µs) */
}
```

with `max_retries = 2048`. This loop is the source of the 142 ms tail.

### 6.2 New flow

```c
die_priority_t prio = caller_supplied_priority;  /* default: HOST_READ/WRITE based on op */
for (retry_count = 0; retry_count < SAFETY_RETRY_BUDGET; retry_count++) {
    rc = cmd_engine_submit(...);
    if (rc != HFSSS_ERR_BUSY) break;
    /* Block until die_dispatcher signals us; returns 0 on signal, ETIMEDOUT
     * on the safety bound. */
    rc_wait = die_dispatcher_wait(d, target_ch, target_chip, target_die,
                                  prio, SAFETY_WAIT_MS);
    if (rc_wait == ETIMEDOUT) {
        rc = HFSSS_ERR_BUSY;  /* propagate */
        break;
    }
}
```

`SAFETY_RETRY_BUDGET = 8` and `SAFETY_WAIT_MS = 50` — these are belt-and-suspenders against a dispatcher bug. In practice the loop terminates on the first or second iteration: the dispatcher wakes a waiter only when the die is actually IDLE.

### 6.3 Caller priority surface

`ftl_mt.c`'s public functions get an optional priority parameter. To keep the existing call-site count minimal:

- `ftl_read_page_mt` / `ftl_write_page_mt` / `ftl_trim_page_mt` keep their current signature and default priority to `DIE_PRIO_HOST_READ` / `DIE_PRIO_HOST_WRITE` / `DIE_PRIO_HOST_WRITE`.
- New variants `ftl_*_page_mt_ex(... die_priority_t prio)` accept explicit priority for GC and WL callers.
- GC code is updated to use the `_ex` variant and pass the mapped priority from `gc_trigger_t`.

This avoids breaking every existing host-IO caller while letting the GC path differentiate.

## 7. Concurrency and lock ordering

### 7.1 Lock graph

```
NBD/host caller thread (e.g., FTL worker)
  └─> ftl_*_page_mt
        └─> cmd_engine_submit
              ├─ acquires channel->lock
              └─ acquires die->die_lock
                    └─ on completion (anchor A or B):
                          └─ die_dispatcher_on_die_ready
                                └─ acquires waitqueue->lock
                                      └─ pthread_cond_signal(waiter.cv)
                                └─ releases waitqueue->lock
                    └─ releases die_lock
                    └─ releases channel->lock
        └─ if BUSY:
              └─> die_dispatcher_wait
                    └─ acquires waitqueue->lock
                    └─ enqueue
                    └─ releases waitqueue->lock
                    └─ pthread_cond_wait(waiter.cv) blocks
                    └─ on wake: returns, caller retries cmd_engine_submit
```

Lock acquisition order on the completion path: **`channel_lock → die_lock → waitqueue_lock → cv_signal`**.

Lock acquisition order on the wait path: **`waitqueue_lock → cv_wait`** (which atomically releases waitqueue_lock; cv_wait then re-acquires on wake).

These two orders are compatible: nothing on the wait path acquires `die_lock` or `channel_lock`. The wait-path caller releases `waitqueue_lock` inside `cv_wait` before retrying `cmd_engine_submit`, which then takes `channel_lock → die_lock` from a clean state.

### 7.2 Spurious-wake handling

`pthread_cond_wait` may wake spuriously. The waiter's `signaled` flag is checked in a loop:

```c
pthread_mutex_lock(&waiter->cv_lock);
while (!waiter->signaled) {
    pthread_cond_wait(&waiter->cv, &waiter->cv_lock);
}
pthread_mutex_unlock(&waiter->cv_lock);
```

The notifier sets `signaled = true` under `cv_lock` before signaling.

### 7.3 Cancellation

`die_dispatcher_destroy` walks every waitqueue and signals every waiter with a poisoned `signaled` flag (e.g., `signaled = true` plus a context-dead bit so the waiter on wake returns `HFSSS_ERR_SHUTDOWN`). Callers must check the return code of `die_dispatcher_wait` and stop retrying on `HFSSS_ERR_SHUTDOWN`.

Per-FTL-context shutdown drains its own waiters in the same way before destroying the dispatcher.

## 8. Testing

### 8.1 Layered test plan

| Layer | File | Scope | LOC |
|---|---|---|---|
| **L1 — pure data structure** | `tests/test_die_dispatcher_unit.c` (new) | waitqueue init/destroy, enqueue/pop in single tier, multi-tier priority order, WFQ deficit, EMA update, T4 yield decision | ~200 |
| **L2 — multi-thread with mock cmd_engine** | `tests/test_die_dispatcher_mt.c` (new) | concurrent enqueue + notifier wakes; lost-wakeup defense (notifier on empty queue); re-queue after racy BUSY; cancellation under shutdown | ~250 |
| **L3 — integration with real cmd_engine** | `tests/test_die_dispatcher_engine.c` (new) | submit→complete→notify cycle on real `nand_device`; reset path notification (anchor B); negative-assertion that anchor C does **not** fire; multi-waiter priority + WFQ ordering observed end-to-end | ~200 |
| **L4 — FTL integration** | extend `tests/test_ftl_mt.c`, `tests/test_gc_mt.c` | host write + critical GC pre-emption; host read + force GC 1:1; host write + normal GC 1:1; host burst + idle GC yield | +150 |
| **L5 — regression red line** | (no new code) | `make test` full suite, in particular `test_media`'s IS-04 / IS-06 / SUSPENDED cases, `test_cmd_engine`, `test_cmd_legality`, `test_channel_worker` — all PASS unchanged | 0 |
| **L6 — end-to-end perf** | (gated by `make pre-checkin`) | full QEMU blackbox; **KPI**: `012_seqwrite_verify` mean ≤ 10 ms, p99 ≤ 20 ms; 010/011/013/014 within ±10 % of baseline | 0 |

### 8.2 KPI table

Every PR-gating run must land within these bounds:

| Case | Metric | Baseline (master@0eb0d83) | Target |
|---|---|---|---|
| 012 seqwrite | write mean lat | 142.0 ms | ≤ 10 ms |
| 012 seqwrite | write p99 lat | 152.0 ms | ≤ 20 ms |
| 012 seqwrite | write IOPS | 112 | ≥ 1500 |
| 012 seqwrite | read mean lat | 9.1 ms | ≤ 12 ms (no regression) |
| 010 randwrite | all | (see 2.1) | ±10 % |
| 011 randrw | all | (see 2.1) | ±10 % |
| 013 trim | all | (see 2.1) | ±10 % |
| 014 stress | all | (see 2.1) | ±10 % |

The 012 seqwrite write IOPS upper bound is intentionally not pinned — better is welcome; what matters is the latency floor and that no other case regresses.

### 8.3 Fault-injection knobs (env-gated only)

Two debug knobs, off by default. Pattern matches `HFSSS_TRACE_IO_ERR`: getenv result cached behind a relaxed atomic; production builds where the env is unset pay a single relaxed load.

| Env | Effect | Use |
|---|---|---|
| `HFSSS_DIE_DISP_FORCE_BUSY=N` | Inject `HFSSS_ERR_BUSY` on `N`% of `cmd_engine_submit` calls from waiters that just woke up (not the first try) | Forces re-queue path repeatedly to stress the wake/retry loop |
| `HFSSS_DIE_DISP_NOTIFIER_DELAY_NS=K` | Sleep `K` ns inside the notifier between dequeue and `cv_signal` | Verifies that no missed-wakeup window exists; correctness must hold for arbitrary K |

A new CI job runs `make pre-checkin` with `HFSSS_DIE_DISP_FORCE_BUSY=10` and `HFSSS_DIE_DISP_NOTIFIER_DELAY_NS=100000` (100 µs) to exercise both. Pass criterion: 8/8 PASS, KPIs within bounds (latency budgets allow the injected slowdown — KPI table relaxes by 2× under fault-injection mode).

These knobs are **not** profile flags; they live in env, are checked at the dispatcher boundary, and never persist into release artifacts.

## 9. Performance assumptions

### 9.1 Write buffer absorbs write tail (documented assumption)

The host-observed write latency is governed by the write buffer / VWC layer that sits above FTL. This spec assumes:

- Write buffer capacity is sized so its high-watermark policy absorbs FTL write tail latency for typical host-write rates.
- When VWC is disabled (e.g., a power-loss-protection scenario), host writes hit FTL synchronously and the dispatcher's T3 policy directly governs host-write tail. The spec does not target a sub-10 ms host-write tail in that mode; whether that is acceptable is a downstream policy decision.

This assumption is captured in code as a comment on the T3 policy and a regression test (`tests/test_die_dispatcher_engine.c::vwc_disabled_t3_path_does_not_regress_existing_baseline`) that confirms VWC-off behavior matches today's master.

### 9.2 Latency budget breakdown (target)

Worst-case `012_seqwrite_verify` write latency under target:

| Component | Time |
|---|---|
| One waiter ahead in same tier (worst case) | ~1.3 ms (one tProg) |
| Channel arbitration + cmd_engine setup | ~50 µs |
| Fault-injection slack (none in normal mode) | 0 |
| **Total target** | **< 2 ms typical, < 10 ms worst case** |

Compared to baseline 142 ms (≈ 2048 × 100 µs × 0.7 hit rate), a 14× reduction is the floor we expect to clear comfortably.

## 10. Out of scope

- **REQ-045 tier 3** (channel_worker retarget): NBD/fio production I/O is not re-routed through `channel_worker`'s CQ path. That is a separate project; this work does not block or pre-empt it.
- **Truly async FTL public API** (option (ii) from brainstorming): callers continue to see the existing synchronous shape.
- **Write buffer / VWC changes**: the assumption that the buffer absorbs write tail is documented but not actively enforced. No new VWC code.
- **Per-channel scheduler thread**: the alternative ("model 3 with per-channel granularity") is not implemented. If future profiling shows the FTL-worker-blocks-on-cv pattern is itself a bottleneck, that is a follow-up project.
- **WFQ quantum tuning UI**: env-only override is sufficient for this PR. Settings file / runtime config is out of scope.
- **Cross-die fairness**: each die's wait queue is independent. Workloads that systematically target one die more than others see no rebalancing here — that is FTL allocation policy's job.

## 11. Risks and mitigations

| Risk | Severity | Mitigation |
|---|---|---|
| Notifier-under-die_lock introduces a deadlock with future code that acquires `waitqueue_lock` first | High | Lock-order invariant documented (Section 7.1) and asserted in debug builds; lockdep-style ordering test in L2 |
| Waiter `cv_wait` misses signal due to lost-wakeup race | High | `signaled` flag pattern (Section 7.2) is the industry-standard fix; L2 explicitly stresses this with `HFSSS_DIE_DISP_NOTIFIER_DELAY_NS` |
| Existing IS-04 / IS-06 / SUSPENDED-state tests regress (cv-revert lesson) | High | L5 layer in test plan is mandatory pass; CI fails the PR if any of those cases break |
| WFQ deficit accounting drifts to starvation under skewed workload | Medium | L4 covers each tier-mix scenario and asserts on rough share ratios; documented behavior with quantum override |
| Idle GC starves under sustained host load | Medium | This is **intended** behavior per spec; L4 has a "host quiet → idle GC drains queue" case to confirm idle GC eventually runs when host backs off |
| GC kick path edits ripple into many call sites | Medium | The `gc_trigger_t` enum + helper is a single addition; existing GC paths default to `GC_TRIGGER_HOST_WRITE` if not updated, preserving today's behavior with no priority differentiation |
| FTL_NUM_WORKERS = 8 saturates dispatcher contention on a 32-die device | Low | Per-die queue avoids cross-die contention; per-queue lock is held only during list-mutate (microseconds) |

## 12. Decision log

Alternatives considered and rejected:

- **Model 1 (condvar block at die-level inside cmd_engine)**: violates fail-fast contract; previously reverted (commit `32e4611`). Rejected on regression risk.
- **Model 3 (per-die service threads)**: 32 extra threads on default geometry; conflicts with the existing 8-FTL-worker design philosophy. Per-channel variant would be 4 threads but is essentially REQ-045 tier 3 and is treated as a separate project.
- **Option (ii) truly-async FTL API**: requires re-architecting NBD pipeline + GC + WL callers. Estimated 1000–1200 LOC across 6+ files. Deferred; option (i) achieves the same fidelity for the dispatch sub-system.
- **Pool capacity tuning at NBD layer (originally A in brainstorming)**: investigation showed the NBD pool of 256 never saturates at fio iodepth=16; the gate is unreachable. Rejected on evidence.
- **Ring buffer instead of linked list for the wait queue**: bounded capacity reintroduces the spin-or-block-submitter problem; priority insertion is awkward; cancellation cost is high. Linked list with embedded waiter nodes wins on every axis except cache friendliness, which is irrelevant at the per-die ≤ 8 waiter scale.
- **Profile-flag for fault injection**: rejected per user direction; debug knobs are env-only and never bake into release artifacts.

## 13. Glossary

- **Anchor A / B / C**: the three points in `cmd_engine.c` where a die transitions to `DIE_IDLE`. A and B fire the notifier; C does not. See Section 4.2.
- **EMA IAT**: exponentially-weighted moving average of host IO inter-arrival time, used as a proxy for "is host bursting".
- **Tier T1–T4**: the four priority bands in the per-die wait queue. T1 strict-high, T2/T3 WFQ-shared, T4 strict-low with host-burst yield.
- **WFQ deficit**: weighted fair queueing via deficit round robin. Each share-slot accumulates a deficit; the dispatcher picks the slot with the largest deficit and increments by quantum.
- **gc_trigger_t**: enum carried through the GC pipeline that names the kick reason; mapped to `die_priority_t` at submit time.

---

End of design spec.
