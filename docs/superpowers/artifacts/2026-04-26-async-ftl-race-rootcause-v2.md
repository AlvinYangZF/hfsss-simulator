# Async FTL Race — Corrected Root Cause (v2)

**Status:** Final root cause located. Supersedes
`2026-04-26-async-ftl-race-rootcause.md` (v1, wrong).

**Branch / PR:** `feat/async-ftl-race-investigation` / PR #115.

**Reproducer:** `make pre-checkin` fio-010 randwrite, async pipeline,
2048 MiB, 4k bs, iodepth=16. Also visible in `ftl_mfc_repro --mode
legacy --threads 128 --rwmix 70 --duration 10 --lbas 256` post-warmup
filter.

## Symptom recap

Guest kernel emits, on writes:
```
nvme0n1: I/O Cmd(0x1) @ LBA N, 8 blocks, I/O Error (sct 0x2 / sc 0x80)
critical medium error, dev nvme0n1, sector N op 0x1:(WRITE) ...
```
fio `error=61` (ENODATA). 010-014 fail; 016/020/021 pass.

## What v1 got wrong

v1 hypothesized a TAA L2P race in `gc.c`'s failure paths
(unconditional `taa_remove`). The instrumented re-baseline
empirically refutes that:

- `taa_remove_if_equal` CAS sibling: zero invocations under workload.
- `LOOKUP_NOENT` events: only on never-written LBAs during workload
  startup (warmup artifact).
- Pre-checkin Write Fault is on writes (op 0x1), not reads. v1 was
  reasoning over the read path entirely.

The reproducer's filtered post-warmup error rate (6.6%) and
pre-checkin's Write Fault are not separate phenomena — they are the
same bug surfacing through two different host paths.

## The actual root cause

`HFSSS_TRACE_IO_ERR=1` instrumentation captured the full chain on a
single fio-010 run. 484 trace lines across 4 layers, 44 final write
failures, 14 distinct LBAs. The pattern:

```
[IO_ERR] L=media_nand_program ch=0 chip=3 die=1 plane=1 block=B page=P submit_rc=-4 cb_rc=0 eff=-4
[IO_ERR] L=hal_nand_program_sync ch=0 chip=3 die=1 plane=1 block=B page=P rc=-4
[IO_ERR] L=ftl_write_page_mt site=hal-prog-retry lba=L ppn=PPN retry=0/3 rc=-4
[IO_ERR] L=media_nand_program ... rc=-4
[IO_ERR] L=hal_nand_program_sync ... rc=-4
[IO_ERR] L=ftl_write_page_mt site=hal-prog-retry lba=L ppn=PPN retry=1/3 rc=-4
[IO_ERR] L=media_nand_program ... rc=-4
[IO_ERR] L=hal_nand_program_sync ... rc=-4
[IO_ERR] L=ftl_write_page_mt site=hal-prog-retry lba=L ppn=PPN retry=2/3 rc=-4
[IO_ERR] L=ftl_write_page_mt site=hal-prog-final lba=L ppn=PPN rc=-4 action=mark-closed
[IO_ERR] L=nbd_async_cq cmd=1 handle=H length=4096 byte_off=0 rc=-4 -> EIO
```

`rc=-4` = `HFSSS_ERR_BUSY`. Origin: `cmd_engine.c::engine_submit`
returns BUSY when `nand_cmd_is_legal_for_profile_state(profile,
die->cmd_state.state, op)` is false — i.e. the target die is in the
middle of another op. Three immediate retries hit the same busy die
state because the FTL retry loop has no backoff.

After the third BUSY, `ftl_write_page_mt` enters the failure branch
(line 822), and because `ret != HFSSS_ERR_IO`, marks the block
CLOSED, sets `cwb->block = NULL`. The next host write to this
(channel, plane) re-allocates a fresh block. Tracing showed *the same
LBA* burned through 5 blocks in 5 successive attempts, all on
`(ch=0, chip=3, die=1, plane=1)` — a single die in heavy use.

The NBD CQ thread sees `slot->status = -4` and replies with
`NBD_EIO`. QEMU's NVMe emulation layer maps EIO on writes (via
`nvme_aio_err`) to `SCT=0x2 SC=0x80` (Media-and-Data-Integrity →
Write Fault), exactly the host-side symptom. v1's reasoning about
`nvme_uspace.c` mapping BUSY differently was irrelevant — pre-checkin
runs through NBD + QEMU NVMe, not our nvme_uspace path.

## Why this is wrong behavior

`HFSSS_ERR_BUSY` from `engine_submit` is by definition transient —
the die WILL become idle in microseconds when the in-flight op
finishes. Two layers of FTL bookkeeping are mishandling it:

1. **Retry loop has no backoff.** Three back-to-back calls hit the
   exact same die state because nothing has yielded in between.
   Adding `sched_yield()` (or a small `usleep`) between retries lets
   the die's worker advance state.

2. **Failure branch marks the block CLOSED.** The block is in fact
   fine — the failure was a die-state contention, not a media
   defect. Marking it CLOSED throws away a usable block and forces
   the next write to allocate a new one. Combined with concentrated
   contention on a hot die, this multiplies the damage: each
   transient busy spurt burns N blocks instead of just adding latency.

The combination explains the observed pattern: under fio iodepth=16
with 4k bs hammering a small set of dies, the FTL retry storm gives
up too fast and torches blocks on every miss, propagating BUSY all
the way to the host as a Write Fault.

## Why fio-016/020/021 pass

`016_fio_nightly_queue_pressure.sh` uses higher iodepth but lower
contention concentration (longer LBA range, sequential-friendly).
`020/021` PR-smoke cases run shorter / smaller. The bug is sensitive
to *concentrated* die contention, which only the random-write
verify cases at this fio config produce.

## Fix shape (as landed)

Three edits in `src/ftl/ftl.c`, all inside `ftl_write_page_mt` and
the symmetric `ftl_read_page_mt`:

**Edit 1 — bump retry budget from 3 to 512 with proper sleep
between attempts.** TLC tProg is 900-1300 µs; under N-thread
contention with pthread mutex unfairness the per-attempt latency
can reach tens of ms. `sched_yield()` is too short (microseconds);
use `nanosleep(100 µs)` and budget 512 retries → ~51 ms cumulative
wait. Well within NVMe command timeout.

```
static inline void ftl_busy_backoff_sleep(void) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 };
    nanosleep(&ts, NULL);
}
const int max_retries = 512;  /* 64 was insufficient for fio-012 */
...
if (ret == HFSSS_ERR_BUSY || ret == HFSSS_ERR_AGAIN) {
    ftl_busy_backoff_sleep();
}
```

**Edit 2 — don't burn blocks on transient BUSY:**
```
if (ret != HFSSS_OK) {
    if (cwb->block) {
        if (ret == HFSSS_ERR_IO) {
            block_mark_bad(&ctx->block_mgr, cwb->block);
            cwb->block = NULL;
            cwb->current_page = 0;
        } else if (ret == HFSSS_ERR_BUSY || ret == HFSSS_ERR_AGAIN) {
            /* Transient — keep block; current_page un-incremented so
             * the next host write retries the same (block, page). */
        } else {
            block_mark_closed(&ctx->block_mgr, cwb->block);
            cwb->block = NULL;
            cwb->current_page = 0;
        }
    }
    return ret;
}
```

**Edit 3 — symmetric retry budget on read path.** Same rationale.

Constraints met:
- Total LOC ≈ 50 (well under spec's 100-line ceiling).
- No new data structures, no new locks, no CWB-lock changes.
- No change to media / HAL / cmd_engine. The fix is FTL retry policy.

## Why this isn't the cleanest possible fix

The retry-with-backoff approach is fundamentally polling. Under
heavy contention, threads waste CPU spinning even when the die's
worker is making no progress. A condvar-based wait-on-busy in
cmd_engine would:
- Remove polling overhead.
- Give FIFO-ish ordering, eliminating the starvation window
  pthread mutex creates.
- Likely allow shorter retry budgets (or eliminate retries
  altogether for transient errors).

That change touches `nand_die` struct + `engine_submit` + the
state-transition exit path, and would invalidate any test that
asserts `engine_submit` returns BUSY synchronously. Out of scope
for this PR; tracked as follow-up.

## Validation plan after fix

1. `make -j12 build/bin/ftl_mfc_repro` and rerun the post-filter
   baseline. Expectation: `err_rate` drops from 6.63e-02 to ≈ 0
   (some BUSY may still leak under extreme contention; goal is < 1%).
2. `HFSSS_TRACE_IO_ERR=1 make qemu-blackbox-ci ... --case 010_...`
   and inspect `nbd-server.log`: expect zero `nbd_async_cq` EIO
   emissions on write commands.
3. Full `make pre-checkin`. Expectation: 010-014 fio cases all PASS.
4. Codex/peer review pass on the FTL diff before merging PR #115.

## Spec / plan deltas to record

- Spec §4.3 "Fix shape table" was wrong about the layer (it
  hypothesized GC vs writer dst block conflict). The actual fix
  layer is the FTL retry-policy, not GC and not L2P.
- Plan Phase 3 estimate (< 100 LOC) holds; the actual diff will
  likely land in 30–50 LOC.
