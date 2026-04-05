# CI Run Isolation

This document describes how HFSSS supports **concurrent CI execution** on a
single host without interference between runs. Any script that boots QEMU,
starts the NBD server, or produces per-run artifacts should use the
isolation primitives described here.

## Scope and non-goals

The isolation primitives here cover **runtime resources** (QEMU processes,
NBD servers, ports, logs, UEFI vars, pid files). Two workflows that both
allocate their own ports via `hfsss_run_alloc_port` and tag their QEMU
with `$HFSSS_RUN_QEMU_NAME` can coexist on one host.

They do **not** isolate:

- **Coverage capture** (`.gcda` counters) — gcov writes `.gcda` files at
  the compile-time build path, so two concurrent coverage runs would race
  on counter updates. `scripts/coverage/run_e2e_coverage.sh` therefore
  **serializes** via a mutex (`build-cov/coverage/.e2e.lock`). This is the
  intentional model: a single canonical coverage measurement per commit.
- **Build artifacts** (`build/`, `build-cov/`) — concurrent `make` calls
  into the same `BUILD_DIR` would corrupt each other. Serialize builds or
  set `BUILD_DIR=build-$RUN_ID` per run.

## Platform support

- **Isolation primitives** (`scripts/lib/run_isolation.sh`): platform-agnostic
  POSIX shell + `python3`. Runs on macOS and Linux.
- **QEMU execution path** in `scripts/coverage/run_e2e_coverage.sh`: hardcodes
  macOS-aarch64 defaults (HVF accel, aarch64, Homebrew OVMF). Override with:
  - `QEMU_BIN=qemu-system-x86_64`
  - `QEMU_ACCEL=kvm` (or `tcg`)
  - `OVMF_CODE_FW=/usr/share/OVMF/OVMF_CODE.fd`
  A Linux-native coverage path is not exercised in CI today.

## Why

Before this module, multiple concurrent test runs on the same host collided on:

| Resource | Before | Symptom |
|----------|--------|---------|
| TCP ports (NBD 10809, SSH 2222) | Hardcoded defaults | Second run fails to bind |
| `/tmp/hfsss_*_cov.pid` | Fixed paths | PID files overwritten, wrong process killed |
| `/tmp/cov_{nbd,qemu}.log` | Fixed paths | Logs from different runs interleaved |
| `guest/ovmf_vars-run.fd` | Single file | UEFI variables corrupted by concurrent writers |
| `pkill -f "qemu-system-aarch64"` | Broadcast pattern | Sibling CI runs killed mid-test |
| `build/blackbox-tests/latest/` | Fixed directory | Artifacts from the last run clobber earlier ones |

## How

`scripts/lib/run_isolation.sh` provides:

### `hfsss_run_init`

Generates a unique `HFSSS_RUN_ID` and creates a per-run workspace directory:

```
build/runs/$HFSSS_RUN_ID/
├── run.manifest          # run_id, workspace, started_at, commit, pid
├── nbd.pid               # NBD server PID
├── qemu.pid              # QEMU PID
├── nbd-server.log        # NBD stderr/stdout
├── qemu-console.log      # QEMU -serial file output
└── ovmf_vars.fd          # per-run copy of UEFI vars
```

RUN_ID resolution:

1. `gh-$GITHUB_RUN_ID-$GITHUB_RUN_ATTEMPT` when running under GitHub Actions
2. `gh-$GITHUB_RUN_ID` if only the run id is set
3. `$(hostname -s)-$$-$(date +%s)` locally

After `hfsss_run_init`, these env vars are exported:

- `HFSSS_RUN_ID`
- `HFSSS_RUN_WORKSPACE`
- `HFSSS_RUN_NBD_PID_FILE`
- `HFSSS_RUN_QEMU_PID_FILE`
- `HFSSS_RUN_NBD_LOG`
- `HFSSS_RUN_QEMU_LOG`
- `HFSSS_RUN_OVMF_VARS`
- `HFSSS_RUN_QEMU_NAME` (e.g. `hfsss-gh-123-1`)

### `hfsss_run_alloc_port <var_name>`

Asks the OS for a free TCP port via `python3 socket.bind(('127.0.0.1', 0))`,
reads the port number, closes the socket, returns. **This is best-effort,
not atomic** — there is a brief TOCTOU window between the socket close and
the downstream service's bind during which another process may claim the
same port.

This is still a significant improvement over hardcoded 10809/2222 (which
_guaranteed_ collision under concurrency), but callers must handle bind
failure themselves. See `scripts/coverage/run_e2e_coverage.sh` for the
retry pattern: allocate → try bind → if service dies within 2s, reallocate
and retry up to 3 times.

### `hfsss_run_kill_pidfile <pid_file> [timeout_s]`

Reads the PID from the file, sends SIGTERM, waits up to `timeout_s` seconds
(default 10), escalates to SIGKILL if needed. Removes the pid file afterwards.
**Never** uses `pkill -f pattern`.

### `hfsss_run_cleanup`

Convenience wrapper: kills QEMU first (so the NBD client fd closes, letting
any NBD async threads drain and flush `.gcda` files), waits briefly, then
kills the NBD server. Use as a `trap` handler.

### QEMU naming convention

Every QEMU launch must include `-name "$HFSSS_RUN_QEMU_NAME"`. This lets
operators safely filter QEMU processes belonging to a specific run with
`pgrep -f "hfsss-$HFSSS_RUN_ID"` without matching sibling runs.

## Usage pattern

```bash
#!/bin/bash
set -euo pipefail

. "$(git rev-parse --show-toplevel)/scripts/lib/run_isolation.sh"

hfsss_run_init
hfsss_run_alloc_port NBD_PORT
hfsss_run_alloc_port SSH_PORT
trap 'hfsss_run_cleanup' EXIT INT TERM

# Start NBD server on the allocated port, record PID
./build-cov/bin/hfsss-nbd-server -p "$NBD_PORT" > "$HFSSS_RUN_NBD_LOG" 2>&1 &
echo $! > "$HFSSS_RUN_NBD_PID_FILE"

# Start QEMU with the unique -name tag, connecting to the allocated port
qemu-system-aarch64 \
    -name "$HFSSS_RUN_QEMU_NAME" \
    -drive "driver=nbd,...,server.port=$NBD_PORT,discard=unmap" \
    -netdev "user,hostfwd=tcp::${SSH_PORT}-:22" \
    ... &
echo $! > "$HFSSS_RUN_QEMU_PID_FILE"

# ... run tests over SSH on $SSH_PORT ...
```

## Artifact retention

Per-run workspaces accumulate under `build/runs/`. They are:

- `.gitignore`-d (part of `build/`)
- Expected to be cleaned by `make coverage-clean` or ad-hoc `rm -rf build/runs/`
- Useful for post-mortem investigation of a single CI run

CI workflows should upload `build/runs/$RUN_ID/` as an artifact for failed runs.

## What this does NOT isolate

This module deliberately scopes to **runtime** isolation (QEMU, NBD server,
ports, logs). The following remain shared (see "Scope and non-goals" above
for the rationale):

- **`.gcda` coverage counters** — gcov writes to compile-time build paths,
  so coverage runs are serialized via `build-cov/coverage/.e2e.lock`. Two
  concurrent calls to `run_e2e_coverage.sh` will result in the second
  failing fast with a clear error until the first releases the lock.
- `build/` and `build-cov/` — concurrent `make all` would corrupt each other.
  Serialize builds or give each run a unique `BUILD_DIR=build-$RUN_ID`.
- `.coverage-baseline.json` — single ratchet file, only written when explicitly
  updating the baseline.
- SSH key at `/tmp/hfsss_qemu_key` — shared, read-only after first creation.
- `guest/alpine-hfsss.qcow2` — read-only guest image, used with
  `snapshot=on` so QEMU writes go to an ephemeral overlay.

A blackbox suite and an e2e coverage run can execute **concurrently** (they
touch independent resources). Two e2e coverage runs **cannot** — by design.
