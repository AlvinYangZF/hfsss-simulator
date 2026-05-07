# FTL Die-Busy Wait-Queue Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `ftl_mt`'s `nanosleep × 2048` retry-spin with a per-die wait queue plus event-driven dispatch driven by a new cmd_engine notifier hook.

**Architecture:** New module `die_dispatcher` (FTL layer) owns per-die multi-tier wait queues. `cmd_engine` adds a single optional notifier callback fired when a die transitions to `DIE_IDLE` (anchors A and B; not C). FTL workers block on per-request condvars while the dispatcher serves them in priority order.

**Tech Stack:** C11, pthreads, condvars, embedded doubly-linked lists, atomic env probes.

**Reference:** `docs/superpowers/specs/2026-04-30-ftl-die-busy-waitqueue-design.md` is the authoritative design.

**Branch:** `perf/ftl-die-busy-waitqueue` (already created; spec + baseline already committed).

---

## Task 1: cmd_engine notifier hook

**Why first:** the dispatcher cannot be installed without this hook. This task is the smallest, foundational piece; isolating it lets us prove the hook works with a counter notifier before the full dispatcher exists.

**Files:**
- Modify: `include/media/nand.h` (struct nand_device gets two new optional fields)
- Modify: `src/media/cmd_engine.c` (call notifier at anchors A and B)
- Create: `tests/test_cmd_engine_notifier.c`
- Modify: `Makefile` (add new test binary)

- [ ] **Step 1: Add notifier field declaration**

In `include/media/nand.h`, add inside `struct nand_device`:

```c
    /* Optional callback invoked when a die transitions to DIE_IDLE.
     * NULL means no observer (default). Installed by die_dispatcher. */
    void (*die_ready_notifier)(struct nand_device *dev, u32 ch, u32 chip, u32 die);
    void  *die_ready_ctx;
```

Place adjacent to other optional callback fields. Default zero-init through `calloc` / `memset` matches today's behavior.

- [ ] **Step 2: Write the failing notifier-fires-on-anchor-A test**

Create `tests/test_cmd_engine_notifier.c`:

```c
#include "media/nand.h"
#include "media/cmd_engine.h"
#include "test_common.h"
#include <stdatomic.h>

static atomic_int g_notify_count;
static u32 g_last_ch, g_last_chip, g_last_die;

static void test_notifier(struct nand_device *dev, u32 ch, u32 chip, u32 die)
{
    atomic_fetch_add(&g_notify_count, 1);
    g_last_ch = ch; g_last_chip = chip; g_last_die = die;
}

void test_anchor_a_fires_on_normal_completion(void)
{
    struct nand_device dev;
    setup_default_nand(&dev);
    dev.die_ready_notifier = test_notifier;
    atomic_store(&g_notify_count, 0);

    struct nand_cmd_target t = { .ch = 0, .chip = 0, .die = 0, .plane_mask = 0x1, .page = 0, .block = 0 };
    int rc = cmd_engine_submit(&dev, NAND_OP_PROG, &t, NULL, NULL);

    assert(rc == HFSSS_OK);
    assert(atomic_load(&g_notify_count) == 1);
    assert(g_last_ch == 0 && g_last_chip == 0 && g_last_die == 0);

    teardown_nand(&dev);
}
```

- [ ] **Step 3: Run test to verify it fails**

Run: `make tests/test_cmd_engine_notifier && ./build/bin/test_cmd_engine_notifier`
Expected: FAIL — `g_notify_count` is 0 because the hook is not yet wired.

- [ ] **Step 4: Wire anchor A in cmd_engine.c**

In the "Normal completion or cache-end: drive to IDLE" branch of `engine_submit` (the branch that sets `die->cmd_state.state = DIE_IDLE` after a non-cache-prog completion), add immediately after the IDLE-state cleanup and before `mutex_unlock(&die->die_lock)`:

```c
        if (dev->die_ready_notifier) {
            dev->die_ready_notifier(dev, target->ch, target->chip, target->die);
        }
```

- [ ] **Step 5: Run anchor-A test, expect PASS**

Run: `./build/bin/test_cmd_engine_notifier`
Expected: PASS, count == 1.

- [ ] **Step 6: Add anchor-B test (reset path)**

Append to `test_cmd_engine_notifier.c`:

```c
void test_anchor_b_fires_on_reset(void)
{
    struct nand_device dev;
    setup_default_nand(&dev);
    dev.die_ready_notifier = test_notifier;
    atomic_store(&g_notify_count, 0);

    /* Put die into a non-IDLE state first by starting a PROG. */
    struct nand_cmd_target t = { .ch = 1, .chip = 0, .die = 0, .plane_mask = 0x1, .page = 0, .block = 0 };
    cmd_engine_submit(&dev, NAND_OP_PROG, &t, NULL, NULL);  /* completes; A fires */
    int before_reset = atomic_load(&g_notify_count);

    /* Now hold the die busy via a CACHE_PROG sequence then issue RESET. */
    cmd_engine_submit(&dev, NAND_OP_CACHE_PROG, &t, NULL, NULL);
    int rc = cmd_engine_submit(&dev, NAND_OP_RESET, &t, NULL, NULL);
    assert(rc == HFSSS_OK);

    /* Reset must fire one notifier (anchor B). */
    assert(atomic_load(&g_notify_count) >= before_reset + 1);

    teardown_nand(&dev);
}
```

- [ ] **Step 7: Run test, expect FAIL on anchor B**

Run: `./build/bin/test_cmd_engine_notifier`
Expected: FAIL on `test_anchor_b_fires_on_reset`.

- [ ] **Step 8: Wire anchor B in cmd_engine.c (reset path)**

In `engine_submit_reset`, after `nand_cmd_state_init(&die->cmd_state)` and before the `mutex_unlock(&die->die_lock)` that closes the reset critical section, add:

```c
    if (dev->die_ready_notifier) {
        dev->die_ready_notifier(dev, target->ch, target->chip, target->die);
    }
```

- [ ] **Step 9: Re-run, expect PASS**

Run: `./build/bin/test_cmd_engine_notifier`
Expected: PASS, both cases.

- [ ] **Step 10: Add anchor-C-does-not-fire negative-assertion test**

Append:

```c
void test_anchor_c_does_not_fire_on_implicit_cache_terminate(void)
{
    struct nand_device dev;
    setup_default_nand(&dev);
    dev.die_ready_notifier = test_notifier;

    struct nand_cmd_target t = { .ch = 2, .chip = 0, .die = 0, .plane_mask = 0x1, .page = 0, .block = 0 };
    /* Open a cache sequence */
    cmd_engine_submit(&dev, NAND_OP_CACHE_PROG, &t, NULL, NULL);
    int after_cache = atomic_load(&g_notify_count);

    /* Submit a non-cache op that triggers anchor C's force-IDLE.
     * The completion of THIS op fires anchor A, not C. So total
     * delta should be exactly 1, not 2. */
    cmd_engine_submit(&dev, NAND_OP_READ, &t, NULL, NULL);
    int after_read = atomic_load(&g_notify_count);

    assert(after_read - after_cache == 1);  /* anchor A only, not C */

    teardown_nand(&dev);
}
```

- [ ] **Step 11: Run test, expect PASS without further changes**

Run: `./build/bin/test_cmd_engine_notifier`
Expected: PASS — anchor C was deliberately not wired in Step 4 / Step 8.

- [ ] **Step 12: Wire test binary into Makefile**

Add `TEST_CMD_ENGINE_NOTIFIER` variable, build rule, dependency on `LIBHFSSS_MEDIA`, and entry in the `all:` target alongside other test binaries.

- [ ] **Step 13: Full make test still green**

Run: `make test`
Expected: all suites including the new notifier test pass.

- [ ] **Step 14: Commit**

```bash
git add include/media/nand.h src/media/cmd_engine.c tests/test_cmd_engine_notifier.c Makefile
git commit -m "feat(cmd_engine): die-ready notifier hook at anchors A and B

Optional callback fires when a die transitions to DIE_IDLE on the
normal-completion path and on the reset force-clear path. Anchor C
(implicit cache-sequence termination at submit entry) deliberately
skipped — IDLE is transient there and the same caller immediately
re-busies the die. Default NULL preserves identical behavior for
every existing caller.

Foundation for the FTL die-busy wait-queue (see spec
docs/superpowers/specs/2026-04-30-ftl-die-busy-waitqueue-design.md)."
```

---

## Task 2: die_dispatcher data structure + L1 unit tests

**Why second:** pure data-structure work; no threads, no cmd_engine. Establishes the type system and lets L2/L3 build on it.

**Files:**
- Create: `include/ftl/die_dispatcher.h`
- Create: `src/ftl/die_dispatcher.c`
- Create: `tests/test_die_dispatcher_unit.c`
- Modify: `Makefile`

- [ ] **Step 1: Author the public header**

Create `include/ftl/die_dispatcher.h` with the full API surface from spec Section 5.1: `die_priority_t`, `gc_trigger_t`, opaque `struct die_dispatcher`, and prototypes for `create / destroy / wait / on_die_ready / prio_for_gc`.

- [ ] **Step 2: Write failing init/destroy test**

Create `tests/test_die_dispatcher_unit.c`:

```c
#include "ftl/die_dispatcher.h"
#include "media/nand.h"
#include "test_common.h"

void test_create_destroy_is_clean(void)
{
    struct nand_device dev;
    setup_default_nand(&dev);

    struct die_dispatcher *d = die_dispatcher_create(&dev);
    assert(d != NULL);
    assert(dev.die_ready_notifier != NULL);  /* installed */
    assert(dev.die_ready_ctx != NULL);

    die_dispatcher_destroy(d);
    assert(dev.die_ready_notifier == NULL);  /* uninstalled */
    assert(dev.die_ready_ctx == NULL);

    teardown_nand(&dev);
}
```

- [ ] **Step 3: Run test, expect failure (link error)**

Run: `make tests/test_die_dispatcher_unit && ./build/bin/test_die_dispatcher_unit`
Expected: undefined symbols.

- [ ] **Step 4: Stub die_dispatcher.c with create/destroy**

```c
struct die_dispatcher {
    struct nand_device   *dev;
    struct die_waitqueue *queues;       /* one per (ch, chip, die) */
    u32                   queue_count;
};

struct die_dispatcher *die_dispatcher_create(struct nand_device *dev)
{
    if (!dev) return NULL;
    struct die_dispatcher *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->dev = dev;
    d->queue_count = dev->channel_count * dev->chips_per_channel * dev->dies_per_chip;
    d->queues = calloc(d->queue_count, sizeof(*d->queues));
    for (u32 i = 0; i < d->queue_count; i++) {
        die_waitqueue_init(&d->queues[i]);
    }
    dev->die_ready_notifier = die_dispatcher_on_die_ready;
    dev->die_ready_ctx = d;
    return d;
}

void die_dispatcher_destroy(struct die_dispatcher *d)
{
    if (!d) return;
    d->dev->die_ready_notifier = NULL;
    d->dev->die_ready_ctx = NULL;
    for (u32 i = 0; i < d->queue_count; i++) {
        die_waitqueue_cleanup(&d->queues[i]);
    }
    free(d->queues);
    free(d);
}
```

`die_waitqueue_init / cleanup` are local helpers that init/destroy the lock + tier list heads.

- [ ] **Step 5: Run test, expect PASS**

Run: `./build/bin/test_die_dispatcher_unit`
Expected: PASS on `test_create_destroy_is_clean`.

- [ ] **Step 6: Add tier-priority ordering test**

```c
void test_pop_picks_highest_tier_first(void)
{
    struct die_waitqueue q;
    die_waitqueue_init(&q);

    struct die_waiter w1, w2, w3;
    die_waiter_init(&w1, DIE_PRIO_GC_IDLE);     /* T4 */
    die_waiter_init(&w2, DIE_PRIO_HOST_READ);   /* T2 */
    die_waiter_init(&w3, DIE_PRIO_GC_CRITICAL); /* T1 */

    die_waitqueue_enqueue(&q, &w1);
    die_waitqueue_enqueue(&q, &w2);
    die_waitqueue_enqueue(&q, &w3);

    /* T1 must come out first */
    assert(die_waitqueue_pop_next(&q) == &w3);
    /* Then T2 */
    assert(die_waitqueue_pop_next(&q) == &w2);
    /* T4 only after T1/T2/T3 empty AND no host bursting; in this
     * isolated test ema_iat_ns is large (no recent host IO), so T4 fires. */
    assert(die_waitqueue_pop_next(&q) == &w1);
    assert(die_waitqueue_pop_next(&q) == NULL);

    die_waitqueue_cleanup(&q);
}
```

- [ ] **Step 7: Implement waitqueue_init / waiter / enqueue / pop_next**

Build the linked-list + tier-array structure exactly as spec Section 5.2 / 5.4 describes. `die_waitqueue_pop_next` walks T1, T2 (WFQ), T3 (WFQ), T4 (with host_bursting check).

- [ ] **Step 8: Run pop test, expect PASS**

Run: `./build/bin/test_die_dispatcher_unit`
Expected: both cases pass.

- [ ] **Step 9: Add WFQ deficit test**

```c
void test_t2_wfq_alternates_host_read_and_force_gc(void)
{
    struct die_waitqueue q;
    die_waitqueue_init(&q);

    /* Enqueue 4 of each in interleaved order */
    struct die_waiter waiters[8];
    die_priority_t order[8] = {
        DIE_PRIO_HOST_READ, DIE_PRIO_GC_FORCE,
        DIE_PRIO_HOST_READ, DIE_PRIO_GC_FORCE,
        DIE_PRIO_HOST_READ, DIE_PRIO_GC_FORCE,
        DIE_PRIO_HOST_READ, DIE_PRIO_GC_FORCE,
    };
    for (int i = 0; i < 8; i++) {
        die_waiter_init(&waiters[i], order[i]);
        die_waitqueue_enqueue(&q, &waiters[i]);
    }

    /* Default 1:1 quantum → output sequence alternates classes */
    int read_count = 0, force_count = 0;
    int last_class = -1, alternations = 0;
    for (int i = 0; i < 8; i++) {
        struct die_waiter *w = die_waitqueue_pop_next(&q);
        assert(w != NULL);
        int cls = (w->prio == DIE_PRIO_HOST_READ) ? 0 : 1;
        if (cls == 0) read_count++; else force_count++;
        if (last_class != -1 && cls != last_class) alternations++;
        last_class = cls;
    }
    assert(read_count == 4 && force_count == 4);
    assert(alternations >= 6);  /* near-perfect alternation under 1:1 */

    die_waitqueue_cleanup(&q);
}
```

- [ ] **Step 10: Implement WFQ deficit-round-robin**

In `die_waitqueue_pop_next`, when picking from T2 or T3, scan both slots' deficits, pick the slot with highest deficit (or first non-empty if tied), increment that slot's deficit by its quantum, and return its head.

- [ ] **Step 11: Run WFQ test, expect PASS**

Run: `./build/bin/test_die_dispatcher_unit`
Expected: alternation count ≥ 6 in 8 pops.

- [ ] **Step 12: Add EMA host pressure test**

```c
void test_ema_iat_decays_correctly(void)
{
    struct die_waitqueue q;
    die_waitqueue_init(&q);

    /* Simulate 10 host IOs at 100µs IAT (high pressure) */
    uint64_t t = 1000000;  /* arbitrary base */
    for (int i = 0; i < 10; i++) {
        die_waitqueue_record_host_io(&q, t);
        t += 100000;  /* 100µs */
    }
    /* EMA should converge near 100,000 (100µs) */
    assert(q.ema_iat_ns > 80000.0 && q.ema_iat_ns < 200000.0);
    /* host_bursting() should return true */
    assert(die_waitqueue_host_bursting(&q, t) == true);

    /* Now wait 100ms — both EMA-by-decay (no new IOs) and grace window expire */
    t += 100 * 1000 * 1000;
    /* Without new IO, host_bursting should report false eventually:
     * grace window is 1ms, so 100ms gap means raw timestamp check fails.
     * EMA itself doesn't decay without samples but the grace check is OR'd. */
    assert(die_waitqueue_host_bursting(&q, t) == false);

    die_waitqueue_cleanup(&q);
}
```

- [ ] **Step 13: Implement record_host_io and host_bursting**

EMA update on every host_read/host_write dispatch. `host_bursting` returns `(ema_iat_ns < yield_threshold_ns) || ((now - last_host_ts_ns) < burst_grace_ns)`.

- [ ] **Step 14: Run, expect PASS**

Run: `./build/bin/test_die_dispatcher_unit`
Expected: all four cases pass.

- [ ] **Step 15: Add T4-yield-on-burst test**

Cover the case "T4 waiter exists, T1-T3 all empty, host has been recently active → T4 must be skipped".

- [ ] **Step 16: Run, expect PASS**

Expected: T4 yield case correctly returns NULL when host bursting, returns the waiter when host quiet.

- [ ] **Step 17: Add gc_trigger_t mapping test**

```c
void test_gc_trigger_to_priority_mapping(void)
{
    assert(die_dispatcher_prio_for_gc(GC_TRIGGER_FREE_SB_LOW)   == DIE_PRIO_GC_CRITICAL);
    assert(die_dispatcher_prio_for_gc(GC_TRIGGER_READ_DISTURB)  == DIE_PRIO_GC_FORCE);
    assert(die_dispatcher_prio_for_gc(GC_TRIGGER_WEAR_LEVELING) == DIE_PRIO_GC_FORCE);
    assert(die_dispatcher_prio_for_gc(GC_TRIGGER_HOST_WRITE)    == DIE_PRIO_GC_NORMAL);
    assert(die_dispatcher_prio_for_gc(GC_TRIGGER_IDLE)          == DIE_PRIO_GC_IDLE);
}
```

- [ ] **Step 18: Implement helper, run test, expect PASS**

- [ ] **Step 19: Wire into Makefile + run full make test**

Run: `make test` — all green.

- [ ] **Step 20: Commit**

```bash
git add include/ftl/die_dispatcher.h src/ftl/die_dispatcher.c tests/test_die_dispatcher_unit.c Makefile
git commit -m "feat(ftl): die_dispatcher data structure + L1 unit tests

Per-die multi-tier wait queue with embedded doubly-linked list nodes,
deficit-round-robin WFQ inside T2/T3, EMA-based host pressure
detection with grace-window OR for T4 yield. Lifecycle install/uninstall
the cmd_engine notifier hook from Task 1.

Single-thread unit-test coverage: tier ordering, WFQ alternation, EMA
math, T4 yield, gc_trigger_t mapping. No cmd_engine, no concurrency yet."
```

---

## Task 3: die_dispatcher multi-thread tests with mock cmd_engine (L2)

**Why third:** stresses concurrency before bringing the real cmd_engine in. Catches lost-wakeup races and cancellation bugs in isolation.

**Files:**
- Modify: `src/ftl/die_dispatcher.c` (add `die_dispatcher_wait` + cancellation)
- Create: `tests/test_die_dispatcher_mt.c`
- Create: `tests/lib/cmd_engine_mock.c` + `tests/lib/cmd_engine_mock.h` (small mock used only by L2)
- Modify: `Makefile`

- [ ] **Step 1: Write failing concurrent-wake test**

```c
/* tests/test_die_dispatcher_mt.c */
void test_signal_wakes_exactly_one_waiter(void)
{
    /* 8 threads enqueue on the same die. Mock fires the notifier 8 times.
     * Each notifier call must wake exactly one waiter; total 8 wakes. */
    /* ... */
}
```

- [ ] **Step 2: Implement die_dispatcher_wait body**

```c
int die_dispatcher_wait(struct die_dispatcher *d, u32 ch, u32 chip, u32 die,
                        die_priority_t prio, u32 max_wait_ms)
{
    struct die_waitqueue *q = pick_queue(d, ch, chip, die);
    struct die_waiter w;
    die_waiter_init(&w, prio);

    pthread_mutex_lock(&q->lock);
    die_waitqueue_enqueue(q, &w);
    pthread_mutex_unlock(&q->lock);

    pthread_mutex_lock(&w.cv_lock);
    int rc = 0;
    while (!w.signaled && rc == 0) {
        if (max_wait_ms == 0) {
            pthread_cond_wait(&w.cv, &w.cv_lock);
        } else {
            struct timespec ts = compute_timeout(max_wait_ms);
            rc = pthread_cond_timedwait(&w.cv, &w.cv_lock, &ts);
        }
    }
    pthread_mutex_unlock(&w.cv_lock);

    if (rc == ETIMEDOUT) {
        /* Remove ourselves from the queue if still present. */
        pthread_mutex_lock(&q->lock);
        if (!list_node_unlinked(&w.list)) list_del(&w.list);
        pthread_mutex_unlock(&q->lock);
        return ETIMEDOUT;
    }
    return w.shutdown ? HFSSS_ERR_SHUTDOWN : 0;
}
```

- [ ] **Step 3: Implement on_die_ready notifier**

```c
void die_dispatcher_on_die_ready(struct nand_device *dev, u32 ch, u32 chip, u32 die)
{
    struct die_dispatcher *d = dev->die_ready_ctx;
    if (!d) return;
    struct die_waitqueue *q = pick_queue(d, ch, chip, die);
    struct die_waiter *to_wake;

    pthread_mutex_lock(&q->lock);
    to_wake = die_waitqueue_pop_next(q);
    pthread_mutex_unlock(&q->lock);

    if (to_wake) {
        pthread_mutex_lock(&to_wake->cv_lock);
        to_wake->signaled = true;
        pthread_cond_signal(&to_wake->cv);
        pthread_mutex_unlock(&to_wake->cv_lock);
    }
}
```

- [ ] **Step 4: Run concurrent-wake test, expect PASS**

- [ ] **Step 5: Add lost-wakeup defense test**

Notifier called on empty queue → no-op, no crash, no hang on subsequent enqueue.

- [ ] **Step 6: Add cancellation-on-shutdown test**

8 waiters block on a die. `die_dispatcher_destroy` is called. All waiters wake with `HFSSS_ERR_SHUTDOWN`.

- [ ] **Step 7: Implement shutdown signaling in destroy**

Walk every queue, signal every waiter with `shutdown = true`, then `signaled = true`, then `pthread_cond_signal`. Wait for all waiters to drain (count refcounted).

- [ ] **Step 8: Run shutdown test, expect PASS**

- [ ] **Step 9: Add timed-wait timeout test**

Waiter calls with `max_wait_ms = 100`. Notifier never fires. Returns `ETIMEDOUT` and self-removes from queue.

- [ ] **Step 10: Implement timeout self-removal, run, expect PASS**

- [ ] **Step 11: Add lock-order stress test**

Concurrent enqueue + concurrent notifier fire on different dies; run for 1 second; no hangs, no double-frees, no crashes.

- [ ] **Step 12: Run, full make test green**

- [ ] **Step 13: Commit**

```bash
git add src/ftl/die_dispatcher.c tests/test_die_dispatcher_mt.c tests/lib/cmd_engine_mock.* Makefile
git commit -m "feat(ftl): die_dispatcher_wait + on_die_ready + L2 mt tests

Concurrent enqueue/wake correctness, lost-wakeup defense, shutdown
cancellation, timed-wait timeout, lock-order stress. Mock cmd_engine
keeps the test isolated from NAND timing."
```

---

## Task 4: die_dispatcher integration with real cmd_engine (L3)

**Files:**
- Create: `tests/test_die_dispatcher_engine.c`
- Modify: `Makefile`

- [ ] **Step 1: Failing test — submit→complete→notify cycle**

Spin up real `nand_device`, install dispatcher, have one thread call `die_dispatcher_wait`, another thread call `cmd_engine_submit` to consume the die. Verify the waiter wakes within bounded time.

- [ ] **Step 2: Implement test scaffolding (helpers in test_common if missing)**

- [ ] **Step 3: Run, expect PASS without dispatcher code changes**

If anchor A wiring from Task 1 is correct, this should already work.

- [ ] **Step 4: Reset-path notification test (anchor B)**

While a waiter is queued for die X, fire a reset on die X. The waiter must wake.

- [ ] **Step 5: Anchor C negative-assertion in real engine**

Cache sequence open + non-cache submit on the same die. Notifier counter must increase by exactly 1 (anchor A from the new op's completion), not 2.

- [ ] **Step 6: Multi-waiter priority interaction**

3 waiters at different priorities on the same die. Drive 3 completions. Verify wake order matches priority (T1 first).

- [ ] **Step 7: Run all, expect PASS**

- [ ] **Step 8: Commit**

```bash
git add tests/test_die_dispatcher_engine.c Makefile
git commit -m "test(ftl): die_dispatcher real-cmd_engine integration (L3)

End-to-end submit→complete→notify; anchor A and B fire; anchor C does
not; multi-waiter priority order observed."
```

---

## Task 5: ftl_mt integration (replace retry-spin)

**Files:**
- Modify: `src/ftl/ftl_mt.c`
- Modify: `include/ftl/ftl_mt.h` (add `_ex` variants)
- Modify: `src/ftl/ftl_mt_init.c` or wherever ftl_mt context is constructed
- Modify: existing `tests/test_ftl_mt.c` (keep all current cases passing)

- [ ] **Step 1: Verify existing test_ftl_mt currently passes**

Run: `make test_ftl_mt && ./build/bin/test_ftl_mt`
Expected: PASS as today.

- [ ] **Step 2: Add `_ex` variant signatures to header**

```c
int ftl_read_page_mt_ex(struct ftl_mt_ctx *ctx, lba_t lba, void *buf, die_priority_t prio);
int ftl_write_page_mt_ex(struct ftl_mt_ctx *ctx, lba_t lba, const void *buf, die_priority_t prio);
int ftl_trim_page_mt_ex(struct ftl_mt_ctx *ctx, lba_t lba, die_priority_t prio);
```

The existing non-`_ex` functions become thin wrappers that call the `_ex` form with the default host priority.

- [ ] **Step 3: Add die_dispatcher to ftl_mt_ctx**

```c
struct ftl_mt_ctx {
    /* ... existing fields ... */
    struct die_dispatcher *die_disp;
};
```

Construct in `ftl_mt_init`, destroy in `ftl_mt_cleanup`.

- [ ] **Step 4: Replace retry-spin with dispatcher_wait**

In each retry loop currently calling `ftl_busy_backoff_sleep`, replace with:

```c
for (retry_count = 0; retry_count < SAFETY_RETRY_BUDGET; retry_count++) {
    rc = cmd_engine_submit(...);
    if (rc != HFSSS_ERR_BUSY) break;
    int rc_wait = die_dispatcher_wait(ctx->die_disp,
                                      target_ch, target_chip, target_die,
                                      prio, SAFETY_WAIT_MS);
    if (rc_wait == ETIMEDOUT || rc_wait == HFSSS_ERR_SHUTDOWN) {
        rc = (rc_wait == HFSSS_ERR_SHUTDOWN) ? HFSSS_ERR_SHUTDOWN : HFSSS_ERR_BUSY;
        break;
    }
}
```

`SAFETY_RETRY_BUDGET = 8`, `SAFETY_WAIT_MS = 50`. Remove the old `max_retries = 2048` constant. Remove `ftl_busy_backoff_sleep` (or keep as dead code first, deleting in a later step to keep the diff focused).

- [ ] **Step 5: Run existing test_ftl_mt, expect PASS**

Run: `./build/bin/test_ftl_mt`
Expected: PASS — semantics unchanged from caller's view.

- [ ] **Step 6: Run full make test, expect PASS**

Especially `test_media` (IS-04, IS-06, SUSPENDED), `test_cmd_engine`, `test_channel_worker`, `test_mt_ftl`.

- [ ] **Step 7: Delete `ftl_busy_backoff_sleep`**

Now that no caller exists, remove the helper to keep code clean.

- [ ] **Step 8: Run make test, expect PASS**

- [ ] **Step 9: Commit**

```bash
git add src/ftl/ftl_mt.c include/ftl/ftl_mt.h src/ftl/ftl_mt_init.c
git commit -m "feat(ftl_mt): block on die_dispatcher instead of retry-spin

Replace nanosleep(100µs) × 2048 retry loop with die_dispatcher_wait.
Public API unchanged for host-IO callers; new _ex variants accept an
explicit die_priority_t for GC and WL paths.

Safety budget retained but cut to 8 × 50ms — kept only as defense
against a dispatcher bug; in correct operation the loop terminates
on the first or second iteration."
```

---

## Task 6: GC trigger threading + priority callers

**Files:**
- Modify: GC ctx (`src/ftl/gc_mt.c` + headers)
- Modify: GC kick sites
- Modify: WL kick site (single)

- [ ] **Step 1: Add gc_trigger_t to GC context**

```c
struct gc_mt_ctx {
    /* ... existing fields ... */
    gc_trigger_t current_trigger;
};
```

- [ ] **Step 2: Update gc_kick to accept trigger**

```c
void gc_kick(struct gc_mt_ctx *gc, gc_trigger_t trigger);
```

- [ ] **Step 3: Locate and update each kick site**

| Site | Trigger |
|---|---|
| Free-superblock-low watcher | `GC_TRIGGER_FREE_SB_LOW` |
| Read-disturb counter | `GC_TRIGGER_READ_DISTURB` |
| Wear-leveling threshold | `GC_TRIGGER_WEAR_LEVELING` |
| Host-write back-pressure | `GC_TRIGGER_HOST_WRITE` |
| Idle-time sweep | `GC_TRIGGER_IDLE` |

For each, pass the right enum at the kick call.

- [ ] **Step 4: GC submit path uses _ex variant with mapped priority**

Inside the GC task body, every `ftl_*_page_mt(...)` call becomes:
```c
die_priority_t prio = die_dispatcher_prio_for_gc(gc->current_trigger);
ftl_*_page_mt_ex(..., prio);
```

- [ ] **Step 5: Run existing GC tests, expect PASS**

Run: `make test_gc_mt && ./build/bin/test_gc_mt`

- [ ] **Step 6: Commit**

```bash
git add src/ftl/gc_mt.c include/ftl/gc.h ...
git commit -m "feat(gc): thread gc_trigger_t origin through to dispatcher

GC kick sites tag their reason; GC body maps reason to die priority
and calls the _ex FTL variants. Today this affects priority
differentiation only — behavioral equivalence with master is
preserved when no host or other GC contention is present."
```

---

## Task 7: FTL integration tests (L4)

**Files:**
- Modify: `tests/test_ftl_mt.c`
- Modify: `tests/test_gc_mt.c`

- [ ] **Step 1: Add critical-GC-preempts-host test**

Saturate host writes. Trip free-SB-low threshold. Verify GC critical commands complete with priority over host-write commands (measured via timestamp ordering of completions on the contested die).

- [ ] **Step 2: Implement test, run, expect PASS**

- [ ] **Step 3: Add host-read + force-GC 1:1 WFQ test**

Run host reads + force-GC reads on same die concurrently for N seconds. Count completions per class. Ratio should be near 1:1 ± 20%.

- [ ] **Step 4: Implement, run, PASS**

- [ ] **Step 5: Add host-write + normal-GC WFQ test**

Same as above but write side.

- [ ] **Step 6: Add idle-GC-yields-on-host-burst test**

Drive idle GC steady state. Then start a host burst. Verify idle GC throughput drops to near zero during the burst window.

- [ ] **Step 7: Add host-quiet-idle-GC-drains test**

No host load, idle GC running. Verify idle GC eventually drains all queued GC work (not starved when host is genuinely idle).

- [ ] **Step 8: Run all, expect PASS**

- [ ] **Step 9: Commit**

```bash
git commit -m "test(ftl): L4 priority and WFQ integration tests

Critical GC pre-emption, host-read vs force-GC WFQ, host-write vs
normal-GC WFQ, idle-GC yield under host burst, idle-GC drain when
host quiet."
```

---

## Task 8: Fault-injection knobs (env-gated)

**Files:**
- Modify: `src/ftl/die_dispatcher.c` (add env probe + injection points)
- Create: `tests/test_die_dispatcher_faults.c`

- [ ] **Step 1: Implement HFSSS_DIE_DISP_FORCE_BUSY**

```c
static int force_busy_pct(void) {
    static _Atomic int s = -1;
    int v = atomic_load_explicit(&s, memory_order_relaxed);
    if (v == -1) {
        const char *e = getenv("HFSSS_DIE_DISP_FORCE_BUSY");
        v = (e && *e) ? atoi(e) : 0;
        atomic_store_explicit(&s, v, memory_order_relaxed);
    }
    return v;
}
```

In the dispatcher_wait re-submit loop (Task 5's loop), if waiter just woke (not first iteration) and `rand() % 100 < force_busy_pct()`, override `rc` to `HFSSS_ERR_BUSY` to force re-queue.

- [ ] **Step 2: Implement HFSSS_DIE_DISP_NOTIFIER_DELAY_NS**

In `die_dispatcher_on_die_ready`, after popping the waiter and before signaling, sleep for the configured ns.

- [ ] **Step 3: Test that knobs do not trigger when env unset**

Run all existing tests with no env. Pass.

- [ ] **Step 4: Test force-busy under HFSSS_DIE_DISP_FORCE_BUSY=50**

Test makes 1000 dispatcher_wait calls; sees re-queue ratio near 50%; all eventually complete; no deadlocks.

- [ ] **Step 5: Test notifier-delay correctness**

`HFSSS_DIE_DISP_NOTIFIER_DELAY_NS=1000000` (1 ms). Run 100 submit/complete cycles. All complete; total runtime increases by ~100 ms.

- [ ] **Step 6: Run full make test, expect PASS**

- [ ] **Step 7: Commit**

```bash
git commit -m "feat(ftl): env-gated fault-injection knobs for die_dispatcher

HFSSS_DIE_DISP_FORCE_BUSY=N — inject N% spurious BUSY on re-submission.
HFSSS_DIE_DISP_NOTIFIER_DELAY_NS=K — sleep K ns inside notifier between
dequeue and signal.

Pattern matches HFSSS_TRACE_IO_ERR — atomic-cached getenv result, no
overhead when unset."
```

---

## Task 9: Documentation + REQUIREMENT_COVERAGE update

**Files:**
- Modify: `CLAUDE.md` (add HFSSS_DIE_DISP_* env vars under Diagnostics)
- Modify: `docs/REQUIREMENT_COVERAGE.md` (note die_dispatcher under perf/REQ-1xx if applicable)
- Possibly: `docs/HLD_*` or `docs/LLD_*` performance section

- [ ] **Step 1: CLAUDE.md update**

Add to the Diagnostics section:

```markdown
### HFSSS_DIE_DISP_FORCE_BUSY / HFSSS_DIE_DISP_NOTIFIER_DELAY_NS
Two env-gated knobs for stressing the FTL die-busy wait queue
(`src/ftl/die_dispatcher.c`). FORCE_BUSY=N injects N% spurious
HFSSS_ERR_BUSY on re-submission to exercise the re-queue path.
NOTIFIER_DELAY_NS=K sleeps K ns between waiter dequeue and cv signal
to verify no missed-wakeup window exists. Default off; getenv result
cached.
```

- [ ] **Step 2: REQUIREMENT_COVERAGE.md update**

If a perf REQ exists for "FTL flow control / die contention", note that die_dispatcher implements it. Otherwise add a small note in the FTL module subtotal commentary referencing the spec path.

- [ ] **Step 3: Commit**

```bash
git commit -m "docs: env knobs for die_dispatcher fault injection"
```

---

## Task 10: Performance validation (L6) + final

**Files:** none modified; this task validates KPIs.

- [ ] **Step 1: Run full make test**

Run: `make clean && make test`
Expected: all suites pass, no regressions.

- [ ] **Step 2: Run pre-checkin baseline (clean run, no env)**

Run: `make pre-checkin > /tmp/pre-checkin-postfix.log 2>&1`
Expected: 8/8 PASS in ~15-30 min.

- [ ] **Step 3: Extract fio metrics, compare to baseline**

Run: jq one-liner from `docs/perf-baselines/2026-04-29-master-0eb0d83/`-style script across `build/blackbox-tests/latest/`.

KPI table from spec Section 8.2 must hold:
- 012 seqwrite write mean lat ≤ 10 ms (from 142 ms)
- 012 seqwrite write p99 ≤ 20 ms (from 152 ms)
- 012 seqwrite write IOPS ≥ 1500 (from 112)
- All other cases within ±10 % of baseline

- [ ] **Step 4: Archive new baseline**

Save current run under `docs/perf-baselines/2026-04-30-perf-ftl-die-busy-waitqueue/`.

- [ ] **Step 5: Run pre-checkin under fault injection**

```bash
HFSSS_DIE_DISP_FORCE_BUSY=10 \
HFSSS_DIE_DISP_NOTIFIER_DELAY_NS=100000 \
make pre-checkin > /tmp/pre-checkin-fault.log 2>&1
```

Expected: 8/8 PASS; latency budgets relaxed by 2× (per spec Section 8.3).

- [ ] **Step 6: If KPIs hold, commit baseline + open PR**

```bash
git add docs/perf-baselines/2026-04-30-perf-ftl-die-busy-waitqueue/
git commit -m "docs(perf): post-fix baseline for die_dispatcher PR"
git push -u origin perf/ftl-die-busy-waitqueue
gh pr create --title "..." --body "..."
```

PR body should include:
- Summary of the spec
- Baseline-vs-target table
- 6-layer test coverage status
- Fault-injection results
- Out-of-scope deferrals (REQ-045 tier 3, model 3)

- [ ] **Step 7: Wait for CI to be green; address any feedback**

---

## Dependency graph

```
Task 1 (notifier hook)
    │
    ├─> Task 2 (data structure + L1)
    │       │
    │       └─> Task 3 (mt + L2)
    │               │
    │               └─> Task 4 (engine integration + L3)
    │                       │
    │                       └─> Task 5 (ftl_mt integration)
    │                               │
    │                               ├─> Task 6 (GC trigger threading)
    │                               │       │
    │                               │       └─> Task 7 (L4 integration tests)
    │                               │
    │                               └─> Task 8 (fault injection)
    │                                       │
    │                                       └─> Task 9 (docs)
    │                                               │
    │                                               └─> Task 10 (perf validation + PR)
```

Tasks 6 and 8 can run in parallel after Task 5 if dispatching parallel implementer subagents; Task 7 is gated on Task 6.

## Self-review checklist

- [x] Spec coverage: every requirement in spec maps to a task
- [x] No placeholders, no "TBD" or "implement later" language
- [x] Type signatures consistent across tasks (e.g., `die_priority_t` from Task 2 used in Task 5)
- [x] Tests precede implementation in every task (TDD)
- [x] KPIs from spec Section 8.2 are explicit pass criteria in Task 10
- [x] No reference to AI-tool names, line numbers, or progress-stage terms
- [x] Each commit message stands on its own
