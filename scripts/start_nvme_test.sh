#!/bin/bash
# Quick-start: HFSSS simulator + QEMU NVMe test environment
# No setup needed — uses pre-saved Alpine image with fio installed
#
# Usage: ./scripts/start_nvme_test.sh [nbd_size_mb] [nbd_port]
#
# SSH into guest: ssh -i /tmp/hfsss_qemu_key -p 2222 root@127.0.0.1

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

NBD_SIZE="${1:-512}"
NBD_PORT="${2:-10809}"
SSH_PORT=2222
SSH_KEY=/tmp/hfsss_qemu_key

# Generate SSH key if missing
if [ ! -f "$SSH_KEY" ]; then
    ssh-keygen -t ed25519 -f "$SSH_KEY" -N "" -q
fi

# Check prerequisites
if [ ! -f build/bin/hfsss-nbd-server ]; then
    echo "Building simulator..."
    make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) >/dev/null 2>&1
fi

if [ ! -f guest/alpine-hfsss.qcow2 ]; then
    echo "ERROR: guest/alpine-hfsss.qcow2 not found. Run the full setup first."
    exit 1
fi

# Cleanup on exit
cleanup() {
    echo ""
    echo "Shutting down..."
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null
    [ -n "$NBD_PID" ] && kill "$NBD_PID" 2>/dev/null
    wait "$QEMU_PID" 2>/dev/null
    wait "$NBD_PID" 2>/dev/null
    echo "Done."
}
trap cleanup EXIT

# Kill stale processes
pkill -f "hfsss-nbd-server.*$NBD_PORT" 2>/dev/null || true
pkill -f "qemu-system-aarch64.*HFSSS" 2>/dev/null || true
sleep 1

# Copy saved UEFI vars (QEMU modifies these at runtime)
cp guest/ovmf_vars-saved.fd guest/ovmf_vars-run.fd

echo "========================================="
echo "HFSSS NVMe Test Environment"
echo "========================================="
echo "NBD size:  ${NBD_SIZE} MB"
echo "NBD port:  ${NBD_PORT}"
echo "SSH port:  ${SSH_PORT}"
echo ""

# Start NBD server (simulator in the data path)
echo "[1/3] Starting HFSSS NBD server..."
./build/bin/hfsss-nbd-server -p "$NBD_PORT" -s "$NBD_SIZE" 2>nbd_server.log &
NBD_PID=$!

for i in $(seq 1 30); do
    if lsof -i :"$NBD_PORT" -P 2>/dev/null | grep -q LISTEN; then
        echo "       NBD server ready (PID $NBD_PID)"
        break
    fi
    sleep 1
done

if ! kill -0 "$NBD_PID" 2>/dev/null; then
    echo "ERROR: NBD server failed to start"
    cat nbd_server.log
    exit 1
fi

# Start QEMU headless
echo "[2/3] Starting QEMU (Alpine Linux + NVMe)..."
qemu-system-aarch64 \
    -M virt,gic-version=3 -accel hvf -cpu host \
    -m 2G -smp 2 \
    -drive if=pflash,format=raw,file=/opt/homebrew/share/qemu/edk2-aarch64-code.fd,readonly=on \
    -drive if=pflash,format=raw,file=guest/ovmf_vars-run.fd \
    -drive file=guest/alpine-hfsss.qcow2,if=virtio,format=qcow2,snapshot=on \
    -drive file=guest/cidata.iso,if=virtio,media=cdrom \
    -drive "driver=nbd,server.type=inet,server.host=127.0.0.1,server.port=$NBD_PORT,if=none,id=nvm0,discard=unmap" \
    -device nvme,serial=HFSSS0001,drive=nvm0 \
    -netdev "user,id=net0,hostfwd=tcp::${SSH_PORT}-:22" \
    -device virtio-net-pci,netdev=net0 \
    -serial file:qemu_console.log \
    -display none &
QEMU_PID=$!

# Wait for SSH to become available
echo "[3/3] Waiting for guest boot..."
for i in $(seq 1 90); do
    if ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
           -o ConnectTimeout=2 -i "$SSH_KEY" -p "$SSH_PORT" root@127.0.0.1 \
           "true" 2>/dev/null; then
        echo "       Guest ready (${i}s)"
        break
    fi
    sleep 1
done

echo ""
echo "========================================="
echo "Environment ready!"
echo "========================================="
echo ""
echo "SSH into guest:"
echo "  ssh -i $SSH_KEY -p $SSH_PORT root@127.0.0.1"
echo ""
echo "Run fio test:"
echo "  ssh -i $SSH_KEY -p $SSH_PORT root@127.0.0.1 \\"
echo "    'fio --name=test --rw=randrw --rwmixread=70 --bs=4k --size=512M \\"
echo "     --filename=/dev/nvme0n1 --direct=1 --ioengine=sync --runtime=60 --time_based'"
echo ""
echo "View simulator log:"
echo "  tail -f nbd_server.log"
echo ""
echo "Press Ctrl-C to shut down."
echo "========================================="

# Wait for user interrupt
wait "$QEMU_PID"
