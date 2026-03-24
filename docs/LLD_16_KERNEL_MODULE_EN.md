# LLD-16: Kernel Module (`hfsss_nvme.ko`) Low-Level Design

## Revision History

| Version | Date       | Author | Description     |
|---------|------------|--------|-----------------|
| V1.0    | 2026-03-15 | HFSSS  | Initial release |

## Table of Contents

1. [Overview](#1-overview)
2. [PCI Device Emulation](#2-pci-device-emulation)
3. [NVMe Register Handling](#3-nvme-register-handling)
4. [Queue Management](#4-queue-management)
5. [DMA Data Path](#5-dma-data-path)
6. [Kernel-User Ring Buffer](#6-kernel-user-ring-buffer)
7. [Host Integration](#7-host-integration)
8. [Build and Load Instructions](#8-build-and-load-instructions)

---

## 1. Overview

`hfsss_nvme.ko` is a Linux PCI driver that presents the HFSSS user-space SSD simulator as a real `/dev/nvme` block device. The host kernel's `nvme` driver binds to the virtual device through the standard NVMe-over-PCIe stack; all command data is forwarded to the HFSSS user-space daemon via a shared-memory ring buffer.

**Linux kernel requirement**: >= 5.15 (blk-mq multi-queue, MSI-X, PCI endpoint framework, and `alloc_pages` APIs used are stable from 5.15 onwards).

**Relationship to user-space simulator**: the kernel module is the PCIe transport layer only. FTL, media simulation, and NAND wear logic remain in the user-space daemon (`sssim`). The two halves communicate exclusively through the shared-memory ring described in section 6.

---

## 2. PCI Device Emulation (REQ-001, REQ-002, REQ-003)

### 2.1 Vendor / Device Identifiers

| Field           | Value    | Notes                         |
|-----------------|----------|-------------------------------|
| Vendor ID       | `0x1DEA` | HFSSS-assigned vendor         |
| Device ID       | `0x4E56` | ASCII 'NV'                    |
| Subsystem Vendor| `0x1DEA` |                               |
| Subsystem Device| `0x0001` |                               |
| Class Code      | `0x010802`| Mass Storage / NVMe           |

### 2.2 BAR0 Layout

BAR0 is 16 KB of 64-bit, prefetchable MMIO. The layout follows NVMe 1.4 section 3.1 exactly so the host `nvme.ko` driver requires no modification.

```
0x0000-0x0007  CAP   (RO)   Controller Capabilities
0x0008-0x000B  VS    (RO)   Version - reports 1.4
0x000C-0x000F  INTMS (WO)   Interrupt Mask Set
0x0010-0x0013  INTMC (WO)   Interrupt Mask Clear
0x0014-0x0017  CC    (RW)   Controller Configuration
0x001C-0x001F  CSTS  (RO)   Controller Status
0x0020-0x0023  NSSR  (WO)   NVM Subsystem Reset
0x0024-0x0027  AQA   (RW)   Admin Queue Attributes
0x0028-0x002F  ASQ   (RW)   Admin SQ Base Address
0x0030-0x0037  ACQ   (RW)   Admin CQ Base Address
0x1000+        Doorbells     SQ Tail / CQ Head (32-bit each, 8-byte stride)
```

### 2.3 PCIe Capabilities Chain

```
0x40  Power Management (cap_id=0x01) -> next 0x50
0x50  MSI-X (cap_id=0x11) -> next 0x70
0x70  PCIe Capability Structure (cap_id=0x10) -> end
```

---

## 3. NVMe Register Handling (REQ-004, REQ-005, REQ-006)

### 3.1 CAP Register

`HFSSS_NVME_CAP_DEFAULT = 0x0020002800000FFF`: MQES=4095, CQR=1, TO=40 (500ms), DSTRD=0, CSS=0x01.

### 3.2 CC.EN State Machine

```
[DISABLED] --CC.EN=1--> [ENABLED, CSTS.RDY=1]
[ENABLED]  --CC.EN=0--> [DISABLED, CSTS.RDY=0]
```

### 3.3 NSSR

Writing `0x4E564D45` ("NVMe") resets CC and CSTS to zero. Host NVMe driver re-initializes after observing CSTS.RDY=0.

---

## 4. Queue Management (REQ-009, REQ-012, REQ-013, REQ-014)

### 4.1 Admin Queue Lifecycle

Host writes AQA/ASQ/ACQ, sets CC.EN=1. `hfsss_admin_queue_create()` allocates MSI-X vector 0. Admin commands processed via shared-memory submit ring.

### 4.2 I/O Queue Limits

- Maximum I/O queue pairs: `HFSSS_MAX_IO_QUEUES = 64`
- Actual count capped at `min(64, num_online_cpus())`
- Queue IDs 1-64 map 1:1 to MSI-X vectors 1-64

### 4.3 MSI-X Vector Mapping

`pci_alloc_irq_vectors(dev, 1, 65, PCI_IRQ_MSIX)` called during admin-queue creation. Each I/O queue uses `request_irq` with its vector.

### 4.4 Interrupt Coalescing

Configured via NVMe Set Features (Feature ID 0x08). Default: immediate interrupt per completion. When `coalesce_thr > 0`, completions accumulate until threshold or time window expires.

---

## 5. DMA Data Path (REQ-019, REQ-020, REQ-021)

### 5.1 PRP Traversal

PRP parsing reuses `src/pcie/prp.c`. Supports PRP1 only (<=1 page), PRP1+PRP2 (<=2 pages), and PRP list modes.

### 5.2 DMA Mapping

```c
dma_addr_t dma = dma_map_page(&dev->dev, page, offset, len, direction);
dma_unmap_page(&dev->dev, dma, len, direction);
```

All mappings use streaming DMA semantics for IOMMU compatibility.

### 5.3 IOMMU Domain

When IOMMU is present, a private domain provides memory isolation for the virtual device.

---

## 6. Kernel-User Ring Buffer (REQ-022)

### 6.1 Shared Memory Layout

```
+----------------------------------------------------+  offset 0x000
|  hfsss_shmem_hdr (64 bytes)                        |
|    magic=0x48465353  version=1  ring_size=1024      |
|    submit_head  submit_tail                         |
|    complete_head  complete_tail                     |
+----------------------------------------------------+  offset 0x040
|  Submit ring: 1024 x struct nvme_command (64 B)    |  = 64 KB
+----------------------------------------------------+  offset 0x10040
|  Complete ring: 1024 x struct nvme_completion (16 B)|  = 16 KB
+----------------------------------------------------+
```

Total: ~80 KB, allocated as physically contiguous pages via `alloc_pages`.

### 6.2 Submit Ring (Kernel -> User-Space)

```
Kernel (producer): write command to slot[submit_head % ring_size]
  -> smp_wmb() -> WRITE_ONCE(submit_head, next)
User-space (consumer): poll submit_tail == submit_head
  -> smp_rmb() -> read command -> WRITE_ONCE(submit_tail, next)
```

### 6.3 Complete Ring (User-Space -> Kernel)

```
User-space (producer): write CQE to slot[complete_head % ring_size]
  -> smp_wmb() -> WRITE_ONCE(complete_head, next)
Kernel (consumer): READ_ONCE(complete_head)
  -> smp_rmb() -> read CQE -> blk-mq completion -> WRITE_ONCE(complete_tail, next)
```

---

## 7. Host Integration (REQ-124, REQ-125, REQ-126)

### 7.1 Block Device Registration

After probe and CC.EN=1, the standard Linux `nvme.ko` discovers the controller, creates `/dev/nvme0`, and registers `/dev/nvme0n1`.

### 7.2 nvme-cli Compatibility

- `nvme id-ctrl /dev/nvme0` - returns Identify Controller
- `nvme smart-log /dev/nvme0` - returns SMART/Health log page
- `nvme format /dev/nvme0n1` - triggers simulated format

### 7.3 fio io_uring Test

```bash
fio --ioengine=io_uring --direct=1 --rw=randread \
    --bs=4k --iodepth=128 --numjobs=32 \
    --filename=/dev/nvme0n1 --name=hfsss_bench
```

---

## 8. Build and Load Instructions

### Build

```bash
sudo apt-get install linux-headers-$(uname -r) build-essential
make -f Makefile.kmod modules
```

### Load

```bash
./build/hfsss_daemon --shmem /dev/shm/hfsss &
sudo insmod hfsss_nvme.ko
lspci | grep -i nvme
sudo nvme list
```

### Unload

```bash
sudo rmmod hfsss_nvme
```

---

**Document Statistics**:
- Requirements covered: REQ-001 through REQ-006, REQ-009, REQ-012-014, REQ-019-022, REQ-124-126
- Source files: `src/kernel/hfsss_nvme_kmod.c`, `src/kernel/hfsss_nvme_pci.c`
- Kernel module: `hfsss_nvme.ko`

## Appendix: Cross-References

| Reference | Document |
|-----------|----------|
| NVMe register definitions | LLD_01_PCIE_NVMe_EMULATION |
| HAL PCIe link management | LLD_13_HAL_ADVANCED |
| User-space FTL/media | LLD_11_FTL_RELIABILITY |
