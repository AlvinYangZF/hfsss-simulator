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
