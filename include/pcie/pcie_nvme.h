#ifndef __HFSSS_PCIE_NVME_H
#define __HFSSS_PCIE_NVME_H

/*
 * PCIe/NVMe Device Emulation Module
 *
 * This module provides a user-space emulation of a PCIe NVMe device,
 * exposing a standard NVMe interface to the host while communicating
 * with the user-space HFSSS daemon via shared memory.
 */

#include "pcie/pci.h"
#include "pcie/nvme.h"
#include "pcie/queue.h"
#include "pcie/msix.h"
#include "pcie/dma.h"
#include "pcie/shmem.h"

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

#endif /* __HFSSS_PCIE_NVME_H */
