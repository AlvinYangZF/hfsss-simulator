#!/bin/bash
# HFSSS + QEMU NVMe Setup Script
# Starts the simulator as a vhost-user-blk backend, then launches QEMU

set -e

SOCKET="/tmp/hfsss-vhost.sock"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
VHOST_BIN="$PROJECT_DIR/build/bin/hfsss-vhost-blk"
GUEST_DIR="$PROJECT_DIR/guest"

# Defaults — use guest/ directory if KERNEL/ROOTFS not set
KERNEL="${KERNEL:-$GUEST_DIR/vmlinuz-lts}"
ROOTFS="${ROOTFS:-$GUEST_DIR/rootfs.qcow2}"
INITRD="${INITRD:-$GUEST_DIR/initramfs-lts}"
MEM="${MEM:-4G}"

# Check prerequisites
if ! command -v qemu-system-aarch64 &>/dev/null; then
    echo "ERROR: qemu-system-aarch64 not found. Install with: brew install qemu"
    exit 1
fi

if [ ! -f "$VHOST_BIN" ]; then
    echo "ERROR: $VHOST_BIN not found. Run 'make' first."
    exit 1
fi

if [ ! -f "$KERNEL" ]; then
    echo "ERROR: Kernel not found at $KERNEL"
    echo ""
    echo "Set up the guest directory:"
    echo "  cd guest/"
    echo "  curl -LO https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/aarch64/netboot/vmlinuz-lts"
    echo "  curl -LO https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/aarch64/netboot/initramfs-lts"
    echo "  qemu-img create -f qcow2 rootfs.qcow2 8G"
    exit 1
fi

# Cleanup on exit
cleanup() {
    echo ""
    echo "Cleaning up..."
    [ -n "$VHOST_PID" ] && kill "$VHOST_PID" 2>/dev/null || true
    rm -f "$SOCKET"
}
trap cleanup EXIT

# Remove stale socket
rm -f "$SOCKET"

# Start vhost-user-blk backend
echo "========================================="
echo "HFSSS vhost-user-blk + QEMU NVMe Launcher"
echo "========================================="
echo "Starting HFSSS vhost-user-blk server..."
"$VHOST_BIN" -s "$SOCKET" &
VHOST_PID=$!

# Wait for socket to appear (up to 30 seconds)
echo "Waiting for vhost server to initialize..."
for i in $(seq 1 30); do
    if [ -S "$SOCKET" ]; then
        break
    fi
    if ! kill -0 "$VHOST_PID" 2>/dev/null; then
        echo "ERROR: vhost-user-blk server exited unexpectedly"
        exit 1
    fi
    sleep 1
done

if [ ! -S "$SOCKET" ]; then
    echo "ERROR: socket $SOCKET not created after 30 seconds"
    kill "$VHOST_PID" 2>/dev/null
    exit 1
fi
echo "vhost-user-blk server running (PID $VHOST_PID, socket: $SOCKET)"

# Build QEMU args
QEMU_ARGS=(
    -M virt,gic-version=3
    -accel hvf
    -cpu host
    -m "$MEM" -smp 4
    -kernel "$KERNEL"
    -initrd "$INITRD"
    -append "console=ttyAMA0 ip=dhcp alpine_repo=http://dl-cdn.alpinelinux.org/alpine/latest-stable/main/"
    -drive "file=$ROOTFS,if=virtio,format=qcow2"
    -chardev "socket,id=char0,path=$SOCKET"
    -device "vhost-user-blk-pci,chardev=char0,num-queues=1"
    -object "memory-backend-file,id=mem,size=$MEM,mem-path=/tmp/qemu-hfsss-mem,share=on"
    -numa "node,memdev=mem"
    -netdev user,id=net0
    -device virtio-net-pci,netdev=net0
    -nographic
)

echo ""
echo "Starting QEMU (Alpine Linux aarch64 guest)..."
echo "  Kernel:  $KERNEL"
echo "  Initrd:  $INITRD"
echo "  Rootfs:  $ROOTFS"
echo "  Memory:  $MEM"
echo ""
echo "Once booted, log in as 'root' (no password), then:"
echo "  lsblk                          # see the vhost-user-blk device"
echo "  apk add nvme-cli fio           # install NVMe tools"
echo "  nvme list                       # list NVMe devices"
echo "  fio --name=t --rw=randwrite --bs=4k --size=16M --filename=/dev/vda --direct=1"
echo ""
echo "Press Ctrl-A X to exit QEMU."
echo "========================================="

qemu-system-aarch64 "${QEMU_ARGS[@]}"

echo "QEMU exited."
