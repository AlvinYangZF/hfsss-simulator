# Async-Pipeline FTL Second Race — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Localize and fix the FTL race that trips `make pre-checkin` (NBD `--mode async`, 2048 MiB) on fio 010-014 with NVMe `SCT=0x2 / SC=0x80` write fault. End state: pre-checkin pass=9 fail=0 skip=1.

**Architecture:** Investigation-style. Phase 1 builds the trigger and locks in a measurable baseline. Phase 2 localizes to specific source. Phase 3 fixes (shape known only after Phase 2). Phase 4 validates end-to-end.

**Tech Stack:** Existing reproducer is C, drives `ftl_mt_submit` / `ftl_mt_poll_completion` (the production async-pipeline interface). Sanitizer is `-fsanitize=thread` via `build-tsan/`. Trace infrastructure already exists (commit `a3ad11c`).

**Spec:** `docs/superpowers/specs/2026-04-26-async-ftl-race-design.md`

---

## File Structure

Phases 1, 3, 4 touch these files. Phase 2 is read-only investigation.

| File | Phase | Reason |
|---|---|---|
| `tools/ftl_mfc_repro.c` | 1 | Add knobs / new mode if needed; capture pre-fix baseline |
| `Makefile` | 1, 4 | Add `ftl-mfc-repro-dispatcher` opt-in target if needed |
| (TBD by Phase 2) | 3 | Fix location |
| `tests/test_ftl_*.c` | 3 | Unit-level race regression test if applicable |
| `docs/superpowers/plans/2026-04-26-async-ftl-race-closeout.md` | 4 | Closeout doc with root cause + fix mapping |
| `docs/REQUIREMENT_COVERAGE.md` | 4 | Only if a REQ row status changes |

`channel_worker.{c,h}`, `hal_nand.c` timing, FTL data structures (allocator, l2p): NOT touched per spec §3.2.

---

## Phase 1 — Baseline + reproducer (1-2 days)

**Goal:** Get failure rate ≥ 1/1K in ≤ 10 s wall-clock, in a single command.

### Task 1.1: Capture pre-fix baseline on existing reproducer

**Files:**
- Modify (run only): `tools/ftl_mfc_repro.c` — no edit yet, just exercise

- [ ] **Step 1: Build the reproducer**

```bash
make -j12 build/bin/ftl_mfc_repro
```

Expected: clean build, binary appears at `build/bin/ftl_mfc_repro`.

- [ ] **Step 2: Run at production-equivalent settings**

```bash
./build/bin/ftl_mfc_repro --threads 8 --rwmix 70 --duration 30 --lbas 524288
```

Expected: process runs 30 s. Capture exit code, mismatch count, total ops. The `--lbas 524288` (2 GiB at 4 KiB) matches pre-checkin's `--size-mb 2048` exported size.

- [ ] **Step 3: Stress run at threads=16 (matches the closeout's mismatch rate test)**

```bash
./build/bin/ftl_mfc_repro --threads 16 --rwmix 70 --duration 60 --lbas 524288
```

Expected: closeout reported "1 mismatch / 56K ops" at this setting. Confirm or refute.

- [ ] **Step 4: Stress at threads=32 with narrow LBA range**

```bash
./build/bin/ftl_mfc_repro --threads 32 --rwmix 70 --duration 30 --lbas 4096
```

Hypothesis: narrow LBA range (4096 = 16 MiB) maximizes cross-worker collision; threads=32 doubles the production worker count.

- [ ] **Step 5: Record baseline metrics in a file for later comparison**

```bash
./build/bin/ftl_mfc_repro --threads 32 --rwmix 70 --duration 30 --lbas 4096 \
    > /tmp/repro-baseline-stage1.log 2>&1
echo "exit=$?" >> /tmp/repro-baseline-stage1.log
```

Expected: log captures total ops, mismatch count, timing. This is the *pre-fix* baseline that Phase 4 compares against.

- [ ] **Step 6: Decision gate**

Three outcomes and what each means:

| Observed | Meaning | Next |
|---|---|---|
| ≥ 1/1K mismatch | Existing reproducer is sufficient; topology hypothesis irrelevant | Skip 1.2, proceed to 1.3 |
| 1/10K – 1/1K | Marginal; rate amplification needed | Proceed to 1.2 |
| < 1/10K | Spec hypothesis is *probably* right; dispatcher topology matters | Proceed to 1.2 (mandatory) |

### Task 1.2: Add `--mode dispatcher` if Step 6 says so

**Skip this task if Step 6 outcome is row 1.**

**Files:**
- Modify: `tools/ftl_mfc_repro.c`

- [ ] **Step 1: Add CLI flag and mode enum**

Insert near the existing CLI parser (search for `--threads` parser entry).

```c
enum repro_mode {
    REPRO_MODE_LEGACY = 0,
    REPRO_MODE_DISPATCHER,
};

/* In CLI: */
} else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
    if (strcmp(argv[++i], "dispatcher") == 0) {
        cfg.mode = REPRO_MODE_DISPATCHER;
    } else if (strcmp(argv[i], "legacy") == 0) {
        cfg.mode = REPRO_MODE_LEGACY;
    } else {
        fprintf(stderr, "ERROR: --mode must be 'legacy' or 'dispatcher'\n");
        return 2;
    }
}
```

- [ ] **Step 2: Implement single-producer dispatcher topology**

In `--mode dispatcher`:
- 1 producer thread submits ops at peak rate using `ftl_mt_submit` (no per-thread submission rings)
- 1 reaper thread polls `ftl_mt_poll_completion` and verifies via existing CRC32C path
- Up to `--threads` worker threads (the FTL worker pool, already existing) consume from the per-worker SPSC rings

This mirrors `nbd_async_init` topology in `src/vhost/hfsss_nbd_server.c`.

- [ ] **Step 3: Build and run**

```bash
make -j12 build/bin/ftl_mfc_repro
./build/bin/ftl_mfc_repro --mode dispatcher --threads 8 --rwmix 70 \
    --duration 30 --lbas 4096 > /tmp/repro-baseline-stage2.log 2>&1
```

- [ ] **Step 4: Compare rates**

```bash
diff /tmp/repro-baseline-stage1.log /tmp/repro-baseline-stage2.log
```

If dispatcher mode shows ≥ 5× higher rate than legacy at the same other knobs, hypothesis confirmed; use dispatcher mode for Phase 2/3.

If dispatcher mode shows the same or lower rate, hypothesis rejected; revert to legacy mode and pivot the search to *workload shape* (look at fio 010 specifically — block sizes, queue-depth variance, trim insertions).

- [ ] **Step 5: Commit Phase 1 artifacts**

```bash
git add tools/ftl_mfc_repro.c
git commit -m "test(ftl-mfc-repro): add --mode dispatcher for async-pipeline race repro

Mirrors the NBD async pipeline topology (1 producer + N workers via
SPSC rings + 1 reaper) so the reproducer triggers the same event
sequencing as production. Pre-fix rate captured at threads=8,
lba-range=4096: see Phase 1 baseline log.

Refs: docs/superpowers/specs/2026-04-26-async-ftl-race-design.md"
```

### Task 1.3: Lock in baseline + hand-off to Phase 2

- [ ] **Step 1: Run final pre-fix baseline at the chosen knobs**

```bash
BIN=./build/bin/ftl_mfc_repro
MODE_FLAG=""    # or "--mode dispatcher" if 1.2 ran
$BIN $MODE_FLAG --threads 16 --rwmix 70 --duration 30 --lbas 4096 \
    > /tmp/repro-pre-fix-baseline.log 2>&1
grep -E "ops|mismatch|exit" /tmp/repro-pre-fix-baseline.log
```

Expected: rate ≥ 1/1K, captured in log.

- [ ] **Step 2: Verify pre-checkin still fails the same way**

```bash
make pre-checkin > /tmp/pre-checkin-pre-fix.log 2>&1
grep -E "fail=|FAIL|sct" /tmp/pre-checkin-pre-fix.log | head -10
```

Expected: same `pass=3 fail=5 skip=1` pattern as the 4-26 e2e audit, NVMe SCT=0x2 SC=0x80.

- [ ] **Step 3: Phase 1 closeout — confirm gate**

Phase 1 passes if:
- A single-command reproducer triggers ≥ 1/1K rate ≤ 10 s
- pre-checkin still fails (so we know we're chasing the right thing)

If Phase 1 cannot push the rate above 1/1K with thread/lba knobs even after dispatcher mode, **stop and escalate** before any code change. The spec's hypothesis is wrong; investigation shape needs revisiting.

---

## Phase 2 — Localize (1-2 days)

**Goal:** Root cause document — file, function, line range, plus the exact happens-before violation or invariant that's broken.

### Task 2.1: TSan run

**Files:** none modified; uses `build-tsan/`.

- [ ] **Step 1: Build instrumented binary**

```bash
make -C . -f Makefile clean-tsan 2>/dev/null || true
make -j12 BUILD_DIR=build-tsan CFLAGS_EXTRA="-fsanitize=thread -g -O1" \
    build-tsan/bin/ftl_mfc_repro
```

(If the project does not already have a `clean-tsan` / `build-tsan` rule wired through, skip the make rule and invoke gcc/clang directly per `tests/Makefile.tsan` if it exists; otherwise fall back to a one-off CFLAGS override on a clean build.)

Expected: instrumented binary at `build-tsan/bin/ftl_mfc_repro`.

- [ ] **Step 2: Run at the Phase 1 baseline knobs, capture TSan report**

```bash
TSAN_OPTIONS="halt_on_error=0:second_deadlock_stack=1:report_atomic_races=1" \
    ./build-tsan/bin/ftl_mfc_repro $MODE_FLAG --threads 16 --rwmix 70 \
    --duration 30 --lbas 4096 > /tmp/tsan-output.log 2>&1
grep -A 30 "WARNING: ThreadSanitizer" /tmp/tsan-output.log | head -100
```

- [ ] **Step 3: Decision**

| Observed | Next |
|---|---|
| TSan emits ≥ 1 happens-before violation | Triage in Task 2.3 |
| TSan silent (no warnings) | Race is *logical* (state-machine ordering, not memory order); proceed to Task 2.2 |
| TSan crashes / can't build | Skip to Task 2.2 |

### Task 2.2: Logical-race tuple tracing (fallback only)

**Skip if Task 2.1 produced a TSan finding.**

**Files:** add temporary trace points to candidate functions; revert before commit.

- [ ] **Step 1: Identify candidate functions**

Three FTL/HAL invariants likely violated. Targets:

| Invariant | Function to instrument |
|---|---|
| Block alloc — at most one CWB per `(channel, plane)` at a time | `block_alloc_from_shard` in `src/ftl/block.c` |
| L2P — only one writer per LBA at a time | grep for `l2p[` assignments in `src/ftl/ftl.c` |
| Physical program — only one program per `(ch, chip, die, plane, block, page)` at a time | `media_nand_program` in `src/hal/hal_nand.c` |

- [ ] **Step 2: Add temporary worker-id+tuple traces**

For each function, add at function entry / critical path:

```c
fprintf(stderr, "[trace] worker=%u %s ch=%u plane=%u block=%u page=%u t=%lld\n",
        thread_id_via_pthread_self_lookup(),
        __func__, ch, plane, block, page, get_time_ns());
fflush(stderr);
```

(Use a project-internal thread-id helper if one exists; otherwise a `pthread_self() % 1024` cast is enough for distinguishing 8 workers in stderr.)

- [ ] **Step 3: Run and post-process**

```bash
./build/bin/ftl_mfc_repro $MODE_FLAG --threads 16 --rwmix 70 \
    --duration 30 --lbas 4096 2> /tmp/race-trace.log
# Find tuples touched by ≥ 2 distinct workers within ε of each other
sort -k 4,9 /tmp/race-trace.log | awk 'BEGIN{prev=""} {
    key=$3" "$4" "$5" "$6" "$7;
    if (key == prev && $2 != prev_worker) print "RACE:", $0, "<-", prev_line;
    prev=key; prev_worker=$2; prev_line=$0
}' | head -20
```

Expected: zero or more lines pointing to specific tuples that two workers touched in adjacent trace events.

### Task 2.3: Root cause document

- [ ] **Step 1: Write `docs/superpowers/artifacts/2026-04-26-async-ftl-race-rootcause.md`**

Required sections:
- 1-paragraph summary
- Exact file/function/line range pointing at the unprotected critical section
- Reproduction steps (Phase 1 commands + the TSan output or trace-derived evidence)
- Why the existing locks (CWB mutex, GC CAS) don't cover this case
- Proposed fix shape (mapping to spec §4.3 table)

- [ ] **Step 2: Phase 2 gate**

Phase 2 passes if the root cause document points at a concrete location with concrete evidence. If after 2 days no candidate has been confirmed, escalate per spec Q3.

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/artifacts/2026-04-26-async-ftl-race-rootcause.md
# Revert any temporary tracing in src/ before committing
git diff src/ tools/ | head
git checkout -- src/  # if traces were added
git commit -m "docs(artifact): async-ftl-race root cause document

Phase 2 closeout. <One-line root-cause summary>.
Refs: docs/superpowers/specs/2026-04-26-async-ftl-race-design.md §4.2"
```

---

## Phase 3 — Fix (0.5-1 day)

**Goal:** 1M ops, 0 faults, TSan clean. Fix scope < 100 LOC.

Tasks for Phase 3 are defined in detail *only after Phase 2 lands a root cause*. The exact code can't be prescribed in advance because the fix shape depends on which invariant is broken.

### Task 3.1: Apply the fix

**Files:** TBD by Phase 2; constrained per spec §4.3 table.

- [ ] **Step 1: Make the smallest change that closes the race**

Constraints:
- < 100 LOC delta
- No FTL data structure refactor
- No `channel_worker` touch
- No HAL NAND timing change
- New lock granularity must not introduce contention worse than 5 % p99 hit (validated in Phase 4)

If the right fix is > 100 LOC, **stop and replan** with a fresh design phase before code.

- [ ] **Step 2: Rebuild + immediate sanity**

```bash
make -j12 build/bin/ftl_mfc_repro
./build/bin/ftl_mfc_repro $MODE_FLAG --threads 16 --rwmix 70 \
    --duration 10 --lbas 4096
```

Expected: 0 mismatches in 10 s. If non-zero, fix is incomplete; iterate.

### Task 3.2: Long-soak validation

- [ ] **Step 1: 1M-op soak**

```bash
./build/bin/ftl_mfc_repro $MODE_FLAG --threads 16 --rwmix 70 \
    --duration 600 --lbas 4096 > /tmp/repro-post-fix-soak.log 2>&1
grep -E "ops|mismatch|exit" /tmp/repro-post-fix-soak.log
```

Expected: ≥ 1M ops, 0 mismatches, exit=0.

- [ ] **Step 2: TSan clean**

```bash
TSAN_OPTIONS="halt_on_error=1" ./build-tsan/bin/ftl_mfc_repro $MODE_FLAG \
    --threads 16 --rwmix 70 --duration 30 --lbas 4096 \
    > /tmp/tsan-post-fix.log 2>&1
echo "exit=$?"
grep -c "ThreadSanitizer" /tmp/tsan-post-fix.log
```

Expected: exit=0, "ThreadSanitizer" hit count = 0.

- [ ] **Step 3: Add unit-level race regression test if applicable**

If the broken invariant can be exercised in a unit-test harness (without QEMU / NBD), add a focused test under `tests/test_ftl_concurrency.c` (new file or extension of an existing one) that fails on master and passes on the fixed branch.

### Task 3.3: Commit the fix

- [ ] **Step 1: Commit**

```bash
git add <fix-files>
git commit -m "fix(ftl): <one-line root cause and fix>

<2-3 paragraph body explaining the race, why existing locks did
not cover it, and the new invariant.>

Phase 1 reproducer: 1M ops, 0 mismatches, post-fix.
TSan: clean.
Refs: docs/superpowers/specs/2026-04-26-async-ftl-race-design.md
      docs/superpowers/artifacts/2026-04-26-async-ftl-race-rootcause.md"
```

---

## Phase 4 — System validation + closeout (0.5 day)

### Task 4.1: Regression smokes

- [ ] **Step 1: `make test`**

```bash
make test > /tmp/post-fix-make-test.log 2>&1
echo "exit=$?"
grep -cE "\\[FAIL\\]" /tmp/post-fix-make-test.log
```

Expected: exit=0, [FAIL] count = 0.

- [ ] **Step 2: `make systest`**

```bash
make systest > /tmp/post-fix-make-systest.log 2>&1
echo "exit=$?"
grep -cE "\\[FAIL\\]" /tmp/post-fix-make-systest.log
```

Expected: exit=0, [FAIL] count = 0.

- [ ] **Step 3: `make pre-checkin` async + 2048 MiB (the original failure)**

```bash
make pre-checkin > /tmp/post-fix-pre-checkin.log 2>&1
echo "exit=$?"
grep -E "summary|fail=" /tmp/post-fix-pre-checkin.log | tail -3
```

Expected: exit=0, `summary: pass=9 fail=0 skip=1`.

### Task 4.2: Performance smoke

- [ ] **Step 1: `make bench-cq` (REQ-045 tier-2 latency bench)**

```bash
STRESS_RESULTS_FILE=/tmp/bench-cq-post-fix.txt make bench-cq
grep -E "latency_p99_ns|ops_per_sec|result=" /tmp/bench-cq-post-fix.txt
```

- [ ] **Step 2: Compare against pre-fix baseline**

If a pre-fix bench-cq run was captured (master before the fix landed), p99 latency must be within ±10 %. If not previously captured, treat current result as the new baseline and check that result=PASS.

### Task 4.3: Closeout document

- [ ] **Step 1: Write `docs/superpowers/plans/2026-04-26-async-ftl-race-closeout.md`**

Mirroring the 2026-04-17 fio 014/015 closeout:
- Summary (1 paragraph)
- What was measured (Phase 1 baselines, post-fix soak, pre-checkin pass)
- Root cause (link to artifact doc)
- Fix scope (commit SHAs, LOC delta)
- Out-of-scope items left for future work
- Lessons learned

### Task 4.4: Final PR push

- [ ] **Step 1: Mark PR #115 ready for review**

```bash
gh pr ready 115
```

- [ ] **Step 2: Update PR description**

Replace the RFC-only body with the final body covering: spec → plan → root cause → fix → validation, with commit SHAs and the closeout doc link.

- [ ] **Step 3: Tag for codex review**

Codex review pass. Address any inline comments before merging (per the standing project workflow established in PRs #111-#114).

---

## Spec compliance checklist

Cross-reference back to spec §3 to confirm plan obeys it:

- [x] **§3.1 Goal:** reproducer ≥ 1/1K → Phase 1
- [x] **§3.1 Goal:** localize to specific source → Phase 2
- [x] **§3.1 Goal:** fix it → Phase 3
- [x] **§3.1 Goal:** pre-checkin async passes 010-014 → Phase 4 Task 4.1 Step 3
- [x] **§3.1 Goal:** make test / make systest unaffected → Phase 4 Task 4.1
- [x] **§3.2 Non-Goal:** no `channel_worker` touch → Phase 3 constraint
- [x] **§3.2 Non-Goal:** no FTL data structure refactor → Phase 3 constraint
- [x] **§3.2 Non-Goal:** correctness first, perf preserved ±10 % → Phase 4 Task 4.2
- [x] **§3.2 Non-Goal:** one race only → Phase 3 Task 3.1 constraint
- [x] **§4.1 Phase 1 detail:** dispatcher topology if hypothesis confirmed → Task 1.2 conditional
- [x] **§4.2 Phase 2 detail:** TSan first, tracing fallback → Task 2.1 / 2.2
- [x] **§4.3 Phase 3 detail:** < 100 LOC fix → Task 3.1 Step 1 constraint
- [x] **§4.4 Phase 4 detail:** all five validation lanes → Task 4.1 / 4.2

---

## Self-review (per writing-plans skill)

Reviewer pass over the plan vs spec:

**Placeholder scan:** Phase 3 has explicit "TBD by Phase 2" markers. Those are not placeholder failures — they are intentional gates because the fix code cannot be written before localization. Any other "TBD" or "TODO" → none found.

**Type / signature consistency:** The reproducer's `ftl_mt_submit` / `ftl_mt_poll_completion` signatures are unchanged across phases. The new `--mode dispatcher` flag is local to the reproducer and does not affect FTL public API.

**Spec coverage:** §3.1 goals all have a Phase task. §3.2 non-goals all expressed as Phase 3 constraints. §6 risks all have a corresponding gate (Phase 1 escalation, Phase 2 escalation, Phase 3 < 100 LOC ceiling, Phase 4 perf smoke).

**Plan-internal consistency:** Phase 1 Step 6 decision gate feeds Task 1.2 / 1.3 cleanly. Phase 2 Task 2.1 / 2.2 are mutually exclusive based on TSan output. Phase 4 closeout doc references the Phase 2 artifact + Phase 3 commits.

**Open from spec §7 not yet resolved:** PR structure (Q1) — defaulting to single PR per Q1 recommendation; Q2 reproducer permanence — deferred to a Phase 4 sub-task to add a Makefile target if needed; Q3 escalation rule — explicitly written into Phase 1 / Phase 2 gates; Q4 out-of-scope confirmed in §3.2 non-goals and Phase 3 constraints.

If reviewer disagrees with any of the Q1-Q4 defaults, edit before plan approval.
