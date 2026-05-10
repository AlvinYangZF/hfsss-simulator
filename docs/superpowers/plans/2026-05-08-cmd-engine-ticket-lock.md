# cmd_engine Ticket-Lock Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Replace `pthread_mutex_t` for `cmd_engine`'s `die_lock` and `channel_lock` with a FIFO-fair ticket lock so the dispatcher (PR #118) actually delivers the ≥ 10× latency reduction on `012_fio_seqwrite_verify`.

**Architecture:** New small primitive `struct ticket_lock` (atomic ticket counter + serving counter + spin-with-yield acquire). Drop-in replacement at the two `cmd_engine` lock sites only.

**Tech Stack:** C11 atomics, `sched_yield()`, no new dependencies.

**Reference:** `docs/superpowers/specs/2026-05-08-cmd-engine-ticket-lock-design.md`.

**Branch:** `perf/cmd-engine-ticket-lock` off origin/master AFTER PR #118 merges.

**Prerequisite:** PR #118 must be merged first; this PR builds on the dispatcher infrastructure landed there.

---

## Task 1: Ticket lock primitive + unit tests

**Why first:** isolated, testable. Establishes the API before any cmd_engine touch.

**Files:**
- Create: `include/common/ticket_lock.h`
- Create: `src/common/ticket_lock.c` (or inline-only if header-only is project norm; check existing utilities)
- Create: `tests/test_ticket_lock.c`
- Modify: `Makefile`

- [ ] **Step 1: Author the header**

```c
#ifndef HFSSS_TICKET_LOCK_H
#define HFSSS_TICKET_LOCK_H

#include "common/common.h"
#include <stdatomic.h>

/*
 * FIFO-fair ticket lock. Two atomic counters: ticket (next to issue)
 * and serving (currently running). lock() takes a ticket and spins
 * until serving == ticket; unlock() increments serving. Order of
 * acquisition is the order in which lock() callers issued tickets,
 * which is exactly FIFO with respect to the atomic_fetch_add.
 *
 * Use only for short-held critical sections (microseconds to a few
 * ms). Not appropriate for sleep-on-lock workloads — use pthread_mutex
 * for those. Spin uses sched_yield so it does not burn one core per
 * waiter, but it is not as cheap as a futex park.
 */
struct ticket_lock {
    _Atomic uint64_t ticket;
    _Atomic uint64_t serving;
};

void ticket_lock_init(struct ticket_lock *l);
void ticket_lock_lock(struct ticket_lock *l);
void ticket_lock_unlock(struct ticket_lock *l);
bool ticket_lock_try_lock(struct ticket_lock *l);

#endif
```

- [ ] **Step 2: Failing single-thread acquire test**

```c
/* tests/test_ticket_lock.c */
void test_single_thread_acquire_release(void)
{
    struct ticket_lock l;
    ticket_lock_init(&l);
    ticket_lock_lock(&l);
    ticket_lock_unlock(&l);
    /* Subsequent acquire should also succeed. */
    ticket_lock_lock(&l);
    ticket_lock_unlock(&l);
    /* Counters should match: 2 issued, 2 served. */
    assert(atomic_load(&l.ticket) == 2);
    assert(atomic_load(&l.serving) == 2);
}
```

- [ ] **Step 3: Verify it fails (link error)**

Run: `make tests/test_ticket_lock && ./build/bin/test_ticket_lock`
Expected: undefined symbols.

- [ ] **Step 4: Implement init/lock/unlock**

```c
void ticket_lock_init(struct ticket_lock *l)
{
    atomic_store(&l->ticket, 0);
    atomic_store(&l->serving, 0);
}

void ticket_lock_lock(struct ticket_lock *l)
{
    uint64_t my = atomic_fetch_add(&l->ticket, 1);
    while (atomic_load(&l->serving) != my) {
        sched_yield();
    }
}

void ticket_lock_unlock(struct ticket_lock *l)
{
    atomic_fetch_add(&l->serving, 1);
}

bool ticket_lock_try_lock(struct ticket_lock *l)
{
    uint64_t expected = atomic_load(&l->serving);
    /* Try to grab a ticket only if no one is queued. */
    return atomic_compare_exchange_strong(&l->ticket, &expected, expected + 1);
}
```

- [ ] **Step 5: Run, expect PASS**

- [ ] **Step 6: FIFO-order multi-thread test**

```c
void test_fifo_acquisition_order(void)
{
    /* 8 threads each take a ticket; record the order in which they
     * actually run their critical section; assert that the order
     * matches the ticket order. */
    /* Use a barrier to synchronize all threads to take their tickets
     * within a small window, then have each block on the lock and
     * record the order. */
}
```

- [ ] **Step 7: Implement, run, PASS**

- [ ] **Step 8: Stress test**

64 threads × 10000 iterations sharing a ticket lock + a counter.
Final counter must equal 64 × 10000 (no lost increments → no double-acquire). No hang.

- [ ] **Step 9: Implement try_lock test**

- [ ] **Step 10: Wire into Makefile + run full make test**

- [ ] **Step 11: Commit**

```
feat(common): FIFO-fair ticket lock primitive

Atomic ticket + serving counters; spin-with-yield acquire. Sized for
the short-held cmd_engine die_lock / channel_lock contention windows
where pthread_mutex_t's lack of FIFO fairness undoes the dispatcher's
wake order.
```

---

## Task 2: Replace die_lock with ticket lock

**Why second:** smallest blast radius. die_lock is the most contended in fio-012 stress; should produce most of the latency win on its own.

**Files:**
- Modify: `include/media/nand.h` (struct nand_die)
- Modify: `src/media/nand.c` (init/cleanup; check actual file)
- Modify: `src/media/cmd_engine.c` (every site that touches die->die_lock)

- [ ] **Step 1: Run baseline pre-checkin** (as the comparison reference for this PR)

```
make pre-checkin > /tmp/precheckin-pre-die.log 2>&1
```

Extract the 012 numbers; archive at `docs/perf-baselines/<date>-pre-die-ticket-lock/`.

- [ ] **Step 2: Change struct nand_die's die_lock type**

```c
/* include/media/nand.h */
struct nand_die {
    /* ... */
    /* struct mutex die_lock;  -- REPLACED */
    struct ticket_lock die_lock;
    /* ... */
};
```

- [ ] **Step 3: Update init / cleanup**

In nand_die_init (find the actual file via grep), replace:
```c
mutex_init(&die->die_lock);
/* with */
ticket_lock_init(&die->die_lock);
```

Cleanup: ticket_lock has no resources, so the existing
`mutex_cleanup(&die->die_lock)` line is removed (or replaced with no-op).

- [ ] **Step 4: Update every cmd_engine call site**

Grep:
```
grep -nE "(mutex_lock|mutex_unlock).*die->die_lock|die->die_lock" src/media/cmd_engine.c
```

For each match:
```c
/* mutex_lock(&die->die_lock, 0)  →  ticket_lock_lock(&die->die_lock) */
/* mutex_unlock(&die->die_lock)   →  ticket_lock_unlock(&die->die_lock) */
```

Note: `mutex_lock(&l, 0)` takes a flags arg; `ticket_lock_lock(&l)` does not. Drop the trailing `, 0`.

- [ ] **Step 5: Build, fix any compile errors**

- [ ] **Step 6: Run targeted tests, expect PASS**

```
./build/bin/test_cmd_engine_notifier   # T1 anchor coverage
./build/bin/test_die_dispatcher_engine # L3
./build/bin/test_media                 # IS-04 / IS-06 / SUSPENDED red line
./build/bin/test_reset_abort_race
```

If any FAIL: **stop and report**. Do not commit.

- [ ] **Step 7: Run full make test, expect PASS**

- [ ] **Step 8: Run pre-checkin, capture numbers**

```
make pre-checkin > /tmp/precheckin-die-only.log 2>&1
```

Extract 012 numbers; expect significant write-latency drop. Even if not all the way to 10ms, this should be in the 20-40ms range (single lock's contribution).

- [ ] **Step 9: Commit**

```
feat(cmd_engine): replace die_lock pthread_mutex with ticket lock

FIFO fairness on die acquisition; dispatcher-signaled waiters now win
the lock against fresh arrivals. Eliminates the lock-race re-queue
cycle that kept 012_seqwrite_verify pinned at the original latency
floor in PR #118.
```

---

## Task 3: Replace channel_lock with ticket lock

**Why third:** validates that the channel-side contention is also relevant. Smaller incremental win expected (channel_lock holds shorter sections), but completes the FIFO-fair coverage.

**Files:**
- Modify: `include/media/nand.h` (struct nand_channel)
- Modify: `src/media/nand.c` (channel init/cleanup)
- Modify: `src/media/cmd_engine.c` (every site that touches channel->lock)

- [ ] **Step 1: Change struct nand_channel's lock type**

- [ ] **Step 2: Update init / cleanup**

- [ ] **Step 3: Update every cmd_engine call site**

```
grep -nE "channel->lock" src/media/cmd_engine.c
```

Replace mutex_lock/unlock pairs with ticket_lock variants.

- [ ] **Step 4: Build, run targeted tests**

- [ ] **Step 5: Run full make test**

- [ ] **Step 6: Run pre-checkin, capture numbers**

```
make pre-checkin > /tmp/precheckin-both.log 2>&1
```

Expected at this point: 012 write mean ≤ 10 ms, p99 ≤ 20 ms, IOPS ≥ 1500.

- [ ] **Step 7: Archive post-impl baseline**

`docs/perf-baselines/<date>-cmd-engine-ticket-lock/`

- [ ] **Step 8: Commit**

```
feat(cmd_engine): replace channel_lock pthread_mutex with ticket lock

FIFO fairness across both die and channel acquisition paths.
Completes the lock-fairness fix; the dispatcher's wake order now
flows through to actual cmd_engine submission ordering.
```

---

## Task 4: Spec / docs reconciliation + open PR

**Files:**
- Modify: `docs/REQUIREMENT_COVERAGE.md` (FTL subtotal: append note about ticket-lock follow-up landing)
- Modify: `docs/superpowers/specs/2026-04-30-ftl-die-busy-waitqueue-design.md` Section 9.2 (mark Option 1 as done; update KPI table to reflect achieved targets)
- Possibly: `CLAUDE.md` if the ticket lock primitive merits a separate Diagnostics entry (probably not; it's just a primitive, not a debug tool)

- [ ] **Step 1: Spec reconciliation**

Update the KPI table in PR #118's spec to reflect the achieved numbers post-ticket-lock. Update Section 9.2 to mark Option 1 as merged.

- [ ] **Step 2: REQUIREMENT_COVERAGE update**

Append to FTL subtotal: "Lock fairness follow-up via ticket lock landed in `docs/superpowers/specs/2026-05-08-cmd-engine-ticket-lock-design.md`; closes the latency gap left open by the dispatcher PR."

- [ ] **Step 3: Commit docs**

- [ ] **Step 4: Push branch, open PR**

PR title: `perf(cmd_engine): FIFO-fair ticket lock — close 012 latency gap from dispatcher PR`

PR body:
- Reference the dispatcher PR (#118).
- Show baseline → post-die-lock → post-both-locks number progression.
- Explain why this is the smallest follow-up that closes the gap (per spec Section 9.2).
- Note: regression red line preserved.

- [ ] **Step 5: Wait for CI; address any feedback.**

---

## Dependency graph

```
Task 1 (ticket lock primitive + unit tests)
    │
    └─> Task 2 (replace die_lock)
            │
            └─> Task 3 (replace channel_lock)
                    │
                    └─> Task 4 (docs + PR)
```

Strictly sequential. Tasks 2 and 3 each gate on the previous task's pre-checkin passing, so they must run in order.

## Execution

After PR #118 merges:
1. `git checkout master && git pull origin master`
2. `git checkout -b perf/cmd-engine-ticket-lock`
3. Use superpowers:subagent-driven-development to execute the four tasks.

## Self-review

- [x] Spec coverage: all four spec sections (primitive, die_lock, channel_lock, KPI) map to a task
- [x] No "TBD" or "implement later" placeholders
- [x] Type signatures consistent (`struct ticket_lock` from Task 1 used in Task 2/3)
- [x] Tests precede implementation (Task 1 builds the primitive with TDD; Task 2/3 run regression suites before committing)
- [x] KPI from spec is explicit pass criterion in Task 3 / 4
- [x] No AI tool names or progress-stage terms
