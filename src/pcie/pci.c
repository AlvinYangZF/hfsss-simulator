#include "pcie/pci.h"
#include <stdlib.h>
#include <string.h>

/* PCI Device Initialization */
int pci_dev_init(struct pci_dev_ctx *dev)
{
    int ret;

    if (!dev) {
        return HFSSS_ERR_INVAL;
    }

    memset(dev, 0, sizeof(*dev));

    /* Initialize lock */
    ret = mutex_init(&dev->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* PCI Configuration Header */
    dev->cfg.vendor_id = HFSSS_VENDOR_ID;
    dev->cfg.device_id = HFSSS_DEVICE_ID;
    dev->cfg.command = 0;
    dev->cfg.status = PCI_STS_CAP_LIST;
    dev->cfg.revision_id = HFSSS_REVISION_ID;
    dev->cfg.class_code[0] = PCI_CLASS_INTERFACE_NVME;
    dev->cfg.class_code[1] = PCI_CLASS_SUBCLASS_NVME;
    dev->cfg.class_code[2] = PCI_CLASS_CODE_STORAGE;
    dev->cfg.header_type = PCI_HEADER_TYPE_NORMAL;
    dev->cfg.subsystem_vendor_id = HFSSS_SUBSYSTEM_VENDOR_ID;
    dev->cfg.subsystem_id = HFSSS_SUBSYSTEM_ID;
    dev->cfg.capabilities_ptr = 0x40;  /* First Capability at 0x40 */
    dev->cfg.interrupt_pin = 1;  /* INTA# */

    /* BAR Configuration */
    dev->cfg.bar[0] = BAR0_SIZE | BAR_TYPE_MEM | BAR_MEM_32BIT;
    dev->cfg.bar[1] = 0;
    dev->cfg.bar[2] = 0;
    dev->cfg.bar[3] = 0;
    dev->cfg.bar[4] = 0;
    dev->cfg.bar[5] = 0;

    /* PCI Capabilities Initialization */
    /* PM Capability at 0x40 */
    dev->caps.pm_cap.hdr.cap_id = PCI_CAP_ID_PM;
    dev->caps.pm_cap.hdr.next = 0x50;
    dev->caps.pm_cap.pm_cap = 0x0003;

    /* MSI Capability at 0x50 */
    dev->caps.msi_cap.hdr.cap_id = PCI_CAP_ID_MSI;
    dev->caps.msi_cap.hdr.next = 0x70;
    dev->caps.msi_cap.message_control = 0x0080;  /* 64-bit capable */

    /* MSI-X Capability at 0x70 */
    dev->caps.msix_cap.hdr.cap_id = PCI_CAP_ID_MSIX;
    dev->caps.msix_cap.hdr.next = 0x90;
    dev->caps.msix_cap.message_control = 0x001F;  /* 32 vectors */
    dev->caps.msix_cap.table_offset = 0x00004000;  /* BAR2 + 0x0000 */
    dev->caps.msix_cap.pba_offset = 0x00008000;    /* BAR4 + 0x0000 */

    /* PCIe Capability at 0x90 */
    dev->caps.exp_cap.hdr.cap_id = PCI_CAP_ID_EXP;
    dev->caps.exp_cap.hdr.next = 0x00;  /* End of list */
    dev->caps.exp_cap.pcie_cap = 0x0002;  /* Express Endpoint */
    dev->caps.exp_cap.dev_cap = 0x00008000;  /* Max Payload 128 bytes */
    dev->caps.exp_cap.link_cap = 0x00000041;  /* 2.5 GT/s, x1 */
    dev->caps.exp_cap.link_sts = 0x00004011;  /* Link up, 2.5 GT/s, x1 */

    dev->enabled = false;
    dev->mem_space_enabled = false;
    dev->bus_master_enabled = false;

    return HFSSS_OK;
}

void pci_dev_cleanup(struct pci_dev_ctx *dev)
{
    if (!dev) {
        return;
    }

    mutex_cleanup(&dev->lock);
    memset(dev, 0, sizeof(*dev));
}

int pci_dev_cfg_read(struct pci_dev_ctx *dev, u32 offset, u32 *value, u32 size)
{
    u8 *cfg_ptr;
    u32 val = 0;

    if (!dev || !value || size != 1 && size != 2 && size != 4) {
        return HFSSS_ERR_INVAL;
    }

    if (offset >= PCI_CONFIG_SPACE_SIZE) {
        *value = 0;
        return HFSSS_OK;
    }

    mutex_lock(&dev->lock, 0);

    cfg_ptr = (u8 *)&dev->cfg;

    /* Read based on size */
    if (size == 1) {
        val = cfg_ptr[offset];
    } else if (size == 2) {
        if (offset + 1 < PCI_CONFIG_SPACE_SIZE) {
            val = *(u16 *)(cfg_ptr + offset);
        }
    } else if (size == 4) {
        if (offset + 3 < PCI_CONFIG_SPACE_SIZE) {
            if (offset >= 0x40 && offset < 0x40 + sizeof(dev->caps)) {
                /* Capability space */
                val = *(u32 *)((u8 *)&dev->caps + (offset - 0x40));
            } else {
                val = *(u32 *)(cfg_ptr + offset);
            }
        }
    }

    *value = val;

    mutex_unlock(&dev->lock);

    return HFSSS_OK;
}

int pci_dev_cfg_write(struct pci_dev_ctx *dev, u32 offset, u32 value, u32 size)
{
    u8 *cfg_ptr;

    if (!dev || size != 1 && size != 2 && size != 4) {
        return HFSSS_ERR_INVAL;
    }

    if (offset >= PCI_CONFIG_SPACE_SIZE) {
        return HFSSS_OK;
    }

    mutex_lock(&dev->lock, 0);

    cfg_ptr = (u8 *)&dev->cfg;

    /* Write based on size */
    if (size == 1) {
        /* Check read-only registers */
        if (offset == 0x00 || offset == 0x02 || offset == 0x08 ||
            offset >= 0x09 && offset <= 0x0B || offset == 0x0E ||
            offset == 0x2C || offset == 0x2E || offset == 0x34 ||
            offset >= 0x3D && offset <= 0x3F) {
            goto out;
        }

        cfg_ptr[offset] = (u8)value;
    } else if (size == 2) {
        if (offset + 1 < PCI_CONFIG_SPACE_SIZE) {
            /* Check read-only registers */
            if (offset == 0x00 || offset == 0x02 || offset == 0x06 ||
                offset == 0x2C || offset == 0x2E) {
                goto out;
            }

            *(u16 *)(cfg_ptr + offset) = (u16)value;

            /* Handle Command register */
            if (offset == 0x04) {
                dev->mem_space_enabled = (value & PCI_CMD_MEM_EN) != 0;
                dev->bus_master_enabled = (value & PCI_CMD_BUS_MASTER) != 0;
            }
        }
    } else if (size == 4) {
        if (offset + 3 < PCI_CONFIG_SPACE_SIZE) {
            /* Handle BARs specially */
            if (offset >= 0x10 && offset <= 0x24) {
                int bar_idx = (offset - 0x10) / 4;
                if (bar_idx == 0) {
                    /* BAR0 - MMIO, write mask based on size */
                    if ((value & 0x1) == 0) {
                        dev->cfg.bar[bar_idx] = (value & ~(BAR0_SIZE - 1)) | (dev->cfg.bar[bar_idx] & 0xF);
                    }
                }
            }
        }
    }

out:
    mutex_unlock(&dev->lock);

    return HFSSS_OK;
}

int pci_dev_bar_read(struct pci_dev_ctx *dev, int bar_idx, u64 offset, u64 *value, u32 size)
{
    if (!dev || !value || bar_idx < 0 || bar_idx >= 6) {
        return HFSSS_ERR_INVAL;
    }

    if (!dev->mem_space_enabled) {
        *value = 0;
        return HFSSS_OK;
    }

    /* For now, just return 0 for BAR access */
    *value = 0;

    return HFSSS_OK;
}

int pci_dev_bar_write(struct pci_dev_ctx *dev, int bar_idx, u64 offset, u64 value, u32 size)
{
    if (!dev || bar_idx < 0 || bar_idx >= 6) {
        return HFSSS_ERR_INVAL;
    }

    if (!dev->mem_space_enabled) {
        return HFSSS_OK;
    }

    /* For now, ignore BAR writes */

    return HFSSS_OK;
}
