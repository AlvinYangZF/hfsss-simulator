# REQ-045 Channel-Worker Completion Queue Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add per-command wall-clock timestamps + opt-in SPSC completion queue + non-blocking batch drain to the existing `channel_worker` runtime, without breaking any current caller.

**Architecture:** Augment `channel_worker` in place. Always-on additions (`submit_ts_ns`, `complete_ts_ns`, `owner` on `channel_cmd`) are unconditional. The completion queue is a second `spsc_ring` allocated only when `channel_worker_init` is called with `cq_capacity > 0`. Three-path mutual exclusion (poll-via-`channel_cmd_wait` / `on_complete` callback / CQ batch drain) is enforced at submit and wait time.

**Tech Stack:** C11, `<stdatomic.h>`, existing `spsc_ring` primitive (`include/common/spsc_ring.h`), `pthread`, project's `get_time_ns()` from `include/common/common.h`.

**Spec:** `docs/superpowers/specs/2026-04-24-req-045-completion-queue-design.md`

---

## File Structure

| File | Action | Purpose |
|---|---|---|
| `include/controller/channel_worker.h` | Modify | Add struct fields, change `channel_worker_init` signature, declare `channel_worker_drain`, update header doc to reflect three-path delivery model. |
| `src/controller/channel_worker.c` | Modify | Submit-side timestamping + `owner` set + path-conflict rejection; worker-side `complete_ts_ns` before publish + spin-retry CQ push when CQ enabled; `channel_worker_drain` implementation; init/cleanup updates. |
| `tests/test_channel_worker.c` | Modify | Update 8 existing `channel_worker_init` call sites to pass `0` for `cq_capacity`. Add 8 new test functions per spec §8.1. |
| `docs/REQUIREMENT_COVERAGE.md` | Modify | REQ-045 row ⚠️→✅ with scope-of-closure note; Media Threads / Core Subtotal / Grand Total bumps. |

No new files. The CQ stays inside the existing `channel_worker` translation unit because it shares lifetime with the worker; pulling it out would just create an indirection without separating responsibilities.

---

## Task 1: Branch off master and add API scaffolding (no behavioral change)

**Goal:** All API-level changes land first as a no-op extension. Build and full test suite stay green. Subsequent tasks only flip behavior, never break compilation.

**Files:**
- Create branch: `feat/req-045-completion-queue` off `origin/master`
- Modify: `include/controller/channel_worker.h`
- Modify: `src/controller/channel_worker.c`
- Modify: `tests/test_channel_worker.c` (call-site updates only)

- [ ] **Step 1.1: Create the working branch from latest master**

```bash
git fetch origin
git checkout master
git pull --ff-only
git checkout -b feat/req-045-completion-queue
```

- [ ] **Step 1.2: Extend `channel_cmd` struct in `include/controller/channel_worker.h`**

Find the existing `struct channel_cmd { ... };` block (currently ends with `_Atomic int done;`). Replace it with the version below. The diff adds three fields and a forward decl for the back-pointer.

```c
struct channel_worker; /* forward; full def appears later in this header */

struct channel_cmd {
    enum channel_cmd_op op;
    u32 ch;
    u32 chip;
    u32 die;
    u32 plane;
    u32 block;
    u32 page;
    /* READ: data_buf receives N bytes (page_size). PROGRAM: data_buf is
     * the source. ERASE: data_buf ignored. spare_buf is optional for
     * READ/PROGRAM paths that carry PI/OOB. */
    void *data_buf;
    void *spare_buf;

    /* Completion hook. Invoked exactly once by the worker thread after
     * the underlying media op returns, before done is published. NULL is
     * legal; pollers rely on the done flag instead. Must be NULL when
     * the owning worker has cq_capacity > 0; that path delivers via
     * channel_worker_drain instead. */
    channel_cmd_complete_fn on_complete;
    void *cb_ctx;

    /* Set by the worker thread. status is the int return from the
     * media_nand_* call; done transitions 0 -> 1 atomically and is
     * published with release ordering so a poller on another thread
     * can safely read status after seeing done == 1. */
    int status;
    _Atomic int done;

    /* REQ-045: wall-clock timestamps. submit_ts_ns is stamped by the
     * producer thread inside channel_worker_submit before the ring put.
     * complete_ts_ns is stamped by the worker thread immediately after
     * the underlying media_nand_* call returns and BEFORE done is
     * published. Both come from get_time_ns() (CLOCK_MONOTONIC).
     * Consumers compute actual_latency = complete_ts_ns - submit_ts_ns
     * and may safely read both fields after observing done == 1 with
     * acquire ordering — the release on done carries the prior stores.
     * Recorded unconditionally (cost: two clock_gettime calls per
     * submission), so callers using channel_cmd_wait or on_complete
     * see them too, not just CQ consumers. */
    u64 submit_ts_ns;
    u64 complete_ts_ns;

    /* REQ-045: back-pointer to the worker that owns this cmd. Set
     * exactly once by channel_worker_submit; read only by
     * channel_cmd_wait to enforce CQ-mode mutual exclusion. Never
     * dereferenced after completion; cmd lifetime does not depend on
     * worker lifetime once done is observed. */
    struct channel_worker *owner;
};
```

- [ ] **Step 1.3: Extend `channel_worker` struct in the same header**

Find the existing `struct channel_worker { ... };` block (currently ends with `_Atomic u64 completed;`). Replace it with the version below. The diff adds two fields.

```c
struct channel_worker {
    u32 channel_id;
    struct media_ctx *media;

    struct spsc_ring ring;       /* of struct channel_cmd * */
    pthread_t thread;
    bool thread_started;
    _Atomic int stop;

    /* Monotonic lifetime counters. submitted is advanced by the
     * producer after each successful tryput; completed is advanced by
     * the worker after each dispatch (regardless of media success or
     * failure). They are published independently so their delta is NOT
     * a reliable instantaneous in-flight depth — a fast consumer can
     * complete before the producer's submitted store lands, which
     * momentarily makes submitted - completed underflow. Use the
     * counters for aggregate rate monitoring, not for gauging
     * saturation. */
    _Atomic u64 submitted;
    _Atomic u64 completed;

    /* REQ-045: opt-in completion queue. cq_enabled is set by
     * channel_worker_init when cq_capacity > 0; cq is the second
     * SPSC ring whose payload is struct channel_cmd *. When enabled,
     * the worker spin-retries cq tryput after each completion (it
     * never drops a completion); when disabled, both fields stay
     * zero-initialised and the runtime behaves as before. */
    bool             cq_enabled;
    struct spsc_ring cq;
};
```

- [ ] **Step 1.4: Update `channel_worker_init` prototype + add `channel_worker_drain` prototype**

Replace the existing `channel_worker_init` declaration with the new 5-parameter form, and add the drain declaration immediately above the closing `#endif`.

```c
/*
 * Initialize a channel_worker. ring_capacity must be a power of two.
 * media must outlive the worker. cq_capacity == 0 disables the
 * completion queue and preserves the existing wait/callback delivery
 * model. cq_capacity > 0 must be a power of two, allocates a second
 * SPSC ring as the dedicated completion queue, and switches delivery
 * to channel_worker_drain (in this mode, on_complete on submitted
 * cmds and channel_cmd_wait calls are rejected — see the three-path
 * mutual exclusion contract in the file header). Returns HFSSS_OK on
 * success. On failure the worker is left in an unusable state and
 * cleanup is a no-op.
 */
int channel_worker_init(struct channel_worker *w,
                        u32 channel_id,
                        struct media_ctx *media,
                        u32 ring_capacity,
                        u32 cq_capacity);

/*
 * REQ-045: non-blocking batch drain of the completion queue.
 *
 *   buf     — caller-provided array of struct channel_cmd * slots.
 *   buf_cap — number of slots in buf. Must be > 0.
 *
 * Returns the number of commands popped (0..buf_cap). Returns 0
 * immediately when the CQ is empty; never blocks or yields. Returns
 * HFSSS_ERR_INVAL when cq_enabled is false (CQ disabled at init) or
 * when buf is NULL or buf_cap == 0.
 *
 * Ordering: pops happen in worker-enqueue order (SPSC FIFO). Media-
 * layer reordering (per EAT) is upstream of the CQ, so this FIFO does
 * not imply submit-time FIFO.
 *
 * Safety: exactly one thread may call this at a time (SPSC consumer).
 * Command ownership/reclamation is the caller's responsibility, same
 * as on the wait/callback paths.
 */
int channel_worker_drain(struct channel_worker *w,
                         struct channel_cmd **buf,
                         u32 buf_cap);
```

- [ ] **Step 1.5: Update the file-header doc block to describe the three-path delivery contract**

Find the comment block above `enum channel_cmd_op` and replace the "Completion model" paragraph (currently mentions only wait + on_complete) with this version:

```c
 * Completion model — callers pick EXACTLY ONE of these patterns per
 * worker, decided at channel_worker_init time:
 *   - cq_capacity == 0  (default / back-compat):
 *       * Poll with channel_cmd_wait: the waiter blocks until done == 1
 *         and reads status. Waiter owns reclamation of the cmd afterwards.
 *       * Install on_complete: the callback fires AFTER done is published
 *         and is the authoritative reclamation hook; the waiter path must
 *         not run concurrently with a reclaiming callback on the same cmd.
 *   - cq_capacity >  0  (REQ-045 batch-drain mode):
 *       * channel_worker_drain pops completed cmd pointers from the
 *         dedicated completion queue. on_complete on submitted cmds is
 *         rejected at submit time; channel_cmd_wait is rejected at wait
 *         time. The worker spin-retries CQ pushes when the CQ fills, so
 *         no completion is ever dropped — back-pressure flows through to
 *         the submit ring instead.
```

- [ ] **Step 1.6: Update `channel_worker_init` signature in `src/controller/channel_worker.c`**

Replace the existing implementation with this version. The body change is just argument validation + zero-init of the new fields; the actual CQ allocation arrives in Task 4.

```c
int channel_worker_init(struct channel_worker *w, u32 channel_id, struct media_ctx *media,
                        u32 ring_capacity, u32 cq_capacity)
{
    if (!w || !media || ring_capacity == 0) {
        return HFSSS_ERR_INVAL;
    }

    memset(w, 0, sizeof(*w));
    w->channel_id = channel_id;
    w->media = media;
    atomic_store(&w->stop, 0);
    atomic_store(&w->submitted, 0);
    atomic_store(&w->completed, 0);

    int rc = spsc_ring_init(&w->ring, (u32)sizeof(struct channel_cmd *), ring_capacity);
    if (rc != HFSSS_OK) {
        return rc;
    }

    /* REQ-045: opt-in completion queue. Allocated only when caller
     * asks for it. cq_capacity inherits the same power-of-two
     * requirement as ring_capacity; spsc_ring_init enforces it. */
    w->cq_enabled = false;
    if (cq_capacity > 0) {
        rc = spsc_ring_init(&w->cq, (u32)sizeof(struct channel_cmd *), cq_capacity);
        if (rc != HFSSS_OK) {
            spsc_ring_cleanup(&w->ring);
            return rc;
        }
        w->cq_enabled = true;
    }

    if (pthread_create(&w->thread, NULL, channel_worker_loop, w) != 0) {
        if (w->cq_enabled) {
            spsc_ring_cleanup(&w->cq);
        }
        spsc_ring_cleanup(&w->ring);
        return HFSSS_ERR_NOMEM;
    }
    w->thread_started = true;
    return HFSSS_OK;
}
```

- [ ] **Step 1.7: Update `channel_worker_cleanup` to release the CQ**

Replace the existing implementation:

```c
void channel_worker_cleanup(struct channel_worker *w)
{
    if (!w) {
        return;
    }
    if (w->thread_started) {
        atomic_store_explicit(&w->stop, 1, memory_order_release);
        pthread_join(w->thread, NULL);
        w->thread_started = false;
    }
    if (w->cq_enabled) {
        spsc_ring_cleanup(&w->cq);
        w->cq_enabled = false;
    }
    spsc_ring_cleanup(&w->ring);
    memset(w, 0, sizeof(*w));
}
```

- [ ] **Step 1.8: Add a stub `channel_worker_drain` at the end of `src/controller/channel_worker.c`**

This stub returns the right error codes but does not actually pop. Final implementation arrives in Task 4. Adding it now keeps the build green.

```c
int channel_worker_drain(struct channel_worker *w, struct channel_cmd **buf, u32 buf_cap)
{
    if (!w || !buf || buf_cap == 0) {
        return HFSSS_ERR_INVAL;
    }
    if (!w->cq_enabled) {
        return HFSSS_ERR_INVAL;
    }
    /* REQ-045 Task 4 lands the actual SPSC pops here. */
    return 0;
}
```

- [ ] **Step 1.9: Update all 8 existing `channel_worker_init` call sites in `tests/test_channel_worker.c`**

Each call gets a trailing `, 0` to disable the CQ (preserving today's behavior). The exact replacements:

| Line (approximate) | Before | After |
|---|---|---|
| 91 | `channel_worker_init(&w, 0, &media, 64);` | `channel_worker_init(&w, 0, &media, 64, 0);` |
| 141 | `channel_worker_init(&w, 0, &media, 32);` | `channel_worker_init(&w, 0, &media, 32, 0);` |
| 202 | `channel_worker_init(&w, 0, &media, 2);` | `channel_worker_init(&w, 0, &media, 2, 0);` |
| 273 | `channel_worker_init(&w, 0, &media, 16);` | `channel_worker_init(&w, 0, &media, 16, 0);` |
| 305 | `channel_worker_init(&w0, 0, &media, 8);` | `channel_worker_init(&w0, 0, &media, 8, 0);` |
| 338 | `channel_worker_init(&w0, 0, &media, 16) == HFSSS_OK` | `channel_worker_init(&w0, 0, &media, 16, 0) == HFSSS_OK` |
| 339 | `channel_worker_init(&w1, 1, &media, 16) == HFSSS_OK` | `channel_worker_init(&w1, 1, &media, 16, 0) == HFSSS_OK` |
| 373 | `channel_worker_init(NULL, 0, NULL, 16) == HFSSS_ERR_INVAL` | `channel_worker_init(NULL, 0, NULL, 16, 0) == HFSSS_ERR_INVAL` |

- [ ] **Step 1.10: Build and run the existing channel_worker tests**

Run:

```bash
make build/bin/test_channel_worker
./build/bin/test_channel_worker
```

Expected: build succeeds, test binary runs, `Tests failed: 0`.

- [ ] **Step 1.11: Run the full regression suite**

Run:

```bash
make test 2>&1 | grep -cE "\[FAIL\]"
```

Expected: prints `0`. Any non-zero count means a regression in this scaffolding step — diagnose and fix before continuing.

- [ ] **Step 1.12: Commit the scaffolding**

```bash
git add include/controller/channel_worker.h src/controller/channel_worker.c tests/test_channel_worker.c
git commit -m "refactor(channel_worker): add API scaffolding for REQ-045 (no behavior change)

Surface-only changes that prepare for the completion-queue work:
- channel_cmd grows submit_ts_ns / complete_ts_ns / owner fields
- channel_worker grows cq_enabled + cq fields (zero-init)
- channel_worker_init takes a new cq_capacity argument; all 8 existing
  call sites pass 0 to disable the CQ and preserve current behavior
- channel_worker_drain prototype + stub returning HFSSS_ERR_INVAL when
  cq is disabled (always, in this commit)

Behavior is byte-compatible with master when cq_capacity == 0, which
is the only path exercised here. Real CQ semantics land in subsequent
commits."
```

---

## Task 2: Write the new test cases (TDD red bar)

**Goal:** Add the 8 new test functions specified in spec §8.1 to `tests/test_channel_worker.c`. They will compile (Task 1 published the surface) and FAIL on the assertions that depend on un-implemented behavior. This is the TDD red bar.

**Files:**
- Modify: `tests/test_channel_worker.c`

- [ ] **Step 2.1: Add a small helper that programs and read-backs a page so each test has a deterministic media op**

Append after the existing `fill()` helper (around line 53). The helper is shared across the new test cases.

```c
/* Submit one ERASE on (ch=0,chip=0,die=0,plane=0,block=K) and return
 * the prepared cmd. Caller fills program/read forms inline. The block
 * id is a parameter so each test exercises different physical state
 * and avoids retry-on-already-erased-block surprises. */
static void prep_erase_cmd(struct channel_cmd *cmd, u32 ch, u32 block)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->op    = CHANNEL_CMD_ERASE;
    cmd->ch    = ch;
    cmd->chip  = 0;
    cmd->die   = 0;
    cmd->plane = 0;
    cmd->block = block;
}
```

- [ ] **Step 2.2: Add `test_cq_basic_drain`**

Append after the last existing test function (just before `main`). This proves the simplest CQ path: submit N, drain N, all timestamps populated.

```c
/* REQ-045: submit 4 cmds with CQ enabled, drain, assert order +
 * timestamps. Tests the hot-path delivery shape end to end. */
static int test_cq_basic_drain(void)
{
    printf("\n=== channel_worker: CQ basic drain ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    int rc = media_init(&media, &cfg);
    TEST_ASSERT(rc == HFSSS_OK, "media_init OK");

    struct channel_worker w;
    rc = channel_worker_init(&w, 0, &media, 16, 16);
    TEST_ASSERT(rc == HFSSS_OK, "init with cq_capacity=16 OK");

    struct channel_cmd cmds[4];
    for (int i = 0; i < 4; i++) {
        prep_erase_cmd(&cmds[i], 0, (u32)i);
        TEST_ASSERT(channel_worker_submit(&w, &cmds[i]) == HFSSS_OK,
                    "submit OK");
    }

    /* Spin-drain until we've collected all 4. The worker may interleave
     * dispatch + CQ push with our drain calls, so accumulate. */
    struct channel_cmd *out[8] = {0};
    int drained = 0;
    for (int spins = 0; spins < 100000 && drained < 4; spins++) {
        int n = channel_worker_drain(&w, &out[drained], (u32)(8 - drained));
        TEST_ASSERT(n >= 0, "drain returns non-negative");
        drained += n;
        if (n == 0) sched_yield();
    }
    TEST_ASSERT(drained == 4, "drained exactly 4 commands");

    /* SPSC FIFO: the order of CQ pops mirrors worker dispatch order,
     * which mirrors submit order in this single-producer test. */
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT(out[i] == &cmds[i],
                    "CQ pops in submit order (FIFO)");
        TEST_ASSERT(cmds[i].submit_ts_ns > 0,
                    "submit_ts_ns populated");
        TEST_ASSERT(cmds[i].complete_ts_ns >= cmds[i].submit_ts_ns,
                    "complete_ts_ns >= submit_ts_ns");
        TEST_ASSERT(atomic_load(&cmds[i].done) == 1,
                    "done flag set");
    }

    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}
```

- [ ] **Step 2.3: Add `test_cq_empty_drain`**

```c
/* REQ-045: drain on an idle CQ-enabled worker returns 0, not an error. */
static int test_cq_empty_drain(void)
{
    printf("\n=== channel_worker: CQ empty drain returns 0 ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    media_init(&media, &cfg);

    struct channel_worker w;
    int rc = channel_worker_init(&w, 0, &media, 16, 8);
    TEST_ASSERT(rc == HFSSS_OK, "init OK");

    struct channel_cmd *out[4] = {0};
    int n = channel_worker_drain(&w, out, 4);
    TEST_ASSERT(n == 0, "empty drain returns 0");

    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}
```

- [ ] **Step 2.4: Add `test_cq_batching_under_slow_consumer`**

```c
/* REQ-045: submit > cq_capacity commands while the consumer drains
 * slowly. No completion may be lost; total drained must equal total
 * submitted; per-cmd timestamps must be monotone. */
static int test_cq_batching_under_slow_consumer(void)
{
    printf("\n=== channel_worker: CQ batching with slow consumer ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    media_init(&media, &cfg);

    struct channel_worker w;
    /* CQ capacity 4 forces back-pressure since we'll submit 16. */
    int rc = channel_worker_init(&w, 0, &media, 16, 4);
    TEST_ASSERT(rc == HFSSS_OK, "init cq_capacity=4 OK");

    enum { N = 16 };
    struct channel_cmd cmds[N];
    for (int i = 0; i < N; i++) {
        prep_erase_cmd(&cmds[i], 0, (u32)i);
        /* Submit ring is 16; CQ is 4; worker stalls on CQ-full when it
         * tries to push past slot 3 until the test drains. We submit
         * eagerly and let the worker stall — that exercises the spin-
         * retry path. */
        TEST_ASSERT(channel_worker_submit(&w, &cmds[i]) == HFSSS_OK,
                    "submit OK");
    }

    /* Drain in small batches to keep the CQ alternating between full
     * and partial. Bound the total spin so a stuck worker fails the
     * test instead of hanging it. */
    struct channel_cmd *out[N] = {0};
    int drained = 0;
    for (int spins = 0; spins < 1000000 && drained < N; spins++) {
        struct channel_cmd *batch[2];
        int n = channel_worker_drain(&w, batch, 2);
        for (int i = 0; i < n; i++) {
            out[drained++] = batch[i];
        }
        if (n == 0) sched_yield();
    }
    TEST_ASSERT(drained == N,
                "drained == submitted under back-pressure");

    /* Per-cmd: each cmd timestamp pair is internally consistent. */
    int bad_pair = 0;
    for (int i = 0; i < N; i++) {
        if (cmds[i].submit_ts_ns == 0 ||
            cmds[i].complete_ts_ns < cmds[i].submit_ts_ns) {
            bad_pair++;
        }
    }
    TEST_ASSERT(bad_pair == 0,
                "every cmd has submit_ts_ns > 0 and complete_ts_ns >= submit_ts_ns");

    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}
```

- [ ] **Step 2.5: Add `test_cq_back_pressure_to_submit_ring`**

```c
/* REQ-045: with the consumer drained-out-never, the submit ring
 * eventually returns HFSSS_ERR_BUSY because the worker stalls
 * spin-retrying the full CQ. Proves back-pressure flows from CQ
 * full into submit-ring full. */
static int test_cq_back_pressure_to_submit_ring(void)
{
    printf("\n=== channel_worker: CQ back-pressure to submit ring ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    media_init(&media, &cfg);

    struct channel_worker w;
    int rc = channel_worker_init(&w, 0, &media, 4, 2);
    TEST_ASSERT(rc == HFSSS_OK, "init small ring + tiny cq OK");

    /* Submit until BUSY. Without back-pressure (or with silent drops)
     * we'd never see BUSY. The submit ring is 4; CQ is 2; on a normal
     * machine the worker fills the CQ within the first 2 ops, then
     * spins on CQ-full, then the producer fills the submit ring. */
    enum { CAP = 64 };
    struct channel_cmd cmds[CAP];
    int got_busy = 0;
    int submitted = 0;
    for (int i = 0; i < CAP; i++) {
        prep_erase_cmd(&cmds[i], 0, (u32)(i & 3)); /* recycle blocks */
        rc = channel_worker_submit(&w, &cmds[i]);
        if (rc == HFSSS_ERR_BUSY) { got_busy = 1; break; }
        TEST_ASSERT(rc == HFSSS_OK, "submit OK pre-BUSY");
        submitted++;
    }
    TEST_ASSERT(got_busy == 1, "submit ring eventually returns BUSY");

    /* Drain everything to release the worker so cleanup doesn't hang. */
    struct channel_cmd *out[CAP] = {0};
    int drained = 0;
    for (int spins = 0; spins < 1000000 && drained < submitted; spins++) {
        int n = channel_worker_drain(&w, out + drained, (u32)(CAP - drained));
        if (n > 0) drained += n;
        else sched_yield();
    }

    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}
```

- [ ] **Step 2.6: Add `test_cq_rejects_on_complete`**

```c
/* REQ-045: submitting a cmd with a non-NULL on_complete to a
 * CQ-enabled worker returns HFSSS_ERR_INVAL. */
static int test_cq_rejects_on_complete(void)
{
    printf("\n=== channel_worker: CQ rejects on_complete ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    media_init(&media, &cfg);

    struct channel_worker w;
    int rc = channel_worker_init(&w, 0, &media, 8, 4);
    TEST_ASSERT(rc == HFSSS_OK, "init cq=4 OK");

    struct sink s;
    atomic_store(&s.fired_count, 0);
    atomic_store(&s.last_status, 0);

    struct channel_cmd cmd;
    prep_erase_cmd(&cmd, 0, 0);
    cmd.on_complete = sink_cb;
    cmd.cb_ctx = &s;

    rc = channel_worker_submit(&w, &cmd);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL,
                "submit rejects on_complete in CQ mode");

    /* And the canary callback never fired. */
    TEST_ASSERT(atomic_load(&s.fired_count) == 0,
                "on_complete did not fire on rejected submit");

    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}
```

- [ ] **Step 2.7: Add `test_cq_rejects_wait`**

```c
/* REQ-045: channel_cmd_wait on a cmd whose owner is a CQ-enabled
 * worker returns HFSSS_ERR_INVAL. The cmd still completes through
 * the CQ; this just refuses the legacy poll path. */
static int test_cq_rejects_wait(void)
{
    printf("\n=== channel_worker: CQ rejects channel_cmd_wait ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    media_init(&media, &cfg);

    struct channel_worker w;
    int rc = channel_worker_init(&w, 0, &media, 8, 4);
    TEST_ASSERT(rc == HFSSS_OK, "init cq=4 OK");

    struct channel_cmd cmd;
    prep_erase_cmd(&cmd, 0, 0);
    rc = channel_worker_submit(&w, &cmd);
    TEST_ASSERT(rc == HFSSS_OK, "submit OK without on_complete");

    /* Refused immediately; not a busy-spin. */
    int wait_rc = channel_cmd_wait(&cmd);
    TEST_ASSERT(wait_rc == HFSSS_ERR_INVAL,
                "wait rejected for CQ-mode cmd");

    /* Drain so cleanup is clean. */
    struct channel_cmd *out[2] = {0};
    for (int spins = 0; spins < 100000; spins++) {
        if (channel_worker_drain(&w, out, 2) > 0) break;
        sched_yield();
    }

    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}
```

- [ ] **Step 2.8: Add `test_cq_disabled_drain_rejects`**

```c
/* REQ-045: drain on a worker initialised with cq_capacity=0 returns
 * HFSSS_ERR_INVAL. Confirms the legacy/back-compat path stays
 * unambiguously distinct. */
static int test_cq_disabled_drain_rejects(void)
{
    printf("\n=== channel_worker: drain on cq=0 worker rejects ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    media_init(&media, &cfg);

    struct channel_worker w;
    int rc = channel_worker_init(&w, 0, &media, 8, 0);
    TEST_ASSERT(rc == HFSSS_OK, "init cq=0 OK");

    struct channel_cmd *out[2] = {0};
    int n = channel_worker_drain(&w, out, 2);
    TEST_ASSERT(n == HFSSS_ERR_INVAL,
                "drain rejected when CQ disabled");

    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}
```

- [ ] **Step 2.9: Add `test_timestamps_present_on_legacy_path`**

```c
/* REQ-045: timestamps are recorded unconditionally — even on the
 * legacy wait/callback path, NOT only when CQ is enabled. */
static int test_timestamps_present_on_legacy_path(void)
{
    printf("\n=== channel_worker: timestamps on cq=0 wait path ===\n");

    struct media_config cfg = make_cfg();
    struct media_ctx media;
    media_init(&media, &cfg);

    struct channel_worker w;
    int rc = channel_worker_init(&w, 0, &media, 8, 0);
    TEST_ASSERT(rc == HFSSS_OK, "init cq=0 OK");

    struct channel_cmd cmd;
    prep_erase_cmd(&cmd, 0, 0);
    rc = channel_worker_submit(&w, &cmd);
    TEST_ASSERT(rc == HFSSS_OK, "submit OK");

    int wait_rc = channel_cmd_wait(&cmd);
    TEST_ASSERT(wait_rc == HFSSS_OK || wait_rc == 0,
                "wait completes (legacy path still works)");
    TEST_ASSERT(cmd.submit_ts_ns > 0,
                "submit_ts_ns set on legacy path");
    TEST_ASSERT(cmd.complete_ts_ns >= cmd.submit_ts_ns,
                "complete_ts_ns >= submit_ts_ns on legacy path");

    channel_worker_cleanup(&w);
    media_cleanup(&media);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}
```

- [ ] **Step 2.10: Wire the 8 new test functions into `main()`**

Find the existing `main()` near the end of `tests/test_channel_worker.c`. Add these calls in the listed order **immediately before** the final summary printf block (matches the existing pattern of one call per existing test).

```c
    test_cq_basic_drain();
    test_cq_empty_drain();
    test_cq_batching_under_slow_consumer();
    test_cq_back_pressure_to_submit_ring();
    test_cq_rejects_on_complete();
    test_cq_rejects_wait();
    test_cq_disabled_drain_rejects();
    test_timestamps_present_on_legacy_path();
```

- [ ] **Step 2.11: Add `<sched.h>` include if not already present**

Check the includes at the top of `tests/test_channel_worker.c`. The new tests call `sched_yield()`. If `<sched.h>` is missing, add `#include <sched.h>` after `#include <unistd.h>`.

- [ ] **Step 2.12: Build and run the new tests — TDD red bar**

Run:

```bash
make build/bin/test_channel_worker
./build/bin/test_channel_worker 2>&1 | tail -40
```

Expected: build succeeds. The 8 new tests **FAIL** because:
- `test_cq_basic_drain`: `drained == 4` fails (drain stub returns 0).
- `test_cq_batching_under_slow_consumer`: same reason.
- `test_cq_back_pressure_to_submit_ring`: never sees BUSY because the worker doesn't actually push into CQ → the producer sees the request ring drain forever.
- `test_cq_rejects_on_complete`: submit accepts on_complete because enforcement is not implemented.
- `test_cq_rejects_wait`: wait succeeds because owner-check is not implemented.
- `test_cq_disabled_drain_rejects`: this one passes (the stub rejects already).
- `test_cq_empty_drain`: passes (stub returns 0 which equals "empty").
- `test_timestamps_present_on_legacy_path`: fails because timestamps are not yet recorded.

Capture the failure list. Pre-existing tests must still pass.

- [ ] **Step 2.13: Commit the failing tests**

```bash
git add tests/test_channel_worker.c
git commit -m "test(channel_worker): REQ-045 CQ + timestamp assertions (red bar)

Eight new test cases per spec section 8.1:
- CQ basic drain / empty drain / batching under slow consumer
- CQ back-pressure flowing into submit ring (HFSSS_ERR_BUSY)
- Three-path mutual exclusion: rejects on_complete + rejects wait +
  drain rejects when CQ disabled
- Timestamps recorded unconditionally (also on the legacy wait path)

These tests fail against the current scaffolding (drain is a stub,
enforcement and timestamping are not yet implemented). The next
commit makes them green."
```

---

## Task 3: Implement always-on instrumentation (timestamps + owner + mutual exclusion)

**Goal:** Make every test pass that depends only on the unconditional bits — timestamping, owner setting, three-path enforcement at submit time and wait time. The CQ payload itself stays absent until Task 4.

**Files:**
- Modify: `src/controller/channel_worker.c`

- [ ] **Step 3.1: Update `complete_one` to stamp `complete_ts_ns` before publishing `done`**

Replace the existing `complete_one` (around line 32 of `src/controller/channel_worker.c`):

```c
static void complete_one(struct channel_worker *w, struct channel_cmd *cmd)
{
    /* REQ-045: stamp the completion timestamp BEFORE publishing done.
     * The release on done carries this store, so any reader that
     * observes done == 1 with acquire ordering also sees the final
     * complete_ts_ns. */
    cmd->complete_ts_ns = get_time_ns();

    /* Publish completion BEFORE invoking the user callback. This lets
     * a callback safely reclaim the command (including cb_ctx) without
     * racing the worker's subsequent publication of done. Waiters and
     * callbacks are therefore alternative completion paths: a caller
     * that both polls via channel_cmd_wait AND installs a reclaiming
     * callback is responsible for serialising them. */
    atomic_store_explicit(&cmd->done, 1, memory_order_release);
    atomic_fetch_add_explicit(&w->completed, 1, memory_order_relaxed);
    if (cmd->on_complete) {
        cmd->on_complete(cmd, cmd->cb_ctx);
    }
}
```

- [ ] **Step 3.2: Update `channel_worker_submit` for timestamping + owner + on_complete enforcement**

Replace the existing `channel_worker_submit` (around line 121):

```c
int channel_worker_submit(struct channel_worker *w, struct channel_cmd *cmd)
{
    if (!w || !cmd) {
        return HFSSS_ERR_INVAL;
    }

    /* REQ-045: three-path mutual exclusion. CQ-enabled worker refuses
     * cmds that carry an on_complete callback — the CQ is the
     * delivery channel in that mode and a callback would race with
     * a CQ consumer that pops the same cmd. Reject before any
     * mutation so the caller can flip the cmd back to a non-CQ worker
     * if needed. */
    if (w->cq_enabled && cmd->on_complete != NULL) {
        return HFSSS_ERR_INVAL;
    }

    /* Reject submits once stop has been signalled. The acquire load
     * pairs with the release store in channel_worker_stop; together
     * they close the window in which a late submit could land in the
     * ring just as the consumer is exiting. A caller that races submit
     * against stop must coordinate externally — this check only shrinks
     * the window, it does not eliminate a pathological TOCTOU. */
    if (atomic_load_explicit(&w->stop, memory_order_acquire)) {
        return HFSSS_ERR_BUSY;
    }

    atomic_store_explicit(&cmd->done, 0, memory_order_relaxed);
    cmd->status = 0;

    /* REQ-045: producer-side timestamp + owner back-pointer set BEFORE
     * the ring put. The worker thread does an acquire load on the ring
     * tail when it dequeues; that pairs with the ring's release on
     * head and carries these stores. The owner pointer is read by
     * channel_cmd_wait for the CQ-mode wait rejection check. */
    cmd->submit_ts_ns  = get_time_ns();
    cmd->complete_ts_ns = 0;
    cmd->owner = w;

    int rc = spsc_ring_tryput(&w->ring, &cmd);
    if (rc != HFSSS_OK) {
        return HFSSS_ERR_BUSY;
    }
    atomic_fetch_add_explicit(&w->submitted, 1, memory_order_relaxed);
    return HFSSS_OK;
}
```

- [ ] **Step 3.3: Update `channel_cmd_wait` for CQ-mode rejection**

Replace the existing `channel_cmd_wait` (around line 148):

```c
int channel_cmd_wait(struct channel_cmd *cmd)
{
    if (!cmd) {
        return HFSSS_ERR_INVAL;
    }
    /* REQ-045: refuse the legacy poll path when the cmd's owner is in
     * CQ mode. Reading owner is safe here because submit set it before
     * the ring put and we are by contract the only legitimate waiter.
     * The cmd still completes via the CQ — this only refuses the
     * caller's attempt to reach for the wrong reaping API. */
    struct channel_worker *owner = cmd->owner;
    if (owner != NULL && owner->cq_enabled) {
        return HFSSS_ERR_INVAL;
    }
    while (atomic_load_explicit(&cmd->done, memory_order_acquire) == 0) {
        sched_yield();
    }
    return cmd->status;
}
```

- [ ] **Step 3.4: Verify `get_time_ns()` is in scope**

`get_time_ns()` lives in `include/common/common.h`, which is already transitively included via `include/controller/channel_worker.h` → `include/common/common.h`. No new include needed in `src/controller/channel_worker.c`. If a future inline change breaks the chain, add `#include "common/common.h"` at the top of the source file.

- [ ] **Step 3.5: Build and run the test binary**

```bash
make build/bin/test_channel_worker
./build/bin/test_channel_worker 2>&1 | tail -50
```

Expected: 4 of the 8 new tests now pass:
- `test_cq_rejects_on_complete` — PASS (submit-time enforcement is in)
- `test_cq_rejects_wait` — PASS (wait-time enforcement is in)
- `test_timestamps_present_on_legacy_path` — PASS (unconditional stamping)
- `test_cq_disabled_drain_rejects` — already PASS

The other 4 still fail because the worker does not yet push to the CQ:
- `test_cq_basic_drain` — drained still 0
- `test_cq_empty_drain` — still PASS (stub returns 0; coincidentally correct)
- `test_cq_batching_under_slow_consumer` — drained still 0
- `test_cq_back_pressure_to_submit_ring` — never sees BUSY

The pre-existing 6 tests continue to pass.

- [ ] **Step 3.6: Commit the always-on instrumentation**

```bash
git add src/controller/channel_worker.c
git commit -m "feat(channel_worker): always-on REQ-045 timestamps + path enforcement

submit stamps submit_ts_ns + sets owner before the ring put;
worker stamps complete_ts_ns immediately before publishing done;
both timestamps land for every submission regardless of CQ mode.

Three-path mutual exclusion now rejects:
- on_complete cmds submitted to a CQ-enabled worker (submit time)
- channel_cmd_wait calls for cmds owned by a CQ-enabled worker
  (wait entry)

The CQ push itself still stubs out — drain returns 0 even when
items have been dispatched. Final commit makes the CQ payload live."
```

---

## Task 4: Implement the CQ payload (worker push + drain)

**Goal:** Worker spin-retries CQ pushes until success; drain pops up to `buf_cap` items in FIFO order. After this task all 8 new tests pass and the full regression stays clean.

**Files:**
- Modify: `src/controller/channel_worker.c`

- [ ] **Step 4.1: Update `complete_one` to push completed cmd into the CQ when enabled**

Replace `complete_one` again (after Task 3 it stamps complete_ts_ns; now it also pushes):

```c
static void complete_one(struct channel_worker *w, struct channel_cmd *cmd)
{
    /* REQ-045: stamp the completion timestamp BEFORE publishing done.
     * The release on done carries this store, so any reader that
     * observes done == 1 with acquire ordering also sees the final
     * complete_ts_ns. */
    cmd->complete_ts_ns = get_time_ns();

    /* REQ-045: when the worker has the CQ enabled, deliver the cmd
     * into the CQ. The push happens BEFORE the done store so a CQ
     * consumer popping the cmd never observes done == 0 — by the
     * time spsc_ring_tryput's release on cq.head is visible, the
     * preceding stores (timestamps + status) are visible too.
     *
     * CQ full is back-pressure, never a drop: spin-retry with
     * sched_yield until either the consumer makes room or stop has
     * been signaled. On stop we bail out without setting done; the
     * cmd stays in the request-ring drain semantics handled by the
     * worker loop and cleanup path (i.e. it's discarded along with
     * any other in-flight work, same as the legacy path).
     *
     * on_complete is rejected at submit-time when CQ is enabled, so
     * the legacy callback branch below is unreachable in CQ mode. */
    if (w->cq_enabled) {
        for (;;) {
            int rc = spsc_ring_tryput(&w->cq, &cmd);
            if (rc == HFSSS_OK) {
                break;
            }
            if (atomic_load_explicit(&w->stop, memory_order_acquire)) {
                /* Bail without publishing done — caller's cleanup
                 * discards in-flight work just like the request-ring
                 * stop path does. */
                return;
            }
            sched_yield();
        }
    }

    atomic_store_explicit(&cmd->done, 1, memory_order_release);
    atomic_fetch_add_explicit(&w->completed, 1, memory_order_relaxed);
    if (cmd->on_complete) {
        cmd->on_complete(cmd, cmd->cb_ctx);
    }
}
```

- [ ] **Step 4.2: Replace the `channel_worker_drain` stub with the live implementation**

Replace the stub at the bottom of `src/controller/channel_worker.c`:

```c
int channel_worker_drain(struct channel_worker *w, struct channel_cmd **buf, u32 buf_cap)
{
    if (!w || !buf || buf_cap == 0) {
        return HFSSS_ERR_INVAL;
    }
    if (!w->cq_enabled) {
        return HFSSS_ERR_INVAL;
    }

    u32 popped = 0;
    while (popped < buf_cap) {
        struct channel_cmd *next = NULL;
        int rc = spsc_ring_tryget(&w->cq, &next);
        if (rc != HFSSS_OK || next == NULL) {
            /* Ring empty (or transient race lost): return whatever we
             * already collected. Drain is non-blocking by contract. */
            break;
        }
        buf[popped++] = next;
    }
    return (int)popped;
}
```

- [ ] **Step 4.3: Build and run the channel_worker tests**

```bash
make build/bin/test_channel_worker
./build/bin/test_channel_worker 2>&1 | tail -50
```

Expected: every test passes (`Tests failed: 0` in the summary line). All 8 new tests + all pre-existing tests green.

If any test fails, diagnose:
- `test_cq_basic_drain`: check the FIFO order assertion — if pops arrive in unexpected order, the SPSC ring is being violated; check that submit ring and CQ are not aliased.
- `test_cq_back_pressure_to_submit_ring`: if BUSY never appears, the loop bound or ring sizes need tuning, or worker is dropping CQ pushes — re-read Step 4.1.
- `test_cq_batching_under_slow_consumer`: if `drained != N`, a completion is being dropped — check the spin-retry loop for an early break.

- [ ] **Step 4.4: Run the full regression suite**

```bash
make test 2>&1 | grep -cE "\[FAIL\]"
```

Expected: prints `0`.

- [ ] **Step 4.5: Commit the live CQ implementation**

```bash
git add src/controller/channel_worker.c
git commit -m "feat(channel_worker): live REQ-045 completion queue + batch drain

complete_one pushes the cmd pointer into the CQ before publishing
done when CQ is enabled, spin-retrying with sched_yield on CQ-full
so back-pressure flows through to the submit ring instead of
silently dropping completions. Bails out without setting done if
stop has been signalled — same in-flight discard semantics as the
legacy path.

channel_worker_drain pops up to buf_cap cmd pointers in worker-
enqueue (FIFO) order, returning the count actually filled and
HFSSS_ERR_INVAL only on bad args or CQ-disabled worker.

All 8 new test cases now pass; full regression remains clean."
```

---

## Task 5: Documentation, regression sweep, push, PR

**Goal:** Flip REQ-045 to ✅ in the coverage doc with the scope-of-closure note, run the full sanity sweep, push, and open the PR.

**Files:**
- Modify: `docs/REQUIREMENT_COVERAGE.md`

- [ ] **Step 5.1: Replace the REQ-045 row note**

Find the existing REQ-045 row in `docs/REQUIREMENT_COVERAGE.md` (Media Threads detail table). Replace just that one row with:

```markdown
| REQ-045 | NAND Media Command Execution Engine - Completion Notification | ✅ | `channel_worker` (REQ-044) now provides per-command wall-clock timestamps (`submit_ts_ns` / `complete_ts_ns` set unconditionally on every submission) and an opt-in lock-free completion queue selectable at `channel_worker_init` via the new `cq_capacity` argument. Batch drain via non-blocking `channel_worker_drain`. Three-path mutual exclusion (legacy `channel_cmd_wait` poll, `on_complete` callback, CQ batch drain) is enforced at submit and wait time so a single command is never delivered through more than one path. CQ-full applies back-pressure (worker spin-retries with `sched_yield`) so no completion is ever dropped. Validation: `tests/test_channel_worker.c` adds 8 cases covering basic drain, empty drain, batching under a slow consumer, back-pressure into the submit ring, three-path enforcement, and unconditional timestamp recording. Scope of closure: REQ-045 is read as "channel_worker exposes a lock-free completion queue with per-command timestamp / actual latency / batch drain"; closure covers the mechanism + validation harness. Follow-up (not gated on this REQ): tier-2 controlled CQ benchmark harness; tier-3 retarget FTL/HAL hot path through `channel_worker` so production NBD/fio I/O flows through the CQ — both are tracked in `docs/superpowers/specs/2026-04-24-req-045-completion-queue-design.md` Section 9 |
```

- [ ] **Step 5.2: Update the Media Threads subtotal row**

Find the `| Media Threads |` line in the Summary by Module table near the top of the file. Replace its counts. The current state after REQ-048 was 18 ✅ / 2 ⚠️ / 0 ❌ (90.0%). REQ-045 was one of the remaining ⚠️ items; flipping it gives 19 ✅ / 1 ⚠️ / 0 ❌ = 95.0%.

```markdown
| Media Threads | 20 | 19 | 1 | 0 | 0 | 95.0% | ↑ REQ-042 multi-plane concurrency + REQ-044 per-channel worker runtime; REQ-045 now ✅ via channel_worker timestamps + opt-in lock-free completion queue + batch drain (`channel_worker_drain`) per `docs/superpowers/specs/2026-04-24-req-045-completion-queue-design.md`; REQ-048 retention-age-driven RBER validated via `tests/test_retention.c` |
```

- [ ] **Step 5.3: Update Core Subtotal and Grand Total rows**

REQ-045 is in the core (Media Threads) section, not the enterprise section. Current state from REQ-137: Core 114 ✅ / 8 ⚠️ / 16 ❌ → flipping REQ-045 gives 115 ✅ / 7 ⚠️ / 16 ❌. Grand Total 154 ✅ / 8 ⚠️ / 16 ❌ → 155 ✅ / 7 ⚠️ / 16 ❌.

Find the Core Subtotal row and replace:

```markdown
| **Core Subtotal** | **138** | **115** | **7** | **16** | **0** | **83.3%** (88.4% partial) | ↑ from 50.0% |
```

Find the Grand Total row and replace:

```markdown
| **Grand Total** | **178** | **155** | **7** | **16** | **0** | **87.1%** (91.0% partial) | ↑ from 38.8% |
```

- [ ] **Step 5.4: Run the full regression suite one more time**

```bash
make test 2>&1 > /tmp/req045_regression.log
echo "EXIT=$?"
grep -cE "\[FAIL\]" /tmp/req045_regression.log
grep -cE "\[PASS\]" /tmp/req045_regression.log
```

Expected: `EXIT=0`, FAIL count `0`, PASS count is the previous total + 8 (channel_worker now reports 8 more passes).

- [ ] **Step 5.5: Commit the doc update**

```bash
git add docs/REQUIREMENT_COVERAGE.md
git commit -m "docs(coverage): flip REQ-045 to implemented (channel_worker CQ)

Media Threads 18/2/0 -> 19/1/0 (90.0% -> 95.0%).
Core Subtotal 114/8/16 -> 115/7/16 (82.6% -> 83.3%).
Grand Total 154/8/16 -> 155/7/16 (86.5% -> 87.1%).

Scope-of-closure note matches the REQ-121 / 048 / 137 convention:
mechanism + validation harness shipped; tier-2 benchmark harness
and tier-3 FTL/HAL hot-path retarget remain explicit follow-ups in
docs/superpowers/specs/2026-04-24-req-045-completion-queue-design.md."
```

- [ ] **Step 5.6: Push the branch**

```bash
git push -u origin feat/req-045-completion-queue
```

- [ ] **Step 5.7: Open the PR**

```bash
gh pr create --title "feat(channel_worker): per-cmd timestamps + opt-in completion queue (REQ-045)" --body "$(cat <<'EOF'
## Summary
Close REQ-045 (Media Threads) by extending the existing \`channel_worker\` runtime with:
- per-command wall-clock timestamps (\`submit_ts_ns\` / \`complete_ts_ns\`) recorded unconditionally
- an opt-in lock-free SPSC completion queue selectable at \`channel_worker_init\` (\`cq_capacity\` arg, \`0\` preserves current behavior)
- non-blocking batch drain via \`channel_worker_drain\`
- three-path mutual exclusion (legacy \`channel_cmd_wait\` poll, \`on_complete\` callback, CQ batch drain) enforced at submit / wait time

CQ-full applies back-pressure (worker spin-retries with \`sched_yield\`) so no completion is ever dropped — back-pressure flows from CQ into the submit ring instead. Scope follows the REQ-121 / REQ-048 / REQ-137 convention: mechanism + harness shipped; tier-2 controlled CQ benchmark and tier-3 retarget of the FTL/HAL hot path through \`channel_worker\` are explicit follow-ups (see spec Section 9).

## Changes
- \`include/controller/channel_worker.h\` — \`channel_cmd\` gets \`submit_ts_ns\` / \`complete_ts_ns\` / \`owner\`; \`channel_worker\` gets \`cq_enabled\` / \`cq\`; \`channel_worker_init\` takes a new \`cq_capacity\` arg; \`channel_worker_drain\` declared.
- \`src/controller/channel_worker.c\` — submit-side timestamp + owner + on_complete-vs-CQ rejection; \`channel_cmd_wait\` rejects in CQ mode via owner back-pointer; worker stamps \`complete_ts_ns\` and spin-retries CQ push before publishing \`done\`; live \`channel_worker_drain\`.
- \`tests/test_channel_worker.c\` — 8 existing call sites pass \`0\` for new arg; 8 new test cases cover all three paths.
- \`docs/REQUIREMENT_COVERAGE.md\` — REQ-045 ⚠️→✅; Media Threads 90.0%→95.0%; Core Subtotal 82.6%→83.3%; Grand Total 86.5%→87.1%.

## Scope boundary
- Mechanism + tests: shipped.
- Tier-2 controlled CQ benchmark harness: follow-up (spec Section 9.1).
- Tier-3 retarget FTL/HAL hot path: follow-up (spec Section 9.2).
- Blocking drain / modeled-EAT latency / per-channel CQ stats: explicitly deferred (spec Section 3.2).

## Test plan
- [x] \`make test\` — full regression 0 [FAIL]; 8 new channel_worker test cases pass
- [x] \`cq_capacity=0\` path is binary-compatible with current behavior; existing tests unchanged in semantics
- [x] CQ basic drain pops in submit order; timestamps populated on every cmd
- [x] CQ-full produces back-pressure into submit ring (HFSSS_ERR_BUSY); no dropped completions
- [x] Three-path enforcement: rejecting on_complete at submit, rejecting wait at entry, rejecting drain on cq=0 worker
- [ ] Codex review pass (queued for user to trigger)
EOF
)"
```

- [ ] **Step 5.8: Note the PR URL**

The previous step prints the PR URL. Record it for the user-facing summary.

---

## Self-Review

### Spec Coverage

Walking spec section by section:

- **§1 Summary** → covered by Tasks 1-4 collectively.
- **§2 Current State** → context only, nothing to implement.
- **§3 Goals/Non-Goals** → goals: timestamps (Task 3), CQ ring (Tasks 1+4), drain (Task 1+4), three-path enforcement (Task 3), binary-compatible cq=0 (Task 1). Non-goals: blocking drain / modeled latency / stats / production retarget — none appear in any task. ✅
- **§4 Data Structure Changes** → channel_cmd timestamps (Task 1.2 + Task 3.2), owner (Task 1.2 + Task 3.2), channel_worker cq fields (Task 1.3 + Task 1.6), init signature (Task 1.4 + Task 1.6). ✅
- **§5 API Surface** → channel_worker_drain signature (Task 1.4 stub + Task 4.2 live); enforcement table (Task 3.2 + Task 3.3 + Task 4.2). ✅
- **§6 Execution Flow** → submit (Task 3.2), worker (Task 4.1), drain (Task 4.2). ✅
- **§7 CQ-Full Back-Pressure Policy** → implemented in Task 4.1's spin-retry loop. ✅
- **§8 Validation Strategy** → 8 test cases in Task 2 cover §8.1; regression in Task 5.4 covers §8.2; Codex review listed in Task 5.7's PR body covers §8.3. ✅
- **§9 Follow-Up Roadmap** → not implemented in this plan by design. Documented in PR body and coverage note (Tasks 5.1, 5.7). ✅
- **§10 Risks** → Risk 2 (`owner` lifetime): handled by the doc comments in Task 1.2 and the design of Task 3.3 (only read, never deref-on-stale). Risk 3 (ordering): timestamps written before `done` release — Task 3.1, Task 4.1. Risk 4 (memory): CQ opt-in only — Task 1.6. ✅
- **§11 Open Decisions Resolved** → all decisions reflected in task-level steps. ✅

No gaps.

### Placeholder Scan

- No "TBD" / "TODO" / "fill in" / "similar to Task N" / "appropriate error handling" patterns in any task body.
- All code blocks contain executable, complete content.
- All `git commit -m` messages use full HEREDOC and end with concrete intent.
- Step 1.9 references "approximate" line numbers — that's honest about the imperfect line-number stability across revisions, not a placeholder. The before/after exact-substring substitution is unambiguous.

### Type / Identifier Consistency

- `channel_worker_init` signature is identical across header (Task 1.4) and impl (Task 1.6) and test call sites (Task 1.9): `(struct channel_worker *, u32, struct media_ctx *, u32, u32)`.
- `channel_worker_drain` signature identical across header (Task 1.4), stub (Task 1.8), and live impl (Task 4.2): `(struct channel_worker *, struct channel_cmd **, u32) -> int`.
- New `channel_cmd` fields (`submit_ts_ns`, `complete_ts_ns`, `owner`) referenced in Tasks 1.2, 3.1, 3.2, 3.3 with the same names and types throughout.
- New `channel_worker` fields (`cq_enabled`, `cq`) referenced in Tasks 1.3, 1.6, 1.7, 3.2, 3.3, 4.1, 4.2 with the same names and types throughout.
- Coverage doc subtotal arithmetic checked: Media Threads 18→19, Core 114→115, Grand 154→155, all single-flip math correct.

No inconsistencies.
