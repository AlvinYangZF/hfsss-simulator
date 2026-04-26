# HFSSS Pre-Checkin Standard

**Status:** MANDATORY — no exceptions, no skipping.

Every code change to this repository MUST pass the pre-checkin gate before
being committed or pushed for review. This applies to human contributors
and to all AI agents operating on this codebase.

## What it is

A single command that runs the full QEMU blackbox bundle — nvme-cli
smokes, fio read/write/trim with CRC32C data verification, and a
calibrated mixed read/write stress case — against the instrumented HFSSS
NBD server through a real Linux guest.

```
make pre-checkin
```

Default bundle (runs in order):

| # | Case | What it covers |
|---|------|----------------|
| 1 | `001_nvme_cli_smoke.sh` | nvme list / id-ctrl / smart-log surface discovery |
| 2 | `002_nvme_namespace_info.sh` | namespace enumeration, id-ns nsze/ncap fields |
| 3 | `003_nvme_flush_smoke.sh` | SYNC-NVM flush command round-trip |
| 4 | `010_fio_randwrite_verify.sh` | 4K random write, 512 MiB, CRC32C verify |
| 5 | `011_fio_randrw_verify.sh` | 4K 70/30 mixed, 256 MiB, CRC32C verify |
| 6 | `012_fio_seqwrite_verify.sh` | 128K sequential write, 256 MiB, CRC32C verify |
| 7 | `013_fio_trim_verify.sh` | DSM deallocate + read-zero semantics |
| 8 | `014_fio_pre_checkin_stress.sh` | **4K 70/30 mixed, 8 GiB cumulative I/O, iodepth=64, numjobs=1, CRC32C verify** — the load-bearing gate case |
| 9 | `900_spdk_nvme_identify.sh` | SPDK-side identify (skips gracefully if SPDK absent) |

Total wall-clock: ~8-12 minutes on Mac Studio (Apple Silicon, HVF,
macOS 14+). Case 014 is the bulk of that — it is the canonical
mixed-rw stress + verification signal, calibrated to churn the working
set ~8x under concurrent queue pressure (1 GiB working set × 8 GiB
cumulative I/O). Concurrency comes from `iodepth=64` on a single job
rather than multiple writers — `numjobs=2` with `verify=1` triggers
fio's "multiple writers may overwrite blocks that belong to other
jobs" warning and can abort the verify phase, so the single-job
choice is deliberate.

## Pass criteria

- `make pre-checkin` exits with code 0.
- No `verify:` error lines in any fio output.
- No `ASSERT FAIL` lines in any case artifact.
- Underlying runner prints `pre-checkin PASS — ready to commit`.

Anything else is a FAIL. Do not commit on a FAIL.

## How to run

### Prerequisites (one-time)

- QEMU with HVF support: `brew install qemu` (Apple Silicon).
- GitHub CLI: `brew install gh` (for `make setup-guest`; also handy for PR work).
- Guest assets under `guest/`:
  - `alpine-hfsss.qcow2` — the HFSSS-enabled Alpine guest image
  - `cidata.iso` — cloud-init seed (built locally for your SSH key)
  - `ovmf_vars-saved.fd` — pre-initialised UEFI vars

The fastest way to bootstrap the three files is:

```
make setup-guest
```

That target downloads the latest published `guest-bundle-*` GitHub
release, verifies the SHA-256, extracts `alpine-hfsss.qcow2` and
`ovmf_vars-saved.fd` into `guest/`, then builds a fresh `cidata.iso`
authorising your local SSH key
(`~/.ssh/hfsss_qemu_key`, generated on first run if missing).

Pin to a specific release if needed:

```
make setup-guest SETUP_GUEST_TAG=guest-bundle-2026-04-26-v0.001
```

The ssh keypair lives at `~/.ssh/hfsss_qemu_key{,.pub}` (persistent
across reboots — the runner used to default to `/tmp/` which macOS
wipes, so the cidata.iso authorisation kept drifting). Override the
location with `HFSSS_SSH_KEY=/path/to/key` if your environment requires
a different path.

If you build the bundle yourself rather than downloading,
`scripts/scrub-guest-image.sh` produces a privacy-clean qcow2 from a
running guest, and `scripts/build-guest-bundle.sh <version>` packages
the result with a built-in PII final scan.

### Every commit

```
make pre-checkin
```

Override the guest directory if needed:

```
make pre-checkin GUEST_DIR=/path/to/guest
```

Forward extra args to the runner:

```
make pre-checkin BLACKBOX_ARGS="--skip-build --nbd-mode mt"
```

### NBD server mode

The gate runs `--mode async` by default. The three NBD server modes are:

| mode | description | gate role |
|---|---|---|
| `sync` | single-threaded NBD, single-threaded FTL | simplest; does not exercise concurrency |
| `mt` | multi-threaded FTL workers, synchronous NBD replies | used by `hfsss-vhost-blk` production path |
| `async` | multi-threaded FTL + async NBD reply pipeline | highest concurrency; closest to target workload |

`async` is the default because (1) it is the production target — real
workloads exercise the async pipeline, so regressions surface here
first; (2) it has been empirically stable across pre-checkin
verification runs since PR #44 landed the MT-mode TRIM/TAA fix; (3) a
gate on `sync` would silently miss concurrency regressions in the
paths that ship. Agents who want explicit mt/sync coverage can run
`BLACKBOX_ARGS="--mode mt"` as a follow-up; the mandatory gate itself
targets the production path.

If async starts flapping on the gate, switch the default to `mt` and
open a tracking issue on the flake — don't weaken the gate silently.

### Evidence in the PR description

Paste the trailer banner (`pre-checkin PASS — ready to commit`) and
the timing line into the PR body. A PR without that evidence is not
eligible for review.

## What happens if the gate is impossible to run

**Nothing.** There is no skip path. If QEMU won't boot, if the guest
image is missing, if the NBD server won't start — that IS the blocker,
and it must be fixed before the commit is merged. Report the failure
upstream and block your own PR.

The only acceptable reason to land code that has not been through
`make pre-checkin` is a genuine infrastructure outage (host power
failure, hypervisor bug) AND explicit written owner sign-off recorded
in the PR thread. "I don't have QEMU installed" is not an acceptable
reason — install QEMU.

## Why this gate exists

The HFSSS firmware is an I/O path. The non-negotiable contract is
data integrity under concurrency. Unit tests cover individual
components; `make pre-checkin` covers the contract end-to-end:

- Real guest Linux → NVMe driver → PCIe queues → NBD wire → HFSSS
  FTL → GC → media → and back up.
- CRC32C verification on every block means silent corruption is
  fatal, not "a flaky test".
- The 8 GiB stress case at `numjobs=1 iodepth=64` keeps the write
  path, GC, and read-after-write interleaved long enough to surface
  races that pass under lighter loads.

Previous incidents that this gate would have caught:
- Discard=unmap passthrough regression (trim returning stale data).
- NBD WRITE response header field ordering bug.
- GC races under write-heavy mixed workload.

## Ratchet & evolution

When a new user-visible I/O feature lands (new admin command, new
namespace type, new data-path optimisation), the owner of that change
is responsible for adding a dedicated case to the bundle, NOT for
bypassing the gate.

The 014 stress profile parameters are calibrated. Do not change them
to make the gate faster. If a code change regresses the 014 runtime,
investigate the regression — don't dilute the signal.

## Related

- `CONTRIBUTING.md` — top-level contribution policy (points here).
- `docs/CI_RUN_ISOLATION.md` — how concurrent CI runs stay isolated.
- `scripts/qemu_blackbox/` — the blackbox runner framework (PR #39).
- `docs/superpowers/specs/2026-04-05-ci-test-framework-roadmap-design.md` — the 5-pillar CI roadmap context.
