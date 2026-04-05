#!/bin/bash
# Run end-to-end fio tests through QEMU guest against the instrumented
# HFSSS NBD server, capturing coverage data from the firmware.
#
# Requires: QEMU installed, lcov, a built-and-prepared QEMU guest image
#           (see scripts/start_nvme_test.sh for one-time setup).
# Outputs: build-cov/coverage/e2e.info and build-cov/coverage/e2e/
#
# Concurrency: safe to run in parallel with other coverage/blackbox runs
# on the same host — each invocation allocates its own ports, workspace
# directory, and QEMU process name.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT/build-cov"
BIN_DIR="$BUILD_DIR/bin"
COV_DIR="$BUILD_DIR/coverage"
INFO="$COV_DIR/e2e.info"
INFO_RAW="$COV_DIR/e2e.raw.info"
GUEST_DIR="$ROOT/guest"

# Run isolation primitives — unique RUN_ID, per-run workspace, safe cleanup.
# shellcheck source=../lib/run_isolation.sh
. "$ROOT/scripts/lib/run_isolation.sh"

mkdir -p "$COV_DIR"

# Preconditions
command -v lcov >/dev/null || { echo "ERROR: lcov not installed"; exit 1; }
[ -x "$BIN_DIR/hfsss-nbd-server" ] || { echo "ERROR: $BIN_DIR/hfsss-nbd-server not built. Run make coverage-build."; exit 1; }
command -v qemu-system-aarch64 >/dev/null || { echo "ERROR: qemu-system-aarch64 not installed"; exit 1; }
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

trap 'hfsss_run_cleanup' EXIT INT TERM

bash "$ROOT/scripts/coverage/reset_counters.sh"

echo "Starting instrumented hfsss-nbd-server on port $COV_NBD_PORT..."
cd "$ROOT"
"$BIN_DIR/hfsss-nbd-server" -a -p "$COV_NBD_PORT" > "$HFSSS_RUN_NBD_LOG" 2>&1 &
echo $! > "$HFSSS_RUN_NBD_PID_FILE"
sleep 2

if ! kill -0 "$(cat "$HFSSS_RUN_NBD_PID_FILE")" 2>/dev/null; then
    echo "ERROR: NBD server died on startup"
    cat "$HFSSS_RUN_NBD_LOG"
    exit 1
fi

echo "Starting QEMU guest (name=$HFSSS_RUN_QEMU_NAME)..."
qemu-system-aarch64 \
    -name "$HFSSS_RUN_QEMU_NAME" \
    -M virt,gic-version=3 -accel hvf -cpu host \
    -m 2G -smp 2 \
    -drive if=pflash,format=raw,file=/opt/homebrew/share/qemu/edk2-aarch64-code.fd,readonly=on \
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

echo "Shutting down..."
hfsss_run_cleanup
trap - EXIT INT TERM
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

echo ""
echo "========================================"
echo "E2E Coverage Summary"
echo "========================================"
lcov --summary "$INFO" --rc branch_coverage=1 --ignore-errors deprecated,inconsistent,inconsistent 2>&1 | grep -E 'lines|functions|branches'
echo ""
echo "HTML report: $COV_DIR/e2e/index.html"
echo "Run workspace: $HFSSS_RUN_WORKSPACE"
