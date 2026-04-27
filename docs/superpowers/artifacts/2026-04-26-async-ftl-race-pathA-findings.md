# Path A Re-baseline — Findings

**Status:** Path A Step 3 done. Filter works. Phase 2 hypothesis (NOENT-from-gc.c) was right that NOENT is warmup-only — but a different post-warmup signal (`HFSSS_ERR_BUSY`) is now visible.

## What changed in the reproducer

`tools/ftl_mfc_repro.c` now snapshots `g_crc_map[lba].valid` under the
stripe lock at submit time (legacy `worker_fn` and dispatcher producer
both). Read errors are only counted when the snapshot was `valid==true`
— i.e. the LBA had already been written by some prior worker. Writes
remain unconditionally counted.

Rationale: a read on a never-written LBA returns NOENT not because of a
race but because the FTL has no L2P mapping yet. Counting that as
"error" was conflating workload-startup behavior with race signal and
hid the actual post-warmup error pattern.

## Re-baseline result (legacy, 128t, rwmix=70, 10s, 256 LBAs)

| Metric | Pre-filter (Phase 1.3 baseline `f1c112c`) | Post-filter (this run) |
|---|---|---|
| ops | ~95,740 | 95,701 |
| mismatches | 26 | 35 |
| errors (read) | 7,818 (all `-6` NOENT) | **6,793 (all `-4` BUSY)** |
| errors (write) | 0 | 0 |
| err_rate | 7.55e-02 | 6.63e-02 |

Filter dropped the rate by ~12%. The remaining 6.63% is a different
status code entirely: every logged error sample shows `status=-4`
(`HFSSS_ERR_BUSY`), not `-6` (`HFSSS_ERR_NOENT`).

This **inverts the Phase 2 conclusion's framing**. Phase 2 thought
NOENT was the race signal and dug into `gc.c` `taa_remove`. The
re-baseline shows:

1. NOENT was a *workload artifact* (correctly suppressed by the
   filter). Phase 2's no-fix-effect observation matches: there was no
   NOENT race to fix.
2. The *real* post-warmup signal is BUSY, not NOENT, and it shows up at
   ~6.6% under this workload — far above the 0.1% Path A threshold for
   "redo Phase 2 triage."

## Where `HFSSS_ERR_BUSY` comes from on the read path

Trace of the call chain:

- `ftl_read_page_mt` (FTL) calls `hal_nand_read_sync` up to 3 times
  back-to-back (`READ_RETRY_MAX_ATTEMPTS`). No yield, no backoff.
- `hal_nand_read_sync` → `hal_nand_read` → `media_nand_read` →
  `nand_cmd_engine_submit_read` → `engine_submit` (in
  `src/media/cmd_engine.c`).
- `engine_submit` returns `HFSSS_ERR_BUSY` when
  `nand_cmd_is_legal_for_profile_state(..., die->cmd_state.state, op)`
  is false — i.e. the target die is currently executing another op.

The reproducer geometry is 4 ch × 4 chip × 2 die × 1 plane = 32 dies.
With 128 worker threads and rwmix=70, workload concentrates on a
handful of hot dies. Three immediate retries against a busy die can
realistically all see the same busy state, and the FTL gives up with
the last `BUSY` return.

## NVMe mapping — why this is *not* pre-checkin's Write Fault

Pre-checkin reports SCT=0x2 / SC=0x80 (Media-and-Data-Integrity →
Write Fault). The simulator's NVMe completion path (`nvme_uspace.c`):

- `HFSSS_ERR_BUSY` → `SC=0x82` (Namespace Not Ready, generic, retryable
  with DNR=0). SCT=0x0 (Generic), not SCT=0x2 (Media).
- All other backend failures → `SC=0x06` (Internal Device Error),
  SCT=0x0.

So the reproducer's BUSY *cannot* surface as the pre-checkin Write
Fault through the existing NVMe path. They are different failure
modes. Issue (A) reproducer-BUSY and Issue (B) pre-checkin-Write-Fault
are now confirmed independent.

## Read of state vs decisions

What we know now:

- The reproducer's pre-existing 7.5% baseline was 88% workload-warmup
  artifact + 12% genuine post-warmup errors that surface as transient
  BUSY caused by the retry-without-backoff pattern in
  `ftl_read_page_mt`.
- `ftl_write_page_mt` has the **same shape**: `for write_retry < 3 {
  hal_nand_program_sync(...); }` with no backoff. If the same
  retry-storm pattern hits the write path, this could be a candidate
  for a different (still-not-localized) pre-checkin failure.
- Pre-checkin's specific Write Fault (SCT=0x2 SC=0x80) is *not*
  produced by the read-side BUSY → NS-Not-Ready path. Where it comes
  from in the simulator is still open.

What that implies for next steps:

- **Issue A** (reproducer post-warmup BUSY) is a real concurrency
  issue worth fixing on its own — the retry loops in `ftl_read_page_mt`
  / `ftl_write_page_mt` should distinguish transient (BUSY/AGAIN —
  retry with backoff) from persistent (IO/uncorrectable — give up).
  This is hardening, not the original "second race" hypothesis.
- **Issue B** (pre-checkin Write Fault) still needs direct
  instrumentation: add a TRACE_EMIT or env-gated `fprintf` at every
  `return HFSSS_ERR_*` on the FTL/HAL/media write path, run pre-checkin
  with the gate on, capture the first non-OK return, and correlate
  that to the SCT/SC the host saw.

The original PR #115 framing as a "second race fix" was wrong. The
work it carries (error tracking + dispatcher topology + warmup filter)
is investigation infrastructure that landed real value but does not
itself fix anything. PR #115 should be re-framed as
"investigation tooling + corrected diagnosis", with the actual fix(es)
landing in follow-up PR(s).
