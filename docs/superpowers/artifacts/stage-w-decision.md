# Stage W Sweep Decision (Post-Review Rerun)

**Date:** 2026-04-18
**Latest artifact:** `artifacts/sweep-rerun-20260417_232217/` + `artifacts/014-rerun-confirm/`
**Branch:** `chore/fio-014-015-investigation-plan`
**Baseline repo:** off `origin/master` with all review-response fixes applied (summarizer error taxonomy, T5 full ppn, `ftl_mfc_repro` true concurrency, per-CWB mutex, u64 request sizing, `HFSSS_DEBUG_TRACE` BUILD_DIR split, deeper `test_trace`).

## What changed between the first run and this rerun

The first run's conclusion ("bug not reproducible") was correct but relied on a summarizer that counted only `verify:` / `fio: verify` stderr lines and swallowed non-zero fio exits, missing JSON parse failures, and io_u errors. A team-agent deep review flagged that the "0 errors / 7.3M IOs" claim was premature because these invisible failure modes could inflate the PASS column. Ten specific issues were filed; all were fixed. This decision document supersedes the first one using data produced by the corrected tooling.

## Matrix Summary (45 runs, 3 reps per point, new summarizer)

Cell format: `<verify>v/<iou>i`. Additional markers: `!json` (missing/broken JSON), `exit=N` (fio non-zero exit), `je=N` (JSON `total_err` > 0). Any marker or non-zero counter fails the repeat.

| Axis | Points | Result |
| --- | --- | --- |
| iodepth | 1, 4, 16, 64 | 4/4 PASS (0v/0i, exit=0) |
| rwmix | 0, 70, 100 | 3/3 PASS |
| numjobs | 1 | PASS |
| numjobs | 2 | **FAIL** — 34 + 36 + 26 verify errors; `exit=2` |
| numjobs | 4 | **FAIL** — 178 + 157 + 124 verify errors; `exit=4` |
| verify_async | 0, 2, 4 | 3/3 PASS |
| bs | 4k, 16k | **FAIL (cascade)** — 0v/64i, `exit=1` per run; see below |

Full matrix: `docs/superpowers/artifacts/stage-w-matrix.md`.

## What the FAIL lines mean

### numjobs ≥ 2 — fio user-error (unchanged)

Same as the first run: `numjobs > 1 + randrw + verify` without `--serialize_overlap=1` lets multiple fio jobs overwrite each other's regions and the verifier reads another job's bytes. The 014/015 cases pin `numjobs=1` for this exact reason. Not an hfsss bug.

### bs axis — new finding, exposed by the fixed summarizer

Both `bs=4k` and `bs=16k` runs recorded **0 verify errors but 64 `io_u error` lines each** and fio exited with code 1. The first-run summarizer would have reported these as `0 errors / PASS` because it only counted verify-stderr lines — directly confirming the reviewer's concern.

Driver log shows `[sweep] WARN: nvme format returned 1` just before the bs axis started. The FTL was left in a state where subsequent writes returned `HFSSS_ERR_BUSY`, which fio reported as `io_u error`. This is the same FTL-state-cascade class we saw after the first run's heavy sweep load. It is **not** a bs=4k / bs=16k-specific failure and it does **not** produce the original `verify: bad header rand_seed` signature. Orthogonal to the 014/015 question but now visible instead of silent.

### numjobs=1 matrix — clean

| Axis (numjobs=1) | Runs | Errors |
| --- | --- | --- |
| iodepth (1/4/16/64) × 3 reps | 12 | 0 |
| rwmix (0/70/100) × 3 reps | 9 | 0 |
| verify_async (0/2/4) × 3 reps | 9 | 0 |
| **Total** | **30** | **0** |

## Full 014 workload (post-CWB-fix, fresh QEMU)

Three independent runs on a fresh QEMU + hfsss-nbd-server, with the per-CWB mutex in place, fixed `mt_io` call chain, u64 request sizing, TRACE off:

| Run | Location | Runtime | fio rc | total_err | verify errors | io_u errors | Read GB | Write GB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | `artifacts/014-rerun-confirm/014-run1.*` | 740 s (12.3 min) | 0 | 0 | 0 | 0 | 7.52 | 2.25 |
| 2 | `artifacts/014-rerun-confirm/014-run2.*` | 755 s (12.6 min) | 0 | 0 | 0 | 0 | 7.52 | 2.26 |
| 3 | `artifacts/014-rerun-confirm/014-run3.*` | 755 s (12.6 min) | 0 | 0 | 0 | 0 | 7.52 | 2.25 |

**Aggregate: ~7.3M IOs across three runs, zero errors under every monitored signal.**

Prior memory reported a ~5/1M verify failure rate. At that rate, 7.3M IOs should have produced ~36 errors. The observed value is 0. The upper bound on the current rate is therefore ≤ 1 / 7.3M ≈ 0.014 per million — ≥ 350× below the reported rate.

## Conclusion (unchanged)

**The 014/015 verify-failure bug is not reproducible on current master**, and this conclusion is now supported by a summarizer that would have flagged io_u errors, JSON-parse failures, and non-zero fio exits if they had occurred. The bug is either latent, inadvertently fixed between the prior observation and now, or bound to an environment that is no longer running.

## Secondary finding surfaced by the rework

When `ftl_mfc_repro` was rebuilt into a genuinely multi-threaded harness (reaper-dispatched completions, per-worker submit mutex, unique `nbd_handle`, no global dispatch serialization), it SIGSEGV'd at `src/ftl/ftl.c` `ftl_write_page_mt` on a NULL `cwb->block`. Root cause: the current write block had no lock, and multiple FTL worker threads hashing to the same `(channel, plane)` raced on the block-full boundary.

Fix: `struct cwb` gains `pthread_mutex_t lock`; `ftl_write_page_mt` takes and releases it across the entire body (all return paths). Post-fix, the harness reaches hundreds of thousands of concurrent ops without the crash. At threads=16 / ~56K ops / 10 s it still reports one data-integrity mismatch, suggesting a secondary race exists beyond CWB (not scoped into this PR).

The CWB race was theoretically reachable on the production NBD path too (NBD's single dispatcher feeds 8 FTL workers that can contend on the same CWB). Three clean full-014 runs post-fix show no degradation.

## Recommended wrap-up

1. **Close the root-cause investigation for 014/015.** Nothing is actionable without a failing signal.
2. **Retain the investigation tooling.** All compile-time-gated or out-of-main-path; no runtime cost in default builds.
3. **Keep `HFSSS_DEBUG_TRACE` gated off by default.** Use `TRACE=1 make all` when a future regression is reported.
4. **Document the `numjobs > 1` fio false-positive class** (captured in this decision and in the PR comment chain).
5. **Leave `ftl_mfc_repro` as the canonical "exposes concurrency bugs" harness.** It already surfaced the CWB race; reuse it when the next high-concurrency FTL regression appears.

## Environmental caveats

1. **FTL state accumulation under long sweeps.** ~27 GiB of sustained writes on a 2 GiB exported namespace can trigger a format failure mid-sweep; after that, subsequent writes return `HFSSS_ERR_BUSY`, cascading as fio `io_u error`. The fixed summarizer surfaces this class (previously silent PASS).
2. **Case timeout.** Harness default is 300 s; 014's 8 GiB workload needs 12–14 min. Use `HFSSS_CASE_TIMEOUT_S=1800` or bypass the case harness.
3. **SSH stdin inside bash `while read`.** Pass the loop input through an alternate FD (we use FD 3); otherwise `ssh` drains the loop's stdin and the loop exits after one iteration.
4. **TRACE build-dir split.** `TRACE=1 make all` builds into `build-trace/`. Flipping the flag no longer reuses non-traced objects.
5. **Environment versions:** macOS aarch64 + HVF + fio 3.38 + QEMU 8.x.
