# HFSSS High-Level Design Document

**Document Name**: PCIe/NVMe Device Emulation Module HLD
**Document Version**: V2.0
**Date**: 2026-03-23
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
| EN-V2.0 | 2026-03-23 | Architecture Team | Enterprise SSD architecture update: T10 PI, Namespace Mgmt, Security commands |

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
11. [Enterprise SSD Extensions](#11-enterprise-ssd-extensions)
12. [Architecture Decision Records](#12-architecture-decision-records)

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
- T10 PI (Protection Information) NVMe command extensions for end-to-end data integrity
- Namespace management admin commands for multi-tenant enterprise deployments
- Security admin commands for TCG Opal and self-encrypting drive (SED) support

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
- T10 PI metadata handling in NVMe read/write commands
- Namespace management admin command routing (NS Create, Delete, Attach, Detach, Format)
- Security admin command routing (TCG Opal Security Send/Receive)

**Not included in this module**:
- FTL algorithms (implemented by user-space daemon)
- NAND media emulation (implemented by user-space daemon)
- GC/WL algorithms (implemented by user-space daemon)
- Actual PI CRC/checksum computation (handled by Common Services or FTL)
- TCG Opal state machine logic (handled by Common Services security key management)

---

## 2. Requirements Review

### 2.1 Requirements Traceability Matrix

| Requirement ID | Description | Priority | Version | Implementation Status |
|----------------|-------------|----------|---------|----------------------|
| REQ-001 | PCIe Configuration Space Emulation | P0 | V1.0 | Stub Only |
| REQ-002 | PCIe Capabilities Emulation | P0 | V1.0 | Stub Only |
| REQ-003 | BAR Mapping | P0 | V1.0 | Stub Only |
| REQ-004 | NVMe Controller Register Emulation | P0 | V1.0 | Stub Only |
| REQ-005 | NVMe Doorbell Processing | P0 | V1.0 | Stub Only |
| REQ-006 | NVMe Controller State Machine | P0 | V1.0 | Stub Only |
| REQ-007 | Doorbell Processing | P0 | V1.0 | Stub Only |
| REQ-008 | Identify Command | P0 | V1.0 | Stub Only |
| REQ-009 | Admin Queue Management | P0 | V1.0 | Stub Only |
| REQ-010 | I/O Queue Management | P0 | V1.0 | Stub Only |
| REQ-011 | Completion Queue Processing | P0 | V1.0 | Stub Only |
| REQ-012 | MSI-X Interrupt Emulation | P0 | V1.0 | Stub Only |
| REQ-013 | MSI-X Table/PBA | P0 | V1.0 | Stub Only |
| REQ-014 | PRP/SGL Parser | P0 | V1.0 | Stub Only |
| REQ-015 | NVMe Admin Command Processing | P0 | V1.0 | Stub Only |
| REQ-016 | NVMe I/O Command Processing | P0 | V1.0 | Stub Only |
| REQ-017 | NVMe Namespace Management | P0 | V1.0 | Stub Only |
| REQ-018 | DMA Engine | P0 | V1.0 | Stub Only |
| REQ-019 | PRP/SGL DMA | P0 | V1.0 | Stub Only |
| REQ-020 | IOMMU Support | P1 | V1.5 | Not Implemented |
| REQ-021 | Shared Memory Interface | P0 | V1.0 | Stub Only |
| REQ-022 | Kernel-User Space Communication | P0 | V1.0 | Stub Only |
| REQ-ENT-001 | T10 PI NVMe Command Extensions | P1 | V2.0 | Design Only |
| REQ-ENT-002 | Namespace Management Admin Commands | P0 | V2.0 | Design Only |
| REQ-ENT-003 | Security Admin Commands (TCG Opal) | P1 | V2.0 | Design Only |

### 2.2 Key Performance Requirements

| Metric | Target | Description |
|--------|--------|-------------|
| Command Processing Latency | < 10us | Latency from SQ fetch to user-space notification |
| Interrupt Delivery Latency | < 5us | Latency from CQ write to MSI-X trigger |
| Max IOPS | > 1,000,000 | Theoretical peak without ECC |
| Queue Depth Support | 65535 | Maximum supported queue depth |
| Queue Pairs | 64 | Maximum supported I/O queue pairs |
| Namespace Mgmt Command Latency | < 100ms | NS Create/Delete/Attach/Detach |
| Security Command Latency | < 50ms | Security Send/Receive round-trip |

---

## 3. System Architecture

### 3.1 Module Layer Architecture

**Design Document Architecture (Kernel Module)**:

```
+-----------------------------------------------------------------+
|                    Host Linux OS                                 |
|  +--------------+  +--------------+  +---------------------+    |
|  |  NVMe Driver |  |  File System |  |  fio / nvme-cli     |    |
|  +------+-------+  +------+-------+  +----------+----------+    |
+---------+---------------------+----------------------------+-----+
          | PCIe/MMIO           | Block I/O              | Sysfs
          |                     |                        |
+---------v---------------------v----------------------------v-----+
|              HFSSS Kernel Module (hfsss_nvme.ko)                 |
|  +---------------------------------------------------------------+
|  |  PCIe Emulation Sublayer (pci.c)                              |
|  |  - PCI config space (256B base + 4KB extended)                |
|  |  - BAR register mapping                                       |
|  |  - PCIe Capabilities list                                     |
|  |  - PCI device register/unregister                              |
|  +------------------------+--------------------------------------+
|                           | MMIO access                           |
|  +------------------------v--------------------------------------+
|  |  NVMe Protocol Sublayer (nvme.c)                              |
|  |  - Controller registers (CAP/VS/CC/CSTS/AQA/ASQ/ACQ etc.)    |
|  |  - Doorbell registers (SQ Tail/CQ Head)                       |
|  |  - Admin command processing (admin.c)                          |
|  |    - Identify, Get/Set Features, Create/Delete I/O SQ/CQ      |
|  |    - NS Management (Create/Delete/Attach/Detach/Format)        |
|  |    - Security Send/Receive (TCG Opal routing)                  |
|  |  - I/O command processing (io.c)                               |
|  |    - Read/Write with PRINFO field for T10 PI                   |
|  |    - PI metadata handling (Type 1/2/3 guard, app, ref tags)    |
|  +------------------------+--------------------------------------+
|                           | Command/Queue                         |
|  +------------------------v--------------------------------------+
|  |  Queue Management Sublayer (queue.c)                           |
|  |  - Admin SQ/CQ (QID=0)                                        |
|  |  - I/O SQ/CQ (QID=1~63)                                       |
|  |  - PRP/SGL parsing engine                                      |
|  |  - CQE construction and write                                  |
|  +------------------------+--------------------------------------+
|                           | Interrupt                              |
|  +------------------------v--------------------------------------+
|  |  MSI-X Interrupt Sublayer (msix.c)                             |
|  |  - MSI-X Table management                                      |
|  |  - MSI-X PBA management                                        |
|  |  - Interrupt delivery (apic->send_IPI)                          |
|  |  - Interrupt coalescing                                         |
|  +------------------------+--------------------------------------+
|                           | DMA                                    |
|  +------------------------v--------------------------------------+
|  |  DMA Engine Sublayer (dma.c)                                   |
|  |  - Host memory mapping (kmap/kmap_atomic)                      |
|  |  - Data copy (memcpy_toio/memcpy_fromio)                       |
|  |  - IOMMU support (dma_map_page/dma_unmap_page)                 |
|  |  - NUMA affinity optimization                                  |
|  +------------------------+--------------------------------------+
|                           | Shared Memory                          |
|  +------------------------v--------------------------------------+
|  |  User-Space Communication Sublayer (shmem.c)                   |
|  |  - Shared memory Ring Buffer (16384 slots x 128B)              |
|  |  - Lock-free SPSC/MPMC queue                                   |
|  |  - eventfd notification mechanism                               |
|  |  - mmap interface (user-space access)                           |
|  +---------------------------------------------------------------+
+-----------------------------------------------------------------+
                          |
                          | Shared Memory / ioctl
                          |
+-----------------------------------------------------------------+
|           User-Space Daemon (hfsss-daemon)                       |
|  - Controller Thread                                              |
|  - Firmware Core Threads                                          |
|  - Media Threads                                                  |
+-----------------------------------------------------------------+
```

**Actual Implementation (User-Space Only)**:

The actual code only provides header file definitions in `include/pcie/`:

```
+-----------------------------------------------------------------+
|           User-Space Application                                 |
|                                                                  |
|  +-----------------------------------------------------------+  |
|  |  Header-Only Structures (No Implementation)               |  |
|  |  - pci.h: PCI config space structures                     |  |
|  |  - nvme.h: NVMe registers, SQ/CQ entries                  |  |
|  |  - queue.h: Queue management structures                    |  |
|  |  - msix.h: MSI-X structures                                |  |
|  |  - dma.h: DMA structures                                   |  |
|  |  - shmem.h: Shared memory structures                       |  |
|  +-----------------------------------------------------------+  |
|                                                                  |
|  No kernel module, no actual PCIe/NVMe emulation                 |
|  (Top-level sssim.h provides direct read/write API instead)      |
+-----------------------------------------------------------------+
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

**Implementation Status**: Structures defined, No implementation

#### 3.2.2 NVMe Protocol Sublayer (nvme.c, admin.c, io.c)

**Responsibilities**:
- Implement NVMe controller register set
- Handle MMIO register reads and writes
- Admin command parsing and processing
- I/O command parsing and processing
- Command dispatch to queue management sublayer
- T10 PI metadata extraction from I/O commands (PRINFO field in CDW12)
- Namespace management admin command parsing (NS Create, Delete, Attach, Detach, Format NVM)
- Security admin command routing (Security Send Opcode 0x81, Security Receive Opcode 0x82)

**Key Components** (from header files):
- `struct nvme_ctrl_ctx`: NVMe controller context (see [include/pcie/nvme.h](../include/pcie/nvme.h))
- `struct nvme_controller_regs`: NVMe controller registers
- `struct nvme_sq_entry`: NVMe submission queue entry
- `struct nvme_cq_entry`: NVMe completion queue entry

**Implementation Status**: Structures defined, No implementation

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

**Implementation Status**: Structures defined, No implementation

#### 3.2.4 MSI-X Interrupt Sublayer (msix.c)

**Responsibilities**:
- MSI-X Table management
- MSI-X PBA (Pending Bit Array) management
- Interrupt delivery mechanism implementation
- Interrupt coalescing support
- Interrupt affinity support

**Key Components** (from header files):
- See [include/pcie/msix.h](../include/pcie/msix.h)

**Implementation Status**: Structures defined, No implementation

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

**Implementation Status**: Structures defined, No implementation

#### 3.2.6 User-Space Communication Sublayer (shmem.c)

**Responsibilities**:
- Shared memory region creation and management
- Ring Buffer implementation (lock-free SPSC/MPMC)
- eventfd notification mechanism implementation
- mmap interface implementation (user-space access)
- ioctl interface implementation (control commands)

**Key Components** (from header files):
- See [include/pcie/shmem.h](../include/pcie/shmem.h)

**Implementation Status**: Structures defined, No implementation

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
    u8  latency_timer;       /* 0x0D: Latency Timer */
    u8  header_type;         /* 0x0E: Header Type */
    u8  bist;                /* 0x0F: BIST */
    u32 bar[6];              /* 0x10-0x27: Base Address Registers */
    u32 cardbus_cis;         /* 0x28: CardBus CIS Pointer */
    u16 subsystem_vendor_id; /* 0x2C: Subsystem Vendor ID */
    u16 subsystem_id;        /* 0x2E: Subsystem ID */
    u32 expansion_rom;       /* 0x30: Expansion ROM Base Address */
    u8  capabilities_ptr;    /* 0x34: Capabilities Pointer */
    u8  reserved1[7];        /* 0x35-0x3B: Reserved */
    u8  interrupt_line;      /* 0x3C: Interrupt Line */
    u8  interrupt_pin;       /* 0x3D: Interrupt Pin */
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
};
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

The register bit definitions are also provided in the header file, including CAP, CC, CSTS, and AQA bit fields.

#### 4.2.2 Controller Register Initialization Values

```c
void nvme_regs_init(struct nvme_controller_regs *regs) {
    /* CAP: Controller Capabilities */
    regs->regs.cap = 0;
    regs->regs.cap |= (65535ULL << NVME_CAP_MQES_SHIFT);    /* MQES: 65535 */
    regs->regs.cap |= (0ULL << NVME_CAP_CQR_SHIFT);         /* CQR: 0 */
    regs->regs.cap |= (3ULL << NVME_CAP_AMS_SHIFT);         /* AMS: RR + WRR */
    regs->regs.cap |= (20ULL << NVME_CAP_TO_SHIFT);         /* TO: 20 (10s) */
    regs->regs.cap |= (0ULL << NVME_CAP_DSTRD_SHIFT);       /* DSTRD: 0 (4B) */
    regs->regs.cap |= (1ULL << NVME_CAP_NSSRS_SHIFT);       /* NSSRS: 1 */
    regs->regs.cap |= (1ULL << NVME_CAP_CSS_SHIFT);         /* CSS: NVM */
    regs->regs.cap |= (0ULL << NVME_CAP_MPSMIN_SHIFT);      /* MPSMIN: 0 (4KB) */
    regs->regs.cap |= (4ULL << NVME_CAP_MPSMAX_SHIFT);      /* MPSMAX: 4 (64KB) */

    /* VS: Version NVMe 2.0 */
    regs->regs.vs = 0x00020000;

    /* All other registers initialized to 0 */
    regs->regs.intms = 0;
    regs->regs.intmc = 0;
    regs->regs.cc = 0;
    regs->regs.csts = 0;
    regs->regs.nssr = 0;
    regs->regs.aqa = 0;
    regs->regs.asq = 0;
    regs->regs.acq = 0;
    regs->regs.cmbloc = 0;
    regs->regs.cmbsz = 0;
    regs->regs.bpinfo = 0;
    regs->regs.bprsel = 0;
    regs->regs.bpmbl = 0;
}
```

### 4.3 MMIO Register Read/Write Processing Design

#### 4.3.1 MMIO Read/Write Callback Functions

```c
/* BAR0 MMIO read callback */
static u64 hfsss_nvme_mmio_read(void *opaque, hwaddr addr, unsigned size) {
    struct hfsss_nvme_dev *dev = opaque;
    u64 val = 0;

    if (addr >= NVME_REG_DBS) {
        return 0;  /* Doorbell register read: return 0 */
    }

    switch (addr) {
    case NVME_REG_CAP:   val = dev->nvme_regs.regs.cap;   break;
    case NVME_REG_VS:    val = dev->nvme_regs.regs.vs;    break;
    case NVME_REG_INTMS: val = dev->nvme_regs.regs.intms; break;
    case NVME_REG_INTMC: val = dev->nvme_regs.regs.intmc; break;
    case NVME_REG_CC:    val = dev->nvme_regs.regs.cc;    break;
    case NVME_REG_CSTS:  val = dev->nvme_regs.regs.csts;  break;
    case NVME_REG_NSSR:  val = dev->nvme_regs.regs.nssr;  break;
    case NVME_REG_AQA:   val = dev->nvme_regs.regs.aqa;   break;
    case NVME_REG_ASQ:   val = dev->nvme_regs.regs.asq;   break;
    case NVME_REG_ACQ:   val = dev->nvme_regs.regs.acq;   break;
    default:             val = 0;                          break;
    }

    /* Truncate based on access size */
    if (size == 1) val &= 0xFF;
    else if (size == 2) val &= 0xFFFF;
    else if (size == 4) val &= 0xFFFFFFFF;

    return val;
}

/* BAR0 MMIO write callback */
static void hfsss_nvme_mmio_write(void *opaque, hwaddr addr,
                                   u64 val, unsigned size) {
    struct hfsss_nvme_dev *dev = opaque;

    if (addr >= NVME_REG_DBS && addr < NVME_REG_DBS + 64 * 8) {
        u32 db_idx = (addr - NVME_REG_DBS) / 8;
        u32 db_offset = (addr - NVME_REG_DBS) % 8;
        if (db_offset == 0)
            hfsss_nvme_sq_doorbell(dev, db_idx, (u32)val);
        else if (db_offset == 4)
            hfsss_nvme_cq_doorbell(dev, db_idx, (u32)val);
        return;
    }

    switch (addr) {
    case NVME_REG_INTMS: dev->nvme_regs.regs.intms |= (u32)val;  break;
    case NVME_REG_INTMC: dev->nvme_regs.regs.intms &= ~(u32)val; break;
    case NVME_REG_CC:    hfsss_nvme_cc_write(dev, (u32)val);      break;
    case NVME_REG_NSSR:
        if (val == 0x4E564D45) hfsss_nvme_nssr_reset(dev);
        break;
    case NVME_REG_AQA: dev->nvme_regs.regs.aqa = (u32)val; break;
    case NVME_REG_ASQ:
        if (size == 4)
            dev->nvme_regs.regs.asq = (dev->nvme_regs.regs.asq & 0xFFFFFFFF00000000ULL) | (u32)val;
        else
            dev->nvme_regs.regs.asq = val;
        break;
    case NVME_REG_ACQ:
        if (size == 4)
            dev->nvme_regs.regs.acq = (dev->nvme_regs.regs.acq & 0xFFFFFFFF00000000ULL) | (u32)val;
        else
            dev->nvme_regs.regs.acq = val;
        break;
    default: break;
    }
}
```

### 4.4 CC Register Write Processing (Controller Enable/Disable)

```c
static void hfsss_nvme_cc_write(struct hfsss_nvme_dev *dev, u32 val) {
    u32 old_cc = dev->nvme_regs.regs.cc;
    u32 new_cc = val;
    bool old_en = (old_cc & NVME_CC_EN_MASK) != 0;
    bool new_en = (new_cc & NVME_CC_EN_MASK) != 0;

    if (!old_en && new_en) {
        hfsss_nvme_controller_enable(dev, new_cc);
    } else if (old_en && !new_en) {
        hfsss_nvme_controller_disable(dev);
    } else if (old_en && new_en) {
        hfsss_nvme_controller_update(dev, old_cc, new_cc);
    }
    dev->nvme_regs.regs.cc = new_cc;
}

static void hfsss_nvme_controller_enable(struct hfsss_nvme_dev *dev, u32 cc) {
    u32 css = (cc & NVME_CC_CSS_MASK) >> NVME_CC_CSS_SHIFT;
    u32 mps = (cc & NVME_CC_MPS_MASK) >> NVME_CC_MPS_SHIFT;
    u32 ams = (cc & NVME_CC_AMS_MASK) >> NVME_CC_AMS_SHIFT;
    u32 iosqes = (cc & NVME_CC_IOSQES_MASK) >> NVME_CC_IOSQES_SHIFT;
    u32 iocqes = (cc & NVME_CC_IOCQES_MASK) >> NVME_CC_IOCQES_SHIFT;

    /* Validate configuration */
    if ((css != 0 && css != 1) || mps > 4 || iosqes < 6 || iosqes > 10
        || iocqes < 4 || iocqes > 10) {
        dev->nvme_regs.regs.csts |= NVME_CSTS_CFS_MASK;
        return;
    }

    /* Save configuration */
    dev->config.css = css;
    dev->config.mps = mps;
    dev->config.page_size = 4096 << mps;
    dev->config.ams = ams;
    dev->config.iosqes = iosqes;
    dev->config.sq_entry_size = 1 << iosqes;
    dev->config.iocqes = iocqes;
    dev->config.cq_entry_size = 1 << iocqes;

    /* Initialize Admin Queue */
    u32 asqs = (dev->nvme_regs.regs.aqa & NVME_AQA_ASQS_MASK) >> NVME_AQA_ASQS_SHIFT;
    u32 acqs = (dev->nvme_regs.regs.aqa & NVME_AQA_ACQS_MASK) >> NVME_AQA_ACQS_SHIFT;
    u64 asq_addr = dev->nvme_regs.regs.asq;
    u64 acq_addr = dev->nvme_regs.regs.acq;

    if (asqs == 0 || acqs == 0 || asq_addr == 0 || acq_addr == 0) {
        dev->nvme_regs.regs.csts |= NVME_CSTS_CFS_MASK;
        return;
    }

    dev->admin_sq = nvme_sq_create(dev, 0, asq_addr, asqs + 1, dev->config.sq_entry_size);
    dev->admin_cq = nvme_cq_create(dev, 0, acq_addr, acqs + 1, dev->config.cq_entry_size, 0);

    if (!dev->admin_sq || !dev->admin_cq) {
        dev->nvme_regs.regs.csts |= NVME_CSTS_CFS_MASK;
        return;
    }

    /* Start I/O Dispatcher thread */
    dev->io_dispatcher_running = true;
    dev->io_dispatcher_task = kthread_create(hfsss_nvme_io_dispatcher, dev, "hfsss_nvme_disp");
    wake_up_process(dev->io_dispatcher_task);

    /* Set CSTS.RDY=1 */
    dev->nvme_regs.regs.csts |= NVME_CSTS_RDY_MASK;
}

static void hfsss_nvme_controller_disable(struct hfsss_nvme_dev *dev) {
    dev->io_dispatcher_running = false;
    /* Destroy all I/O SQ/CQ */
    for (int i = 1; i < 64; i++) {
        if (dev->io_sqs[i]) { nvme_sq_destroy(dev->io_sqs[i]); dev->io_sqs[i] = NULL; }
        if (dev->io_cqs[i]) { nvme_cq_destroy(dev->io_cqs[i]); dev->io_cqs[i] = NULL; }
    }
    /* Destroy Admin SQ/CQ */
    if (dev->admin_sq) { nvme_sq_destroy(dev->admin_sq); dev->admin_sq = NULL; }
    if (dev->admin_cq) { nvme_cq_destroy(dev->admin_cq); dev->admin_cq = NULL; }
    dev->nvme_regs.regs.csts &= ~NVME_CSTS_RDY_MASK;
}
```

---

## 5. Interface Design

### 5.1 User-Space API (Actual Implementation)

**From [include/pcie/pcie_nvme.h](../include/pcie/pcie_nvme.h)**:

```c
/* PCIe NVMe Device Context */
struct pcie_nvme_dev {
    struct pci_dev_ctx pci;
    struct nvme_ctrl_ctx nvme;
    struct nvme_queue_mgr qmgr;
    struct msix_ctx msix;
    struct dma_ctx dma;
    struct shmem_ctx shmem;

    bool initialized;
    bool running;

    char shmem_path[256];
    u32 max_queue_pairs;
    u32 namespace_count;
    u64 namespace_size;

    struct mutex lock;
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

## 11. Enterprise SSD Extensions

### 11.1 T10 PI NVMe Command Extensions

#### 11.1.1 Overview

T10 Protection Information (PI) provides end-to-end data integrity for enterprise workloads. The NVMe emulation module must handle the PRINFO field in NVMe Read and Write commands and route PI metadata between the host and the storage backend.

#### 11.1.2 PRINFO Field in Read/Write Commands

The PRINFO field is located in CDW12 bits [29:26] of NVMe Read and Write commands:

```c
/* PRINFO field bits in CDW12 of NVMe Read/Write commands */
#define NVME_RW_PRINFO_SHIFT     26
#define NVME_RW_PRINFO_MASK      (0xFU << NVME_RW_PRINFO_SHIFT)

#define NVME_RW_PRINFO_PRCHK_GUARD  (1U << 26)  /* Check Guard field */
#define NVME_RW_PRINFO_PRCHK_APP    (1U << 27)  /* Check Application Tag */
#define NVME_RW_PRINFO_PRCHK_REF    (1U << 28)  /* Check Reference Tag */
#define NVME_RW_PRINFO_PRACT       (1U << 29)  /* PI Action (strip/insert) */

/* PI Types supported */
enum nvme_pi_type {
    NVME_PI_TYPE_NONE = 0,
    NVME_PI_TYPE1 = 1,  /* Guard + App + Ref tags, Ref = LBA */
    NVME_PI_TYPE2 = 2,  /* Guard + App + Ref tags, Ref = initial value */
    NVME_PI_TYPE3 = 3,  /* Guard + App tags only, no Ref tag check */
};

/* T10 PI Tuple (8 bytes per LBA, stored in metadata area) */
struct t10_pi_tuple {
    u16 guard_tag;       /* CRC-16 of user data */
    u16 app_tag;         /* Application-defined tag */
    u32 ref_tag;         /* Reference tag (logical block address) */
} __attribute__((packed));
```

#### 11.1.3 PI Metadata Handling in I/O Command Processing

When the emulation module processes an NVMe Read or Write command:

1. **Write path**: If PRINFO.PRACT is clear and PI is enabled for the namespace, the host sends PI metadata along with user data. The module extracts the PI metadata from the host transfer (either interleaved with data or as a separate metadata buffer) and forwards it to the FTL layer. If PRINFO.PRACT is set, the controller generates PI metadata and the host sends only user data.

2. **Read path**: If PRINFO.PRACT is clear, the module returns PI metadata to the host alongside user data. If PRINFO.PRACT is set, the controller strips PI metadata and returns only user data. In both cases, if PRINFO.PRCHK_GUARD, PRINFO.PRCHK_APP, or PRINFO.PRCHK_REF are set, the controller validates the respective PI fields before returning data.

3. **Identify Namespace response**: The emulation layer populates the namespace PI capabilities in the Identify Namespace data structure:
   - DPS field (byte 29): Protection Information type and location (first/last 8 bytes of metadata)
   - MC field (byte 27): Metadata capabilities (extended LBA vs. separate buffer)

```c
/* Identify Namespace DPS field */
#define NVME_NS_DPS_PI_TYPE_SHIFT 0
#define NVME_NS_DPS_PI_TYPE_MASK  0x07
#define NVME_NS_DPS_PI_FIRST      (1U << 3)  /* PI in first 8 bytes of metadata */

/* Namespace PI configuration in emulation */
struct nvme_ns_pi_config {
    enum nvme_pi_type pi_type;     /* 0=none, 1/2/3 */
    bool pi_first;                  /* true = PI at start of metadata */
    u16 metadata_size;              /* Total metadata per LBA (e.g. 8 for PI only) */
};
```

#### 11.1.4 PI Verification Flow

```
Host Write with PI:
  Host data + PI metadata
      |
      v
  [NVMe Emulation Module]
      | Extract PRINFO from CDW12
      | If PRACT=0: pass PI metadata to FTL
      | If PRACT=1: generate PI (guard=CRC16, ref=LBA)
      v
  [Controller Thread] --> [FTL] --> [Media]
      (PI stored in NAND OOB/spare area)

Host Read with PI:
  [Media] --> [FTL] --> [Controller Thread]
      | Retrieve PI metadata from NAND OOB
      v
  [NVMe Emulation Module]
      | If PRCHK bits set: verify guard/app/ref tags
      | If verification fails: return NVMe error
      |   SC=0x81 (End-to-End Guard Check Error)
      |   SC=0x82 (End-to-End Application Tag Check Error)
      |   SC=0x83 (End-to-End Reference Tag Check Error)
      | If PRACT=0: return PI metadata to host
      | If PRACT=1: strip PI, return data only
      v
  Host receives data [+/- PI metadata]
```

### 11.2 Namespace Management Admin Commands Architecture

#### 11.2.1 Overview

Enterprise SSDs require dynamic namespace management for multi-tenant environments. The NVMe emulation module supports the following Namespace Management admin commands per NVMe specification:

| Opcode | Command | Description |
|--------|---------|-------------|
| 0x0D | Namespace Management | Create or Delete a namespace |
| 0x15 | Namespace Attachment | Attach or Detach a namespace to/from a controller |
| 0x80 | Format NVM | Format a namespace or all namespaces |

#### 11.2.2 Namespace Management Command (Opcode 0x0D)

```c
/* Namespace Management - SEL field in CDW10 */
#define NVME_NS_MGMT_SEL_CREATE  0x00
#define NVME_NS_MGMT_SEL_DELETE  0x01

/* Namespace Create Parameters (Host Buffer - Identify Namespace data) */
struct nvme_ns_create_params {
    u64 nsze;    /* Namespace Size (in logical blocks) */
    u64 ncap;    /* Namespace Capacity (in logical blocks) */
    u8  flbas;   /* Formatted LBA Size */
    u8  dps;     /* End-to-End Data Protection Type Settings */
    u8  nmic;    /* Namespace Multi-path I/O and Sharing Capabilities */
    u8  reserved[5];
};
```

**Processing flow**:
1. Extract SEL field from CDW10
2. If SEL=Create: read namespace creation parameters from the data buffer pointed to by PRP1/PRP2, validate capacity availability, allocate namespace ID (NSID), initialize FTL structures for the new namespace, return new NSID in CQE CDW0
3. If SEL=Delete: validate NSID from CDW1, verify namespace is not attached to any controller, deallocate FTL structures and free blocks, return success

#### 11.2.3 Namespace Attachment Command (Opcode 0x15)

```c
/* Namespace Attachment - SEL field in CDW10 */
#define NVME_NS_ATTACH_SEL_ATTACH  0x00
#define NVME_NS_ATTACH_SEL_DETACH  0x01

/* Controller List (in host buffer, up to 2048 controller IDs) */
struct nvme_ctrl_list {
    u16 num_identifiers;
    u16 identifiers[2047];
};
```

**Processing flow**:
1. Extract SEL and NSID
2. If SEL=Attach: read controller list from data buffer, attach namespace NSID to each listed controller, update Identify Controller response for each affected controller
3. If SEL=Detach: read controller list, detach namespace from listed controllers, update Identify Controller
4. Return appropriate NVMe status: Success, NS Not Ready, NS Already Attached, Invalid Controller List, etc.

#### 11.2.4 Format NVM Command (Opcode 0x80)

```c
/* Format NVM - CDW10 fields */
#define NVME_FORMAT_LBAF_SHIFT    0   /* LBA Format index */
#define NVME_FORMAT_LBAF_MASK     0x0F
#define NVME_FORMAT_MSET_SHIFT    4   /* Metadata Settings */
#define NVME_FORMAT_MSET_MASK     (1U << 4)
#define NVME_FORMAT_PI_SHIFT      5   /* PI type */
#define NVME_FORMAT_PI_MASK       (0x7U << 5)
#define NVME_FORMAT_PIL_SHIFT     8   /* PI Location */
#define NVME_FORMAT_PIL_MASK      (1U << 8)
#define NVME_FORMAT_SES_SHIFT     9   /* Secure Erase Settings */
#define NVME_FORMAT_SES_MASK      (0x7U << 9)
```

**Processing flow**:
1. Parse CDW10 for LBA format, metadata settings, PI type, PI location, and secure erase settings
2. If NSID = 0xFFFFFFFF, format all namespaces; otherwise format specified namespace
3. For each namespace: invalidate all FTL mappings, erase all user data blocks, update namespace metadata (LBA format, PI type, metadata size)
4. If SES indicates secure erase (user data erase or crypto erase), invoke the corresponding secure erase routine via HAL
5. Return NVMe status: Success, Invalid Format, Namespace Not Ready

### 11.3 Security Admin Commands Architecture

#### 11.3.1 Overview

Enterprise SSDs must support self-encrypting drive (SED) functionality via the TCG Opal protocol. The NVMe emulation module provides the transport layer for Security Send and Security Receive commands, routing them to the security key management service in the Common Services layer.

#### 11.3.2 Security Send Command (Opcode 0x81)

```c
/* Security Send command fields */
struct nvme_security_send {
    /* CDW10 */
    u8  secp;     /* Security Protocol (e.g., 0x01 = TCG, 0x02 = JEDEC) */
    u8  reserved;
    u16 spsp;     /* SP Specific (ComID for TCG Opal) */
    /* CDW11 */
    u32 tl;       /* Transfer Length */
};
```

**Processing flow**:
1. Extract SECP (Security Protocol) and SPSP (SP Specific) from CDW10
2. Extract Transfer Length from CDW11
3. Read security payload from host memory via PRP1/PRP2
4. Route to appropriate security protocol handler:
   - SECP=0x01 (TCG): forward to TCG Opal command processor
   - SECP=0x02 (JEDEC): forward to JEDEC protocol handler
   - SECP=0xEF (ATA Security): forward to ATA security handler
5. Return NVMe completion status

#### 11.3.3 Security Receive Command (Opcode 0x82)

```c
/* Security Receive command fields */
struct nvme_security_recv {
    /* CDW10 */
    u8  secp;     /* Security Protocol */
    u8  reserved;
    u16 spsp;     /* SP Specific */
    /* CDW11 */
    u32 al;       /* Allocation Length */
};
```

**Processing flow**:
1. Extract SECP and SPSP from CDW10
2. Extract Allocation Length from CDW11
3. Query the security protocol handler for the response data
4. Write response data to host memory via PRP1/PRP2
5. Set the transfer length in CQE and return completion status

#### 11.3.4 TCG Opal Command Routing Architecture

```
Host sends Security Send (SECP=0x01, SPSP=ComID)
    |
    v
[NVMe Emulation Module]
    | Parse command, extract ComID and payload
    | Forward via shared memory ring buffer to user-space daemon
    v
[Controller Thread]
    | Route to security service
    v
[Common Services - Security Key Management]
    | Process TCG Opal session:
    |   - StartSession / SyncSession
    |   - Authenticate (SID, Admin SP, Locking SP)
    |   - Get/Set on MBR Control, Locking Range
    |   - GenKey, RevertSP, Activate
    v
[Response routed back through the same path]
    |
    v
Host receives Security Receive response
```

---

## 12. Architecture Decision Records

### ADR-PCIE-001: Kernel Module vs. User-Space Emulation

**Context**: HFSSS needs to present a virtual NVMe device to the host OS. Two approaches were considered: (a) a Linux kernel module intercepting the NVMe driver (similar to NVMeVirt), or (b) a fully user-space approach using vfio-user or direct API calls.

**Decision**: The design targets a kernel module approach for maximum fidelity in PCIe/NVMe protocol behavior, with user-space stubs as an interim solution to enable early development of FTL and media layers.

**Consequences**:
- Positive: Full NVMe specification compliance, host sees a real block device, standard tools (fio, nvme-cli) work out of the box.
- Negative: Kernel module development is complex, requires privileged access, harder to debug. The interim stub approach means PCIe/NVMe testing requires the kernel module to be completed first.

**Status**: Accepted. Kernel module planned for V2.0; user-space stubs active for V1.0.

### ADR-PCIE-002: T10 PI Metadata Transport via Extended LBA vs. Separate Buffer

**Context**: NVMe supports two methods for transferring PI metadata: (a) extended LBA format where metadata is appended to each LBA data block, or (b) a separate metadata buffer pointed to by the MPTR field.

**Decision**: Support both methods, with extended LBA as the default. The metadata buffer method is useful for workloads that need to inspect or modify PI metadata independently. The choice is configured per-namespace via the Format NVM command.

**Consequences**:
- Positive: Maximum compatibility with enterprise host stacks (Linux, Windows) and storage benchmarks.
- Negative: Increased complexity in DMA transfer logic, as the module must handle two distinct data layouts.

**Status**: Accepted.

### ADR-PCIE-003: Security Command Processing in User-Space

**Context**: TCG Opal session management is complex and stateful. Processing it in the kernel module would increase kernel complexity and attack surface.

**Decision**: The NVMe emulation module acts only as a transport for Security Send/Receive commands. All TCG Opal session state, key management, and Locking SP logic is handled by the Common Services security key management service in user space.

**Consequences**:
- Positive: Simpler kernel module, better testability of security logic, easier to update TCG Opal implementation.
- Negative: Additional latency due to kernel-user space round trip for security commands. Acceptable because security commands are infrequent and not latency-sensitive.

**Status**: Accepted.

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
| T10 PI | PRINFO handling in Read/Write | Design only |
| Namespace Mgmt | NS Create/Delete/Attach/Detach/Format | Design only |
| Security | TCG Opal routing via Security Send/Receive | Design only |
| Requirements Coverage | 25/25 designed | 0/25 implemented |

---

## References

1. NVMe Specification: NVM Express Base Specification Revision 2.0c
2. NVMeVirt Paper: NVMeVirt: A Versatile Software-defined Virtual NVMe Device (USENIX FAST 2023)
3. FEMU Paper: The CASE of FEMU: Cheap, Accurate, Scalable and Extensible Flash Emulator (USENIX FAST 2018)
4. T10 DIF/DIX: SCSI Block Commands-3 (SBC-3), T10 Protection Information
5. TCG Opal: TCG Storage Security Subsystem Class: Opal, Specification Version 2.01
6. [ARCHITECTURE.md](./ARCHITECTURE.md) - Actual system architecture
7. [REQUIREMENT_COVERAGE.md](./REQUIREMENT_COVERAGE.md) - Requirement coverage analysis
