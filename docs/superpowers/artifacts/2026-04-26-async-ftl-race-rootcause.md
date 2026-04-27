# Async-Pipeline FTL Second Race — Root Cause

**Date:** 2026-04-26
**Phase:** 2 closeout per `docs/superpowers/plans/2026-04-26-async-ftl-race.md`
**Predecessor finding:** `docs/superpowers/plans/2026-04-17-fio-014-015-investigation-closeout.md` (CWB block-pointer race fix at commit `33c1485`)

## Summary

GC's relocation failure path in `src/ftl/gc.c` deletes the L2P entry for a host-owned LBA whenever a NAND read or program of the source/destination page fails — *even if the host has already remapped that LBA to a new physical page since GC sampled the source PPN*. The fix from commit `7b5a92e` introduced a CAS pattern (`taa_update_if_equal`) on the *success* path so a stale relocation cannot overwrite a fresher host write, but the *failure* path was missed and still uses an unconditional `taa_remove(taa, lba)`. As a result, GC's failure can wipe out a perfectly valid host mapping, which a host reader observes as `HFSSS_ERR_NOENT (-6)` from `taa_lookup`.

## Evidence

### Pre-fix reproducer

```
./build/bin/ftl_mfc_repro --mode legacy --threads 128 \
                          --rwmix 70 --duration 10 --lbas 256
ops=95740 mismatches=26 errors=7818 (write=0 read=7818) err_rate=7.55e-02
```

100% of completion errors are READs returning `status=-6`. Zero write errors. Stable across runs (3.7%–7.7% range). Logged samples:

```
ERR op=READ lba=215 status=-6
ERR op=READ lba=15  status=-6
ERR op=READ lba=26  status=-6
…
```

### Source location of the unconditional remove

`src/ftl/gc.c` (the GC relocation loop in `gc_run_mt`):

- Line 493 — branch for when `hal_nand_read_sync` of the *source* page fails.
- Line 534 — branch for when `hal_nand_program_sync` to the *destination* page fails.

Both branches do:

```c
taa_remove(taa, lba);
```

…without verifying that the L2P slot still maps to the source PPN that GC sampled at the top of the relocation loop. The success path on line 558 is correctly defensive:

```c
taa_update_if_equal(taa, lba, src_ppn, dst_ppn, &reloc_installed);
```

### TSan triage

`build-tsan/bin/ftl_mfc_repro` ran cleanly to 95K ops and produced 6 unique ThreadSanitizer warnings — all of them stats-counter races on shared `ctx->stats.host_write_pages`, `ctx->error.uncorrectable_count`, `ctx->error.read_retry_count/_success`, plus `total_ops` / `mismatch_count` in the harness `main`. Each is a benign undercount under contention; none of them explain the `HFSSS_ERR_NOENT` pattern.

The race is logical (state-machine ordering between the GC relocation loop and host writers), not a memory-order issue, so TSan was structurally unable to catch it.

### Why this matches what pre-checkin sees

`make pre-checkin` reports NVMe `SCT=0x2 / SC=0x80` (Write Fault) on fio cases 010-014 in async + 2048 MiB mode. The reproducer surfaces `HFSSS_ERR_NOENT` on reads, not writes. Different surface but same root cause: GC's failure path corrupts the L2P, and downstream operations on the affected LBA fail in different ways depending on which op the host issues next.

In the production NBD path:
- An LBA whose mapping was wiped by GC's failure handler will fail the next write attempt at the FTL layer (the new write tries to look up the *old* PPN to mark it superseded, fails, and returns an error that propagates to NVMe as Write Fault).
- The reproducer's read shows it directly because the workload reads more often than it writes (rwmix=70).

This explains why both signals coexist on the same code path despite the different opcodes.

## Why the existing protection misses this

Commit `33c1485` added per-CWB mutex to serialize the *write* side of the L2P (block-pointer transitions during writes). It does not touch the GC relocation path.

Commit `7b5a92e` added a CAS pattern to the GC *success* path so stale relocations cannot win over fresh host writes. The CAS path is `taa_update_if_equal(lba, src_ppn, dst_ppn)`. It was implemented for the case where everything works; the same defensive lens was not applied to the two failure branches in the same loop.

Both protections together cover ~99 % of the concurrent-host-write-during-GC window. The remaining window — host writes during GC's failed relocation — is a low-probability path on the production single-dispatcher mt mode (per the 2026-04-18 closeout's "negligible rate" caveat) but is the load-bearing path under the async pipeline because:

1. Async mode runs more workers concurrently → more likely a host writer is active during any given GC step.
2. 2048 MiB exported size → more LBAs, more candidate relocations per GC pass, more failure-path triggers.

## Proposed fix shape

Add a CAS-style sibling to `taa_update_if_equal` and use it in both GC failure branches:

```c
/* taa.h */
int taa_remove_if_equal(struct taa_ctx *ctx, u64 lba,
                        union ppn expected_old, bool *removed_out);
```

Implementation in `taa.c` mirrors `taa_update_if_equal` — take the shard lock, verify `s->l2p[local_lba].valid && s->l2p[local_lba].ppn.raw == expected_old.raw`, only then clear the slot and decrement `valid_count`.

Replace the two call sites in `gc.c`:

```c
/* Before:
 *   taa_remove(taa, lba);
 * After: */
taa_remove_if_equal(taa, lba, src_ppn, NULL);
```

`src_ppn` is the PPN GC sampled at the top of the relocation loop; if a host writer has since remapped `lba`, the CAS sees a different PPN and the remove becomes a no-op, leaving the host's newer mapping intact.

The wasted dst page slot (if program succeeded but a host write raced in) follows the same comment block already present at line 548 — implicitly invalid, reclaimed on the next GC pass.

## Scope confirmation vs spec §3.2

- Touches `src/ftl/taa.{c,h}` (add 1 function) and `src/ftl/gc.c` (replace 2 call sites). Estimated < 50 LOC.
- No `channel_worker` change.
- No FTL data structure refactor.
- No HAL NAND timing change.
- Stats-counter races in `error.c` and the new TSan `host_write_pages++` / `total_ops++` warnings are *separately* benign and out of scope for this PR — they should be fixed in a follow-up so future TSan runs are clean, but they are not the cause of the pre-checkin failure.

## Validation plan (executed in Phase 3)

| Check | Expected |
|---|---|
| `ftl_mfc_repro --mode legacy --threads 128 --duration 10 --lbas 256` | 0 read errors, 0 mismatches |
| Same at `--duration 600` (~6 M ops soak) | 0 errors |
| `build-tsan/bin/ftl_mfc_repro` | TSan reports no NEW race (existing benign ones still appear; that's expected) |
| `make test` | 0 [FAIL] |
| `make systest` | 0 [FAIL] |
| `make pre-checkin` async + 2048 MiB | `pass=9 fail=0 skip=1` |
| `make bench-cq` | p99 within ±10 % of baseline |
