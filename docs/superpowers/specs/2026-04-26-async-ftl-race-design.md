# Async-Pipeline FTL Second Race — Investigation & Fix (Design)

**Date:** 2026-04-26
**Predecessor:** [docs/superpowers/plans/2026-04-17-fio-014-015-investigation-closeout.md](../plans/2026-04-17-fio-014-015-investigation-closeout.md)
**Closure model:** Localize-and-fix (not a "improve concurrency story" rewrite)
**Scope:** One race. The one that trips `make pre-checkin` on fio cases 010-014.

## 1. Summary

`make pre-checkin` (NBD `--mode async`, exported size 2048 MiB) trips an NVMe write fault (`SCT=0x2 SC=0x80`) on every fio case 010-014. The same suite under `make qemu-blackbox` (`--mode mt`, 512 MiB) passes. The 2026-04-18 fio 014/015 closeout fixed a CWB block-pointer race (commit `33c1485`) and explicitly deferred a *second* race that still surfaced under `tools/ftl_mfc_repro threads=16` at ~1 mismatch / 56K ops:

> "Under very high concurrency we still observe an occasional data-integrity mismatch, implying a second race beyond CWB — left as future work because it does not manifest in the single-dispatcher NBD production path above a negligible rate."

The async-pipeline production path (1 SQ thread + 8 FTL workers + 1 CQ thread) is multi-dispatcher; the closeout's "single-dispatcher" caveat does not apply, and the rate goes from negligible to fail-stop.

This spec describes a four-phase, ~5-day investigation to localize and fix the second race without touching `channel_worker`, the FTL core data structures, or the production NAND simulation.

## 2. Current State

### 2.1 Production async pipeline (the path that fails)

Topology established by `src/vhost/hfsss_nbd_server.c` when `--mode async` and a client connects:

```
NBD client (Linux NVMe driver in guest)
    ↓ NBD wire over TCP
NBD server SQ thread  (nbd_async_ctx.sq_thread)
    ↓ SPSC request_ring per worker (capacity 256, set in nbd_async_init)
8 × FTL worker threads  (FTL_NUM_WORKERS, src/ftl/ftl_worker.c)
    ↓ ftl_{read,write,trim}_page_mt (src/ftl/ftl.c)
        ↓ block_alloc_from_shard (src/ftl/block.c)
        ↓ media_nand_program / media_nand_read (src/hal/hal_nand.c)
            ↓ media_ctx (src/media/)
    ↓ io_completion via shared completion ring
NBD server CQ thread  (nbd_async_ctx.cq_thread)
    ↓ NBD reply over TCP
NBD client
```

8 worker threads can concurrently issue `media_nand_program` on overlapping `(channel, plane, block)` tuples whenever the dispatcher hashes two adjacent LBAs into different workers' rings.

### 2.2 Existing concurrency protection

Already in place:
- **CWB lock** (`include/ftl/block.h`, added 2026-04-17 in `33c1485`) — serializes `cwb->block` pointer transitions in `ftl_write_page_mt`.
- **GC CAS guard** (commit `7b5a92e`) — protects against GC moving a page that the host is concurrently writing.
- **Per-CWB mutex** acquired around the page program in `ftl_write_page_mt`.
- **Worker request ring SPSC** + `request_lock` / `request_cond` on the empty path.

What is *not* established:
- Whether `block_alloc_from_shard` is concurrent-safe across multiple workers selecting from the same free shard.
- Whether the L2P (`ctx->l2p`) update path serializes against concurrent updates to the same LBA from different workers.
- Whether `media_nand_program` can be issued concurrently for the same physical `(ch, chip, die, plane, block, page)` tuple — and if it can, whether the simulator returns an error or silently corrupts.
- Whether the GC thread + a host writer racing on a *destination* block (not a source page, which `7b5a92e` covered) leaves the destination in an inconsistent state.
- Whether the CQ thread completion ordering matters for any cross-cmd invariant.

### 2.3 Failure signature on the production path

From `docs/superpowers/artifacts/2026-04-26-e2e-test-status.md` §5:
- fio: `io_u error on file /dev/nvme0n1: No data available: write offset=N, buflen=4096`
- guest dmesg: `nvme0n1: I/O Cmd(0x1) @ LBA N, 8 blocks, I/O Error (sct 0x2 / sc 0x80)`
- NBD server log: clean handshake, switches to ASYNC PIPELINE on connect, no error on its side, exits cleanly when fio aborts.

The simulator returns a write fault upward; the host kernel surfaces it. So the failure is *fail-loud* on the production path (not silent corruption like the `ftl_mfc_repro` mismatch). That suggests the simulator has a defensive check that fires before the bad state is committed — useful for instrumentation.

### 2.4 Why the existing reproducer underrepresents the rate

`tools/ftl_mfc_repro.c` worker model: each worker calls `ftl_write_page_mt` directly in a loop. There is no producer/consumer boundary, no per-worker request ring, no completion stream. The dispatcher topology of the production path (which is *the* differentiator vs `mt` mode that passes) is absent.

Hypothesis: the production path's *event sequencing* (SQ → ring → worker → ring → CQ, with back-pressure flowing through SPSC bounded queues) creates ordering windows the existing reproducer's tight worker loop does not. Whatever the second race is, its trigger is more likely a specific interleaving the dispatcher topology produces frequently than a uniform random pick of LBAs. This hypothesis is testable in Phase 1 — if the extended reproducer produces the same low rate as the current one, it's wrong.

## 3. Goals & Non-Goals

### 3.1 Goals
- Build a reproducer that triggers ≥ 1 fault per 1K ops, in ≤ 10 s wall-clock.
- Localize the race to specific source location(s), with a written root-cause document.
- Fix it.
- `make pre-checkin` async + 2048 MiB passes 010-014.
- Existing `make test` and `make systest` regression unaffected.

### 3.2 Non-Goals
- Touching `channel_worker` (REQ-045 is a separate line; tier-3 retarget is later work).
- Refactoring FTL data structures beyond the minimum required for the fix.
- Optimizing for performance — correctness first; perf preserved within ±10 % p99.
- Eliminating *all* concurrency bugs in FTL — the goal is the race that trips pre-checkin. A separate race surfacing afterward gets its own investigation.
- Investigating any HAL-level NAND simulation timing changes.

## 4. Approach

Four phases, each with its own go/no-go gate.

### 4.1 Phase 1 — Dispatcher-topology reproducer (1-2 days)

**Goal:** Failure rate ≥ 1 per 1K ops in ≤ 10 s.

**Deliverable:** `tools/ftl_mfc_repro` extended with a new mode (`--mode dispatcher`) that mirrors the production async pipeline:

```
1 producer thread     emits READ / WRITE / TRIM at target rate
N=8 worker threads    each consumes its own SPSC request ring
1 consumer thread     drains completion ring + verifies (write+immediate-read+CRC)
```

Conflict-amplification knobs:
- `--threads N` (default 8, matches production)
- `--lba-range R` (default 256 — narrow enough to force collision; production has wider range but slower trigger)
- `--ops N` (default 1M for soak; reduce for fast iteration)
- `--verify on|off` (write-then-read-back-and-CRC each op)
- `--rate ops/sec` (0 = unpaced, default)

Failure detection: the consumer tracks `(LBA → expected CRC)` and any read returning a different CRC, or a write returning a non-OK status, or a trimmed LBA returning non-zero, is a fault. Fault count + first failing op recorded.

**Gate:** if Phase 1 cannot push the rate above 1/1K with thread/lba knobs, the hypothesis (that dispatcher topology drives the rate) is wrong; investigation pivots to *why pre-checkin's IO mix is special* before continuing.

### 4.2 Phase 2 — Localize (1-2 days)

**Strategy A (first attempt): TSan**

Project already builds with `-fsanitize=thread` into `build-tsan/`. Run the Phase 1 reproducer under TSan; if a happens-before violation fires, the location is given directly in the TSan trace.

**Strategy B (if A is silent): tuple tracing**

Three FTL/HAL invariants likely to be violated. For each, instrument with a `(channel, chip, die, plane, block, page)` tuple trace at:

| Invariant | Where to trace |
|---|---|
| Block alloc — at most one CWB per `(channel, plane)` at a time | `block_alloc_from_shard` entry/exit + `cwb` updates in `ftl.c` |
| L2P — only one writer per LBA at a time | `ctx->l2p` update sites (grep for `l2p[` assignments) |
| Physical program — only one program per `(ch, chip, die, plane, block, page)` at a time | `media_nand_program` entry in `src/hal/hal_nand.c` |

Look for:
- Same tuple, two distinct worker IDs, no intervening `unlock`.
- Same LBA, two L2P updates, no intervening media write.
- Allocator returning the same physical block to two CWBs concurrently.

**Gate:** Phase 2 ends with a written 1-2 paragraph root-cause document pointing at concrete file/function/line range. If neither strategy produces a candidate after 2 days, escalate (the race may be HAL-internal or in the simulator state machine; broaden the search).

### 4.3 Phase 3 — Fix (0.5-1 day)

Fix shape depends on Phase 2 finding. Rough mapping:

| Root cause | Fix shape |
|---|---|
| `block_alloc_from_shard` not atomic | Per-shard mutex or `compare_exchange` on the free-list head |
| L2P update race | Per-bucket lock or `compare_exchange_strong` on `l2p[lba]` |
| HAL parallel program same-tuple | Per-`(ch, plane, block)` mutex at HAL entry, or detect+reject (defensive) |
| GC vs writer on destination block | Extend `7b5a92e`'s CAS pattern to cover the dst-block path |

Constraint: each fix should be < 100 LOC. If the right fix is larger, that signals architectural work and gets its own design phase before this PR.

**Gate:** post-fix, the Phase 1 reproducer runs 1M ops with 0 faults, TSan stays clean.

### 4.4 Phase 4 — Validation + PR (0.5 day)

- `tools/ftl_mfc_repro --mode dispatcher --ops 1000000` — 0 faults
- `make test` — 0 [FAIL] (REQ-122 max-of-N already de-flaked)
- `make systest` — 0 FAIL
- `make pre-checkin` async + 2048 MiB — `pass=9 fail=0 skip=1`
- `make bench-cq` — p99 latency unchanged ±10 % (sanity perf check)
- Write `docs/superpowers/plans/2026-04-26-async-ftl-race-closeout.md` with root cause + fix mapping
- Update `docs/REQUIREMENT_COVERAGE.md` if any REQ row changes status (none expected; this is a defect fix, not a new capability)
- Open PR; codex review

## 5. Validation Strategy

### 5.1 Pre-fix baseline (mandatory before any code change)
- Phase 1 reproducer FAILS (≥ 1 fault per 1K ops on master)
- TSan output captured, even if benign
- pre-checkin output captured (already have this in `build/blackbox-tests/latest/`)

### 5.2 Post-fix validation
| Check | Pass criterion |
|---|---|
| Reproducer | 1M ops, 0 faults, TSan clean |
| `make test` | 0 [FAIL] |
| `make systest` | 0 FAIL |
| `make pre-checkin` | `pass=9 fail=0 skip=1` |
| `make bench-cq` | p99 within ±10 % of pre-fix baseline |

### 5.3 Reproducer becomes part of the regression?
Open question (§7).

## 6. Risks & Mitigations

| Risk | Probability | Mitigation |
|---|---|---|
| Phase 1 reproducer can't reach 1/1K rate even at threads=32 / lba=256 | Medium | Hypothesis (dispatcher topology drives rate) is wrong; pivot to investigating *what fio 010 sends* that the reproducer doesn't (size mix? trim ordering? barriers?) |
| TSan stays silent (race is logical) | Medium | Strategy B tuple tracing covers logical races; project has trace infrastructure (commit `a3ad11c`) |
| Fix introduces deadlock | Low | 1M-op reproducer + pre-checkin re-run catch immediately |
| Fix tanks performance | Low | bench-cq smoke catches; rare for correctness fixes to exceed 10 % p99 hit |
| The race is in `media/` (NAND simulator state machine) not FTL | Low-Medium | If Phase 2 points there, scope expands; mitigation is to flag at gate and replan rather than push forward |
| There are *several* concurrent races, and fixing one only changes the failure signature | Low | Phase 3 gate is "1M ops 0 faults"; if a different fault appears post-fix, that's a separate Phase 1-3 cycle on the new race |

## 7. Open Questions (for spec review)

These are decisions to confirm before Phase 1.

### Q1 — PR structure
**Option A (recommended):** Single PR containing reproducer extension + the fix. Reviewer sees the whole story. Reproducer commits first, fix commits last — reviewable in order.
**Option B:** Two PRs. PR 1 lands reproducer-extension + Phase 2 root-cause document; PR 2 lands the fix. Lower per-PR review surface but two review cycles.

### Q2 — Reproducer permanence
**Option A (recommended):** Add `make ftl-mfc-repro-dispatcher` target that runs the new mode at moderate scale (50K ops) as opt-in. Keeps the regression catchable but does not pay 10 s on every `make test`.
**Option B:** Wire into `make test` (10 s × every contributor × forever).
**Option C:** Keep entirely manual (re-run only on demand).

### Q3 — Phase 2 escalation rule
If Phase 2 takes more than 2 days, who signs off on continuing vs cutting losses? My default: escalate to the user with current evidence and either deeper investigation or a "treat the symptom" workaround (e.g. limit `FTL_NUM_WORKERS` to 1 in async mode = effectively mt-mode = correct but slower).

### Q4 — Out-of-scope confirmation
Confirm I should *not* in this PR:
- Touch `channel_worker.{c,h}` (REQ-045 line)
- Touch HAL NAND simulator timing constants
- Refactor FTL data structures
- Add per-channel queueing beyond what the fix requires

## 8. Out of Scope

- channel_worker integration (REQ-045 tier-3, separate)
- General concurrency story rewrite
- HAL/NAND simulator modeling fidelity
- Performance optimization beyond preserving p99 ±10 %
- Phase-7 NVMe kernel-module path

## 9. Cross-references

- `docs/superpowers/plans/2026-04-17-fio-014-015-investigation-closeout.md` — predecessor; explicitly defers this race
- `docs/superpowers/artifacts/2026-04-26-e2e-test-status.md` §5 — failure-signature audit
- `docs/superpowers/specs/2026-04-24-req-045-completion-queue-design.md` — REQ-045 completion queue (tier-3 will retarget through this; A1 must land first)
- `tools/ftl_mfc_repro.c` — current reproducer
- `src/vhost/hfsss_nbd_server.c` — async pipeline orchestrator
- `src/ftl/ftl_worker.c` + `src/ftl/ftl.c` — worker pool + ftl_{read,write,trim}_page_mt
- commit `33c1485` — first race fix (CWB block-pointer)
- commit `7b5a92e` — GC vs host CAS
