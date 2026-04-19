# QEMU NVMe Black-Box Test Framework

This framework runs guest-visible NVMe tests against the live HFSSS stack:

`guest tool -> /dev/nvme0n1 -> Linux NVMe driver -> QEMU NVMe -> NBD -> hfsss-nbd-server -> HFSSS`

It is designed to expand guest-side validation without changing the simulator data path. The framework lives under `scripts/qemu_blackbox/` and provides:

- host-side environment orchestration for `hfsss-nbd-server` and QEMU
- per-case artifact capture
- case discovery by file name
- reusable guest helpers for `nvme-cli`, `fio`, and future `SPDK` tools
- stable result files for local CI/nightly wrappers

The default QEMU runtime is tuned for Apple Silicon macOS. For Linux or other local environments, override `HFSSS_QEMU_BIN`, `HFSSS_QEMU_ACCEL`, and `HFSSS_QEMU_CPU`.

## Entry Points

- `scripts/run_qemu_blackbox_tests.sh`
- `scripts/qemu_blackbox/run.sh`

## Guest Asset Requirement

The repository does not track `guest/` assets. Provide them in one of two ways:

1. Put them under `<repo>/guest`
2. Pass `--guest-dir /path/to/guest`

Required files:

- `alpine-hfsss.qcow2`
- `cidata.iso`
- `ovmf_vars-saved.fd`

The QEMU firmware image defaults to `/opt/homebrew/share/qemu/edk2-aarch64-code.fd` and can be overridden with `HFSSS_QEMU_CODE_FD`.

## Running

List available cases:

```bash
./scripts/run_qemu_blackbox_tests.sh --list
```

Run the full default suite:

```bash
./scripts/run_qemu_blackbox_tests.sh \
  --guest-dir /path/to/guest \
  --mode mt
```

Run with stable CI/nightly artifact paths:

```bash
./scripts/run_qemu_blackbox_ci.sh \
  --guest-dir /path/to/guest \
  --skip-build
```

This always writes the latest run to:

```text
build/blackbox-tests/latest/
```

That directory contains stable machine-readable outputs:

- `summary.tsv`
- `summary.json`
- `junit.xml`
- `environment.txt`

Run a single case:

```bash
./scripts/run_qemu_blackbox_tests.sh \
  --guest-dir /path/to/guest \
  --case nvme/001_nvme_cli_smoke.sh
```

Reuse an already running environment:

```bash
./scripts/run_qemu_blackbox_tests.sh \
  --reuse-env \
  --ssh-port 10022 \
  --case 001_nvme_cli_smoke.sh
```

Artifacts are written to `build/blackbox-tests/<timestamp>/` by default.

If the requested `--nbd-port` or `--ssh-port` is already in use, the runner will automatically move to the next free port and print a warning. The resolved runtime values are recorded in:

```text
build/blackbox-tests/<timestamp>/environment.txt
```

## Case Layout

Each case is a standalone shell script under `scripts/qemu_blackbox/cases/`. The runner discovers them recursively, so you can group by tool or feature such as:

- `cases/nvme/`
- `cases/fio/`
- `cases/spdk/`

The runner starts the environment once, then executes each case in order.

Case exit codes:

- `0`: pass
- `2`: skip
- anything else: fail

Each case gets:

- `HFSSS_CASE_NAME`
- `HFSSS_CASE_ARTIFACT_DIR`
- `HFSSS_GUEST_NVME_DEV`
- `HFSSS_GUEST_NVME_CTRL`

Each suite run also writes:

- `summary.tsv`
- `summary.json`
- `junit.xml`
- `environment.txt`

The CI wrapper keeps these file names stable by forcing `--artifacts-dir build/blackbox-tests/latest`.

Helpers are exposed by `scripts/qemu_blackbox/lib/case.sh`:

- `hfsss_case_require_guest_tool`
- `hfsss_case_guest_run`
- `hfsss_case_run_fio_json`
- `hfsss_case_assert_file_contains`
- `hfsss_case_skip`

## Adding a New Case

1. Add a new executable `.sh` file under `scripts/qemu_blackbox/cases/`
2. Source `../lib/case.sh`
3. Use the helper functions
4. Return `0` for pass, `2` for skip, non-zero for fail

Minimal example:

```bash
#!/bin/bash
set -euo pipefail

CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$CASE_DIR/../lib/case.sh"

hfsss_case_require_guest_tool nvme
hfsss_case_guest_run "nvme-version" "nvme version"
```

## Seed Cases

The initial framework seeds guest-visible black-box cases in three groups:

`nvme-cli`
- `nvme/001_nvme_cli_smoke.sh`
- `nvme/002_nvme_namespace_info.sh`
- `nvme/003_nvme_flush_smoke.sh`

`fio`
- `fio/010_fio_randwrite_verify.sh`
- `fio/011_fio_randrw_verify.sh`
- `fio/012_fio_seqwrite_verify.sh`
- `fio/013_fio_trim_verify.sh`

`SPDK`
- `spdk/900_spdk_nvme_identify.sh`

The SPDK seed case is intentionally opt-in. It will skip cleanly until the guest image includes the relevant SPDK tool.

You can select cases either by relative path under `cases/` or by basename if it is unique:

```bash
./scripts/run_qemu_blackbox_tests.sh --case fio/013_fio_trim_verify.sh
./scripts/run_qemu_blackbox_tests.sh --case 013_fio_trim_verify.sh
```

These are intentionally black-box only. They validate the guest-facing NVMe surface and leave simulator internals untouched.

## Debugging with the TRACE Build

The per-thread trace ring (`include/common/trace.h`, `src/common/trace.c`) is compile-time-gated behind `-DHFSSS_DEBUG_TRACE=1`. The default build omits every trace site so release paths stay at zero runtime cost. When a regression needs to be classified per-hop (T1..T5 on the NBD / FTL / HAL I/O path), run the blackbox against a separately compiled TRACE binary.

### 1. Build

```bash
TRACE=1 make all
```

This variant lives in its own `build-trace/` tree so the default `build/` objects are never silently reused with the flag flipped.

### 2. Point the harness at the TRACE binary

```bash
HFSSS_NBD_BIN=build-trace/bin/hfsss-nbd-server \
  ./scripts/run_qemu_blackbox_tests.sh --case <name>
```

`HFSSS_NBD_BIN` overrides the default `build/bin/hfsss-nbd-server` path used by `scripts/qemu_blackbox/lib/env.sh`.

### 3. Enable the binary dump

Trace sites still fire without a dump path — they populate each thread's ring, but nothing is written on shutdown. To capture a `trace.bin` for `scripts/qemu_blackbox/phase_a/analyze_trace.py`, set `HFSSS_TRACE_DUMP` in the server's environment:

```bash
HFSSS_TRACE_DUMP=/tmp/hfsss-trace.bin \
HFSSS_NBD_BIN=build-trace/bin/hfsss-nbd-server \
  ./scripts/run_qemu_blackbox_tests.sh --case nvme/001_nvme_cli_smoke.sh
```

The server prints `[trace] binary dump enabled -> <path>` at startup when the variable is set; it prints `[trace] HFSSS_TRACE_DUMP unset; trace points will run but no dump produced` otherwise.

### 4. Classify the first corrupt hop

```bash
python3 scripts/qemu_blackbox/phase_a/analyze_trace.py /tmp/hfsss-trace.bin
```

### Per-case timeout caveat

TRACE instrumentation makes the I/O path noticeably slower. The harness applies a per-case deadline via `HFSSS_CASE_TIMEOUT_S` (default **300 seconds**, enforced by `scripts/qemu_blackbox/lib/common.sh`). `fio` verify workloads that fit inside 300 s under the default build can exceed it under TRACE and will be killed with:

```
[hfsss-blackbox] ERROR: case timed out after 300s
```

Mitigations, in order of preference:

1. Pick a lighter case (e.g. `nvme/001_nvme_cli_smoke.sh`) for the initial hop-classification pass.
2. Raise the per-case cap when running a heavier verify under TRACE, e.g. `HFSSS_CASE_TIMEOUT_S=900 ...`.
3. Shrink the workload (smaller `--size`, lower `iodepth`) rather than lengthen the cap — trace ring capacity is finite, and a huge run can wrap before the interesting hop.
