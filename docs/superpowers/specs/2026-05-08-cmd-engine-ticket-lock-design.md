# cmd_engine Ticket-Lock Design

## 1. Goal

Replace `pthread_mutex_t` for `cmd_engine`'s `die_lock` and
`channel_lock` with a FIFO-fair ticket lock so the per-die wait
queue dispatcher (PR #118) actually delivers the projected
≥ 10× reduction in `012_fio_seqwrite_verify` write latency.

This is the smallest-scoped follow-up among the three options
documented in `docs/superpowers/specs/2026-04-30-ftl-die-busy-waitqueue-design.md`
Section 9.2. It does not touch the dispatcher itself.

## 2. Background

PR #118 landed the per-die wait queue + completion-event-driven
dispatch but did **not** reduce the 012 seqwrite latency floor:
141.8 ms mean (baseline 142.0 ms). The dispatcher provides FIFO
ordering on the **wake side**, but `pthread_mutex_t` on macOS (and
Linux with `PTHREAD_MUTEX_DEFAULT`) is not FIFO-fair: a thread that
just released the mutex can re-acquire it ahead of waiters who have
been blocked longer. Under fio-012 contention, fresh FTL workers win
the lock race against dispatcher-signaled waiters, undoing the
dispatcher's ordering. See PR #118 spec Section 9.2 for the full
analysis.

## 3. Architecture

### 3.1 Ticket lock primitive

A ticket lock guarantees FIFO acquisition order:

```
struct ticket_lock {
    _Atomic uint64_t ticket;   /* next ticket to issue */
    _Atomic uint64_t serving;  /* ticket currently being served */
};

void ticket_lock(struct ticket_lock *l) {
    uint64_t my = atomic_fetch_add(&l->ticket, 1);
    while (atomic_load(&l->serving) != my) {
        sched_yield();   /* or futex/cv wait */
    }
}

void ticket_unlock(struct ticket_lock *l) {
    atomic_fetch_add(&l->serving, 1);
}
```

Two fields, FIFO by construction. Lock-acquisition cost is one
atomic increment (always non-blocking) plus a spin/yield until your
ticket is up. Under contention the spin terminates after at most
`(ticket - serving)` lock releases, which equals the queue depth in
front of you.

### 3.2 Spin vs blocking

Pure spin (`while (...) sched_yield()`) is acceptable when contention
is short-lived (ms-scale). NAND tProg is ~1.3 ms, so a spinning
waiter gets through within a few yields. For longer waits, a
condvar-based variant is preferable.

The contention windows in cmd_engine are bounded by NAND timing:
- `channel_lock` is held briefly at submit setup and reset cleanup
  (microseconds; spin is fine)
- `die_lock` is held across a state transition (microseconds; same)

Only the engine_run_array_busy phase holds locks for ms-scale
duration, but that is between the dispatcher's caller and itself —
no other waiter is supposed to be racing for the SAME die during
that window (legality matrix already guards). So FIFO fairness only
needs to hold during the small window where multiple waiters race
for the same released lock.

**Decision**: pure spin-with-yield ticket lock. No condvar. Simplest
implementation that solves the problem.

### 3.3 Where the lock lives

Replace these two field types in `struct nand_die` and
`struct nand_channel`:

```c
/* Before: */
struct mutex die_lock;
struct mutex channel_lock;   /* on nand_channel */

/* After: */
struct ticket_lock die_lock;
struct ticket_lock channel_lock;
```

Project's `struct mutex` is a wrapper over `pthread_mutex_t` plus a
debug instrumentation hook. The wrapper API (`mutex_lock`,
`mutex_unlock`, etc.) stays — we provide a parallel
`ticket_lock_lock`, `ticket_lock_unlock` API and update only the two
touch sites in `cmd_engine` to use it.

This minimizes blast radius. Other users of `struct mutex` (FTL ctx
lock, block manager lock, etc.) remain `pthread_mutex_t`-backed.

### 3.4 Compatibility with existing code

`cmd_engine.c` calls `mutex_lock(&die->die_lock, 0)` and
`mutex_unlock(&die->die_lock)` at multiple anchors (submit entry,
completion path, reset path, the dispatcher notifier hook). Each
becomes `ticket_lock(&die->die_lock)` /
`ticket_unlock(&die->die_lock)`.

`nand_die_init` / `nand_channel_init` change one line each: replace
`mutex_init` with `ticket_lock_init`.

The dispatcher's `q->lock` is unaffected (it is a per-waitqueue
mutex internal to the dispatcher, not a cmd_engine lock).

## 4. Testing

### 4.1 Unit test for the primitive

`tests/test_ticket_lock.c`:
- Single-thread acquire/release (smoke).
- Multi-thread FIFO order: 8 threads each take a ticket; verify
  acquisitions happen in ticket order (not arrival order, which is
  the same here, but in lock-release order under contention).
- Stress: 64 threads × 10000 iterations on a shared ticket lock;
  no deadlock, no double-acquire.

### 4.2 cmd_engine regression

All existing cmd_engine tests must pass byte-equivalent. Notably:
- `tests/test_cmd_engine_notifier.c` (T1 anchors)
- `tests/test_media.c` IS-04 / IS-06 / SUSPENDED state cases
- `tests/test_reset_abort_race.c`
- `tests/test_cmd_integration_*.c`

### 4.3 Dispatcher integration regression

PR #118's L1-L4 must continue passing:
- `tests/test_die_dispatcher_unit.c`
- `tests/test_die_dispatcher_mt.c`
- `tests/test_die_dispatcher_engine.c`
- `tests/test_die_dispatcher_faults.c`

### 4.4 Performance KPI (the actual goal)

`make pre-checkin` 8/8 PASS, plus the spec target finally landing:

| Case | Metric | Pre-impl | Target |
|---|---|---|---|
| 012 seqwrite | write mean lat | 141.8 ms | ≤ 10 ms |
| 012 seqwrite | write p99 | 143.6 ms | ≤ 20 ms |
| 012 seqwrite | write IOPS | 112 | ≥ 1500 |
| Others | all | (current PR baseline) | ±10 % |

Archive new baseline at
`docs/perf-baselines/<date>-cmd-engine-ticket-lock/`.

## 5. Out of scope

- Replacing `pthread_mutex_t` anywhere outside cmd_engine.
- Die-reservation protocol (option 2 from PR #118 spec 9.2).
- REQ-045 tier 3 channel_worker retarget (option 3).
- Switching the ticket lock to a futex/condvar-backed variant (only
  worth it if profiling shows sched_yield spinning is meaningful).
- Adding ticket lock to the dispatcher's `q->lock` (already not the
  bottleneck; touched only briefly under enqueue/dequeue).

## 6. Risks

| Risk | Mitigation |
|---|---|
| `sched_yield` spin under heavy contention burns CPU. | Contention window is bounded by NAND tProg (~1.3 ms). Profile post-impl to confirm yield count is small. |
| Ticket lock deadlock if a holder dies. | Project does not kill threads holding NAND locks; same risk as today's pthread mutex. |
| Lock-order assumptions encoded in pthread debug instrumentation break. | The wrapper hook is bypassed; code review confirms cmd_engine's lock-order guarantees do not rely on the wrapper. |
| Existing tests assume specific scheduling timing (flake). | Run new tests 3× before merge per project norm; loosen statistical bands if needed. |
| FTL worker count grows beyond 8 in future and starves the spin loop. | The spin is bounded by queue depth, not worker count. |

## 7. Decision log

- **Why ticket lock over MCS lock**: ticket lock is ~30 LOC; MCS is
  fairer under high contention but ~80 LOC and requires per-thread
  node allocation. Our contention windows are short and the queue
  depth is bounded by `FTL_NUM_WORKERS = 8`, so ticket lock's spin
  is cheap.
- **Why spin instead of futex**: macOS lacks Linux futexes; portable
  alternative requires per-lock condvar plus mutex, doubling the
  state size. Spin-with-yield is portable and adequate.
- **Why not change all locks**: minimizing blast radius. Only
  cmd_engine's two locks demonstrably suffer from the unfairness
  problem (per PR #118 evidence). Other locks are not on the
  dispatcher's wake path.
