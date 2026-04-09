#!/bin/bash
# Run end-to-end fio tests through QEMU guest against the instrumented
# HFSSS NBD server, capturing coverage data from the firmware.
#
# Requires: QEMU installed, lcov, a built-and-prepared QEMU guest image
#           (see scripts/start_nvme_test.sh for one-time setup).
# Outputs: build-cov/coverage/e2e.info and build-cov/coverage/e2e/
#          (also copied to $HFSSS_RUN_WORKSPACE/coverage/ for audit trail)
#
# Concurrency: this script SERIALIZES against other e2e coverage runs via
# a file lock (build-cov/coverage/.e2e.lock). gcov's .gcda files are written
# at the compile-time build path, so a single canonical coverage measurement
# per commit is the correct model — attempting parallel coverage runs would
# race on .gcda counter updates.
#
# Runtime resources (NBD port, SSH port, QEMU process, logs, OVMF vars) ARE
# per-run isolated via scripts/lib/run_isolation.sh, so blackbox suites and
# ad-hoc QEMU sessions can run alongside this script without interference.
#
# Platform: the QEMU execution path below is macOS-aarch64 with HVF today.
# Override QEMU_BIN / QEMU_ACCEL / OVMF_CODE_FW to retarget.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT/build-cov"
BIN_DIR="$BUILD_DIR/bin"
COV_DIR="$BUILD_DIR/coverage"
INFO="$COV_DIR/e2e.info"
INFO_RAW="$COV_DIR/e2e.raw.info"
GUEST_DIR="$ROOT/guest"
LOCK_DIR="$COV_DIR/.e2e.lock"

# Platform overrides (macOS-aarch64 defaults)
QEMU_BIN="${QEMU_BIN:-qemu-system-aarch64}"
QEMU_ACCEL="${QEMU_ACCEL:-hvf}"
OVMF_CODE_FW="${OVMF_CODE_FW:-/opt/homebrew/share/qemu/edk2-aarch64-code.fd}"

# Run isolation primitives — unique RUN_ID, per-run workspace, safe cleanup.
# shellcheck source=../lib/run_isolation.sh
. "$ROOT/scripts/lib/run_isolation.sh"

mkdir -p "$COV_DIR"

# Acquire exclusive lock on coverage capture. mkdir is atomic on POSIX,
# so this is a portable mutex that works on macOS and Linux without flock.
acquire_coverage_lock() {
    if mkdir "$LOCK_DIR" 2>/dev/null; then
        echo "$$" > "$LOCK_DIR/pid"
        return 0
    fi
    # Lock exists — check if owner is still alive
    local owner_pid
    owner_pid="$(cat "$LOCK_DIR/pid" 2>/dev/null || echo "")"
    if [ -n "$owner_pid" ] && kill -0 "$owner_pid" 2>/dev/null; then
        echo "ERROR: another e2e coverage run is in progress (pid $owner_pid)" >&2
        echo "       Lock: $LOCK_DIR" >&2
        echo "       Coverage runs are serialized — gcov .gcda counters cannot be safely shared." >&2
        return 1
    fi
    echo "WARN: reclaiming stale coverage lock (owner pid=$owner_pid not running)" >&2
    rm -rf "$LOCK_DIR"
    mkdir "$LOCK_DIR"
    echo "$$" > "$LOCK_DIR/pid"
}
release_coverage_lock() {
    [ -d "$LOCK_DIR" ] && rm -rf "$LOCK_DIR"
}

acquire_coverage_lock
# Release the lock if we exit before the full cleanup trap is installed below.
trap 'release_coverage_lock' EXIT INT TERM

# Preconditions
command -v lcov >/dev/null || { echo "ERROR: lcov not installed"; exit 1; }
[ -x "$BIN_DIR/hfsss-nbd-server" ] || { echo "ERROR: $BIN_DIR/hfsss-nbd-server not built. Run make coverage-build."; exit 1; }
command -v "$QEMU_BIN" >/dev/null || { echo "ERROR: $QEMU_BIN not installed (override via QEMU_BIN=...)"; exit 1; }
[ -f "$OVMF_CODE_FW" ] || { echo "ERROR: UEFI firmware $OVMF_CODE_FW not found (override via OVMF_CODE_FW=...)"; exit 1; }
[ -f "$GUEST_DIR/alpine-hfsss.qcow2" ] || { echo "ERROR: $GUEST_DIR/alpine-hfsss.qcow2 not found. Run the QEMU image setup first."; exit 1; }
[ -f "$GUEST_DIR/cidata.iso" ] || { echo "ERROR: $GUEST_DIR/cidata.iso not found"; exit 1; }
[ -f "$GUEST_DIR/ovmf_vars-saved.fd" ] || { echo "ERROR: $GUEST_DIR/ovmf_vars-saved.fd not found"; exit 1; }

hfsss_run_init
echo "Run isolation: RUN_ID=$HFSSS_RUN_ID workspace=$HFSSS_RUN_WORKSPACE"

# Allocate a fresh NBD port and SSH forward port for this run
hfsss_run_alloc_port COV_NBD_PORT
hfsss_run_alloc_port COV_SSH_PORT
echo "Allocated ports: nbd=$COV_NBD_PORT ssh=$COV_SSH_PORT"

# Copy UEFI vars into workspace so parallel runs don't clobber each other
cp "$GUEST_DIR/ovmf_vars-saved.fd" "$HFSSS_RUN_OVMF_VARS"

# SSH key is shared across runs (read-only after first create); if missing, bail
SSH_KEY="${HFSSS_SSH_KEY:-/tmp/hfsss_qemu_key}"
[ -f "$SSH_KEY" ] || { echo "ERROR: SSH key $SSH_KEY not found. Run scripts/start_nvme_test.sh once to generate."; exit 1; }

trap 'hfsss_run_cleanup; release_coverage_lock' EXIT INT TERM

bash "$ROOT/scripts/coverage/reset_counters.sh"

cd "$ROOT"
# Start NBD server with retry: port allocation has a TOCTOU window, so if
# bind fails we reallocate and retry up to 3 times.
start_nbd_with_retry() {
    local attempt
    for attempt in 1 2 3; do
        echo "Starting instrumented hfsss-nbd-server on port $COV_NBD_PORT (attempt $attempt)..."
        "$BIN_DIR/hfsss-nbd-server" -a -p "$COV_NBD_PORT" > "$HFSSS_RUN_NBD_LOG" 2>&1 &
        echo $! > "$HFSSS_RUN_NBD_PID_FILE"
        sleep 2
        if ! kill -0 "$(cat "$HFSSS_RUN_NBD_PID_FILE")" 2>/dev/null; then
            echo "WARN: NBD server died on startup (attempt $attempt). Log tail:"
            tail -n 20 "$HFSSS_RUN_NBD_LOG" >&2 || true
            if [ "$attempt" -lt 3 ]; then
                hfsss_run_alloc_port COV_NBD_PORT
                echo "Retrying on port $COV_NBD_PORT..."
            fi
            continue
        fi

        # The server may still be initializing after the process is alive.
        # Wait until it advertises that it is ready to accept an NBD client
        # before launching QEMU, otherwise QEMU can lose a startup race and
        # exit on an early connection-refused.
        local ready=0
        local i
        for i in $(seq 1 30); do
            if grep -q 'Waiting for NBD client' "$HFSSS_RUN_NBD_LOG" 2>/dev/null; then
                ready=1
                break
            fi
            if ! kill -0 "$(cat "$HFSSS_RUN_NBD_PID_FILE")" 2>/dev/null; then
                break
            fi
            sleep 1
        done

        if [ "$ready" -eq 1 ]; then
            return 0
        fi

        echo "WARN: NBD server did not become ready (attempt $attempt). Log tail:"
        tail -n 20 "$HFSSS_RUN_NBD_LOG" >&2 || true
        if [ "$attempt" -lt 3 ]; then
            hfsss_run_kill_pidfile "$HFSSS_RUN_NBD_PID_FILE" 2
            hfsss_run_alloc_port COV_NBD_PORT
            echo "Retrying on port $COV_NBD_PORT..."
        fi
    done
    echo "ERROR: NBD server failed to become ready after 3 attempts"
    cat "$HFSSS_RUN_NBD_LOG"
    return 1
}
start_nbd_with_retry

echo "Starting QEMU guest (name=$HFSSS_RUN_QEMU_NAME, bin=$QEMU_BIN, accel=$QEMU_ACCEL)..."
"$QEMU_BIN" \
    -name "$HFSSS_RUN_QEMU_NAME" \
    -M virt,gic-version=3 -accel "$QEMU_ACCEL" -cpu host \
    -m 2G -smp 2 \
    -drive if=pflash,format=raw,file="$OVMF_CODE_FW",readonly=on \
    -drive if=pflash,format=raw,file="$HFSSS_RUN_OVMF_VARS" \
    -drive file="$GUEST_DIR/alpine-hfsss.qcow2",if=virtio,format=qcow2,snapshot=on \
    -drive file="$GUEST_DIR/cidata.iso",if=virtio,media=cdrom \
    -drive "driver=nbd,server.type=inet,server.host=127.0.0.1,server.port=$COV_NBD_PORT,if=none,id=nvm0,discard=unmap" \
    -device nvme,serial=HFSSS0001,drive=nvm0 \
    -netdev "user,id=net0,hostfwd=tcp::${COV_SSH_PORT}-:22" \
    -device virtio-net-pci,netdev=net0 \
    -serial "file:$HFSSS_RUN_QEMU_LOG" \
    -display none &
echo $! > "$HFSSS_RUN_QEMU_PID_FILE"

echo "Waiting for SSH on port $COV_SSH_PORT..."
for i in $(seq 1 180); do
    if ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
           -i "$SSH_KEY" -p "$COV_SSH_PORT" -o ConnectTimeout=2 \
           root@127.0.0.1 'true' 2>/dev/null; then
        echo "SSH ready after ${i}s"
        break
    fi
    sleep 1
    if [ "$i" -eq 180 ]; then
        echo "ERROR: SSH not ready after 180s (check $HFSSS_RUN_QEMU_LOG)"
        exit 1
    fi
done

echo "========================================"
echo "Running fio verification suite..."
echo "========================================"
bash "$ROOT/scripts/fio_verify_suite.sh" "$COV_SSH_PORT" /dev/nvme0n1 || {
    echo "WARN: fio suite reported failures (coverage data still captured)"
}

echo "========================================"
echo "Running blackbox nvme-cli coverage cases..."
echo "========================================"
# Run selected blackbox test cases in the existing QEMU guest to generate
# additional E2E coverage of NVMe admin and I/O paths.  We reuse the same
# QEMU+NBD environment that fio_verify_suite just used (ports, SSH key).
#
# Only run lightweight cases that don't conflict with the fio suite's
# prior writes.  Heavy fio cases are already covered above.
E2E_COVERAGE_CASES=(
    "001_nvme_cli_smoke.sh"
    "002_nvme_namespace_info.sh"
    "003_nvme_flush_smoke.sh"
    "005_nvme_get_set_features_smoke.sh"
    "006_nvme_smart_log_smoke.sh"
    "004_nvme_trim_zero_verify.sh"
)
E2E_BB_ARTIFACTS="$HFSSS_RUN_WORKSPACE/blackbox-coverage"
mkdir -p "$E2E_BB_ARTIFACTS"

for casefile in "${E2E_COVERAGE_CASES[@]}"; do
    # Find the case script
    case_path="$(find "$ROOT/scripts/qemu_blackbox/cases" -name "$casefile" -type f | head -1)"
    if [ -z "$case_path" ]; then
        echo "WARN: case $casefile not found, skipping"
        continue
    fi
    case_name="${casefile%.sh}"
    case_artifact_dir="$E2E_BB_ARTIFACTS/$case_name"
    mkdir -p "$case_artifact_dir"

    echo "  Running: $casefile"
    export HFSSS_CASE_NAME="$case_name"
    export HFSSS_CASE_ARTIFACT_DIR="$case_artifact_dir"
    export HFSSS_GUEST_NVME_DEV="/dev/nvme0n1"
    export HFSSS_GUEST_NVME_CTRL="/dev/nvme0"
    export COV_SSH_KEY="$SSH_KEY"

    if timeout 120 bash "$case_path" > "$case_artifact_dir/case.stdout.log" 2>&1; then
        echo "    PASS: $casefile"
    else
        rc=$?
        if [ "$rc" -eq 2 ]; then
            echo "    SKIP: $casefile"
        else
            echo "    FAIL: $casefile (rc=$rc, coverage data still captured)"
        fi
    fi
done

echo "Shutting down..."
hfsss_run_cleanup
# Runtime cleanup is done, but lcov/genhtml still need to run. Replace the
# trap with a lock-only release so: (a) if lcov/genhtml fails, the lock is
# still released; (b) on normal exit, the trap fires and releases the lock.
# This is what closes the "stale lock left on success" path that earlier
# relied on dead-PID reclamation.
trap 'release_coverage_lock' EXIT INT TERM
sleep 1

echo "========================================"
echo "Capturing lcov data..."
echo "========================================"
lcov --capture --directory "$BUILD_DIR" --output-file "$INFO_RAW" \
     --rc branch_coverage=1 \
     --ignore-errors deprecated,unsupported,inconsistent,inconsistent \
     --quiet

lcov --remove "$INFO_RAW" \
     "*/src/vhost/*" \
     "*/src/kernel/*" \
     "*/src/tools/*" \
     "*/tests/*" \
     "/usr/include/*" \
     "/usr/lib/*" \
     "*/guest/*" \
     --output-file "$INFO" \
     --rc branch_coverage=1 \
     --ignore-errors deprecated,unsupported,inconsistent,inconsistent,unused,unused \
     --quiet

if grep -q 'SF:.*src/vhost/' "$INFO" 2>/dev/null; then
    echo "ERROR: src/vhost/ not excluded from $INFO"; exit 1
fi

genhtml "$INFO" --output-directory "$COV_DIR/e2e" \
        --title "HFSSS E2E Coverage (QEMU+fio)" \
        --branch-coverage \
        --ignore-errors inconsistent,inconsistent,corrupt \
        --quiet

# Copy canonical artifacts into per-run workspace for audit trail.
# The canonical report in build-cov/coverage/ is still the source of truth
# (ratchet + PR comment read it), but preserving a per-run copy lets
# operators investigate which run produced which numbers.
RUN_COV_DIR="$HFSSS_RUN_WORKSPACE/coverage"
mkdir -p "$RUN_COV_DIR"
cp "$INFO" "$RUN_COV_DIR/e2e.info"
cp -R "$COV_DIR/e2e" "$RUN_COV_DIR/e2e-html"

echo ""
echo "========================================"
echo "E2E Coverage Summary"
echo "========================================"
lcov --summary "$INFO" --rc branch_coverage=1 --ignore-errors deprecated,inconsistent,inconsistent 2>&1 | grep -E 'lines|functions|branches'
echo ""
echo "HTML report: $COV_DIR/e2e/index.html"
echo "Run workspace: $HFSSS_RUN_WORKSPACE"
echo "Run coverage copy: $RUN_COV_DIR"
