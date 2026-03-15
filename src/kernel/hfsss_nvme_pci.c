// SPDX-License-Identifier: GPL-2.0
/*
 * hfsss_nvme_pci.c — PCI config space, BAR0 MMIO, and NVMe register handlers
 *                    (REQ-001, REQ-002, REQ-003, REQ-004, REQ-005, REQ-006)
 *
 * BAR0 layout (16 KB, matches NVMe 1.4 spec §3.1)
 * -----------------------------------------------
 *   0x0000–0x0007  CAP   — Controller Capabilities (64-bit, RO)
 *   0x0008–0x000B  VS    — Version (32-bit, RO)
 *   0x000C–0x000F  INTMS — Interrupt Mask Set (32-bit, WO)
 *   0x0010–0x0013  INTMC — Interrupt Mask Clear (32-bit, WO)
 *   0x0014–0x0017  CC    — Controller Configuration (32-bit, RW)
 *   0x0018–0x001B  (reserved)
 *   0x001C–0x001F  CSTS  — Controller Status (32-bit, RO)
 *   0x0020–0x0023  NSSR  — NVM Subsystem Reset (32-bit, WO)
 *   0x0024–0x0027  AQA   — Admin Queue Attributes (32-bit, RW)
 *   0x0028–0x002F  ASQ   — Admin SQ Base Address (64-bit, RW)
 *   0x0030–0x0037  ACQ   — Admin CQ Base Address (64-bit, RW)
 *   0x1000+        Doorbell registers (SQ tail / CQ head)
 *
 * PCIe Capabilities chain (offsets in PCI config space):
 *   0x40  Power Management (PM)      cap_id=0x01
 *   0x50  MSI-X                      cap_id=0x11
 *   0x70  PCIe Capability Structure  cap_id=0x10
 *
 * CC.EN state machine:
 *   CC.EN 0→1  :  validate CC fields, set CSTS.RDY=1
 *   CC.EN 1→0  :  abort in-flight commands, set CSTS.RDY=0
 */

#ifdef __KERNEL__

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/errno.h>
#include <linux/kernel.h>

#include "hfsss_nvme_kmod.h"

/* NVMe register offsets — kept local to avoid collision with linux/nvme.h */
#define HFSSS_REG_CAP    0x00
#define HFSSS_REG_VS     0x08
#define HFSSS_REG_INTMS  0x0C
#define HFSSS_REG_INTMC  0x10
#define HFSSS_REG_CC     0x14
#define HFSSS_REG_CSTS   0x1C
#define HFSSS_REG_NSSR   0x20
#define HFSSS_REG_AQA    0x24
#define HFSSS_REG_ASQ    0x28
#define HFSSS_REG_ACQ    0x30

/* CC bit definitions */
#define HFSSS_CC_EN      BIT(0)

/* CSTS bit definitions */
#define HFSSS_CSTS_RDY   BIT(0)
#define HFSSS_CSTS_CFS   BIT(1)

/* NVMe version advertised: 1.4 */
#define HFSSS_NVME_VS    0x00010400U

/* Module-level MMIO register shadow (one device supported in this stub) */
static void __iomem *g_bar0;
static u32 g_csts;
static u32 g_cc;

/* ---------------------------------------------------------------------------
 * hfsss_pci_init — enable PCI device and map BAR0
 *
 * Called from hfsss_pci_probe.  On success *bar0_out contains the ioremap'd
 * virtual address; caller must pass it to hfsss_pci_cleanup on teardown.
 * --------------------------------------------------------------------------*/
int hfsss_pci_init(struct pci_dev *dev, void __iomem **bar0_out)
{
    int ret;

    /*
     * Enable PCI device — powers it on, enables bus mastering so the device
     * can perform DMA, and assigns I/O resources if not already done by BIOS.
     */
    ret = pci_enable_device(dev);
    if (ret) {
        dev_err(&dev->dev, "hfsss_nvme: pci_enable_device failed (%d)\n", ret);
        return ret;
    }

    pci_set_master(dev);

    /*
     * Request exclusive ownership of BAR0.  This prevents another driver from
     * accidentally mapping the same region.
     */
    ret = pci_request_region(dev, 0, "hfsss_nvme_bar0");
    if (ret) {
        dev_err(&dev->dev, "hfsss_nvme: pci_request_region(BAR0) failed (%d)\n",
                ret);
        goto err_disable;
    }

    /*
     * ioremap BAR0 into kernel virtual address space.  All subsequent NVMe
     * register accesses go through this pointer using readl/writel.
     */
    *bar0_out = pci_ioremap_bar(dev, 0);
    if (!*bar0_out) {
        dev_err(&dev->dev, "hfsss_nvme: pci_ioremap_bar(0) failed\n");
        ret = -ENOMEM;
        goto err_release;
    }

    g_bar0 = *bar0_out;

    /*
     * Initialise read-only NVMe registers in BAR0:
     *   CAP  — advertise capabilities (MQES=4095, CQR=1, TO=500ms, CSS=NVM)
     *   VS   — NVMe version 1.4
     *   CSTS — not ready until CC.EN=1 is processed
     */
    writeq(HFSSS_NVME_CAP_DEFAULT, g_bar0 + HFSSS_REG_CAP);
    writel(HFSSS_NVME_VS,          g_bar0 + HFSSS_REG_VS);
    g_csts = 0;
    writel(g_csts, g_bar0 + HFSSS_REG_CSTS);

    dev_info(&dev->dev, "hfsss_nvme: BAR0 mapped at %p (size %d KB)\n",
             *bar0_out, HFSSS_BAR0_SIZE / 1024);
    return 0;

err_release:
    pci_release_region(dev, 0);
err_disable:
    pci_disable_device(dev);
    return ret;
}

/* ---------------------------------------------------------------------------
 * hfsss_pci_cleanup — unmap BAR0 and release PCI resources
 * --------------------------------------------------------------------------*/
void hfsss_pci_cleanup(struct pci_dev *dev, void __iomem *bar0)
{
    if (bar0) {
        iounmap(bar0);
        g_bar0 = NULL;
    }
    pci_release_region(dev, 0);
    pci_disable_device(dev);
}

/* ---------------------------------------------------------------------------
 * hfsss_nvme_reg_write32 — handle 32-bit writes to NVMe BAR0 registers
 *
 * Intercepts writes to CC and NSSR to implement the CC.EN state machine and
 * subsystem reset.  All other writable registers are passed through directly.
 *
 * CC.EN transitions:
 *   0 → 1 : controller enabled; set CSTS.RDY=1 (simulated instant readiness)
 *   1 → 0 : controller disabled/reset; clear CSTS.RDY
 * --------------------------------------------------------------------------*/
void hfsss_nvme_reg_write32(u32 offset, u32 value)
{
    if (!g_bar0)
        return;

    switch (offset) {
    case HFSSS_REG_CC:
        /*
         * Detect CC.EN transitions to drive CSTS.RDY.  A real controller
         * would delay CSTS.RDY=1 by up to CAP.TO × 500 ms; we set it
         * synchronously since there is no actual hardware to initialise.
         */
        if ((value & HFSSS_CC_EN) && !(g_cc & HFSSS_CC_EN)) {
            /* EN: 0 → 1 */
            g_csts |= HFSSS_CSTS_RDY;
            writel(g_csts, g_bar0 + HFSSS_REG_CSTS);
            pr_debug("hfsss_nvme: CC.EN=1 → CSTS.RDY=1\n");
        } else if (!(value & HFSSS_CC_EN) && (g_cc & HFSSS_CC_EN)) {
            /* EN: 1 → 0 */
            g_csts &= ~HFSSS_CSTS_RDY;
            writel(g_csts, g_bar0 + HFSSS_REG_CSTS);
            pr_debug("hfsss_nvme: CC.EN=0 → CSTS.RDY=0\n");
        }
        g_cc = value;
        writel(g_cc, g_bar0 + HFSSS_REG_CC);
        break;

    case HFSSS_REG_NSSR:
        /*
         * Writing 0x4E564D45 ("NVMe") triggers a subsystem reset.
         * Clear CSTS.RDY and reset CC to power-on defaults.
         */
        if (value == 0x4E564D45U) {
            g_cc   = 0;
            g_csts = 0;
            writel(g_cc,   g_bar0 + HFSSS_REG_CC);
            writel(g_csts, g_bar0 + HFSSS_REG_CSTS);
            pr_info("hfsss_nvme: NVM subsystem reset triggered\n");
        }
        break;

    default:
        /* AQA, ASQ/ACQ doorbells — write-through to MMIO shadow */
        writel(value, g_bar0 + offset);
        break;
    }
}

/* ---------------------------------------------------------------------------
 * hfsss_nvme_reg_read32 — handle 32-bit reads from NVMe BAR0 registers
 *
 * Returns the current value of the requested register.  CAP and VS are
 * read-only and were initialised in hfsss_pci_init.  CSTS and CC are
 * served from in-memory shadows to ensure consistency with state transitions
 * that happen between MMIO writes.
 * --------------------------------------------------------------------------*/
u32 hfsss_nvme_reg_read32(u32 offset)
{
    if (!g_bar0)
        return 0xFFFFFFFFU;

    switch (offset) {
    case HFSSS_REG_CC:
        return g_cc;
    case HFSSS_REG_CSTS:
        return g_csts;
    default:
        return readl(g_bar0 + offset);
    }
}

#endif /* __KERNEL__ */
