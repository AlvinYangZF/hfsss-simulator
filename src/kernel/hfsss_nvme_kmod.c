// SPDX-License-Identifier: GPL-2.0
/*
 * hfsss_nvme_kmod.c — HFSSS NVMe kernel module main entry point (REQ-022)
 *
 * Presents the HFSSS user-space simulator as a PCI NVMe device to the host
 * kernel.  Communication with the user-space simulator is via a shared-memory
 * ring buffer (see hfsss_nvme_shmem.c).
 *
 * Architectural overview
 * ----------------------
 * +------------------+        shmem ring        +---------------------+
 * | Linux NVMe driver| <--- hfsss_nvme.ko -----> | hfsss user-space    |
 * | (nvme.ko)        |   /dev/hfsss_shmem        | daemon (sssim)      |
 * +------------------+                           +---------------------+
 *
 * The kernel module registers as a PCI driver.  When the matching virtual PCI
 * device is discovered (vendor 0x1DEA / device 0x4E56), probe() maps BAR0,
 * initialises the NVMe register set, allocates queues, and starts the
 * completion-poll thread that drains the shared-memory ring.
 */

#ifdef __KERNEL__

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/errno.h>

#include "hfsss_nvme_kmod.h"

/* Forward declarations for sub-modules */
int  hfsss_pci_init(struct pci_dev *dev, void __iomem **bar0_out);
void hfsss_pci_cleanup(struct pci_dev *dev, void __iomem *bar0);
int  hfsss_admin_queue_create(struct pci_dev *dev);
void hfsss_admin_queue_destroy(struct pci_dev *dev);
int  hfsss_shmem_init(size_t size);
void hfsss_shmem_cleanup(void);

/* Per-device private data stored in pci_dev driver-data slot */
struct hfsss_dev {
    struct pci_dev  *pdev;
    void __iomem    *bar0;       /* BAR0 ioremap — NVMe register window   */
    u32              csts;       /* shadow of CSTS register               */
    u32              cc;         /* shadow of CC register                 */
};

/* ---------------------------------------------------------------------------
 * PCI device ID table
 * Matches the virtual device advertised by the VFIO/QEMU configuration that
 * loads the HFSSS user-space daemon.
 * --------------------------------------------------------------------------*/
static const struct pci_device_id hfsss_pci_ids[] = {
    {
        PCI_DEVICE_SUB(HFSSS_PCI_VENDOR_ID,
                       HFSSS_PCI_DEVICE_ID,
                       HFSSS_PCI_SUBSYS_VENDOR,
                       HFSSS_PCI_SUBSYS_DEVICE)
    },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, hfsss_pci_ids);

/* ---------------------------------------------------------------------------
 * hfsss_pci_probe — called when the kernel matches a PCI device to this driver
 *
 * Steps performed:
 *   1. Enable the PCI device and request ownership of BAR0.
 *   2. ioremap BAR0 so NVMe registers are accessible as MMIO.
 *   3. Initialise the shared-memory ring buffer used to forward NVMe commands
 *      to the user-space simulator.
 *   4. Create the admin queue (SQ/CQ pair for queue-0).
 *   5. Set CSTS.RDY=0 until CC.EN=1 is written by the host NVMe driver.
 *
 * Returns 0 on success, negative errno on failure.
 * --------------------------------------------------------------------------*/
static int hfsss_pci_probe(struct pci_dev *dev,
                            const struct pci_device_id *id)
{
    struct hfsss_dev *hdev;
    int ret;

    dev_info(&dev->dev, "hfsss_nvme: probing device %04x:%04x\n",
             dev->vendor, dev->device);

    hdev = devm_kzalloc(&dev->dev, sizeof(*hdev), GFP_KERNEL);
    if (!hdev)
        return -ENOMEM;

    hdev->pdev = dev;
    pci_set_drvdata(dev, hdev);

    /* Step 1–2: Enable device and map BAR0 */
    ret = hfsss_pci_init(dev, &hdev->bar0);
    if (ret) {
        dev_err(&dev->dev, "hfsss_nvme: PCI init failed (%d)\n", ret);
        return ret;
    }

    /* Step 3: Allocate shared-memory ring buffer */
    ret = hfsss_shmem_init(HFSSS_SHMEM_RING_SIZE *
                            sizeof(struct nvme_command));
    if (ret) {
        dev_err(&dev->dev,
                "hfsss_nvme: shmem init failed (%d)\n", ret);
        goto err_pci;
    }

    /* Step 4: Admin queue creation */
    ret = hfsss_admin_queue_create(dev);
    if (ret) {
        dev_err(&dev->dev,
                "hfsss_nvme: admin queue init failed (%d)\n", ret);
        goto err_shmem;
    }

    dev_info(&dev->dev, "hfsss_nvme: probe complete\n");
    return 0;

err_shmem:
    hfsss_shmem_cleanup();
err_pci:
    hfsss_pci_cleanup(dev, hdev->bar0);
    return ret;
}

/* ---------------------------------------------------------------------------
 * hfsss_pci_remove — called on device removal or driver unload
 *
 * Tears down in reverse probe order: queues → shmem → BAR0 unmap.
 * --------------------------------------------------------------------------*/
static void hfsss_pci_remove(struct pci_dev *dev)
{
    struct hfsss_dev *hdev = pci_get_drvdata(dev);

    dev_info(&dev->dev, "hfsss_nvme: removing device\n");

    hfsss_admin_queue_destroy(dev);
    hfsss_shmem_cleanup();

    if (hdev && hdev->bar0)
        hfsss_pci_cleanup(dev, hdev->bar0);

    dev_info(&dev->dev, "hfsss_nvme: device removed\n");
}

/* ---------------------------------------------------------------------------
 * PCI driver registration structure
 * --------------------------------------------------------------------------*/
static struct pci_driver hfsss_pci_driver = {
    .name     = "hfsss_nvme",
    .id_table = hfsss_pci_ids,
    .probe    = hfsss_pci_probe,
    .remove   = hfsss_pci_remove,
};

/*
 * module_pci_driver expands to module_init / module_exit wrappers that call
 * pci_register_driver / pci_unregister_driver respectively.
 */
module_pci_driver(hfsss_pci_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("HFSSS Project");
MODULE_DESCRIPTION("HFSSS NVMe kernel module — exposes simulator as /dev/nvme block device");
MODULE_VERSION("0.1.0");

#endif /* __KERNEL__ */
