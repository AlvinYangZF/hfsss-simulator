# Async-FTL-Race Investigation — Context Handoff (mid-session)

**Status:** Pause point at end of Phase 2 ATTEMPT-1. Path A reframe pending.
**Branch:** `feat/async-ftl-race-investigation`
**PR:** [#115](https://github.com/AlvinYangZF/hfsss-simulator/pull/115) (draft)
**Local commits ahead of master:**

| SHA | Subject | Status |
|---|---|---|
| `198ac18` | docs(spec): async-pipeline FTL second race investigation design | ✅ keep |
| `0b13fee` | docs(plan): async-pipeline FTL second race implementation plan | ✅ keep |
| `f1c112c` | test(ftl-mfc-repro): add error tracking + dispatcher mode | ✅ keep — value-adds independent of Phase 2 conclusion |
| `10d78f8` | docs(artifact): async-ftl-race root cause document | ⚠️ **WRONG ROOT CAUSE** — needs to be revised or superseded |

## Critical context: Phase 2 conclusion is wrong

The root-cause doc at `10d78f8` claimed the race was unconditional `taa_remove` in `src/ftl/gc.c:493/534`. **That is incorrect for the reproducer's workload.** Evidence:

- Added `taa_remove_if_equal` (CAS-style) and replaced both gc.c sites — **error rate unchanged** (~7.5% before and after).
- Added trace prints to `taa_remove_if_equal`: **fires zero times** in a 2-second reproducer run.
- Added trace prints to `taa_lookup` NOENT-return path: **637 events** in 2 seconds.
- Per-LBA chronological trace (lba=231, 41, 250, 59): every `LOOKUP_NOENT` event for those LBAs occurred BEFORE the first `taa_update` for the same LBA. **`LOOKUP_NOENT after first UPDATE = 0` for all 4 sample LBAs.**

**Real cause of the 7.5% error rate**: the reproducer counts as "error" every read on an LBA whose first write hasn't completed yet. With 128 worker threads, 256 LBAs, and rwmix=70 (more reads than writes), each LBA gets read several times before the first write to it lands. After the first write, the LBA's L2P slot is permanently `valid=true` (no path clears it in this workload — `taa_remove` is only called from `ftl_trim_page_mt` which the workload doesn't issue, and the GC failure paths I "fixed" don't fire here).

**Implication**: The 2026-04-18 closeout's "1 mismatch / 56K ops at threads=16" was likely the same workload artifact, not a real race. The reproducer at the current shape **cannot see** the race that pre-checkin trips.

## What pre-checkin actually fails on (different signal)

`make pre-checkin` (NBD `--mode async`, 2048 MiB) trips:
- `nvme0n1: I/O Cmd(0x1) @ LBA N, ... I/O Error (sct 0x2 / sc 0x80)` — NVMe **Write Fault**
- `op 0x1:(WRITE)` — write op, not read
- 010-014 fio cases all fail; 016/020/021 pass

The reproducer surfaces NOENT on **reads of unwritten LBAs** (a workload artifact, not a real race). The pre-checkin Write Fault and the reproducer NOENT are *different surfaces*. We do not yet have evidence they share a root cause.

## Uncommitted local changes (to discard for Path A)

```
modified:   include/ftl/taa.h          (taa_remove_if_equal declaration)
modified:   src/ftl/taa.c              (taa_remove_if_equal impl + debug trace)
modified:   src/ftl/gc.c               (2 unconditional remove → conditional)
modified:   src/ftl/ftl.c              (read retry adds taa relookup + debug trace)
```

These are defensive correctness improvements (the CAS pattern matches existing `taa_update_if_equal`; the read-retry relookup is a standard idiom), but **none of them touch the actual cause of the reproducer's NOENT and none move pre-checkin from FAIL to PASS.** Discard via `git checkout -- include/ftl/taa.h src/ftl/taa.c src/ftl/gc.c src/ftl/ftl.c`.

If you ever want to land them as standalone hardening, do it in a separate PR with no claim of fixing the async race.

## Path A — what to do after compact

### Step 1: Discard speculative fixes
```
cd /Users/zifengyang/Desktop/hfsss-simulator
git checkout -- include/ftl/taa.h src/ftl/taa.c src/ftl/gc.c src/ftl/ftl.c
git status   # verify clean
```

### Step 2: Update reproducer to filter warmup
In `tools/ftl_mfc_repro.c`, gate read-error counting on `g_crc_map[lba].valid` at submit time:
- Legacy `worker_fn` read path: only call `error_count++` if `g_crc_map[lba].valid == true` was observed when the read was scheduled. (Take the stripe lock, snapshot `valid`, submit; if status is non-OK and the snapshot was `valid==true`, the error is real.)
- Dispatcher reaper: same — capture valid-at-submit on the `dispatcher_op` and only count errors when the LBA was already written.
- WRITE errors stay unconditionally counted (no warmup ambiguity).

### Step 3: Re-baseline
Re-run `--mode legacy --threads 128 --rwmix 70 --duration 10 --lbas 256` after the filter. Two outcomes:

| Observed | Meaning | Next step |
|---|---|---|
| Read errors drop to ~0 | Reproducer cannot see the pre-checkin race; warmup was the only signal | Pivot to Step 4a |
| Read errors stay > 0.1% | A real race exists post-warmup; investigate it | Triage with traces, redo Phase 2 |

### Step 4a: If reproducer goes silent — pivot to fio-010 instrumentation
Direct investigation of pre-checkin's write fault. Likely paths:
- Add `fprintf(stderr, ...)` at every `return HFSSS_ERR_*` in `src/hal/hal_nand.c` and `src/ftl/ftl.c` (compile-time-gated by an env var so production builds aren't polluted).
- Run `make pre-checkin` with the gate on; capture the first non-OK return on the write path.
- Cross-reference the LBA / PPN / op type with what fio 010 was doing at that moment.
- Likely candidates for fio-specific triggers: TRIM ordering, write-after-trim, large block writes (128 KB seq), iodepth=64 backpressure into FTL ring exhaustion (`ftl_mt_submit` returns false on full ring → NBD might propagate as Write Fault).

### Step 4b: Update Phase 2 root-cause doc
Replace `docs/superpowers/artifacts/2026-04-26-async-ftl-race-rootcause.md` content with the corrected analysis:
- The closeout doc's "second race" hypothesis is unverified by the current reproducer.
- The reproducer's "1/56K mismatch / 7.5% error rate" was a startup artifact.
- Pre-checkin's Write Fault has not yet been localized; investigation requires direct production-path instrumentation.

### Step 5: Update PR #115 description
Move out of the "fix" frame and into "investigation" frame:
- Phase 1 added error tracking that exposed the warmup-NOENT issue (real value).
- Phase 2 attempt-1 followed the closeout's hypothesis → couldn't reproduce.
- Next step: investigate pre-checkin Write Fault directly, separate PR.

## Spec / plan — known deviations to record

The spec at `docs/superpowers/specs/2026-04-26-async-ftl-race-design.md`:
- §4.1 "Phase 1 dispatcher topology hypothesis" — refuted (legacy mode + error tracking is sufficient, dispatcher topology adds hangs without amplifying signal).
- §4.2 "TSan first" — TSan ran clean except for benign stats counters; logical-tracing fallback at §4.2 was the actual signal source.
- §4.3 "Fix shape table" — every guess in that table is wrong for *this* reproducer (block_alloc, L2P update, HAL parallel program, GC vs writer dst block — none of them fire under the workload). The fix shape for pre-checkin is not yet known.

The plan at `docs/superpowers/plans/2026-04-26-async-ftl-race.md`:
- Phase 1.1 ✅ done (`f1c112c`).
- Phase 1.2 ✅ done — but the `--mode dispatcher` was not the value-add. Error tracking was.
- Phase 1.3 ✅ baseline locked at `docs/superpowers/artifacts/2026-04-26-async-ftl-race-phase1-baseline.log`.
- Phase 2 ⚠️ root cause doc exists (`10d78f8`) but conclusion wrong. Needs revision per Path A.
- Phase 3, 4 — not started.

## Files to read first when resuming

In order:
1. This handoff doc.
2. `docs/superpowers/artifacts/2026-04-26-async-ftl-race-rootcause.md` — to see what the current (wrong) doc claims, so you can replace it.
3. `tools/ftl_mfc_repro.c` lines 410-470 (legacy `worker_fn` read+write paths) — for the warmup filter edit.
4. `src/ftl/ftl.c` lines 686-750 (`ftl_read_page_mt`) — for context on what NOENT-return-paths look like, no edits needed unless reverting.
5. `src/vhost/hfsss_nbd_server.c` async-pipeline section — if Step 4a triggers and we need to instrument.

## Tasks state

| ID | Status | What |
|---|---|---|
| 204 | ✅ | Spec written |
| 205 | ✅ | User reviewed spec |
| 206 | ✅ | Plan written |
| 207 | ✅ | User reviewed plan |
| 208 | ✅ | Phase 1.1 baseline existing repro |
| 209 | ✅ | Phase 1.2 dispatcher mode |
| 210 | ✅ | Phase 1.3 lock pre-fix baseline |
| 211 | ✅ (but conclusion wrong) | Phase 2 localize |
| 212 | 🟡 in_progress | Phase 3 fix — needs to revert and reframe per Path A |
| 213 | ⏳ pending | Phase 4 validate |

After compact, reset 212 back to pending, mark 211 as needing re-do, optionally add a new task: "Phase 2 attempt-2 — pivot to direct fio-010 instrumentation".

## Quick-restart commands

```
# Verify state
git status
git log --oneline origin/master..HEAD
gh pr view 115 --json state,statusCheckRollup --jq '.'

# Discard speculative fixes (Path A Step 1)
git checkout -- include/ftl/taa.h src/ftl/taa.c src/ftl/gc.c src/ftl/ftl.c
git status   # should be clean

# Verify reproducer still builds against current src/ftl
make -j12 build/bin/ftl_mfc_repro
./build/bin/ftl_mfc_repro --mode legacy --threads 128 --rwmix 70 --duration 5 --lbas 256
# should show same ~7.5% error rate (workload artifact baseline)
```
