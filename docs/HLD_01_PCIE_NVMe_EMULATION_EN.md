# HFSSS High-Level Design Document

**Document Name**: PCIe/NVMe Device Emulation Module HLD
**Document Version**: V1.0
**Date**: 2026-03-14
**Design Phase**: V1.0 (Alpha)

---

## IMPORTANT NOTE: Implementation Status

**Design Document**: Describes a kernel-module-based PCIe/NVMe emulation
**Actual Implementation**: User-space only stub structures (no kernel module)

**Coverage Status**: 0/22 requirements implemented for this module (0%)

See [REQUIREMENT_COVERAGE.md](./REQUIREMENT_COVERAGE.md) for complete details.

---

## Revision History

| Version | Date | Author | Description |
|---------|------|--------|-------------|
| V0.1 | 2026-03-08 | Architecture Team | Initial draft |
| V1.0 | 2026-03-08 | Architecture Team | Official release |
| EN-V1.0 | 2026-03-14 | Translation Agent | English translation with implementation notes |

---

## Table of Contents

1. [Module Overview](#1-module-overview)
2. [Requirements Review](#2-requirements-review)
3. [System Architecture](#3-system-architecture)
4. [Detailed Design](#4-detailed-design)
5. [Interface Design](#5-interface-design)
6. [Data Structures](#6-data-structures)
7. [Flow Diagrams](#7-flow-diagrams)
8. [Performance Design](#8-performance-design)
9. [Error Handling](#9-error-handling)
10. [Test Design](#10-test-design)

---

## 1. Module Overview

### 1.1 Module Positioning

The PCIe/NVMe device emulation module is the interface layer between HFSSS and the host Linux OS. The design document describes a Linux kernel module implementation, similar to NVMeVirt, that virtualizes a standard PCIe NVMe device in the host Linux kernel. The core challenge is to trick the Linux NVMe driver into believing a real PCIe NVMe SSD exists without using real PCIe hardware IP.

**Implementation Note**: The actual codebase only contains user-space stub header files. No kernel module is implemented. See [include/pcie/](../include/pcie/) for the stub structures.

### 1.2 Module Responsibilities

This module is responsible for:
- PCIe configuration space emulation, allowing `lspci` to recognize the virtual NVMe device
- NVMe controller register emulation, implementing the complete NVMe 2.0 controller register set
- NVMe queue management, including creation, deletion, and management of Admin Queue and I/O Queues
- MSI-X interrupt emulation, delivering command completion interrupts to the host
- DMA data transfer, transferring data between host memory and the emulated storage backend
- Communication with user-space daemon via shared memory and Ring Buffer

### 1.3 Module Boundaries

**Included in this module**:
- PCIe device driver framework
- PCI configuration space emulation
- NVMe controller register emulation
- NVMe Admin/I/O command processing
- SQ/CQ management
- MSI-X interrupt mechanism
- DMA engine emulation
- Kernel-user space communication interface

**Not included in this module**:
- FTL algorithms (implemented by user-space daemon)
- NAND media emulation (implemented by user-space daemon)
- GC/WL algorithms (implemented by user-space daemon)

---

## 2. Requirements Review

### 2.1 Requirements Traceability Matrix

| Requirement ID | Description | Priority | Version | Implementation Status |
|----------------|-------------|----------|---------|----------------------|
| REQ-001 | PCIe Configuration Space Emulation | P0 | V1.0 | ❌ Stub Only |
| REQ-002 | PCIe Capabilities Emulation | P0 | V1.0 | ❌ Stub Only |
| REQ-003 | BAR Mapping | P0 | V1.0 | ❌ Stub Only |
| REQ-004 | NVMe Controller Register Emulation | P0 | V1.0 | ❌ Stub Only |
| REQ-005 | NVMe Doorbell Processing | P0 | V1.0 | ❌ Stub Only |
| REQ-006 | NVMe Controller State Machine | P0 | V1.0 | ❌ Stub Only |
| REQ-007 | Doorbell Processing | P0 | V1.0 | ❌ Stub Only |
| REQ-008 | Identify Command | P0 | V1.0 | ❌ Stub Only |
| REQ-009 | Admin Queue Management | P0 | V1.0 | ❌ Stub Only |
| REQ-010 | I/O Queue Management | P0 | V1.0 | ❌ Stub Only |
| REQ-011 | Completion Queue Processing | P0 | V1.0 | ❌ Stub Only |
| REQ-012 | MSI-X Interrupt Emulation | P0 | V1.0 | ❌ Stub Only |
| REQ-013 | MSI-X Table/PBA | P0 | V1.0 | ❌ Stub Only |
| REQ-014 | PRP/SGL Parser | P0 | V1.0 | ❌ Stub Only |
| REQ-015 | NVMe Admin Command Processing | P0 | V1.0 | ❌ Stub Only |
| REQ-016 | NVMe I/O Command Processing | P0 | V1.0 | ❌ Stub Only |
| REQ-017 | NVMe Namespace Management | P0 | V1.0 | ❌ Stub Only |
| REQ-018 | DMA Engine | P0 | V1.0 | ❌ Stub Only |
| REQ-019 | PRP/SGL DMA | P0 | V1.0 | ❌ Stub Only |
| REQ-020 | IOMMU Support | P1 | V1.5 | ❌ Not Implemented |
| REQ-021 | Shared Memory Interface | P0 | V1.0 | ❌ Stub Only |
| REQ-022 | Kernel-User Space Communication | P0 | V1.0 | ❌ Stub Only |

### 2.2 Key Performance Requirements

| Metric | Target | Description |
|--------|--------|-------------|
| Command Processing Latency | < 10μs | Latency from SQ fetch to user-space notification |
| Interrupt Delivery Latency | < 5μs | Latency from CQ write to MSI-X trigger |
| Max IOPS | > 1,000,000 | Theoretical peak without ECC |
| Queue Depth Support | 65535 | Maximum supported queue depth |
| Queue Pairs | 64 | Maximum supported I/O queue pairs |

---

## 3. System Architecture

### 3.1 Module Layer Architecture

**Design Document Architecture (Kernel Module)**:

```
┌─────────────────────────────────────────────────────────────────┐
│                    Host Linux OS                                │
│  ┌──────────────┐  ┌──────────────┐  ┌─────────────────────┐  │
│  │  NVMe Driver │  │  File System │  │  fio / nvme-cli     │  │
│  └──────┬───────┘  └──────┬───────┘  └──────────┬──────────┘  │
└─────────┼─────────────────────┼───────────────────────────┼───────┘
          │ PCIe/MMIO           │ Block I/O              │ Sysfs
          │                     │                       │
┌─────────▼─────────────────────▼───────────────────────────▼───────┐
│              HFSSS Kernel Module (hfsss_nvme.ko)                │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  PCIe Emulation Sublayer (pci.c)                           │  │
│  │  - PCI config space (256B base + 4KB extended)             │  │
│  │  - BAR register mapping                                      │  │
│  │  - PCIe Capabilities list                                   │  │
│  │  - PCI device register/unregister                            │  │
│  └──────────────────────┬────────────────────────────────────────┘  │
│                         │ MMIO access                                 │
│  ┌──────────────────────▼────────────────────────────────────────┐  │
│  │  NVMe Protocol Sublayer (nvme.c)                            │  │
│  │  - Controller registers (CAP/VS/CC/CSTS/AQA/ASQ/ACQ etc.)  │  │
│  │  - Doorbell registers (SQ Tail/CQ Head)                     │  │
│  │  - Admin command processing (admin.c)                        │  │
│  │  - I/O command processing (io.c)                             │  │
│  └──────────────────────┬────────────────────────────────────────┘  │
│                         │ Command/Queue                              │
│  ┌──────────────────────▼────────────────────────────────────────┐  │
│  │  Queue Management Sublayer (queue.c)                         │  │
│  │  - Admin SQ/CQ (QID=0)                                       │  │
│  │  - I/O SQ/CQ (QID=1~63)                                     │  │
│  │  - PRP/SGL parsing engine                                     │  │
│  │  - CQE construction and write                                 │  │
│  └──────────────────────┬────────────────────────────────────────┘  │
│                         │ Interrupt                                   │
│  ┌──────────────────────▼────────────────────────────────────────┐  │
│  │  MSI-X Interrupt Sublayer (msix.c)                          │  │
│  │  - MSI-X Table management                                     │  │
│  │  - MSI-X PBA management                                       │  │
│  │  - Interrupt delivery (apic->send_IPI)                       │  │
│  │  - Interrupt coalescing                                       │  │
│  └──────────────────────┬────────────────────────────────────────┘  │
│                         │ DMA                                         │
│  ┌──────────────────────▼────────────────────────────────────────┐  │
│  │  DMA Engine Sublayer (dma.c)                                 │  │
│  │  - Host memory mapping (kmap/kmap_atomic)                    │  │
│  │  - Data copy (memcpy_toio/memcpy_fromio)                     │  │
│  │  - IOMMU support (dma_map_page/dma_unmap_page)               │  │
│  │  - NUMA affinity optimization                                 │  │
│  └──────────────────────┬────────────────────────────────────────┘  │
│                         │ Shared Memory                               │
│  ┌──────────────────────▼────────────────────────────────────────┐  │
│  │  User-Space Communication Sublayer (shmem.c)                 │  │
│  │  - Shared memory Ring Buffer (16384 slots × 128B)           │  │
│  │  - Lock-free SPSC/MPMC queue                                  │  │
│  │  - eventfd notification mechanism                              │  │
│  │  - mmap interface (user-space access)                         │  │
│  └───────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
                          │
                          │ Shared Memory / ioctl
                          │
┌─────────────────────────▼─────────────────────────────────────────┐
│           User-Space Daemon (hfsss-daemon)                        │
│  - Controller Thread                                               │
│  - Firmware Core Threads                                          │
│  - Media Threads                                                   │
└─────────────────────────────────────────────────────────────────────┘
```

**Actual Implementation (User-Space Only)**:

The actual code only provides header file definitions in `include/pcie/`:

```
┌─────────────────────────────────────────────────────────────────┐
│           User-Space Application                                  │
│                                                                   │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Header-Only Structures (No Implementation)              │  │
│  │  - pci.h: PCI config space structures                    │  │
│  │  - nvme.h: NVMe registers, SQ/CQ entries                 │  │
│  │  - queue.h: Queue management structures                   │  │
│  │  - msix.h: MSI-X structures                               │  │
│  │  - dma.h: DMA structures                                   │  │
│  │  - shmem.h: Shared memory structures                       │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                   │
│  No kernel module, no actual PCIe/NVMe emulation                │
│  (Top-level sssim.h provides direct read/write API instead)    │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 Component Decomposition

#### 3.2.1 PCIe Emulation Sublayer (pci.c)

**Responsibilities**:
- Implement virtual PCIe device registration and enumeration
- Emulate PCI configuration space (Type 0)
- Implement PCIe Capabilities list
- Manage BAR (Base Address Register) mapping
- Handle PCI configuration space reads and writes

**Key Components** (from header files):
- `struct pci_dev_ctx`: PCI device context (see [include/pcie/pci.h](../include/pcie/pci.h))
- `struct pci_config_header`: PCI configuration header
- `struct pci_capabilities`: PCI capabilities structure

**Implementation Status**: ✅ Structures defined, ❌ No implementation

#### 3.2.2 NVMe Protocol Sublayer (nvme.c, admin.c, io.c)

**Responsibilities**:
- Implement NVMe controller register set
- Handle MMIO register reads and writes
- Admin command parsing and processing
- I/O command parsing and processing
- Command dispatch to queue management sublayer

**Key Components** (from header files):
- `struct nvme_ctrl_ctx`: NVMe controller context (see [include/pcie/nvme.h](../include/pcie/nvme.h))
- `struct nvme_controller_regs`: NVMe controller registers
- `struct nvme_sq_entry`: NVMe submission queue entry
- `struct nvme_cq_entry`: NVMe completion queue entry

**Implementation Status**: ✅ Structures defined, ❌ No implementation

#### 3.2.3 Queue Management Sublayer (queue.c)

**Responsibilities**:
- Admin SQ/CQ creation and management
- Dynamic creation and deletion of I/O SQ/CQ
- Monitoring and processing of SQ Tail Doorbell
- Processing of CQ Head Doorbell
- PRP/SGL parsing
- CQE construction and writing

**Key Components** (from header files):
- See [include/pcie/queue.h](../include/pcie/queue.h)

**Implementation Status**: ✅ Structures defined, ❌ No implementation

#### 3.2.4 MSI-X Interrupt Sublayer (msix.c)

**Responsibilities**:
- MSI-X Table management
- MSI-X PBA (Pending Bit Array) management
- Interrupt delivery mechanism implementation
- Interrupt coalescing support
- Interrupt affinity support

**Key Components** (from header files):
- See [include/pcie/msix.h](../include/pcie/msix.h)

**Implementation Status**: ✅ Structures defined, ❌ No implementation

#### 3.2.5 DMA Engine Sublayer (dma.c)

**Responsibilities**:
- Host memory mapping and access
- Data transfer between emulated storage and host memory
- PRP List traversal and concatenation
- SGL Descriptor parsing and processing
- IOMMU support
- NUMA affinity optimization

**Key Components** (from header files):
- See [include/pcie/dma.h](../include/pcie/dma.h)

**Implementation Status**: ✅ Structures defined, ❌ No implementation

#### 3.2.6 User-Space Communication Sublayer (shmem.c)

**Responsibilities**:
- Shared memory region creation and management
- Ring Buffer implementation (lock-free SPSC/MPMC)
- eventfd notification mechanism implementation
- mmap interface implementation (user-space access)
- ioctl interface implementation (control commands)

**Key Components** (from header files):
- See [include/pcie/shmem.h](../include/pcie/shmem.h)

**Implementation Status**: ✅ Structures defined, ❌ No implementation

---

## 4. Detailed Design

### 4.1 PCIe Configuration Space Emulation Design

#### 4.1.1 PCI Configuration Header Structure

**From Actual Implementation** ([include/pcie/pci.h](../include/pcie/pci.h)):

```c
#define PCI_CONFIG_SPACE_SIZE 256
#define PCI_EXT_CONFIG_SPACE_SIZE 4096

struct pci_config_header {
    /* 0x00 - 0x3F: PCI Type 0 Configuration Header */
    u16 vendor_id;           /* 0x00: Vendor ID */
    u16 device_id;           /* 0x02: Device ID */
    u16 command;             /* 0x04: Command Register */
    u16 status;              /* 0x06: Status Register */
    u8  revision_id;         /* 0x08: Revision ID */
    u8  class_code[3];       /* 0x09: Class Code */
    u8  cache_line_size;     /* 0x0C: Cache Line Size */
    u8  latency_timer;        /* 0x0D: Latency Timer */
    u8  header_type;          /* 0x0E: Header Type */
    u8  bist;                /* 0x0F: BIST */
    u32 bar[6];              /* 0x10-0x27: Base Address Registers */
    u32 cardbus_cis;         /* 0x28: CardBus CIS Pointer */
    u16 subsystem_vendor_id; /* 0x2C: Subsystem Vendor ID */
    u16 subsystem_id;         /* 0x2E: Subsystem ID */
    u32 expansion_rom;       /* 0x30: Expansion ROM Base Address */
    u8  capabilities_ptr;     /* 0x34: Capabilities Pointer */
    u8  reserved1[7];        /* 0x35-0x3B: Reserved */
    u8  interrupt_line;       /* 0x3C: Interrupt Line */
    u8  interrupt_pin;        /* 0x3D: Interrupt Pin */
    u8  min_gnt;             /* 0x3E: Minimum Grant */
    u8  max_lat;             /* 0x3F: Maximum Latency */
} __attribute__((packed));

#define PCI_CLASS_CODE_STORAGE 0x01
#define PCI_CLASS_SUBCLASS_NVME 0x08
#define PCI_CLASS_INTERFACE_NVME 0x02

/* Vendor ID / Device ID (Research-use reserved ID range) */
#define HFSSS_VENDOR_ID 0x1D1D
#define HFSSS_DEVICE_ID 0x2001
#define HFSSS_REVISION_ID 0x01
```

#### 4.1.2 PCIe Capabilities List Design

**From Actual Implementation** ([include/pcie/pci.h](../include/pcie/pci.h)):

```c
/* PCI Capability IDs */
#define PCI_CAP_ID_PM    0x01  /* Power Management */
#define PCI_CAP_ID_MSI   0x05  /* MSI */
#define PCI_CAP_ID_MSIX  0x11  /* MSI-X */
#define PCI_CAP_ID_EXP   0x10  /* PCI Express */

/* PCI Capability Header */
struct pci_cap_header {
    u8 cap_id;
    u8 next;
} __attribute__((packed));

/* PCI Power Management Capability (0x01) */
struct pci_cap_pm {
    struct pci_cap_header hdr;
    u16 pm_cap;
    u16 pm_ctrl_sts;
    u8  pm_ext;
    u8  data[3];
} __attribute__((packed));

/* PCI MSI Capability (0x05) */
struct pci_cap_msi {
    struct pci_cap_header hdr;
    u16 message_control;
    u32 message_addr_low;
    u32 message_addr_high;
    u16 message_data;
    u16 reserved;
    u32 mask_bits;
    u32 pending_bits;
} __attribute__((packed));

/* PCI MSI-X Capability (0x11) */
struct pci_cap_msix {
    struct pci_cap_header hdr;
    u16 message_control;
    u32 table_offset;
    u32 pba_offset;
} __attribute__((packed));

/* PCI Express Capability (0x10) */
struct pci_cap_exp {
    struct pci_cap_header hdr;
    u16 pcie_cap;
    u32 dev_cap;
    u16 dev_ctrl;
    u16 dev_sts;
    u32 link_cap;
    u16 link_ctrl;
    u16 link_sts;
    u32 slot_cap;
    u16 slot_ctrl;
    u16 slot_sts;
    u16 root_ctrl;
    u16 root_cap;
    u32 root_sts;
    u32 dev_cap2;
    u32 dev_ctrl2;
    u32 link_cap2;
    u32 slot_cap2;
    u32 slot_ctrl2;
} __attribute__((packed));

/* Capabilities List Assembly */
struct pci_capabilities {
    struct pci_cap_pm    pm_cap;      /* Offset: 0x40 */
    struct pci_cap_msi   msi_cap;     /* Offset: 0x50 */
    struct pci_cap_msix  msix_cap;    /* Offset: 0x70 */
    struct pci_cap_exp   exp_cap;     /* Offset: 0x90 */
} __attribute__((packed));
```

#### 4.1.3 BAR Configuration Design

**From Actual Implementation** ([include/pcie/pci.h](../include/pcie/pci.h)):

```c
/* BAR Definitions */
#define BAR0_SIZE 0x4000  /* 16KB: NVMe Controller Registers */
#define BAR2_SIZE 0x1000  /* 4KB: MSI-X Table (optional) */
#define BAR4_SIZE 0x1000  /* 4KB: MSI-X PBA (optional) */

#define BAR_TYPE_MEM       0x00
#define BAR_TYPE_IO        0x01
#define BAR_MEM_32BIT      0x00
#define BAR_MEM_64BIT      0x04
#define BAR_MEM_PREFETCH   0x08
```

**Implementation Note**: In the user-space implementation, BARs are represented as virtual pointers in `struct pci_dev_ctx`:
```c
/* BAR virtual addresses (for user-space emulation) */
void *bar0_virt;
void *bar2_virt;
void *bar4_virt;
```

### 4.2 NVMe Controller Register Emulation Design

#### 4.2.1 Controller Register Layout

**From Actual Implementation** ([include/pcie/nvme.h](../include/pcie/nvme.h)):

```c
/* NVMe Controller Register Offsets */
#define NVME_REG_CAP      0x00  /* Controller Capabilities (64-bit) */
#define NVME_REG_VS       0x08  /* Version (32-bit) */
#define NVME_REG_INTMS    0x0C  /* Interrupt Mask Set (32-bit) */
#define NVME_REG_INTMC    0x10  /* Interrupt Mask Clear (32-bit) */
#define NVME_REG_CC       0x14  /* Controller Configuration (32-bit) */
#define NVME_REG_CSTS     0x1C  /* Controller Status (32-bit) */
#define NVME_REG_NSSR     0x20  /* NVM Subsystem Reset (32-bit) */
#define NVME_REG_AQA      0x24  /* Admin Queue Attributes (32-bit) */
#define NVME_REG_ASQ      0x28  /* Admin Submission Queue Base (64-bit) */
#define NVME_REG_ACQ      0x30  /* Admin Completion Queue Base (64-bit) */
#define NVME_REG_CMBLOC   0x38  /* Controller Memory Buffer Location (32-bit) */
#define NVME_REG_CMBSZ    0x3C  /* Controller Memory Buffer Size (32-bit) */
#define NVME_REG_BPINFO   0x40  /* Boot Partition Information (32-bit) */
#define NVME_REG_BPRSEL   0x44  /* Boot Partition Read Select (32-bit) */
#define NVME_REG_BPMBL    0x48  /* Boot Partition Memory Buffer Location (64-bit) */
#define NVME_REG_DBS      0x1000 /* Doorbell Registers Start */

/* NVMe Controller Registers */
struct nvme_controller_regs {
    union {
        struct {
            u64 cap;      /* 0x00: CAP */
            u32 vs;       /* 0x08: VS */
            u32 intms;    /* 0x0C: INTMS */
            u32 intmc;    /* 0x10: INTMC */
            u32 cc;       /* 0x14: CC */
            u32 rsvd1;    /* 0x18: Reserved */
            u32 csts;     /* 0x1C: CSTS */
            u32 nssr;     /* 0x20: NSSR */
            u32 aqa;      /* 0x24: AQA */
            u64 asq;      /* 0x28: ASQ */
            u64 acq;      /* 0x30: ACQ */
            u32 cmbloc;   /* 0x38: CMBLOC */
            u32 cmbsz;    /* 0x3C: CMBSZ */
            u32 bpinfo;   /* 0x40: BPINFO */
            u32 bprsel;   /* 0x44: BPRSEL */
            u64 bpmbl;    /* 0x48: BPMBL */
            u8  rsvd2[0x1000 - 0x50];
        };
        u8 raw[0x1000];
    } regs;

    /* 0x1000+: Doorbell Registers */
    struct {
        u32 sq_tail;
        u32 cq_head;
    } doorbell[64];
} __attribute__((packed));
```

The register bit definitions are also provided in the header file.

---

## 5. Interface Design

### 5.1 User-Space API (Actual Implementation)

**From [include/pcie/pcie_nvme.h](../include/pcie/pcie_nvme.h)**:

```c
/* PCIe NVMe Device Context */
struct pcie_nvme_dev {
    /* PCI Device */
    struct pci_dev_ctx pci;

    /* NVMe Controller */
    struct nvme_ctrl_ctx nvme;

    /* Queue Manager */
    struct nvme_queue_mgr qmgr;

    /* MSI-X */
    struct msix_ctx msix;

    /* DMA */
    struct dma_ctx dma;

    /* Shared Memory */
    struct shmem_ctx shmem;

    /* Device State */
    bool initialized;
    bool running;

    /* Configuration */
    char shmem_path[256];
    u32 max_queue_pairs;
    u32 namespace_count;
    u64 namespace_size;

    /* Lock */
    struct mutex lock;

    /* Thread for processing commands */
    void *cmd_thread;
    bool cmd_thread_running;
};

/* Device Configuration */
struct pcie_nvme_config {
    const char *shmem_path;
    u32 max_queue_pairs;
    u32 namespace_count;
    u64 namespace_size;
    u32 page_size;
};

/* Function Prototypes */
int pcie_nvme_dev_init(struct pcie_nvme_dev *dev, struct pcie_nvme_config *config);
void pcie_nvme_dev_cleanup(struct pcie_nvme_dev *dev);
int pcie_nvme_dev_start(struct pcie_nvme_dev *dev);
void pcie_nvme_dev_stop(struct pcie_nvme_dev *dev);

/* PCI Configuration Space Access */
int pcie_nvme_cfg_read(struct pcie_nvme_dev *dev, u32 offset, u32 *val, u32 size);
int pcie_nvme_cfg_write(struct pcie_nvme_dev *dev, u32 offset, u32 val, u32 size);

/* BAR Access */
int pcie_nvme_bar_read(struct pcie_nvme_dev *dev, int bar, u64 offset, u64 *val, u32 size);
int pcie_nvme_bar_write(struct pcie_nvme_dev *dev, int bar, u64 offset, u64 val, u32 size);

/* Command Processing */
int pcie_nvme_process_admin_cmd(struct pcie_nvme_dev *dev, struct nvme_sq_entry *cmd,
                                 struct nvme_cq_entry *cpl);
int pcie_nvme_process_io_cmd(struct pcie_nvme_dev *dev, struct nvme_sq_entry *cmd,
                              struct nvme_cq_entry *cpl);

/* Default Config */
void pcie_nvme_config_default(struct pcie_nvme_config *config);
```

**Implementation Note**: These functions are declared but not implemented. The actual top-level interface uses `sssim.h` instead.

---

## 6. Data Structures

All data structures are defined in the header files under `include/pcie/`:

- [include/pcie/pci.h](../include/pcie/pci.h) - PCIe configuration space structures
- [include/pcie/nvme.h](../include/pcie/nvme.h) - NVMe controller, SQ/CQ, Identify structures
- [include/pcie/queue.h](../include/pcie/queue.h) - Queue management structures
- [include/pcie/msix.h](../include/pcie/msix.h) - MSI-X structures
- [include/pcie/dma.h](../include/pcie/dma.h) - DMA structures
- [include/pcie/shmem.h](../include/pcie/shmem.h) - Shared memory structures
- [include/pcie/pcie_nvme.h](../include/pcie/pcie_nvme.h) - Top-level device structure

---

## 7. Flow Diagrams

The design document includes detailed flow diagrams for:
- Controller initialization flow (write CC.EN=1)
- SQ Tail Doorbell processing flow
- CQ write-back and interrupt trigger flow
- PRP/SGL parsing flow
- Admin command processing flow

**Implementation Note**: None of these flows are implemented in code. The actual implementation uses a direct API via `sssim.h`.

---

## 8. Performance Design

See Section 2.2 for performance requirements. None of the performance optimizations are implemented.

---

## 9. Error Handling

The design document includes comprehensive error handling for NVMe status codes and error conditions. See `nvme.h` for status code definitions.

---

## 10. Test Design

The design document outlines a comprehensive test strategy. For actual tests, see the `tests/` directory (no PCIe/NVMe tests exist yet).

---

## Summary of Differences

| Aspect | Design Document | Actual Implementation |
|--------|-----------------|---------------------|
| Execution Context | Linux kernel module | User-space only |
| PCIe Emulation | Complete with lspci support | Stub structures only |
| NVMe Emulation | Complete with nvme-cli support | Stub structures only |
| Interrupts | MSI-X with real APIC | Not implemented |
| DMA | Real kernel DMA with IOMMU | Not implemented |
| Host Interface | Real block device via kernel | Direct function calls via sssim.h |
| Requirements Coverage | 22/22 designed | 0/22 implemented |

---

## References

1. NVMe Specification: NVM Express Base Specification Revision 2.0c
2. NVMeVirt Paper: NVMeVirt: A Versatile Software-defined Virtual NVMe Device (USENIX FAST 2023)
3. FEMU Paper: The CASE of FEMU: Cheap, Accurate, Scalable and Extensible Flash Emulator (USENIX FAST 2018)
4. [ARCHITECTURE.md](./ARCHITECTURE.md) - Actual system architecture
5. [REQUIREMENT_COVERAGE.md](./REQUIREMENT_COVERAGE.md) - Requirement coverage analysis
