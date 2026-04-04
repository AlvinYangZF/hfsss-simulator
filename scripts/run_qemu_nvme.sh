#!/bin/bash
# HFSSS + QEMU NVMe Setup Script
# Starts the simulator as a vhost-user-blk backend, then launches QEMU

set -e

SOCKET="/tmp/hfsss-vhost.sock"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
VHOST_BIN="$PROJECT_DIR/build/bin/hfsss-vhost-blk"

# Check prerequisites
if ! command -v qemu-system-aarch64 &>/dev/null; then
    echo "ERROR: qemu-system-aarch64 not found. Install with: brew install qemu"
    exit 1
fi

if [ ! -f "$VHOST_BIN" ]; then
    echo "ERROR: $VHOST_BIN not found. Run 'make' first."
    exit 1
fi

# Check for Linux guest image
KERNEL="${KERNEL:-}"
ROOTFS="${ROOTFS:-}"
if [ -z "$KERNEL" ] || [ -z "$ROOTFS" ]; then
    echo "Usage: KERNEL=vmlinuz ROOTFS=rootfs.qcow2 $0"
    echo ""
    echo "Download an Alpine Linux virt image for aarch64:"
    echo "  wget https://dl-cdn.alpinelinux.org/alpine/v3.19/releases/aarch64/alpine-virt-3.19.1-aarch64.iso"
    echo ""
    echo "Or use a cloud image with extracted kernel/initramfs."
    exit 1
fi

# Cleanup on exit
cleanup() {
    echo "Cleaning up..."
    [ -n "$VHOST_PID" ] && kill "$VHOST_PID" 2>/dev/null || true
    rm -f "$SOCKET"
}
trap cleanup EXIT

# Remove stale socket
rm -f "$SOCKET"

# Start vhost-user-blk backend
echo "Starting HFSSS vhost-user-blk server..."
"$VHOST_BIN" -s "$SOCKET" &
VHOST_PID=$!
sleep 1

if ! kill -0 "$VHOST_PID" 2>/dev/null; then
    echo "ERROR: vhost-user-blk server failed to start"
    exit 1
fi

echo "Starting QEMU with NVMe device..."
qemu-system-aarch64 \
    -M virt,gic-version=3 -accel hvf -cpu host \
    -m 4G -smp 4 \
    -kernel "$KERNEL" \
    -append "root=/dev/vda console=ttyAMA0" \
    -drive "file=$ROOTFS,if=virtio,format=qcow2" \
    -chardev "socket,id=char0,path=$SOCKET,reconnect=1" \
    -device "vhost-user-blk-pci,chardev=char0,num-queues=1" \
    -object "memory-backend-file,id=mem,size=4G,mem-path=/tmp/qemu-hfsss-mem,share=on" \
    -numa "node,memdev=mem" \
    -nographic

echo "QEMU exited."
