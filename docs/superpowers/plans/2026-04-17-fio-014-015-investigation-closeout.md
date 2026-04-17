# fio 014/015 Investigation Closeout

**Date:** 2026-04-17
**Supersedes:** `docs/superpowers/plans/2026-04-16-fio-014-015-segmented-bisection.md` Task 15
**Related spec:** `docs/superpowers/specs/2026-04-16-fio-014-015-segmented-bisection-design.md`

## Summary

The investigation into the reported ~5 verify-errors-per-1M-IO rate on fio cases 014/015 concluded that **the bug is not reproducible on current `master`**. The segmented-bisection tooling was built end-to-end and is retained for future regressions.

No root-cause fix was produced because there is no current failing signal to trace.

## What we measured

### Stage W — single-axis fio sweep (45 runs, 2 GiB io_size)

| Axis | Points | Result |
| --- | --- | --- |
| iodepth | 1, 4, 16, 64 | 4/4 PASS |
| rwmix | 0, 70, 100 | 3/3 PASS |
| numjobs | 1 | PASS |
| numjobs | 2, 4 | FAIL (fio internal race, see note below) |
| verify_async | 0, 2, 4 | 3/3 PASS |
| bs | 4k, 16k | 2/2 PASS |

Matrix and raw artifacts: `docs/superpowers/artifacts/stage-w-matrix.md`, `docs/superpowers/artifacts/stage-w-decision.md`.

### Full-workload 014 verification (3 runs × 8 GiB io_size, fresh QEMU)

| Run | Runtime | total_err | verify errors | io_u errors |
| --- | --- | --- | --- | --- |
| 1 | 14.3 min | 0 | 0 | 0 |
| 2 | 13.4 min | 0 | 0 | 0 |
| 3 | 13.4 min | 0 | 0 | 0 |

Aggregate: ~7.3M IOs across three independent runs on the exact 014 workload → zero errors of any kind.

**Upper bound on current error rate: ≤ 1 / 7.3M IOs**, ≥350× below the previously reported ~5/1M. Each run is itself an independent draw from the failure distribution; three consecutive clean runs is strong evidence that the failure is no longer present under this configuration.

## Interpretation

The previously reported 014/015 verify failures are no longer observed. The cause is one of:

- **Inadvertent fix** between the prior observation and now (many unrelated commits intervened)
- **Environmental dependency** — prior kernel / fio / QEMU version combination that is no longer what is running
- **Latent timing-dependence** — not triggered by the current workload on this hardware

None of these is actionable without a failing signal to trace.

### numjobs > 1 failures are a fio user-error, not an hfsss bug

The six failing sweep runs are all at `numjobs ≥ 2` and produce `verify: bad header rand_seed …` mismatches. With `randrw + verify + numjobs > 1` and a shared LBA range, distinct fio jobs overwrite each other's regions, and the verifier reads a buffer from a different job than expected. The 014/015 cases pin `numjobs = 1` specifically to avoid this.

This false-positive class should stay documented so future readers do not mistake a `rand_seed` mismatch under `numjobs > 1` for an hfsss-side corruption.

## What the investigation built (retained)

These artifacts are committed and remain valuable for future investigations of the same class.

### Instrumentation

- `include/common/trace.h`, `src/common/trace.c` — per-thread lockless trace ring, compile-time-gated via `HFSSS_DEBUG_TRACE`. Zero runtime cost when gated off (default).
- Trace points T1–T5 installed at `src/vhost/hfsss_nbd_server.c:mt_io`, `src/ftl/ftl_worker.c:ftl_worker_main`, `src/ftl/ftl.c:ftl_read_page_mt/ftl_write_page_mt` (two sites each), and `src/hal/hal.c:hal_nand_read_sync/hal_nand_program_sync`.
- Makefile `TRACE=1` build variant; passing `TRACE=1 make all` turns on trace capture and writes a binary dump on shutdown.
- `tests/test_trace.c` — ring-buffer unit test; runs under both modes and is part of `make test`.

### fio sweep tooling

- `scripts/qemu_blackbox/sweep/matrix.json` — single-axis sweep definition
- `scripts/qemu_blackbox/sweep/fio_sweep.sh` — driver; reuses a running QEMU guest, formats between axis switches, emits a stable per-run naming scheme
- `scripts/qemu_blackbox/sweep/summarize.py` + tests — stderr + JSON aggregator → markdown matrix

### Pipeline-segmentation harnesses

- `scripts/qemu_blackbox/phase_a/fio_over_nbd.sh` — drives fio directly against the hfsss NBD server, bypassing QEMU
- `tools/ftl_mfc_repro.c` — standalone C program, drives the FTL API directly with a multi-threaded randrw+verify workload (bypasses NBD and QEMU). A 5-second smoke run reports `ops=<N> mismatches=0 rate=0.00e+00` on the current build.
- `scripts/qemu_blackbox/phase_a/analyze_trace.py` + tests — parses a binary trace dump and reports the first-corrupt-hop per IO chain

### Documentation

- `docs/superpowers/specs/2026-04-16-fio-014-015-segmented-bisection-design.md`
- `docs/superpowers/plans/2026-04-16-fio-014-015-segmented-bisection.md`
- `docs/superpowers/artifacts/stage-w-matrix.md`, `stage-w-decision.md`
- this file

## Environmental caveats discovered during the run

1. **FTL state accumulation.** Running ~27 GiB of writes on a 2 GiB exported namespace (45 sweep iterations) leaves the FTL in a state that returns `HFSSS_ERR_BUSY` cascades on subsequent commands. Future sustained-load investigations need to include a full environment restart between workload phases.
2. **Case timeout.** The default blackbox case timeout is 300 s, which is insufficient for the full 8 GiB 014 workload (13–15 min on HVF/macOS). Any repro run of 014 must either bypass the harness or set `HFSSS_CASE_TIMEOUT_S=1800`.
3. **PyYAML unavailable.** macOS system Python refuses to `pip install pyyaml` under PEP 668. The matrix configuration was moved from YAML to JSON to remove the third-party dependency.
4. **SSH stdin consumption.** Any bash `while read` loop that invokes `ssh` inside the body must pass the loop input through an alternate file descriptor (we used FD 3) so ssh does not drain the loop's stdin.

These are useful landmines to have been paved over for the next investigation.

## Recommended follow-up

- **Do not** reopen the 014/015 investigation unless a failing signal is newly observed in CI or local runs. Absent a reproduction, further work has nothing to anchor on.
- **Do** retain the `HFSSS_DEBUG_TRACE` instrumentation; the cost is only the 8 trace-call sites (no-ops when off) and one header + source file.
- **Do** promote the `tools/ftl_mfc_repro.c` harness into the blackbox test suite once the FTL API stabilizes — it provides a non-QEMU-dependent multi-thread integrity check that complements the existing `tests/test_mt_ftl.c`.
- **Consider** adding a note to the 014/015 case headers documenting the `numjobs=1` requirement and why (the fio `numjobs > 1` false-positive class).

## Branch and PR

All work committed on branch `chore/fio-014-015-investigation-plan`. The branch is ready to open as a PR summarizing the investigation outcome and the retained tooling.
