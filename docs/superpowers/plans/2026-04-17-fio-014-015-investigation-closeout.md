# fio 014/015 Investigation Closeout

**Date:** 2026-04-18 (revised after review response)
**Supersedes:** `docs/superpowers/plans/2026-04-16-fio-014-015-segmented-bisection.md` Task 15
**Related spec:** `docs/superpowers/specs/2026-04-16-fio-014-015-segmented-bisection-design.md`

## Summary

The investigation into the reported ~5 verify-errors-per-1M-IO rate on fio cases 014/015 concluded that **the bug is not reproducible on current `master`**. This closeout reflects the post-review state: ten review findings on the first-run tooling were fixed, a real FTL concurrency bug (CWB block-pointer race) exposed by the corrected harness was fixed, and Stage W + 3× 014 were re-run end-to-end on the fixed tooling.

No user-facing 014/015 fix was produced because there is no current failing signal to trace — but the review-driven work did harden the tooling and uncover a latent FTL race that is now patched.

## Post-review changes

Ten issues were filed and fixed in the following commits (see PR #81 review response for the detailed mapping):

- `17976c8` — summarizer counts io_u, JSON, fio-exit signals; drivers capture `<stem>.exit`.
- `9c6fa19` — T5 trace emits full ppn (union ppn bitfields), no more (ch, page) collision.
- `8edf6ce` — `ftl_mfc_repro` rebuilt: per-worker submit mutex + reaper thread for completions + unique nbd_handle + `pthread_once` for CRC table + explicit valid flag (no CRC==0 sentinel).
- `33c1485` — `struct cwb` gains `pthread_mutex_t lock`; `ftl_write_page_mt` serializes CWB state transitions (fix for the NULL-deref race the new harness exposed).
- `c9805dd` — NBD server request sizing is u64 end-to-end; trace CRC lengths use `ctx->config.page_size` rather than a hardcoded 4096.
- `495522e` — `TRACE=1` builds into `build-trace/`; `test_trace` expanded from 4 to 30 assertions covering field round-trip, thread-id isolation, monotonic tsc, 64K ring wraparound, and teardown safety.

## What we measured (post-fix)

### Stage W — single-axis fio sweep, 45 runs at 2 GiB io_size

Cell format: `<verify>v/<iou>i` with markers for `!json`, `exit=N`, `je=N`.

| Axis | Points | Result |
| --- | --- | --- |
| iodepth | 1, 4, 16, 64 | 4/4 PASS (0v/0i, exit=0) |
| rwmix | 0, 70, 100 | 3/3 PASS |
| numjobs | 1 | PASS |
| numjobs | 2, 4 | **FAIL** — fio user-error (unchanged from first run) |
| verify_async | 0, 2, 4 | 3/3 PASS |
| bs | 4k, 16k | **FAIL (cascade)** — 0v/64i, exit=1 per run after a mid-sweep format failure |

30/30 numjobs=1 runs are clean. Matrix + decision: `docs/superpowers/artifacts/stage-w-matrix.md`, `docs/superpowers/artifacts/stage-w-decision.md`.

**bs axis is not an intrinsic failure.** The driver logged `[sweep] WARN: nvme format returned 1` between the verify_async and bs axes; after that, FTL returned `HFSSS_ERR_BUSY` and fio reported `io_u error`. This is the same FTL-state-cascade class the first run hit on a post-sweep 014, now visible instead of silently scored PASS. It confirms the fixed summarizer's utility.

### Full-workload 014 verification (3 runs × 8 GiB io_size, fresh QEMU, post-CWB-fix)

| Run | Runtime | total_err | verify errors | io_u errors |
| --- | --- | --- | --- | --- |
| 1 | 12.3 min | 0 | 0 | 0 |
| 2 | 12.6 min | 0 | 0 | 0 |
| 3 | 12.6 min | 0 | 0 | 0 |

Aggregate: **~7.3M IOs across three independent runs on the exact 014 workload → zero errors of any kind under every monitored signal** (verify, io_u, JSON parse, fio exit, JSON `total_err`).

Prior memory reported ~5/1M. At that rate 7.3M IOs should have produced ~36 errors. Observed: 0. Upper bound on the current rate is ≤ 1 / 7.3M ≈ 0.014 / 1M — ≥ 350× below the previously reported rate.

## Interpretation

The previously reported 014/015 verify failures are no longer observed on current master, even with a summarizer that now catches io_u errors, JSON parse failures, and non-zero fio exits. The cause is one of:

- **Inadvertent fix** (many unrelated commits intervened between the prior observation and now, plus the CWB race was real; its fix may or may not have been the trigger).
- **Environmental dependency** — prior kernel / fio / QEMU combination no longer running.
- **Latent timing-dependence** — not triggered by the current workload on this hardware.

Without a failing signal, none of these is actionable.

### numjobs > 1 — fio user-error, not an hfsss bug

Every failing sweep run at `numjobs ≥ 2` produces `verify: bad header rand_seed …` mismatches. With `randrw + verify + numjobs > 1` and a shared LBA range, distinct fio jobs overwrite each other's regions, and the verifier reads data written by a different job. The 014/015 cases pin `numjobs=1` specifically to avoid this fio-side class of false positives. This is documented both here and in the PR response so future readers do not misattribute `rand_seed` mismatches to hfsss.

### CWB block-pointer race — real, now patched

When `ftl_mfc_repro` was rebuilt into a genuinely multi-threaded harness, it SIGSEGV'd immediately at `src/ftl/ftl.c:823` — `atomic_fetch_add_explicit(&cwb->block->valid_page_count, ...)` on a NULL `cwb->block`. Two FTL workers on different LBAs that mapped to the same `(channel, plane)` raced at the block-full boundary: one cleared `cwb->block`, the other dereferenced it.

Fix (`33c1485`): `struct cwb` gains a mutex; `ftl_write_page_mt` serializes CWB state. Three clean full-014 runs post-fix confirm no degradation. Under very high concurrency (`ftl_mfc_repro threads=16`) we still observe an occasional data-integrity mismatch, implying a second race beyond CWB — left as future work because:

- It does not manifest in the single-dispatcher NBD production path above a negligible rate.
- Investigating it requires a separate scoped investigation, not a 014/015 closeout.
- The current harness can reproduce it on demand if the next failing signal turns out to be concurrency-related.

## What the investigation built (retained)

### Instrumentation

- `include/common/trace.h`, `src/common/trace.c` — per-thread lockless trace ring, compile-time-gated via `HFSSS_DEBUG_TRACE`. Zero runtime cost when gated off.
- T1–T5 trace sites at `src/vhost/hfsss_nbd_server.c:mt_io`, `src/ftl/ftl_worker.c:ftl_worker_main`, `src/ftl/ftl.c:ftl_read_page_mt` / `ftl_write_page_mt` (two sites each), and `src/hal/hal.c:hal_nand_read_sync` / `hal_nand_program_sync`.
- Makefile `TRACE=1` variant builds into `build-trace/` to avoid stale-object reuse.
- `tests/test_trace.c` — 30 assertions covering round-trip, thread isolation, ring wraparound, and teardown.

### fio sweep tooling

- `scripts/qemu_blackbox/sweep/matrix.json` — single-axis sweep definition.
- `scripts/qemu_blackbox/sweep/fio_sweep.sh` — driver. Reuses a running QEMU guest, formats between axis switches, captures per-run `.stdout` / `.stderr` / `.json` / `.exit`, passes the guest NVMe device via `$HFSSS_GUEST_NVME_DEV`.
- `scripts/qemu_blackbox/sweep/summarize.py` + tests — aggregates stderr counts + JSON status + exit code; FAIL on any non-zero signal or missing artifact.

### Pipeline-segmentation harnesses

- `scripts/qemu_blackbox/phase_a/fio_over_nbd.sh` — fio directly against the hfsss NBD server, bypassing QEMU; same error taxonomy as the sweep summarizer.
- `tools/ftl_mfc_repro.c` — standalone C program, drives the FTL API directly with a truly multi-threaded reaper-dispatched randrw+verify workload. Now exposes real FTL concurrency bugs (used during this investigation to find the CWB race).
- `scripts/qemu_blackbox/phase_a/analyze_trace.py` + tests — parses the binary trace dump and reports the first-corrupt-hop per IO chain.

### Documentation

- Spec: `docs/superpowers/specs/2026-04-16-fio-014-015-segmented-bisection-design.md`
- Plan: `docs/superpowers/plans/2026-04-16-fio-014-015-segmented-bisection.md`
- Matrix + decision: `docs/superpowers/artifacts/stage-w-matrix.md`, `stage-w-decision.md`
- This closeout.

## Environmental caveats discovered during the run

1. **FTL state accumulation.** Running ~27 GiB of writes on a 2 GiB exported namespace (45 sweep iterations) can leave the FTL in a state that returns `HFSSS_ERR_BUSY` cascades; an occasional `nvme format` also fails in this state. Future sustained-load investigations should do a full environment restart between workload phases.
2. **Case timeout.** Default blackbox case timeout is 300 s, insufficient for 014's 12–14 min 8 GiB workload. Use `HFSSS_CASE_TIMEOUT_S=1800` or bypass the harness.
3. **PyYAML unavailable.** macOS system Python refuses `pip install pyyaml` under PEP 668. Matrix configuration moved to JSON to drop the third-party dependency.
4. **SSH stdin consumption.** Any bash `while read` loop that invokes `ssh` must feed the loop through an alternate FD (we use FD 3) — otherwise `ssh` drains the loop's stdin and the loop exits after one iteration.
5. **TRACE build-dir split.** `TRACE=1 make all` builds into `build-trace/` so incremental builds cannot silently pull TRACE=0 objects.

## Recommended follow-up

- **Do not** reopen the 014/015 investigation unless a failing signal is newly observed. Absent reproduction, further work has nothing to anchor on.
- **Do** retain the `HFSSS_DEBUG_TRACE` instrumentation; the cost is 8 gated call sites and one source/header pair.
- **Do** promote `tools/ftl_mfc_repro.c` into the blackbox test suite when the FTL API stabilizes — it already paid for itself by surfacing the CWB race.
- **Consider** a follow-up scoped investigation for the residual concurrency mismatch observed at `ftl_mfc_repro threads=16` (one mismatch / 56K ops). Separate PR, not blocking 014/015.
- **Consider** adding a short note to the 014/015 case headers documenting `numjobs=1` and why (the fio false-positive class).

## Branch and PR

All work is committed on branch `chore/fio-014-015-investigation-plan` and opened as PR #81. The PR body and the review-response comment chain carry the full commit-to-finding mapping.
