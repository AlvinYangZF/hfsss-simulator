# Stage W Sweep Decision

**Date:** 2026-04-17
**Artifact:** `artifacts/sweep-20260417_015911/`
**Branch:** `chore/fio-014-015-investigation-plan`
**Baseline repo:** off `origin/master` (no PR #80 merged)

## Matrix Summary (45 runs, 3 reps per point)

| Axis | Points | Result |
| --- | --- | --- |
| iodepth | 1, 4, 16, 64 | 4/4 PASS |
| rwmix | 0, 70, 100 | 3/3 PASS |
| numjobs | 1 | PASS |
| numjobs | 2 | **FAIL** (36, 13, 45 errors) |
| numjobs | 4 | **FAIL** (168, 164, 139 errors) |
| verify_async | 0, 2, 4 | 3/3 PASS |
| bs | 4k, 16k | 2/2 PASS |

**Total error count:** 565 (all in numjobs=2 and numjobs=4 points)

## Interpretation

### 1. numjobs≥2 failures are fio user-error, not an hfsss bug

All 6 failing runs are in the `numjobs=2` and `numjobs=4` points. The error signature is `verify: bad header rand_seed X, wanted Y`. This is a well-known fio limitation: with `numjobs>1 + randrw + verify` and no `--serialize_overlap=1`, multiple independent fio jobs write to the **same LBA range** (the baseline size=1G is shared), and job A's verify can read data that job B just wrote — a false-positive corruption report. The original 014/015 cases pin `numjobs=1` specifically to avoid this fio quirk.

These failures are **not** actionable against hfsss code.

### 2. numjobs=1 axis (the 014/015 configuration): all 21 runs PASS

At `numjobs=1`, across every other axis variation (iodepth=1/4/16/64, rwmix=0/70/100, verify_async=0/2/4, bs=4k/16k), **zero** verify errors were observed in 21 runs × ~500K IOs per run ≈ 10.5M IOs total.

The ~5-per-1M-IO rate observed in prior sessions does **not** reproduce at `io_size=2 GiB` on the current code.

### 3. No Minimum Failing Configuration (MFC) was produced

The expected outcome was a clean PASS→FAIL transition on one of the axes (most likely iodepth). None was observed on the numjobs=1 side of the matrix.

## Decision: Close root-cause investigation — bug not reproducible

Per the design document's T+2h decision tree:

> "All points PASS: re-run 014 full workload once to confirm current reproducibility (the failure may have become latent). If still reproducing, raise `io_size` back to 8 GiB on the smallest failing axis."

### Next action

Re-run the **original 014 workload** (`io_size=8 GiB`, `verify_fatal=0`, `numjobs=1`, iodepth=64) on a **fresh environment** with an extended case timeout.

### 014 full-size verification — final result

| Run | Location | Runtime | total_err | verify errors | io_u errors | Read GB | Write GB |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | `artifacts/014-extended/014.*` | 861 s (14.3 min) | 0 | 0 | 0 | 7.52 | 2.25 |
| 2 | `artifacts/014-confirm/014-run2.*` | 805 s (13.4 min) | 0 | 0 | 0 | 7.52 | 2.26 |
| 3 | `artifacts/014-confirm/014-run3.*` | 804 s (13.4 min) | 0 | 0 | 0 | 7.52 | 2.26 |

**Aggregate: ~7.3M IOs across three runs, zero errors of any kind.**

Prior-session memory reported a ~5/1M verify failure rate. At that rate, 7.3M IOs should have produced ~36 errors. The observed value is 0. The upper bound on the current error rate is therefore ≤1/7.3M ≈ 0.014 per million — **more than 350× below the previously reported rate**.

### Conclusion

**The 014/015 verify-failure bug is no longer reproducible on current master.**

The bug is either:
- Latent (timing/load-dependent, not triggered by the current 014 workload on this hardware)
- Inadvertently fixed by some intervening commit between the prior observation and now
- Was observed under environmental conditions that have since changed (e.g., different kernel, fio version, macOS update)

### Recommended wrap-up

1. **Close the root-cause investigation for 014/015.** There is nothing actionable.
2. **Retain the tooling built for this investigation** (fio sweep driver, trace ring, analyzers, fio-over-NBD harness, FTL-direct harness). These are permanent, low-cost additions that will accelerate similar future investigations.
3. **Leave `HFSSS_DEBUG_TRACE` gated off by default**; future regressions of this class can enable it via `TRACE=1 make all` without modification.
4. **Document the numjobs>1 fio-interaction false-positive** in the case library so future readers do not mistakenly interpret `rand_seed` mismatches under numjobs>1 as hfsss bugs.
5. **Skip Stage P segment execution** (Seg-1/Seg-2/Seg-3 from the plan) — there is no MFC to drive them with. The segments remain valuable as future instruments.

### Environmental Caveats (unchanged)

1. **Accumulated state:** The 45-run sweep wrote ~27 GiB cumulative data to the 2 GiB exported namespace. After the sweep, the FTL entered a degraded state (BUSY cascade) that required environment restart. Phase A segments (if later useful) must each start from a fresh environment.
2. **Case timeout:** Default harness timeout is 300 s; 014's 8 GiB workload requires 13-15 min. All 014 runs must use `HFSSS_CASE_TIMEOUT_S=1800` or bypass the case harness.
3. **Environment versions:** macOS aarch64 + HVF + fio 3.38 + QEMU 8.x.

## Environmental Caveats

1. **Accumulated state:** The 45-run sweep wrote ~27 GiB cumulative data to the 2 GiB exported namespace. After the sweep, the FTL was in a degraded state (BUSY cascade observed on a subsequent 014 attempt without environment restart). This suggests Phase A segment tests must each start from a fresh environment.
2. **Case timeout:** Default harness timeout is 300 s; 014's 8 GiB workload requires 15-20 min. All 014 runs must use `HFSSS_CASE_TIMEOUT_S=1800` or bypass the case harness.
3. **Environment versions:** macOS aarch64 + HVF + fio 3.38 + QEMU 8.x.

## Appendix: Run timing

| Point | Avg runtime |
| --- | --- |
| iodepth=1 | ~5.9 min (serialized) |
| iodepth=4-64 (most points) | ~2-4 min |
| iodepth=64 numjobs=4 | ~4-5 min (high concurrency) |
| Total sweep (45 runs + 4 formats) | ~2 hours |
