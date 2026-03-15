#ifndef __HFSSS_NVME_USPACE_H
#define __HFSSS_NVME_USPACE_H

#include "pcie/nvme.h"
#include "pcie/queue.h"
#include "sssim.h"

/* User-space NVMe Device Context */
struct nvme_uspace_dev {
    /* NVMe Controller */
    struct nvme_ctrl_ctx ctrl;

    /* Queue Manager */
    struct nvme_queue_mgr qmgr;

    /* SSD Simulator Context */
    struct sssim_ctx sssim;

    /* Data buffers for user-space commands */
    void *data_buffer;
    size_t data_buffer_size;

    /* Device State */
    bool initialized;
    bool running;

    /* Lock */
    struct mutex lock;
};

/* User-space NVMe Configuration */
struct nvme_uspace_config {
    struct sssim_config sssim_cfg;
    size_t data_buffer_size;
};

/* Function Prototypes */
int nvme_uspace_dev_init(struct nvme_uspace_dev *dev, struct nvme_uspace_config *config);
void nvme_uspace_dev_cleanup(struct nvme_uspace_dev *dev);
int nvme_uspace_dev_start(struct nvme_uspace_dev *dev);
void nvme_uspace_dev_stop(struct nvme_uspace_dev *dev);

/* High-level User-space Commands */
int nvme_uspace_identify_ctrl(struct nvme_uspace_dev *dev, struct nvme_identify_ctrl *id);
int nvme_uspace_identify_ns(struct nvme_uspace_dev *dev, u32 nsid, struct nvme_identify_ns *id);
int nvme_uspace_create_io_cq(struct nvme_uspace_dev *dev, u16 qid, u16 qsize, bool intr_en);
int nvme_uspace_delete_io_cq(struct nvme_uspace_dev *dev, u16 qid);
int nvme_uspace_create_io_sq(struct nvme_uspace_dev *dev, u16 qid, u16 qsize, u16 cqid, u8 prio);
int nvme_uspace_delete_io_sq(struct nvme_uspace_dev *dev, u16 qid);

/* NVM I/O Commands */
int nvme_uspace_read(struct nvme_uspace_dev *dev, u32 nsid, u64 lba, u32 count, void *data);
int nvme_uspace_write(struct nvme_uspace_dev *dev, u32 nsid, u64 lba, u32 count, const void *data);
int nvme_uspace_flush(struct nvme_uspace_dev *dev, u32 nsid);

/* Default Config Helper */
void nvme_uspace_config_default(struct nvme_uspace_config *config);

#endif /* __HFSSS_NVME_USPACE_H */
