# Handoff — FTL die-busy wait-queue + ticket-lock follow-up

**Date:** 2026-05-08
**Author of handoff:** previous session (Claude Code)
**For:** next agent (Codex) picking up the work
**Project:** `/Users/zifengyang/Desktop/hfsss-simulator`

This document is self-contained: read it and you can pick up exactly
where the previous session stopped. The work is split between an
open PR (#118, in review-response state) and a queued follow-up
(ticket-lock, ready to start once #118 merges).

---

## 1. State summary

### 1.1 What is DONE

**PR #118** — `perf(ftl): per-die wait-queue + completion-driven dispatch`

URL: https://github.com/AlvinYangZF/hfsss-simulator/pull/118
Branch: `perf/ftl-die-busy-waitqueue`
HEAD commit: `867651a` (fix(ftl): address PR #118 review — thread-safe injection + memory order)

10 implementation tasks complete (all committed and pushed):

| Task | Commit | What it delivered |
|---|---|---|
| T1 | `eaf9967` | cmd_engine die-ready notifier hook (anchors A and B; not C) |
| T2 | `f069758` | `die_dispatcher` data structure + L1 unit tests (25 cases) |
| T3 | `a3e0f80` | `die_dispatcher_wait` + `on_die_ready` + L2 mt tests (11 cases) |
| T4 | `567578e` | L3 real-cmd_engine integration tests (22 cases) |
| T5 | `bf62ef6` | ftl_mt replaces retry-spin with dispatcher_wait + `_ex` variants |
| T6 | `648c1bd` | GC `gc_trigger_t` threading + per-trigger priority |
| T7 | `fc78c31` | L4 priority + WFQ integration tests (5 statistical cases) |
| T8 | `ee24847` | env-gated fault-injection knobs (`HFSSS_DIE_DISP_FORCE_BUSY`, `HFSSS_DIE_DISP_NOTIFIER_DELAY_NS`) |
| T9 | `8fac7d7` | CLAUDE.md Diagnostics + REQUIREMENT_COVERAGE updates |
| T10 | `ffc7f1a` + `eaa5a1a` | retry budget tuned to outlast lock starvation; post-impl baseline archived; spec KPI reconciliation |
| review fix | `867651a` | thread-safe RNG, acquire/release ordering, GC budget rationale |

**Spec + plans (committed in PR #118):**
- Dispatcher spec: `docs/superpowers/specs/2026-04-30-ftl-die-busy-waitqueue-design.md`
- Dispatcher plan: `docs/superpowers/plans/2026-04-30-ftl-die-busy-waitqueue.md`
- Ticket-lock spec: `docs/superpowers/specs/2026-05-08-cmd-engine-ticket-lock-design.md`
- Ticket-lock plan: `docs/superpowers/plans/2026-05-08-cmd-engine-ticket-lock.md`

**Baselines archived:**
- `docs/perf-baselines/2026-04-29-master-0eb0d83/` — pre-impl (master baseline)
- `docs/perf-baselines/2026-04-30-perf-ftl-die-busy-waitqueue/` — post-impl

**Tests added (all green on `make test`):**
- `tests/test_cmd_engine_notifier.c` (13 cases)
- `tests/test_die_dispatcher_unit.c` (25 cases)
- `tests/test_die_dispatcher_mt.c` (11 cases)
- `tests/test_die_dispatcher_engine.c` (22 cases)
- `tests/test_die_dispatcher_faults.c` (6 cases)
- `tests/cmd_engine_mock.{h,c}` (mock for L2)
- L4 cases extending `test_ftl_mt.c` and `test_gc_mt.c`

### 1.2 What is NOT delivered

PR #118 **does not meet the original perf KPI**. Honest numbers:

| Metric | Baseline (master) | Original target | Achieved |
|---|---|---|---|
| 012 W mean lat | 142.0 ms | ≤ 10 ms | 141.8 ms ❌ |
| 012 W p99 lat | 152.0 ms | ≤ 20 ms | 143.6 ms (slight) |
| 012 W IOPS | 112 | ≥ 1500 | 112 ❌ |
| pre-checkin | (master fix path could fail under stress) | 8/8 PASS | 8/8 PASS ✅ |
| Other cases (010/011/013/014) | — | within ±10 % | within ±2 % ✅ |

**Why**: cmd_engine's `die_lock` and `channel_lock` are
`pthread_mutex_t` — not FIFO-fair. The dispatcher's wake order is
undone by lock races against fresh arrivals, so a signaled waiter
loses repeatedly to newly-arriving FTL workers and re-queues. The
total wait time converges to roughly the same value as the previous
nanosleep retry-spin.

This is documented in PR #118's spec Section 9.2 with three
follow-up paths.

---

## 2. Immediate next step — finish PR #118

### 2.1 Wait for CI

After commit `867651a` was pushed, GitHub Actions re-triggered:
- Build on macos-latest
- Build on ubuntu-latest
- Kernel Module Build (Linux)

When this handoff was written, all three were `pending`. Likely
durations are 30–60 s each based on previous runs.

**Do**: `gh pr checks 118` until all three are `pass` or `fail`.

```sh
until gh pr checks 118 --json bucket --jq 'all(.bucket != "pending")' 2>/dev/null | grep -q true; do
  sleep 30
done
gh pr checks 118
```

### 2.2 If CI is green

Hand off to the human reviewer for merge. Do **not** merge the PR
yourself unless the human explicitly says so — this is a non-trivial
infrastructure PR that the human wants to eyeball one last time.

### 2.3 If CI fails

Read the failure log via `gh run view <run-id> --log-failed`. Most
likely candidates:
- Compile error from the review-fix commit (already verified locally
  but CI is on Linux which has subtly different headers).
- A race-sensitive test flake — re-run; if it persists 2/3 runs, it's
  real.

Do not push speculative fixes. Diagnose first, then patch.

### 2.4 Open review comment

The reviewer (AlvinYangZF) deferred three IMPORTANT items to the
ticket-lock follow-up:
- IMPORTANT 3: `dispatch_list_del` overloads "not in list" with
  "already dequeued" via self-loop. The fix proposal is an explicit
  state enum (`WAITING / DEQUEUED / TIMED_OUT`) on `struct die_waiter`.
- NIT 2: `gc_hal_*` retry helpers are duplicated.
- NIT 3: `die_priority_slot(DIE_PRIO_GC_CRITICAL)` returns -1.

Decide during the ticket-lock work if any of these naturally fold
into the changes you make there. None blocks the merge.

---

## 3. Follow-up work — ticket-lock PR

Spec: `docs/superpowers/specs/2026-05-08-cmd-engine-ticket-lock-design.md`
Plan: `docs/superpowers/plans/2026-05-08-cmd-engine-ticket-lock.md`

The plan has four bite-sized tasks (TL1–TL4) with explicit code
samples and TDD steps. Read the plan file end-to-end before starting.

### 3.1 Pre-conditions

- PR #118 merged.
- `git checkout master && git pull origin master`.
- `git checkout -b perf/cmd-engine-ticket-lock`.
- The dispatcher infrastructure (notifier hook, per-die wait queue,
  GC trigger threading) is live on master after #118 merges.

### 3.2 Task summary (full content in the plan)

**TL1 — Ticket lock primitive + unit tests**
- Files: `include/common/ticket_lock.h`, `src/common/ticket_lock.c`,
  `tests/test_ticket_lock.c`, Makefile.
- Atomic ticket + serving counters; spin-with-yield acquire.
- Unit tests: single-thread, FIFO order under contention,
  64-thread × 10000 stress, try_lock semantics.
- ~50 LOC primitive + ~150 LOC tests.

**TL2 — Replace `die_lock` with ticket lock**
- Files: `include/media/nand.h` (struct nand_die), `src/media/nand.c`
  (init/cleanup), `src/media/cmd_engine.c` (every site that touches
  `die->die_lock`).
- Run regression suite (`test_media` IS-04/IS-06/SUSPENDED,
  `test_cmd_engine_notifier`, `test_die_dispatcher_*`,
  `test_reset_abort_race`).
- Run `make pre-checkin` and capture the 012 numbers — expect a
  significant drop (mean from ~142 ms to perhaps 20–40 ms range).

**TL3 — Replace `channel_lock` with ticket lock**
- Same treatment for `struct nand_channel`.
- After this task `make pre-checkin` should show 012 mean ≤ 10 ms,
  p99 ≤ 20 ms, IOPS ≥ 1500. That is the KPI the original spec
  asked for.
- Archive new baseline at
  `docs/perf-baselines/<date>-cmd-engine-ticket-lock/`.

**TL4 — Spec reconciliation + open PR**
- Update PR #118's spec Section 9.2 to mark Option 1 as merged with
  achieved KPI numbers.
- Update `docs/REQUIREMENT_COVERAGE.md` FTL subtotal.
- Push branch, open PR titled
  `perf(cmd_engine): FIFO-fair ticket lock — close 012 latency gap from dispatcher PR`.
- Body: reference PR #118; show baseline → post-die-lock →
  post-both-locks number progression; explain why this is the
  smallest follow-up that closes the gap (per spec 9.2).

Tasks IDs already created in the local task list: #227–#230 (one per
TL task with proper `blockedBy` chain). If you use a different task
system, ignore those IDs.

---

## 4. Critical context that is NOT in committed docs

### 4.1 Why the 2048 × 10 ms retry budget

The original spec called for `8 × 50 ms = 400 ms`. Under fio-012
(`bs=128k iodepth=16` with 8 FTL workers feeding from the SQ) the
budget exhausted: max retry count observed was 63. The bump to
2048 × 10 ms = ~20 s is a "outlast worst-case lock starvation"
ceiling, well below the NVMe 30 s timeout. After the ticket lock
lands, the budget can probably be cut back; that's a TL3 sub-decision
worth leaving to whoever picks up the work.

### 4.2 Why GC has a different (tighter) budget

GC: 8 × 50 ms = 400 ms.
Host IO: 2048 × 10 ms = 20 s.

GC runs inline from the host-write path under NOSPC. A stalled GC
absorbs every host write waiting for free space. The 400 ms ceiling
intentionally bounds GC's blocking window so a stuck GC pass surfaces
BUSY → host retry rather than a multi-second NVMe stall. After the
ticket-lock lands GC contention drops dramatically; the tighter
budget can stay.

### 4.3 Why `dispatch_list_del` overloads "not in list" via self-loop

`die_waiter_init` calls `dispatch_list_init(&w.list)` which makes the
list node self-loop. `dispatch_list_del` after popping ALSO leaves
the node self-looping. So `dispatch_list_empty(&w.list)` returns true
both for a fresh node and for a popped node. The `die_dispatcher_wait`
timeout path uses this to decide "is the waiter still queued, or has
the notifier already taken it?" without an extra state field.

If the ticket-lock follow-up changes `dispatch_list_del` semantics
(e.g., reuses the list pointer for something else), the wait-path
self-removal logic must be reworked. The reviewer suggested an
explicit state enum — consider whether that drop-in feels worth ~10
LOC during TL2/TL3.

### 4.4 The dispatcher's perf overhead is roughly zero

PR #118 adds the dispatcher in front of cmd_engine. We confirmed that
014_stress, 010_randwrite, 011_randrw, and 013_trim all stay within
±2 % of the master baseline. The dispatcher is not slowing things
down even under workloads where it does not help. This means the
ticket-lock fix can land independently and cleanly — no rollback risk
from the dispatcher itself.

### 4.5 Sub-agent dispatch worked well for tasks of similar shape

Tasks T1, T2, T3, T4, T6, T7, T8 were dispatched as `general-purpose`
sub-agents using a strict task-prompt template. Tasks T5 and T10 were
done in-line by the main session because (a) T5 needed the previous
session's branch/file state for confidence and (b) T10 was the
reconciliation step. The plan for ticket-lock (TL1–TL4) is sized for
sub-agent dispatch — TL1 in particular is self-contained.

### 4.6 Build system quirk

Once during T10 a `make` invocation went up-to-date silently after a
header edit. Force-rebuild via `rm -f build/lib/libhfsss-ftl.a
build/bin/hfsss-nbd-server` then `make all` resolves it. Worth knowing
if you see "0 byte changes but the test still fails the same way".

### 4.7 macOS pthread_mutex unfairness specifically

The fairness problem is documented for macOS in particular, but
PTHREAD_MUTEX_DEFAULT on Linux glibc is also non-FIFO. The ticket
lock fix is correct on both platforms. The CI runs on
ubuntu-latest + macos-latest + kernel-module-Linux — all three should
benefit equally.

---

## 5. Decisions left to the next agent

1. **Should TL2 land independently from TL3?** The plan has them as
   sequential tasks, but technically TL2 alone (just `die_lock`)
   should produce most of the latency win, since `die_lock` is the
   most contended. Worth measuring after TL2 and deciding whether to
   ship TL2 alone if `channel_lock` contention turns out to be
   negligible.

2. **Should the IMPORTANT 3 review item (state enum) land in the
   ticket-lock PR?** ~10 LOC, makes the timeout-path self-remove
   logic clearer. Mild preference for yes.

3. **Should the GC retry budget change?** After ticket lock the
   contention storm goes away, so GC's 400 ms ceiling has more
   margin. Leave it alone unless profiling shows GC is hitting the
   ceiling.

4. **Spec Section 9.2 mark-up**: when the ticket-lock PR opens,
   should it amend PR #118's spec doc inline, or add a new spec
   document? In-line update is more discoverable; new doc is more
   historically clean. Going with the in-line approach matches the
   existing pattern in this repo (`docs/superpowers/specs/` shows
   single-doc-per-feature, with reconciliation in the same file).

---

## 6. Quick orientation for the next agent

**Repo layout:**
- `src/ftl/die_dispatcher.c` (+ internal header) — the new dispatcher
- `src/ftl/ftl.c` — host-IO retry loop with `_ex` variants
- `src/ftl/gc.c` — GC body with `gc_hal_*_with_wait` wrappers
- `src/media/cmd_engine.c` — anchor A and B notifier sites
- `include/ftl/die_dispatcher.h` — public API
- `include/media/nand.h` — `die_ready_notifier` field on `nand_device`

**Build/test:**
- `make all` — full build
- `make test` — full unit/system suite (~30 s)
- `make pre-checkin` — QEMU blackbox 9-case bundle (~15 min)
- Force rebuild: `rm -rf build && make all`

**CI:** GitHub Actions on push to any branch with PR open. macos +
ubuntu + kernel module build (no test runs in CI; test is local).

**Project rules** (`CLAUDE.md`):
- English-only in code.
- No "FIXED / Step / Phase / Week" progress terms in commits, code
  comments, or PR bodies.
- No AI tool names anywhere.
- Avoid line-number-anchored docs ("see line 432") — use conceptual
  descriptions.
- Test-driven: tests precede implementation in every task.

**Skill the previous session used:**
`superpowers:subagent-driven-development` — dispatch a sub-agent per
task with a self-contained prompt. The plan file's Task sections are
already shaped to be copied directly into a sub-agent prompt.

---

## 7. Final state at handoff time

- `git status` clean (relative to push at `867651a`)
- Local branch: `perf/ftl-die-busy-waitqueue` (16 commits ahead of
  master)
- Pushed to `origin/perf/ftl-die-busy-waitqueue`
- PR #118 status: review-fix pushed, CI re-running
- No stuck background tasks (verified earlier in session)
- `/tmp/pre-checkin.log` contains the most recent run output
  (8/8 PASS)

Good luck.
