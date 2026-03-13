#include "pcie/pcie_nvme.h"
#include <stdlib.h>
#include <string.h>

void pcie_nvme_config_default(struct pcie_nvme_config *config)
{
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));

    config->shmem_path = SHMEM_PATH_DEFAULT;
    config->max_queue_pairs = 64;
    config->namespace_count = 1;
    config->namespace_size = 1ULL << 30;  /* 1GB */
    config->page_size = 4096;
}

int pcie_nvme_dev_init(struct pcie_nvme_dev *dev, struct pcie_nvme_config *config)
{
    struct pcie_nvme_config cfg;
    int ret;

    if (!dev) {
        return HFSSS_ERR_INVAL;
    }

    memset(dev, 0, sizeof(*dev));

    if (!config) {
        pcie_nvme_config_default(&cfg);
        config = &cfg;
    }

    ret = mutex_init(&dev->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Save configuration */
    if (config->shmem_path) {
        strncpy(dev->shmem_path, config->shmem_path, sizeof(dev->shmem_path) - 1);
    }
    dev->max_queue_pairs = config->max_queue_pairs;
    dev->namespace_count = config->namespace_count;
    dev->namespace_size = config->namespace_size;

    /* Initialize NVMe Controller (includes PCI device) */
    ret = nvme_ctrl_init(&dev->nvme);
    if (ret != HFSSS_OK) {
        mutex_cleanup(&dev->lock);
        return ret;
    }

    /* Initialize Queue Manager */
    ret = nvme_queue_mgr_init(&dev->qmgr, dev);
    if (ret != HFSSS_OK) {
        nvme_ctrl_cleanup(&dev->nvme);
        mutex_cleanup(&dev->lock);
        return ret;
    }

    /* Initialize MSI-X */
    ret = msix_init(&dev->msix, 32);
    if (ret != HFSSS_OK) {
        nvme_queue_mgr_cleanup(&dev->qmgr);
        nvme_ctrl_cleanup(&dev->nvme);
        mutex_cleanup(&dev->lock);
        return ret;
    }

    /* Initialize DMA */
    ret = dma_init(&dev->dma, 16 * 1024 * 1024);  /* 16MB buffer */
    if (ret != HFSSS_OK) {
        msix_cleanup(&dev->msix);
        nvme_queue_mgr_cleanup(&dev->qmgr);
        nvme_ctrl_cleanup(&dev->nvme);
        mutex_cleanup(&dev->lock);
        return ret;
    }

    /* Link PCI device to NVMe controller */
    dev->pci.private_data = dev;
    dev->nvme.backend_data = dev;

    dev->initialized = true;
    dev->running = false;

    return HFSSS_OK;
}

void pcie_nvme_dev_cleanup(struct pcie_nvme_dev *dev)
{
    if (!dev) {
        return;
    }

    if (dev->running) {
        pcie_nvme_dev_stop(dev);
    }

    dma_cleanup(&dev->dma);
    msix_cleanup(&dev->msix);
    nvme_queue_mgr_cleanup(&dev->qmgr);
    nvme_ctrl_cleanup(&dev->nvme);

    mutex_cleanup(&dev->lock);
    memset(dev, 0, sizeof(*dev));
}

int pcie_nvme_dev_start(struct pcie_nvme_dev *dev)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&dev->lock, 0);

    if (dev->running) {
        mutex_unlock(&dev->lock);
        return HFSSS_OK;
    }

    /* Try to create shared memory (emulation mode) */
    int ret = shmem_create(&dev->shmem, dev->shmem_path[0] ? dev->shmem_path : SHMEM_PATH_DEFAULT);
    if (ret != HFSSS_OK) {
        /* Try to open existing one */
        ret = shmem_open(&dev->shmem, dev->shmem_path[0] ? dev->shmem_path : SHMEM_PATH_DEFAULT);
    }

    if (ret != HFSSS_OK) {
        /* Shared memory not available, continue without it */
        dev->shmem.fd = -1;
    }

    /* Enable MSI-X */
    msix_enable(&dev->msix);

    dev->running = true;
    dev->cmd_thread_running = true;

    mutex_unlock(&dev->lock);

    return HFSSS_OK;
}

void pcie_nvme_dev_stop(struct pcie_nvme_dev *dev)
{
    if (!dev) {
        return;
    }

    mutex_lock(&dev->lock, 0);

    if (!dev->running) {
        mutex_unlock(&dev->lock);
        return;
    }

    dev->cmd_thread_running = false;
    dev->running = false;

    /* Disable MSI-X */
    msix_disable(&dev->msix);

    /* Close shared memory */
    shmem_close(&dev->shmem);

    mutex_unlock(&dev->lock);
}

int pcie_nvme_cfg_read(struct pcie_nvme_dev *dev, u32 offset, u32 *val, u32 size)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }

    return pci_dev_cfg_read(&dev->nvme.pci_dev, offset, val, size);
}

int pcie_nvme_cfg_write(struct pcie_nvme_dev *dev, u32 offset, u32 val, u32 size)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }

    return pci_dev_cfg_write(&dev->nvme.pci_dev, offset, val, size);
}

int pcie_nvme_bar_read(struct pcie_nvme_dev *dev, int bar, u64 offset, u64 *val, u32 size)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }

    if (bar == 0) {
        /* BAR0: NVMe Registers */
        return nvme_ctrl_reg_read(&dev->nvme, offset, val, size);
    } else if (bar == 2) {
        /* BAR2: MSI-X Table */
        return msix_table_read(&dev->msix, (u32)offset, val, size);
    } else if (bar == 4) {
        /* BAR4: MSI-X PBA */
        return msix_pba_read(&dev->msix, (u32)offset, val, size);
    }

    return pci_dev_bar_read(&dev->nvme.pci_dev, bar, offset, val, size);
}

int pcie_nvme_bar_write(struct pcie_nvme_dev *dev, int bar, u64 offset, u64 val, u32 size)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }

    if (bar == 0) {
        /* BAR0: NVMe Registers */
        return nvme_ctrl_reg_write(&dev->nvme, offset, val, size);
    } else if (bar == 2) {
        /* BAR2: MSI-X Table */
        return msix_table_write(&dev->msix, (u32)offset, val, size);
    } else if (bar == 4) {
        /* BAR4: MSI-X PBA */
        return msix_pba_write(&dev->msix, (u32)offset, val, size);
    }

    return pci_dev_bar_write(&dev->nvme.pci_dev, bar, offset, val, size);
}

int pcie_nvme_process_admin_cmd(struct pcie_nvme_dev *dev, struct nvme_sq_entry *cmd,
                                 struct nvme_cq_entry *cpl)
{
    if (!dev || !dev->initialized || !cmd || !cpl) {
        return HFSSS_ERR_INVAL;
    }

    return nvme_ctrl_process_admin_cmd(&dev->nvme, cmd, cpl);
}

int pcie_nvme_process_io_cmd(struct pcie_nvme_dev *dev, struct nvme_sq_entry *cmd,
                              struct nvme_cq_entry *cpl)
{
    if (!dev || !dev->initialized || !cmd || !cpl) {
        return HFSSS_ERR_INVAL;
    }

    return nvme_ctrl_process_io_cmd(&dev->nvme, cmd, cpl);
}
