#!/bin/bash
# Run end-to-end fio tests through QEMU guest against the instrumented
# HFSSS NBD server, capturing coverage data from the firmware.
#
# Requires: QEMU installed, port 10809/2222 free, lcov.
# Outputs: build-cov/coverage/e2e.info and build-cov/coverage/e2e/
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT/build-cov"
BIN_DIR="$BUILD_DIR/bin"
COV_DIR="$BUILD_DIR/coverage"
INFO="$COV_DIR/e2e.info"
INFO_RAW="$COV_DIR/e2e.raw.info"
NBD_PID_FILE="/tmp/hfsss_nbd_cov.pid"
QEMU_PID_FILE="/tmp/hfsss_qemu_cov.pid"

mkdir -p "$COV_DIR"

# Preconditions
command -v lcov >/dev/null || { echo "ERROR: lcov not installed"; exit 1; }
[ -x "$BIN_DIR/hfsss-nbd-server" ] || { echo "ERROR: $BIN_DIR/hfsss-nbd-server not built. Run make coverage-build."; exit 1; }
command -v qemu-system-x86_64 >/dev/null || { echo "ERROR: qemu-system-x86_64 not installed"; exit 1; }

cleanup() {
    echo "Cleaning up E2E processes..."
    # Kill QEMU first so NBD client_fd closes, unblocking nbd_async sq_thread.
    # This lets the NBD server reach main() return and flush .gcda via gcov.
    if [ -f "$QEMU_PID_FILE" ]; then
        qpid=$(cat "$QEMU_PID_FILE")
        # Kill children of the start_nvme_test.sh wrapper (QEMU, etc.)
        pkill -P "$qpid" 2>/dev/null || true
        kill "$qpid" 2>/dev/null || true
        # Also kill any lingering qemu-system-aarch64 directly
        pkill -f "qemu-system-aarch64" 2>/dev/null || true
        rm -f "$QEMU_PID_FILE"
        sleep 2  # give NBD async threads time to unblock after client_fd closes
    fi
    if [ -f "$NBD_PID_FILE" ]; then
        pid=$(cat "$NBD_PID_FILE")
        if kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
            for i in $(seq 1 20); do
                kill -0 "$pid" 2>/dev/null || break
                sleep 0.5
            done
            kill -0 "$pid" 2>/dev/null && { echo "WARN: NBD server didn't exit, sending SIGKILL (will lose coverage)"; kill -KILL "$pid" || true; }
        fi
        rm -f "$NBD_PID_FILE"
    fi
}
trap cleanup EXIT INT TERM

bash "$ROOT/scripts/coverage/reset_counters.sh"

echo "Starting instrumented hfsss-nbd-server..."
cd "$ROOT"
"$BIN_DIR/hfsss-nbd-server" -a > /tmp/cov_nbd.log 2>&1 &
echo $! > "$NBD_PID_FILE"
sleep 2

if ! kill -0 "$(cat $NBD_PID_FILE)" 2>/dev/null; then
    echo "ERROR: NBD server died on startup"
    cat /tmp/cov_nbd.log
    exit 1
fi

echo "Starting QEMU guest..."
bash "$ROOT/scripts/start_nvme_test.sh" > /tmp/cov_qemu.log 2>&1 &
echo $! > "$QEMU_PID_FILE"

echo "Waiting for SSH in guest..."
for i in $(seq 1 180); do
    if ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
           -i /tmp/hfsss_qemu_key -p 2222 -o ConnectTimeout=2 \
           root@127.0.0.1 'true' 2>/dev/null; then
        echo "SSH ready after ${i}s"
        break
    fi
    sleep 1
    if [ "$i" -eq 180 ]; then
        echo "ERROR: SSH not ready after 180s"
        exit 1
    fi
done

echo "========================================"
echo "Running fio verification suite..."
echo "========================================"
bash "$ROOT/scripts/fio_verify_suite.sh" 2222 /dev/nvme0n1 || {
    echo "WARN: fio suite reported failures (coverage data still captured)"
}

echo "Shutting down..."
cleanup
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
