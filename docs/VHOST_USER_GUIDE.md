# HFSSS vhost-user-blk Integration Guide

## What It Does

The `hfsss-vhost-blk` binary exposes the HFSSS NVMe simulator as a
**vhost-user block device** over a Unix domain socket.  QEMU connects to
that socket and presents the device to the guest as a `vhost-user-blk-pci`
NVMe-backed drive — without any kernel driver or real hardware.

This lets you boot a Linux guest and exercise the full HFSSS firmware stack
(FTL, media model, PCIe/NVMe) from inside a VM.

---

## Architecture

```
 Guest OS (Linux)
   |  virtio-blk driver
   v
 QEMU (qemu-system-aarch64)
   |  vhost-user protocol over Unix socket
   v
 hfsss-vhost-blk  <-- this server
   |  nvme_uspace API
   v
 HFSSS NVMe simulator
   |
   +-- FTL / Flash Translation Layer
   +-- HAL / Media model
   +-- PCIe/NVMe command queue engine
```

Data path:
1. Guest issues a virtio-blk read/write request.
2. QEMU forwards it via the vhost-user socket.
3. `hfsss-vhost-blk` translates it to an NVMe I/O command.
4. The command passes through the full HFSSS stack (queue, FTL, flash model).
5. Completion status and data are returned to QEMU and injected into the guest.

---

## Prerequisites

```bash
# macOS
brew install qemu

# Linux
apt install qemu-system-aarch64   # Debian/Ubuntu
dnf install qemu-system-aarch64   # Fedora
```

You also need a Linux aarch64 guest image (kernel + root filesystem).  A
minimal Alpine Linux virt image works well:

```bash
wget https://dl-cdn.alpinelinux.org/alpine/v3.19/releases/aarch64/alpine-virt-3.19.1-aarch64.iso
```

---

## Quick Start

```bash
# 1. Build the project
make -j$(nproc)

# 2. Run the vhost tests (optional sanity check)
./build/bin/test_vhost_proto

# 3. Launch QEMU with the HFSSS backend
KERNEL=/path/to/vmlinuz ROOTFS=/path/to/rootfs.qcow2 \
    scripts/run_qemu_nvme.sh
```

The script starts `hfsss-vhost-blk` in the background, then launches QEMU.
Press `Ctrl-A X` to exit QEMU; the script will clean up the socket on exit.

---

## Manual Invocation

Start the server separately:

```bash
./build/bin/hfsss-vhost-blk -s /tmp/hfsss-vhost.sock
```

Then start QEMU manually with:

```
-chardev socket,id=char0,path=/tmp/hfsss-vhost.sock,reconnect=1
-device  vhost-user-blk-pci,chardev=char0,num-queues=1
-object  memory-backend-file,id=mem,size=4G,mem-path=/tmp/qemu-mem,share=on
-numa    node,memdev=mem
```

The `-object memory-backend-file ... share=on` and `-numa node,memdev=mem`
lines are **required** by the vhost-user protocol so QEMU can share guest
memory with the backend via file descriptors.

---

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| `bind: Address already in use` | Stale socket file | `rm /tmp/hfsss-vhost.sock` |
| QEMU exits immediately | Server not running | Start `hfsss-vhost-blk` before QEMU |
| `accel hvf not available` | Not on Apple Silicon or no HVF | Replace `-accel hvf` with `-accel tcg` |
| Guest sees no block device | Incompatible QEMU version | Requires QEMU >= 5.0 for `vhost-user-blk-pci` |
| Permission denied on socket | Socket directory not writable | Use a path under `/tmp` or your home directory |

Minimum supported QEMU version: **5.0**.
