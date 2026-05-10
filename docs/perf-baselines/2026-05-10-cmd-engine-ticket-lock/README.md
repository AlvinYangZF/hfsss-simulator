# cmd_engine Ticket-Lock Baseline

**Date**: 2026-05-10
**Branch**: `perf/cmd-engine-ticket-lock` (PR #119, merged)
**Commit**: `e93942c` (squash-merge to master)
**Predecessors**: PR #118 (per-die wait queue + completion-driven dispatch)

## Pre-checkin

| Case | Status |
|------|--------|
| 001_nvme_cli_smoke | PASS |
| 002_nvme_namespace_info | PASS |
| 003_nvme_flush_smoke | PASS |
| 010_fio_randwrite_verify | PASS |
| 011_fio_randrw_verify | PASS |
| 012_fio_seqwrite_verify | PASS |
| 013_fio_trim_verify | PASS |
| 014_fio_pre_checkin_stress | PASS |
| 900_spdk_nvme_identify | SKIP |

## 012_fio_seqwrite_verify — Latency KPI

| Metric | Master baseline | After dispatcher (PR #118) | After ticket lock (PR #119) | Target |
|--------|----------------|---------------------------|---------------------------|--------|
| Write mean lat | 142.0 ms | 141.8 ms | 141.9 ms | ≤ 10 ms |
| Write IOPS | 112 | 112 | 113 | ≥ 1500 |
| Write BW | — | — | 14.8 MB/s | — |
| Write total IO | — | — | 2048 | — |
| Read mean lat | — | — | 9.4 ms | — |
| Read IOPS | — | — | 1697 | — |

## Assessment

The ticket lock did **not** improve write latency. The root cause
identified in PR #118 Section 9.2 (pthread_mutex unfairness) was
incorrect. Both the dispatcher and the FIFO-fair ticket lock together
produce zero measurable change from the original retry-spin baseline
(142.0 → 141.9 ms, within noise).

Read latency (9.4 ms mean, 1697 IOPS) is healthy, confirming the
bottleneck is specific to the NAND write path (tProg ~1.3 ms per
page × die contention), not the NBD/channel-worker layer.

The actual bottleneck remains undiagnosed. Candidates:
- FTL write amplification or GC interference during seqwrite
- Die-level contention that the dispatcher cannot resolve (all dies
  busy, waiter must wait for tProg regardless of queue ordering)
- Channel-level serialization that outlives lock fairness
- NBD server overhead or fio configuration

## Soak Test (qemu-blackbox-soak, 2 rounds)

| Round | Result |
|-------|--------|
| 1 | 7/7 PASS |
| 2 | 7/7 PASS |

## make test

All 1,750+ assertions pass. No regressions.
