# Segmented Bisection Plan for fio 014/015 Verify Failures

**Date:** 2026-04-16
**Repo baseline:** `origin/master` (PR #80 `fix/fio-stress-verify-race` may or may not be merged at execution time; the plan does not depend on it)
**Scope:** Root-cause investigation and fix for the ~5 per 1M IO data-verification failures observed in `scripts/qemu_blackbox/cases/fio/014_fio_pre_checkin_stress.sh` and `scripts/qemu_blackbox/cases/fio/015_fio_high_iodepth_verify.sh` under the default `mt` and async (`-a`) NBD modes.
**Non-goals:**

- Not redesigning NBD or FTL architecture
- Not investigating the separate sync-mode guest crash (different symptom class)
- Not accepting an fio-configuration workaround as the primary fix (root cause first)

---

## 1. Summary

This design lays out a two-stage bisection to locate and fix the cause of rare verify failures in the pre-checkin fio stress tests.

**Stage W — Workload Sweep:** a ~2-hour single-axis fio parameter scan that narrows the observed failure to a **Minimum Failing Configuration (MFC)** and a failure-rate matrix.

**Stage P — Pipeline Segmentation:** four segment tests (driven by the MFC) that isolate the failing end-to-end layer across `guest fio → kernel → QEMU → NBD → FTL worker → TAA/CWB → media`, and a compile-time trace instrumentation that pinpoints the code path.

The work produces a signed root cause, a focused fix, and a new permanent regression unit test that reproduces the MFC without QEMU or fio.

## 2. Current State

### 2.1 Observed failure

- Default and async NBD modes: ~5 verify errors per ~1M 4 KiB IOs (0.0005% rate)
- Error form: `verify: bad magic header`, `verify: hdr_fail`, rand_seed mismatch
- Sync mode: guest kernel crashes before any IO completes (out of scope, different bug class)
- Workload: `randrw` 70/30, 4 KiB bs, iodepth=64, numjobs=1, crc32c verify, direct=1 libaio

### 2.2 Previously tried and excluded

- `--serialize_overlap=1` added to both cases — no effect on failure rate
- `--verify_async` removed — no effect
- GC relocation vs. concurrent host-write race — PR #80 adds `taa_update_if_equal` CAS and fixes a real bug of this class, but GC does not trigger in 014/015's topology (32 GiB raw / 512 MiB exported → 64× over-provisioning keeps free blocks well above `gc_threshold`)

### 2.3 Remaining suspicion (for later confirmation, not assumptions)

- Current Write Block (CWB) concurrency after the FTL worker pool replaced the single global FTL lock
- NBD async SQ/CQ completion ordering under iodepth=64
- FTL worker LBA-hash routing across TAA shard boundaries

## 3. Stage W — Workload Sweep

### 3.1 Goal

Produce a reproducible **MFC** and a single-axis failure-rate matrix that discriminates between candidate causes before instrumenting the codebase.

### 3.2 Baseline workload

Per-run fio configuration (each sweep point mutates exactly one parameter):

```
--rw=randrw --rwmixread=70 --bs=4k --direct=1 --ioengine=libaio
--iodepth=64 --numjobs=1 --size=1G --io_size=2G
--verify=crc32c --verify_fatal=0 --verify_async=4 --do_verify=1
--randrepeat=0 --name=sweep
```

Key deviations from the 014 case:

- `io_size` 8 GiB → 2 GiB: fits each run to ~2 min; at 0.0005% failure rate, ~500K IOs × 0.0005% ≈ 2–3 errors expected per failing run (detectable above 0-error pass configs)
- `verify_fatal` 1 → 0: run completes regardless of first error, enabling error-count statistics across the sweep

### 3.3 Sweep axes

Single-axis scans (hold other parameters at baseline). Total 15 unique points × 3 repeats = 45 runs.

| Axis           | Points                | Hypothesis                                                                 |
| -------------- | --------------------- | -------------------------------------------------------------------------- |
| `iodepth`      | 1, 4, 16, 64          | Primary suspect; low iodepth passing implies concurrency/completion causal |
| `rwmix`        | 0 (pure write), 70, 100 (pure read) | Pure-read pass vs. mixed fail narrows to write path or read-after-write    |
| `numjobs`      | 1, 2, 4               | numjobs>1 not worsening rules out fio cross-job pseudo-failure             |
| `verify_async` | 0, 2, 4               | `=0` still failing rules out fio verify-thread race                        |
| `bs`           | 4k, 16k               | 16k passing implies 4 KiB / page-aligned granularity specificity           |

### 3.4 Detection

Each run produces a `.json` (fio `--output-format=json`) and a `.stderr`. An error count per run is computed from:

- **Primary:** `grep -cE '^(verify|fio: verify)' <run>.stderr` — fio emits one stderr line per verify failure regardless of `verify_fatal`.
- **Cross-check:** fio JSON `jobs[0].error` (non-zero = at least one error) and `jobs[0].total_err` (sum).
- **Exit code:** non-zero fio exit is a further sanity indicator.

Per-point status (aggregated over 3 repeats):

| Repeats with errors | Status    |
| ------------------- | --------- |
| 0 / 3               | PASS      |
| 1 / 3               | FLAKY     |
| 2 / 3               | SUSPECT   |
| 3 / 3               | FAIL      |

### 3.5 Execution

- Reused QEMU instance for the full sweep (`run.sh --reuse-env --keep-env`)
- `nvme format /dev/nvme0n1 -s 0 -f` only between **axis switches** (5 formats total; same-axis points and 3-repeat cycles share state deliberately)
- Serial execution, one run at a time; full machine dedicated
- Expected total wall-clock: 45 × ~2 min + 5 × ~30 s formats + overhead ≈ **2 hours**

### 3.6 Decision at T+2h

- **Clean monotonic signal** (e.g., `iodepth ≤ 16 PASS, iodepth = 64 FAIL`, `rwmix = 100 PASS`): MFC = baseline + minimal failing axis point. Proceed to Stage P.
- **All points PASS:** re-run 014 full workload once to confirm current reproducibility (the failure may have become latent). If still reproducing, raise `io_size` back to 8 GiB on the smallest failing axis.
- **All points FAIL:** baseline is more fragile than assumed. Lower baseline (`iodepth=8`, `io_size=512m`) and re-scan.
- **Noisy (many FLAKY):** add 3 more repeats per point (6 total) to regain statistical power.

Decision point is autonomous (agreed up front); re-check only needed if scope fundamentally changes.

### 3.7 Stage W deliverables

- `scripts/qemu_blackbox/sweep/fio_sweep.sh` — driver (single QEMU, loops axis × points × repeats)
- `scripts/qemu_blackbox/sweep/matrix.yml` — 15-point definition
- `scripts/qemu_blackbox/sweep/summarize.py` — stderr + JSON aggregator → markdown matrix
- `artifacts/sweep-<ts>/matrix.md` — per-point status table
- `artifacts/sweep-<ts>/decision.md` — MFC + next-step decision

## 4. Stage P — Pipeline Segmentation

### 4.1 Goal

Given the MFC, isolate which segment of the `guest → QEMU → NBD → FTL → media` path contains the failing code path, and narrow to function/code-region granularity.

### 4.2 Segment layout and execution order

Segments executed in the order below; each segment's outcome directs the next.

```
Seg-4 (UT regression) ── sanity ──→ all pass (expected)
       │
       ↓
Seg-1 (fio-over-NBD)  ── bypass QEMU/kernel/fio stack ──→
       ├── PASS: bug is in layers 1–4 (QEMU/kernel/fio), mark out-of-scope
       └── FAIL: bug is in hfsss stack (layers 5–8)
              │
              ↓
       Seg-2 (FTL direct API) ── bypass NBD ──→
              ├── PASS: bug is in NBD layer (layer 5)
              │    └── Seg-3 trace analysis focuses on NBD → FTL dispatch
              └── FAIL: bug is in FTL + media (layers 6–8)
                   └── Seg-3 trace analysis focuses on FTL write path
```

### 4.3 Seg-4: unit test regression

- Command: `make test`
- Pass criterion: all existing unit tests green (no regression from current master)
- Purpose: sanity baseline — if any UT regresses, fix that first and restart Stage P

### 4.4 Seg-1: fio-over-NBD harness

- Tool: host-side `fio --ioengine=nbd --uri=nbd://127.0.0.1:<port>` directly against the running hfsss NBD server
- Workload: exact MFC parameters; same `verify_fatal=0` + stderr-count detection as Stage W
- Repeat: 3× for consistency
- Script: `scripts/qemu_blackbox/phase_a/fio_over_nbd.sh`
- Effort: ~30 min
- Interpretation:
  - PASS ⇒ failure requires the QEMU or guest-kernel path; out of scope for this work
  - FAIL with comparable error rate ⇒ layers 5–8 (hfsss) own the bug; proceed to Seg-2

### 4.5 Seg-2: FTL direct API harness

- Tool: new C program `tools/ftl_mfc_repro.c`
- Design:
  - Initializes FTL with the same configuration as the NBD server
  - Spawns N threads (N = MFC iodepth; e.g., 64), each maintaining a local queue
  - Each thread loops: pick `randrw`-weighted op, pick random LBA within the exported range, either `ftl_write` with a unique data pattern or `ftl_read` + local CRC compare
  - Writer threads record `(lba, sha256(data))` into a shared concurrent map (mutex-protected, plain hash table is sufficient)
  - Reader threads verify read data's SHA256 equals the stored hash; mismatch ⇒ log `(lba, expected, got, thread_id, op_count)` and increment `mismatch_count`
- Pass criterion: `mismatch_count == 0` over total ~2 min of ops
- Effort: ~3 hours (templates exist under `tests/test_ftl.c`)
- Semantic note: not a 1:1 reproduction of fio's verify semantics (fio uses its own header format + randrepeat-driven LBA seeding). For the current debug goal, semantic-approximate is acceptable because we are looking for **data integrity violations**, not for reproducing fio's exact IO pattern.
- Interpretation:
  - PASS ⇒ bug is in NBD layer (between `recv_request` and dispatch to FTL); proceed to Seg-3 with NBD focus
  - FAIL ⇒ bug is in FTL + media; proceed to Seg-3 with FTL-write focus

### 4.6 Seg-3: FTL trace comparator

#### 4.6.1 Instrumentation framework

- New files: `src/common/trace.h`, `src/common/trace.c`
- Build gating: enabled when compiled with `-DHFSSS_DEBUG_TRACE=1` (default `0` — zero runtime cost when off)
- Backend: per-core lockless ringbuffer with fixed-size records, no blocking on record emit; dump-on-shutdown serializes all buffers to a binary file
- Rationale: in-memory per-core ringbuffer is the only design that avoids perturbing the timing behavior we are trying to observe (printf to file would mask the race)

#### 4.6.2 Trace points (writes and reads)

| ID  | Location                                                     | Fields                                                           |
| --- | ------------------------------------------------------------ | ---------------------------------------------------------------- |
| T1  | NBD server: just after parsing a WRITE/READ request          | `op, lba, len, crc32c(data_in), tsc`                             |
| T2  | FTL worker: just after dequeuing the request (LBA→worker)    | `op, lba, worker_id, tsc`                                        |
| T3  | FTL mapping: after TAA lookup/insert, PPN determined         | `lba, ppn, shard_id, tsc`                                        |
| T4  | HAL submission: just before `media_nand_program` / read      | `ppn, crc32c(data_to_media), tsc`                                |
| T5  | HAL completion: after `media_nand_*` returns                 | `ppn, crc32c(data_from_media_or_written), status, tsc`           |

#### 4.6.3 Analysis

- Script: `scripts/qemu_blackbox/phase_a/analyze_trace.py`
- Algorithm:
  - Reconstruct each `(op, lba)` chain by joining T1..T5 on `op` + `lba` + `tsc` proximity
  - For writes: expect `T1.crc == T4.crc` (data not corrupted en route to media)
  - For reads: expect `T5.crc` of read op equals `T4.crc` of the matching prior write for the same LBA (media returned the bytes that were written)
  - Emit the **first mismatch hop** for each failing IO chain (`T1→T2` vs `T2→T3` vs `T3→T4` vs `T4→T5` vs cross-op read-vs-write)
- Single run is sufficient: at ~2-3 errors per 2-minute run, even one run produces enough signal to locate the mismatch hop

#### 4.6.4 Effort

~4 hours (ringbuffer infra + 5 instrumentation sites + Python analyzer)

### 4.7 Stage P deliverables

- `scripts/qemu_blackbox/phase_a/fio_over_nbd.sh`
- `scripts/qemu_blackbox/phase_a/analyze_trace.py`
- `tools/ftl_mfc_repro.c`
- `src/common/trace.{h,c}` + instrumentation call-sites in FTL and NBD code
- `artifacts/phase-a-<ts>/seg{1,2,3,4}-results.md`
- `artifacts/phase-a-<ts>/final-decision.md`

## 5. Root Cause Mapping

When Seg-3 identifies the first corrupt hop, the following table guides the initial investigation:

| First-corrupt hop                 | Candidate code area                                     | Candidate root-cause class                                                                       |
| --------------------------------- | ------------------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| T1 → T2                           | NBD server + worker dispatch                            | IOV/buffer lifetime across async boundary; multi-worker completion reordering                    |
| T2 → T3                           | FTL worker entry, CWB allocation                        | CWB PPN allocation race; TAA shard-edge contention                                               |
| T3 → T4                           | FTL mapping, HAL glue                                   | Data buffer aliasing; PPN encoding error; copy before write                                      |
| T4 → T5                           | Media / NAND command engine                             | Plane/channel scheduling collision; page overwrite in cache path                                 |
| Read vs prior write (cross-op T5) | L2P persistence or GC                                   | Stale L2P from format residual; WAL replay; GC (unlikely here, PR #80 covered the main class)    |

This table is a starting point — the fix must be derived from trace evidence, not assumed.

## 6. Fix and Regression Strategy

### 6.1 Fix commit

- Branch: new `fix/fio-014-015-root-cause` off current `master` (or post-PR-#80 master). Independent of PR #80 so that PR #80's review is not diluted.
- Commit granularity: one `fix:` commit focused on the single root cause (no refactors bundled)
- Commit body cites the trace-analysis artifact

### 6.2 Regression unit test

The failing segment harness is promoted to a permanent unit test:

| Failing segment        | New or extended UT                              | Workload                                               |
| ---------------------- | ----------------------------------------------- | ------------------------------------------------------ |
| NBD                    | `tests/test_nbd_concurrent.c` (new)             | Multi-threaded direct NBD client; small randrw+verify  |
| FTL worker / CWB       | `tests/test_ftl_mfc.c` (promoted from Seg-2)    | N-thread concurrent write/read; SHA256 verify          |
| TAA (shard-edge)       | `tests/test_taa.c` (extended)                   | LBA stressing shard boundary under concurrent ops      |
| Media                  | `tests/test_media.c` (extended)                 | Multi-plane/channel concurrent program + read-back     |

All new/extended UTs must:

- Run in under 60 seconds under `make test`
- Reproduce the MFC's core invariant at small scale (10–100× smaller than real workload)
- Not depend on QEMU, fio, or NBD protocol externally (pure C / pthread)

### 6.3 Instrumentation retention

- Keep `HFSSS_DEBUG_TRACE` gated instrumentation in `master` (default off → zero runtime cost)
- Rationale: write-once, reusable for future similar investigations; low maintenance cost

## 7. Deliverables and Branch Strategy

```
docs/superpowers/specs/
  └── 2026-04-16-fio-014-015-segmented-bisection-design.md   [this spec]

scripts/qemu_blackbox/sweep/                                 [stage W tooling]
  ├── fio_sweep.sh
  ├── matrix.yml
  └── summarize.py

scripts/qemu_blackbox/phase_a/                               [stage P tooling]
  ├── fio_over_nbd.sh
  └── analyze_trace.py

tools/
  └── ftl_mfc_repro.c

src/common/trace.{h,c}                                       [trace infra]
src/ftl/*.c, src/vhost/*.c                                   [trace sites, guarded]

tests/
  ├── test_ftl_mfc.c                                         [promoted regression]
  └── test_*.c (extended by root-cause segment)

artifacts/sweep-<ts>/                                        [run output]
artifacts/phase-a-<ts>/                                      [run output]
```

### Commit and PR sequencing

1. Spec commit (this document) on a planning branch (`chore/fio-014-015-investigation-plan` recommended)
2. Stage W tooling commit (scripts only, no main-code changes)
3. Stage P tooling commit (trace infra + harness + analyzer; compile-time-gated, no behavior change when `HFSSS_DEBUG_TRACE=0`)
4. Root-cause fix commit + regression UT commit → new PR, independent of PR #80

## 8. Out of Scope

- Sync-mode guest crash (different symptom class; separate investigation)
- Any redesign of the NBD or FTL threading model beyond a localized fix
- CI integration of the new tooling (manual-invocation only; may follow later if useful)
- Performance optimization of the trace infrastructure (correctness-first; it is compile-time-off by default)

## 9. Success Criteria

- Stage W produces a documented MFC and matrix
- Stage P identifies the failing segment with a trace-evidenced root cause
- A fix commit addresses the root cause; the fix is under ~100 LOC if possible (a focused bug, not a redesign)
- A new regression UT reproduces the MFC invariant and fails on pre-fix code, passes on post-fix code
- Running `014_fio_pre_checkin_stress.sh` on the fixed code yields 0 verify errors across 3 independent runs at the original 8 GiB `io_size`
