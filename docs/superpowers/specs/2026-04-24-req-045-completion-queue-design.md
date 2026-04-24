# REQ-045 — Channel-Worker Completion Queue Design

**Date:** 2026-04-24
**Repo baseline:** `origin/master` at `da1672a`
**Scope:** `include/controller/channel_worker.h` + `src/controller/channel_worker.c` + `tests/test_channel_worker.c`
**Closure model:** Mechanism + validation harness (tier 1 of a three-tier roadmap; tiers 2-3 are follow-ups)

---

## 1. Summary

Close the REQ-045 gap documented in `docs/REQUIREMENT_COVERAGE.md`:

> Lock-free completion queue with per-command timestamp / actual latency / batch drain (the PRD wording) is a follow-up

The implementation adds, to the existing `channel_worker` runtime:

- Per-command wall-clock timestamps (`submit_ts_ns`, `complete_ts_ns`) recorded unconditionally on every submission.
- An **opt-in** SPSC completion queue (second ring) selectable at `channel_worker_init` time; worker pushes completed `channel_cmd *` into the CQ once the existing `done` flag is published.
- A non-blocking batch drain API `channel_worker_drain`.
- Strict three-path mutual exclusion: a command is delivered through **exactly one** of `channel_cmd_wait` (poll), `on_complete` (callback), or the CQ (batch drain). The chosen path is determined by the `cq_capacity` init argument and enforced at construction / submission time.

The delivery mechanism is scoped to the `channel_worker` boundary. The production I/O path (NBD → NVMe → controller → FTL → HAL → `media_nand_*`) does **not** flow through `channel_worker` today and is **not** retargeted by this work. End-to-end fio/NBD latency observability through the CQ is a tier-3 follow-up (Section 9).

---

## 2. Current State

### 2.1 What exists

- `channel_worker` (`include/controller/channel_worker.h`, `src/controller/channel_worker.c`) — one pthread per channel, SPSC request ring of `channel_cmd *`, worker thread dispatches through the synchronous `media_nand_*` API, publishes a release-ordered `_Atomic int done` flag, optionally fires `on_complete` callback.
- Lifetime counters `submitted` / `completed` (`_Atomic u64`) for aggregate rate monitoring.
- Two completion patterns, mutually exclusive: `channel_cmd_wait` (spin with `sched_yield`) or `on_complete` callback.
- Unit tests in `tests/test_channel_worker.c` cover single/multi-cmd submit, back-pressure, channel-mismatch rejection, and both completion paths.

### 2.2 What is missing (per REQUIREMENT_COVERAGE REQ-045 note)

- No per-command timestamps → consumers cannot compute actual latency without bolting their own `clock_gettime` calls around submit/wait.
- No dedicated completion queue → a caller that submits N commands and wants to reap them as they finish must track an in-flight array and poll `done` flags or rely on callbacks that run on the worker thread.
- No batch drain → the natural "give me up to K completed commands" shape is unavailable.

### 2.3 Why not instrument the existing done-flag path alone

A scan-based "poll this array of in-flight cmds and return the done ones" API is possible without a new ring, but:

- The PRD wording is explicitly "lock-free completion queue". Scope-of-closure honesty requires we ship a queue, not a scan helper.
- The scan shape forces every caller to maintain an in-flight array sized to their peak concurrency; the CQ shape decouples caller bookkeeping from the worker's delivery order.

---

## 3. Goals and Non-Goals

### 3.1 Goals

- Record `submit_ts_ns` and `complete_ts_ns` on every submitted command, regardless of CQ opt-in, so any consumer can derive wall-clock latency.
- Provide a **lock-free, SPSC** completion queue that any channel_worker caller can opt into at init time.
- Expose a **non-blocking** batch drain API.
- Enforce three-path mutual exclusion so the same command is never delivered twice or raced across paths.
- Preserve binary-level behavioral compatibility when `cq_capacity == 0`.

### 3.2 Non-Goals

- Blocking drain / `drain_wait` variants (left as a seam; current API trivially composes into a spin loop if a caller needs it).
- Modeled-EAT latency in the CQ entry (the channel_worker sits at the submit/complete boundary; modeled time belongs to `media/cmd_state`, where it is already recorded).
- Per-channel CQ stats (dropped / peak-depth / drained-rate). No stats are exposed beyond what the existing `submitted`/`completed` counters already provide. A future trace consumer may add these.
- Any production IO path retarget. Specifically: FTL / HAL / NBD continue to call `media_nand_*` synchronously. Tier-3 follow-up (Section 9) covers that work.
- Benchmark harness that exercises the CQ under realistic load. Tier-2 follow-up covers that work.

---

## 4. Data Structure Changes

### 4.1 `struct channel_cmd` additions

```c
struct channel_cmd {
    /* ... existing fields unchanged ... */

    /* REQ-045: wall-clock timestamps recorded by the worker thread.
     * submit_ts_ns is stamped at channel_worker_submit entry (on the
     * producer thread, before the ring put). complete_ts_ns is
     * stamped after media_nand_* returns, before 'done' is published.
     * Both come from get_time_ns() (CLOCK_MONOTONIC); consumers
     * compute actual_latency = complete_ts_ns - submit_ts_ns.
     *
     * Both fields are written exactly once per submission and are
     * safe to read on any thread after observing done == 1 (the
     * release/acquire edge on 'done' carries the timestamps). */
    u64 submit_ts_ns;
    u64 complete_ts_ns;

    /* REQ-045: back-pointer to the worker this cmd was submitted to.
     * Set once by channel_worker_submit under its own thread; read
     * only by channel_cmd_wait to enforce CQ-mode mutual exclusion
     * (see Section 5.2). Never dereferenced after completion; cmd
     * lifetime does not depend on worker lifetime after 'done'. */
    struct channel_worker *owner;
};
```

- Timestamps are **always** recorded. Cost is two `clock_gettime` calls per submission — negligible vs the media op itself.
- No alignment padding required; placed at the end of the struct for ABI stability relative to the existing layout.

### 4.2 `struct channel_worker` additions

```c
struct channel_worker {
    /* ... existing fields unchanged ... */

    /* REQ-045: opt-in completion ring. Allocated only when
     * channel_worker_init is called with cq_capacity > 0. Empty
     * when cq_enabled == false; in that case the worker's completion
     * path is unchanged (done-flag publish + optional callback). */
    bool             cq_enabled;
    struct spsc_ring cq;   /* payload: struct channel_cmd * */
};
```

### 4.3 `channel_worker_init` signature change

```c
/* cq_capacity: 0 disables the completion queue (back-compat with the
 * existing behavior); > 0 must be a power of two and enables the CQ.
 * Callers that enable the CQ MUST NOT set on_complete on any
 * submitted cmd, and MUST NOT call channel_cmd_wait on any cmd
 * submitted to this worker — both restrictions are enforced at
 * submit/wait time and return HFSSS_ERR_INVAL. */
int channel_worker_init(struct channel_worker *w,
                        u32 channel_id,
                        struct media_ctx *media,
                        u32 ring_capacity,
                        u32 cq_capacity);
```

Test callers update to pass `0` for the new parameter; existing behavior preserved.

---

## 5. API Surface

### 5.1 New function

```c
/*
 * Non-blocking batch drain of the completion queue.
 *
 *   buf     — caller-provided array of struct channel_cmd * slots.
 *   buf_cap — number of slots in buf. Must be > 0.
 *
 * Returns the number of commands popped (0..buf_cap). Returns 0
 * immediately when the CQ is empty; never blocks or yields. Returns
 * HFSSS_ERR_INVAL when cq_capacity was 0 at init (CQ disabled) or
 * when buf is NULL / buf_cap == 0.
 *
 * Ordering: the pops happen in worker-enqueue order (SPSC FIFO).
 * Media-layer reordering (per EAT) is upstream of the CQ, so this
 * FIFO does not imply submit-time FIFO.
 *
 * Safety: exactly one thread may call this at a time (SPSC consumer).
 * Any number of threads may hold stale pointers in their buf AFTER
 * drain returns; the command ownership/reclamation contract is
 * unchanged from the existing runtime.
 */
int channel_worker_drain(struct channel_worker *w,
                         struct channel_cmd **buf,
                         u32 buf_cap);
```

### 5.2 Enforcement paths

| Path | When legal | Enforcement site |
|---|---|---|
| `channel_cmd_wait(cmd)` | `cq_enabled == false` | `channel_cmd_wait` returns `HFSSS_ERR_INVAL` when the owning worker has `cq_enabled == true`. (The cmd does not carry a back-pointer to its worker today; we add one: `_Atomic struct channel_worker *owner` set by `submit`.) |
| `cmd->on_complete` callback | `cq_enabled == false` | `channel_worker_submit` rejects the cmd with `HFSSS_ERR_INVAL` when `cq_enabled && cmd->on_complete != NULL`. |
| CQ batch drain | `cq_enabled == true` | `channel_worker_drain` returns `HFSSS_ERR_INVAL` when `cq_enabled == false`. |

---

## 6. Execution Flow

### 6.1 Submit path (producer thread)

1. Producer calls `channel_worker_submit(w, cmd)`.
2. `submit` validates: worker not stopping, CQ compatibility (`on_complete == NULL` when `cq_enabled`), channel match.
3. `submit` stamps `cmd->submit_ts_ns = get_time_ns()` before the ring put. Rationale: submit-side stamp captures the producer's notion of "I released this"; any queuing delay up to the worker is part of latency.
4. `submit` tryputs the cmd pointer on the request ring. On ring-full, `HFSSS_ERR_BUSY`.
5. `submit` increments `submitted` counter (relaxed).

### 6.2 Worker path (worker thread)

1. Worker pops cmd from request ring.
2. Worker validates the channel id and dispatches through the synchronous `media_nand_*` API as today.
3. Worker stamps `cmd->complete_ts_ns = get_time_ns()` **before** publishing `done` — so any reader observing `done == 1` with acquire ordering sees the final timestamps.
4. If `cq_enabled`:
   - Worker spin-retries `spsc_ring_tryput(&w->cq, cmd)` with `sched_yield` between attempts, until success or `stop` is signaled. CQ full is a back-pressure event, **not** a drop. See Section 7.
   - Worker publishes `cmd->done = 1` (release).
   - No `on_complete` path (guaranteed NULL by submit-time enforcement).
5. If `!cq_enabled`: worker fires `on_complete` (if set), then publishes `done = 1` (release). Unchanged from today.
6. Worker increments `completed` counter (relaxed).

### 6.3 Drain path (consumer thread)

1. Consumer calls `channel_worker_drain(w, buf, buf_cap)`.
2. Drain validates `cq_enabled` and args.
3. Drain loops `spsc_ring_trygeta(&w->cq, &buf[i])` up to `buf_cap` times or until empty.
4. Returns count actually filled.

---

## 7. CQ-Full Back-Pressure Policy

When the CQ fills and the consumer lags, the worker **spins** on `sched_yield` until room opens. No completed command is ever dropped.

Rationale:

- Dropping would lose the cmd pointer and silently leak it in the caller's bookkeeping.
- Blocking the submit ring indirectly (worker stops pulling from request ring while it waits to push to CQ) mirrors the SPSC submit-ring semantics that already return `HFSSS_ERR_BUSY` on saturation.
- The caller sizes `cq_capacity` at init for their expected in-flight depth. Misconfiguration manifests as channel stall, which is visible and diagnosable, rather than as silent corruption.

During `channel_worker_stop` / `_cleanup`, the worker exits the retry loop on `atomic_load(&w->stop)` and leaves any unpushed cmd in the request ring (same shutdown semantics as today).

---

## 8. Validation Strategy

### 8.1 New test cases (added to `tests/test_channel_worker.c`)

- **CQ basic drain** — submit N commands, assert `drain` returns them in order, each with both timestamps set and `complete_ts_ns >= submit_ts_ns`.
- **CQ empty drain** — call `drain` on an idle worker; assert returns 0 (not an error).
- **CQ batching** — submit > `cq_capacity` commands with a slow consumer; assert no command is lost, drained count equals submitted count, timestamps are monotone per cmd.
- **CQ back-pressure** — submit aggressively without draining, assert submit ring eventually returns `HFSSS_ERR_BUSY` (i.e., back-pressure flows through from CQ to submit ring).
- **CQ mutual exclusion** — CQ-enabled worker + cmd with non-NULL `on_complete` → submit returns `HFSSS_ERR_INVAL`.
- **CQ mutual exclusion (wait)** — CQ-enabled worker → `channel_cmd_wait` returns `HFSSS_ERR_INVAL`.
- **Disabled CQ → drain rejects** — `cq_capacity == 0` worker → `drain` returns `HFSSS_ERR_INVAL`.
- **Timestamps always present** — `cq_capacity == 0` worker + `channel_cmd_wait` path → observed cmd still has both timestamps populated.

### 8.2 Regression guarantees

- The `cq_capacity == 0` path is byte-compatible with current behavior: all existing tests in `test_channel_worker.c` pass unchanged except for the one-arg addition in `channel_worker_init` call sites.
- Full `make test` regression runs clean (0 [FAIL]).

### 8.3 Codex review

A single rigorous review pass via `codex:adversarial-review` after the PR is opened, matching the REQ-121 / 048 / 137 cadence.

---

## 9. Follow-Up Roadmap (Explicit Tier-2 and Tier-3 Work)

**This is the "full information" record requested separately from the PR scope.** Neither tier is included in the REQ-045 PR; both are called out here so they are not forgotten.

### 9.1 Tier 2 — Controlled CQ benchmark harness

**Goal:** Prove the CQ delivers observable batch-drain latency benefits under realistic load in a *controlled* harness (not the production IO path).

**Deliverables:**

- A new test binary, provisionally `tests/bench_channel_worker_cq.c`, that:
  - Boots a small `media_ctx`.
  - Spawns one `channel_worker` per channel with a CQ enabled.
  - Runs a producer thread submitting mixed read/program ops at target rate.
  - Runs a consumer thread draining the CQ and computing p50 / p99 / p99.9 of `complete_ts_ns - submit_ts_ns`.
  - Emits a stable key=value summary for CI consumption, mirroring `STRESS_RESULTS_FILE` shape.
- New Makefile target `make bench-cq` (not wired into default `make test`).

**Estimated effort:** 1 day.

**Out of scope for tier 2:** FTL/NBD routing; still standalone.

### 9.2 Tier 3 — Retarget FTL / HAL hot path onto `channel_worker`

**Goal:** Route production IO through `channel_worker_submit` so that fio / NBD traffic actually flows through the CQ and produces real E2E latency numbers.

**Deliverables:**

- Identify the current `media_nand_*` call sites in FTL and HAL hot paths (plausibly in `src/ftl/gc.c`, `src/hal/hal_nand.c`, and the NBD path in `src/vhost/hfsss_nbd_server.c`).
- Introduce a per-channel `channel_worker` in the controller / media init, sized from the existing NAND geometry.
- Switch hot-path producers to `channel_worker_submit` + CQ drain loop.
- Preserve the synchronous `media_nand_*` entry points for callers that must remain synchronous (tests, stress harnesses, compatibility paths).
- Add a fio-visible latency export (SMART log page, `/proc` file, or exporter metric) derived from the CQ timestamps.
- Full soak + performance regression: `make stress-burn-in`, `make systest`, NBD/QEMU blackbox suite.

**Estimated effort:** 1 week minimum, with dedicated performance regression cycles. Warrants its own design doc.

**Out of scope for tier 3:** NVMe kernel-module path (Phase 7); schema changes to profile tables; modeled-EAT latency export (separate follow-up).

### 9.3 Gating order

Tier 2 is independent of tier 3 and can land anytime after tier 1 (this PR). Tier 3 should land after tier 2 because tier 2's benchmark harness is the right vehicle to catch a regression when tier 3 flips real IO onto the CQ.

---

## 10. Risks and Mitigations

### Risk 1 — CQ-full back-pressure masks a buggy consumer as a "slow channel"

Mitigation: bench harness (tier 2) should include an explicit "drain-never" scenario that demonstrates the submit ring returns `HFSSS_ERR_BUSY` and does not crash or leak. Exit clean in tier-1 test coverage.

### Risk 2 — `channel_cmd.owner` back-pointer introduces a cmd-level lifetime dependency on the worker

Mitigation: `owner` is written once by `submit` under the worker's own thread, read by `channel_cmd_wait` only (the enforcement check). It is never dereferenced after completion. Document this in the header.

### Risk 3 — Two release stores on the same cmd (timestamps + done) could let a reader see stale timestamps with fresh done

Mitigation: timestamps are written *before* the `done = 1` release store; the release ordering carries the prior stores. Callers observe timestamps only after observing `done == 1` with acquire load — this is the existing contract, extended to the new fields. Spelled out in the `channel_cmd` field comments.

### Risk 4 — Second SPSC ring doubles memory per worker, proportional to `cq_capacity`

Mitigation: CQ is opt-in. Callers that don't need it pay zero. Callers that do need it size it themselves. No hidden cost.

---

## 11. Open Decisions Resolved During Brainstorm

- **Mechanism shape:** opt-in CQ ring (chosen over always-on ring and scan-helper).
- **Timestamp source:** `CLOCK_MONOTONIC` via `get_time_ns()` (project standard from `src/media/cmd_state.c`), chosen over TSC and modeled EAT.
- **Latency semantic:** wall-clock delta only; modeled EAT latency stays in the media layer.
- **Drain API:** non-blocking only; blocking variant deferred as future seam.
- **CQ-full policy:** spin-retry (never drop), chosen over drop-with-counter and worker-block-on-ring.
- **PR scope:** tier 1 only (mechanism + tests); tier 2 (bench) and tier 3 (FTL retarget) explicitly deferred, documented in Section 9.

---

## 12. References

- REQ-045 row in `docs/REQUIREMENT_COVERAGE.md` (Media Threads section).
- Existing runtime: `include/controller/channel_worker.h`, `src/controller/channel_worker.c`, `tests/test_channel_worker.c`.
- SPSC ring primitive: `include/common/spsc_ring.h`, `src/common/spsc_ring.c` (REQ-085).
- Project timestamp convention: `src/media/cmd_state.c` `start_ts_ns` / `phase_start_ts_ns`.
- Prior NAND/media command-coverage design: `docs/superpowers/specs/2026-04-10-nand-media-command-coverage-design.md`.
- Prior scope-of-closure precedent: REQ-121 (PR #108), REQ-048 (PR #109), REQ-137 (PR #110) — all "mechanism + harness, not production integration" pattern.
