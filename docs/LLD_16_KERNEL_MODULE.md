# LLD-16: Kernel Module (`hfsss_nvme.ko`)

## 1. Overview

`hfsss_nvme.ko` is a Linux PCI driver that presents the HFSSS user-space SSD
simulator as a real `/dev/nvme` block device.  The host kernel's `nvme` driver
binds to the virtual device through the standard NVMe-over-PCIe stack; all
command data is forwarded to the HFSSS user-space daemon via a shared-memory
ring buffer.

**Linux kernel requirement**: ≥ 5.15 (blk-mq multi-queue, MSI-X, PCI endpoint
framework, and `alloc_pages` APIs used are stable from 5.15 onwards).

**Relationship to user-space simulator**: the kernel module is the PCIe
transport layer only.  FTL, media simulation, and NAND wear logic remain in the
user-space daemon (`sssim`).  The two halves communicate exclusively through the
shared-memory ring described in §6.

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

These values are defined in `include/kernel/hfsss_nvme_kmod.h` and referenced
by the `hfsss_pci_ids` table in `src/kernel/hfsss_nvme_kmod.c`.

### 2.2 BAR0 Layout

BAR0 is 16 KB of 64-bit, prefetchable MMIO.  The layout follows NVMe 1.4
§3.1 exactly so the host `nvme.ko` driver requires no modification.

```
0x0000–0x0007  CAP   (RO)   Controller Capabilities
0x0008–0x000B  VS    (RO)   Version — reports 1.4
0x000C–0x000F  INTMS (WO)   Interrupt Mask Set
0x0010–0x0013  INTMC (WO)   Interrupt Mask Clear
0x0014–0x0017  CC    (RW)   Controller Configuration
0x0018–0x001B             (reserved)
0x001C–0x001F  CSTS  (RO)   Controller Status
0x0020–0x0023  NSSR  (WO)   NVM Subsystem Reset
0x0024–0x0027  AQA   (RW)   Admin Queue Attributes
0x0028–0x002F  ASQ   (RW)   Admin SQ Base Address
0x0030–0x0037  ACQ   (RW)   Admin CQ Base Address
0x1000+        Doorbells     SQ Tail / CQ Head (32-bit each, 8-byte stride)
```

### 2.3 PCIe Capabilities Chain

All three capabilities reside in PCI config space (offsets relative to the
standard Type 0 header at offset 0x34 → `capabilities_pointer`):

```
0x40  Power Management (cap_id=0x01)
        next_cap → 0x50
0x50  MSI-X (cap_id=0x11)
        Table BIR=BAR1, Table Offset=0x0000
        PBA   BIR=BAR1, PBA   Offset=0x0800
        next_cap → 0x70
0x70  PCIe Capability Structure (cap_id=0x10)
        Device/Port Type = Endpoint
        next_cap → 0x00 (end of chain)
```

Populated by the VFIO/QEMU device model that fronts the kernel module;
the driver itself reads these capabilities via standard `pci_find_capability`.

---

## 3. NVMe Register Handling (REQ-004, REQ-005, REQ-006)

### 3.1 CAP Register

`HFSSS_NVME_CAP_DEFAULT = 0x0020002800000FFF`:

| Field  | Value | Meaning                        |
|--------|-------|--------------------------------|
| MQES   | 4095  | max queue entries per queue    |
| CQR    | 1     | contiguous queues required     |
| TO     | 40    | 500 ms ready timeout           |
| DSTRD  | 0     | 4-byte doorbell stride         |
| CSS    | 0x01  | NVM command set supported      |

### 3.2 CC.EN State Machine

```
              CC.EN=1
  [DISABLED] ─────────────→ [ENABLED]
      ↑          set CSTS.RDY=1   │
      │                           │
      └───────────────────────────┘
              CC.EN=0 / NSSR write
              clear CSTS.RDY
```

Implementation: `hfsss_nvme_reg_write32()` in `src/kernel/hfsss_nvme_pci.c`
detects the 0→1 and 1→0 transitions on bit CC[0] and updates the `g_csts`
shadow register immediately (no deferred timer in the stub).

### 3.3 NSSR

Writing `0x4E564D45` ("NVMe") resets both CC and CSTS to zero and logs a
kernel info message.  The host NVMe driver re-initialises after observing
CSTS.RDY=0.

---

## 4. Queue Management (REQ-009, REQ-012, REQ-013, REQ-014)

### 4.1 Admin Queue Lifecycle

1. Host writes AQA, ASQ, ACQ registers (queue depth and DMA base addresses).
2. Host sets CC.EN=1.
3. `hfsss_admin_queue_create()` is called from probe; allocates MSI-X vector 0
   for the admin CQ and registers `hfsss_msix_handler`.
4. Admin commands (Identify, Create I/O SQ/CQ, Set Features) are processed via
   the shared-memory submit ring.

### 4.2 I/O Queue Creation Limits

- Maximum I/O queue pairs: `HFSSS_MAX_IO_QUEUES = 64`.
- Actual queue count capped at `min(HFSSS_MAX_IO_QUEUES, num_online_cpus())` to
  avoid over-committing MSI-X vectors.
- Queue IDs 1–64 map 1:1 to MSI-X vectors 1–64.

### 4.3 MSI-X Vector Mapping

`pci_alloc_irq_vectors(dev, 1, HFSSS_MAX_IO_QUEUES + 1, PCI_IRQ_MSIX)` is
called once during admin-queue creation.  Each I/O queue then calls
`pci_irq_vector(dev, qid)` to obtain its vector and `request_irq` to register
the handler.  Affinity hints are set via `/proc/irq/<n>/smp_affinity` by the
user-space administrator after `insmod`.

### 4.4 Interrupt Coalescing Parameters

Configured via NVMe Set Features, Feature ID 0x08:

| Parameter        | Default | Range    | Unit     |
|------------------|---------|----------|----------|
| `coalesce_thr`   | 0       | 0–255    | completions |
| `coalesce_time`  | 0       | 0–255    | 100 µs   |

When `coalesce_thr == 0` (default), every completion raises an immediate
interrupt.  When non-zero, `hfsss_process_completion()` accumulates
`pending_cpls` and fires the interrupt only when the threshold is reached or
the time window expires.

---

## 5. DMA Data Path (REQ-019, REQ-020, REQ-021)

### 5.1 PRP Traversal

PRP (Physical Region Page) parsing reuses `src/pcie/prp.c` from the existing
user-space simulator.  The kernel-mode caller wraps each host physical address
in a `struct page *` obtained via `pfn_to_page(prp_addr >> PAGE_SHIFT)`.

PRP modes supported:
- **PRP1 only**: transfer ≤ one page.
- **PRP1 + PRP2**: transfer ≤ two pages.
- **PRP list**: PRP2 points to a page containing an array of PRP entries;
  traversed iteratively until transfer length is satisfied.

### 5.2 DMA Mapping

```c
dma_addr_t dma = dma_map_page(&dev->dev, page, offset, len, direction);
/* ... DMA transfer ... */
dma_unmap_page(&dev->dev, dma, len, direction);
```

`dma_map_page` ensures cache coherency and programs the IOMMU if present.
All mappings use streaming (non-coherent) DMA semantics for compatibility with
hardware IOMMU passthrough mode.

### 5.3 IOMMU Domain

When an IOMMU is present, `iommu_domain_alloc(&platform_bus_type)` creates a
private domain for the HFSSS virtual device.  DMA addresses seen by the device
are IOVA (I/O Virtual Addresses) that the IOMMU translates to physical
addresses, providing memory isolation.

---

## 6. Kernel-User Ring Buffer (REQ-022)

### 6.1 Shared Memory Layout

```
┌────────────────────────────────────────────────────┐  offset 0x000
│  hfsss_shmem_hdr (64 bytes)                        │
│    magic=0x48465353  version=1  ring_size=1024     │
│    submit_head  submit_tail                        │
│    complete_head  complete_tail                    │
├────────────────────────────────────────────────────┤  offset 0x040
│  Submit ring: 1024 × struct nvme_command (64 B)    │  = 64 KB
├────────────────────────────────────────────────────┤  offset 0x10040
│  Complete ring: 1024 × struct nvme_completion (16 B)│ = 16 KB
└────────────────────────────────────────────────────┘
```

Total: ~80 KB, allocated as physically contiguous pages via `alloc_pages`.

### 6.2 Submit Ring (Kernel → User-Space)

```
Kernel (producer):
  1. Write NVMe command into slot[submit_head % ring_size]
  2. smp_wmb()                       ← store barrier
  3. WRITE_ONCE(submit_head, next)

User-space (consumer):
  1. Poll: while submit_tail == submit_head → spin/yield
  2. smp_rmb()                       ← load barrier
  3. Read command from slot[submit_tail % ring_size]
  4. WRITE_ONCE(submit_tail, next)
```

### 6.3 Complete Ring (User-Space → Kernel)

```
User-space (producer):
  1. Write CQE into slot[complete_head % ring_size]
  2. smp_wmb()
  3. WRITE_ONCE(complete_head, next)

Kernel (consumer — hfsss_shmem_poll_completion):
  1. head = READ_ONCE(complete_head)
  2. smp_rmb()
  3. Read CQE from slot[complete_tail % ring_size]
  4. Invoke blk-mq completion path
  5. WRITE_ONCE(complete_tail, next)
```

---

## 7. Host Integration (REQ-124, REQ-125, REQ-126)

### 7.1 Block Device Registration

After successful probe and CC.EN=1 transition, the standard Linux `nvme.ko`
(already loaded) discovers the controller through the PCI subsystem, creates
`/dev/nvme0`, and registers namespace `/dev/nvme0n1` visible to `lsblk` and
`fdisk`.

### 7.2 nvme-cli Compatibility

All mandatory Identify responses are implemented:
- `nvme id-ctrl /dev/nvme0`   — returns `struct nvme_identify_ctrl`
- `nvme smart-log /dev/nvme0` — returns SMART/Health log page
- `nvme format /dev/nvme0n1`  — triggers a simulated format sequence

### 7.3 fio io_uring Test

```bash
fio --ioengine=io_uring --direct=1 --rw=randread \
    --bs=4k --iodepth=128 --numjobs=32            \
    --filename=/dev/nvme0n1 --name=hfsss_bench
```

`io_uring` with `direct=1` bypasses the page cache and exercises the full DMA
path described in §5.  `iodepth=128` with `numjobs=32` drives 4096 concurrent
I/O requests across all I/O queues.

---

## 8. Build and Load Instructions

### Build

```bash
# On a Linux system with kernel headers installed:
sudo apt-get install linux-headers-$(uname -r) build-essential
make -f Makefile.kmod modules
```

The `Kbuild` file at the project root drives compilation of the four source
files under `src/kernel/` into a single `hfsss_nvme.ko` object.

### Load

```bash
# Start the user-space daemon first (provides the shmem backend):
./build/hfsss_daemon --shmem /dev/shm/hfsss &

# Load the kernel module:
sudo insmod hfsss_nvme.ko

# Verify PCI device is visible:
lspci | grep -i nvme

# Verify NVMe device is enumerated:
sudo nvme list
# Expected: /dev/nvme0   HFSSS Virtual NVMe   ...
```

### Unload

```bash
sudo rmmod hfsss_nvme
```
