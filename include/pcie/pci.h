#ifndef __HFSSS_PCI_H
#define __HFSSS_PCI_H

#include "common/common.h"
#include "common/mutex.h"

/* PCI Configuration Space Constants */
#define PCI_CONFIG_SPACE_SIZE 256
#define PCI_EXT_CONFIG_SPACE_SIZE 4096

/* PCI Class Codes for NVMe */
#define PCI_CLASS_CODE_STORAGE 0x01
#define PCI_CLASS_SUBCLASS_NVME 0x08
#define PCI_CLASS_INTERFACE_NVME 0x02

/* Vendor/Device ID (Research-use reserved ID range) */
#define HFSSS_VENDOR_ID 0x1D1D
#define HFSSS_DEVICE_ID 0x2001
#define HFSSS_REVISION_ID 0x01
#define HFSSS_SUBSYSTEM_VENDOR_ID 0x1D1D
#define HFSSS_SUBSYSTEM_ID 0x2001

/* PCI Capability IDs */
#define PCI_CAP_ID_PM    0x01  /* Power Management */
#define PCI_CAP_ID_MSI   0x05  /* MSI */
#define PCI_CAP_ID_MSIX  0x11  /* MSI-X */
#define PCI_CAP_ID_EXP   0x10  /* PCI Express */

/* PCI Command Register Bits */
#define PCI_CMD_IO_EN       0x0001  /* I/O Space Enable */
#define PCI_CMD_MEM_EN      0x0002  /* Memory Space Enable */
#define PCI_CMD_BUS_MASTER  0x0004  /* Bus Master Enable */
#define PCI_CMD_PERR_RESP   0x0040  /* Parity Error Response */
#define PCI_CMD_SERR_EN     0x0100  /* SERR# Enable */
#define PCI_CMD_INT_DIS     0x0400  /* Interrupt Disable */

/* PCI Status Register Bits */
#define PCI_STS_CAP_LIST    0x0010  /* Capabilities List */
#define PCI_STS_PERR_DETECT 0x0800  /* Parity Error Detected */
#define PCI_STS_SERR_DETECT 0x1000  /* SERR# Detected */

/* PCI Header Type */
#define PCI_HEADER_TYPE_NORMAL 0x00  /* Type 0 */
#define PCI_HEADER_TYPE_BRIDGE 0x01  /* Type 1 */
#define PCI_HEADER_TYPE_MULTI  0x80  /* Multi-function */

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

#define PCI_MSIX_CTRL_ENABLE  0x8000
#define PCI_MSIX_CTRL_FUNC_MASK 0x4000
#define PCI_MSIX_CTRL_TABLE_SIZE_MASK 0x07FF

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
    u32 link_ctrl2;
    u32 slot_cap2;
    u32 slot_ctrl2;
} __attribute__((packed));

/* PCI Type 0 Configuration Header */
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

/* PCI Capabilities Structure */
struct pci_capabilities {
    struct pci_cap_pm    pm_cap;      /* Offset: 0x40 */
    struct pci_cap_msi   msi_cap;     /* Offset: 0x50 */
    struct pci_cap_msix  msix_cap;    /* Offset: 0x70 */
    struct pci_cap_exp   exp_cap;     /* Offset: 0x90 */
} __attribute__((packed));

/* BAR Definitions */
#define BAR0_SIZE 0x4000  /* 16KB: NVMe Controller Registers */
#define BAR2_SIZE 0x1000  /* 4KB: MSI-X Table (optional) */
#define BAR4_SIZE 0x1000  /* 4KB: MSI-X PBA (optional) */

#define BAR_TYPE_MEM       0x00
#define BAR_TYPE_IO        0x01
#define BAR_MEM_32BIT      0x00
#define BAR_MEM_64BIT      0x04
#define BAR_MEM_PREFETCH   0x08

/* PCI Device Context */
struct pci_dev_ctx {
    /* PCI Configuration Space */
    struct pci_config_header cfg;
    struct pci_capabilities caps;

    /* BAR virtual addresses (for user-space emulation) */
    void *bar0_virt;
    void *bar2_virt;
    void *bar4_virt;

    /* Device state */
    bool enabled;
    bool mem_space_enabled;
    bool bus_master_enabled;

    /* Lock for config space access */
    struct mutex lock;

    /* Parent device data (for NVMe) */
    void *private_data;
};

/* Function Prototypes */
int pci_dev_init(struct pci_dev_ctx *dev);
void pci_dev_cleanup(struct pci_dev_ctx *dev);
int pci_dev_cfg_read(struct pci_dev_ctx *dev, u32 offset, u32 *value, u32 size);
int pci_dev_cfg_write(struct pci_dev_ctx *dev, u32 offset, u32 value, u32 size);
int pci_dev_bar_read(struct pci_dev_ctx *dev, int bar_idx, u64 offset, u64 *value, u32 size);
int pci_dev_bar_write(struct pci_dev_ctx *dev, int bar_idx, u64 offset, u64 value, u32 size);

#endif /* __HFSSS_PCI_H */
