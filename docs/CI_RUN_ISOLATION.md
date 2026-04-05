# CI Run Isolation

This document describes how HFSSS supports **concurrent CI execution** on a
single host (Mac Studio, Linux runner, developer laptop) without interference
between runs. Any script that boots QEMU, starts the NBD server, or produces
per-run artifacts should use the isolation primitives described here.

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

Asks the OS for a free TCP port via `python3 socket.bind(('127.0.0.1', 0))`.
This is atomic (no TOCTOU race) and gives a port guaranteed unused at bind time.
Use this for **both** the NBD server port and the QEMU SSH forward port.

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
ports, logs). The following remain shared:

- `build/` and `build-cov/` — concurrent `make all` would corrupt each other.
  Serialize builds or give each run a unique `BUILD_DIR=build-$RUN_ID`.
- `.coverage-baseline.json` — single ratchet file, only written when explicitly
  updating the baseline.
- SSH key at `/tmp/hfsss_qemu_key` — shared, read-only after first creation.
- `guest/alpine-hfsss.qcow2` — read-only guest image, used with
  `snapshot=on` so QEMU writes go to an ephemeral overlay.
