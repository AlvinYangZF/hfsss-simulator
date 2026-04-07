#include "pcie/nvme_uspace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* SMART/Health log page (LID=2) layout used by get_log_page */
typedef struct {
    uint8_t critical_warning;
    uint16_t temperature;
    uint8_t avail_spare;
    uint8_t avail_spare_thresh;
    uint8_t percent_used;
    uint8_t rsvd[26];
    uint64_t data_units_read;
    uint64_t data_units_written;
    uint64_t host_read_cmds;
    uint64_t host_write_cmds;
    uint64_t ctrl_busy_time;
    uint64_t power_cycles;
    uint64_t power_on_hours;
    uint64_t unsafe_shutdowns;
    uint64_t media_errors;
    uint64_t num_err_log_entries;
} smart_log_t;

void nvme_uspace_config_default(struct nvme_uspace_config *config)
{
    if (!config) {
        return;
    }

    sssim_config_default(&config->sssim_cfg);
    config->data_buffer_size = 1 * 1024 * 1024; /* 1 MB - smaller for test */
}

int nvme_uspace_dev_init(struct nvme_uspace_dev *dev, struct nvme_uspace_config *config)
{
    int ret;
    struct nvme_uspace_config default_cfg;

    if (!dev) {
        return HFSSS_ERR_INVAL;
    }

    memset(dev, 0, sizeof(*dev));

    /* Use default config if none provided */
    if (!config) {
        nvme_uspace_config_default(&default_cfg);
        config = &default_cfg;
    }

    /* Initialize lock */
    ret = mutex_init(&dev->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Initialize NVMe controller */
    ret = nvme_ctrl_init(&dev->ctrl);
    if (ret != HFSSS_OK) {
        mutex_cleanup(&dev->lock);
        return ret;
    }

    /* Initialize queue manager */
    ret = nvme_queue_mgr_init(&dev->qmgr, dev);
    if (ret != HFSSS_OK) {
        nvme_ctrl_cleanup(&dev->ctrl);
        mutex_cleanup(&dev->lock);
        return ret;
    }

    /* Initialize SSD simulator */
    ret = sssim_init(&dev->sssim, &config->sssim_cfg);
    if (ret != HFSSS_OK) {
        nvme_queue_mgr_cleanup(&dev->qmgr);
        nvme_ctrl_cleanup(&dev->ctrl);
        mutex_cleanup(&dev->lock);
        return ret;
    }

    /* Allocate data buffer */
    dev->data_buffer_size = config->data_buffer_size;
    dev->data_buffer = malloc(config->data_buffer_size);
    if (!dev->data_buffer) {
        sssim_cleanup(&dev->sssim);
        nvme_queue_mgr_cleanup(&dev->qmgr);
        nvme_ctrl_cleanup(&dev->ctrl);
        mutex_cleanup(&dev->lock);
        return HFSSS_ERR_NOMEM;
    }

    dev->initialized = true;
    dev->running = false;

    return HFSSS_OK;
}

void nvme_uspace_dev_cleanup(struct nvme_uspace_dev *dev)
{
    if (!dev) {
        return;
    }

    if (dev->data_buffer) {
        free(dev->data_buffer);
    }

    if (dev->fw_staging_buf) {
        free(dev->fw_staging_buf);
    }

    if (dev->initialized) {
        sssim_cleanup(&dev->sssim);
    }

    nvme_queue_mgr_cleanup(&dev->qmgr);
    nvme_ctrl_cleanup(&dev->ctrl);
    mutex_cleanup(&dev->lock);

    memset(dev, 0, sizeof(*dev));
}

int nvme_uspace_dev_start(struct nvme_uspace_dev *dev)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&dev->lock, 0);
    dev->running = true;
    mutex_unlock(&dev->lock);

    return HFSSS_OK;
}

void nvme_uspace_dev_stop(struct nvme_uspace_dev *dev)
{
    if (!dev || !dev->initialized) {
        return;
    }

    mutex_lock(&dev->lock, 0);
    dev->running = false;
    mutex_unlock(&dev->lock);
}

int nvme_uspace_identify_ctrl(struct nvme_uspace_dev *dev, struct nvme_identify_ctrl *id)
{
    if (!dev || !dev->initialized || !id) {
        return HFSSS_ERR_INVAL;
    }

    nvme_build_identify_ctrl(id);

    /* Reflect committed firmware revision when available */
    if (dev->fw_revision[0] != 0) {
        memcpy(id->fr, dev->fw_revision, 8);
    }

    /* Update with actual SSD capacity information */
    u64 total_lbas = dev->sssim.config.total_lbas;
    u64 total_bytes = total_lbas * dev->sssim.config.lba_size;

    /* Set Total NVM Capacity (TNVMCP) - 16 bytes little endian */
    id->tnvmcap[0] = (u8)(total_bytes & 0xFF);
    id->tnvmcap[1] = (u8)((total_bytes >> 8) & 0xFF);
    id->tnvmcap[2] = (u8)((total_bytes >> 16) & 0xFF);
    id->tnvmcap[3] = (u8)((total_bytes >> 24) & 0xFF);
    id->tnvmcap[4] = (u8)((total_bytes >> 32) & 0xFF);
    id->tnvmcap[5] = (u8)((total_bytes >> 40) & 0xFF);
    id->tnvmcap[6] = (u8)((total_bytes >> 48) & 0xFF);
    id->tnvmcap[7] = (u8)((total_bytes >> 56) & 0xFF);

    /* Unallocated NVM Capacity is same as total for now */
    memcpy(id->unvmcap, id->tnvmcap, 16);

    return HFSSS_OK;
}

int nvme_uspace_identify_ns(struct nvme_uspace_dev *dev, u32 nsid, struct nvme_identify_ns *id)
{
    if (!dev || !dev->initialized || !id) {
        return HFSSS_ERR_INVAL;
    }

    if (nsid != 1 && nsid != 0) {
        return HFSSS_ERR_INVAL;
    }

    nvme_build_identify_ns(id, nsid);

    /* Update with actual SSD configuration */
    u64 total_lbas = dev->sssim.config.total_lbas;
    u32 lba_size = dev->sssim.config.lba_size;

    id->nsze = total_lbas;
    id->ncap = total_lbas;
    id->nuse = 0;

    /* Update LBA format */
    int lbads = 0;
    u32 size = lba_size;
    while (size > 1) {
        size >>= 1;
        lbads++;
    }
    id->lbaf[0].lbads = lbads;

    return HFSSS_OK;
}

int nvme_uspace_create_io_cq(struct nvme_uspace_dev *dev, u16 qid, u16 qsize, bool intr_en)
{
    int ret;

    if (!dev || !dev->initialized || qid == 0 || qid >= MAX_QUEUE_PAIRS) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&dev->lock, 0);

    /* Check if CQ already exists */
    if (dev->qmgr.io_cqs[qid].enabled) {
        ret = HFSSS_ERR_EXIST;
        goto out;
    }

    ret = nvme_cq_create(&dev->qmgr.io_cqs[qid], qid, 0, qsize, 16, intr_en);
    if (ret == HFSSS_OK) {
        dev->qmgr.num_io_cqs++;
    }

out:
    mutex_unlock(&dev->lock);
    return ret;
}

int nvme_uspace_delete_io_cq(struct nvme_uspace_dev *dev, u16 qid)
{
    if (!dev || !dev->initialized || qid == 0 || qid >= MAX_QUEUE_PAIRS) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&dev->lock, 0);

    if (!dev->qmgr.io_cqs[qid].enabled) {
        mutex_unlock(&dev->lock);
        return HFSSS_ERR_NOENT;
    }

    /* Check if any SQs are associated with this CQ */
    for (int i = 0; i < MAX_QUEUE_PAIRS; i++) {
        if (dev->qmgr.io_sqs[i].enabled && dev->qmgr.io_sqs[i].cqid == qid) {
            mutex_unlock(&dev->lock);
            return HFSSS_ERR_BUSY;
        }
    }

    nvme_cq_destroy(&dev->qmgr.io_cqs[qid]);
    dev->qmgr.num_io_cqs--;

    mutex_unlock(&dev->lock);
    return HFSSS_OK;
}

int nvme_uspace_create_io_sq(struct nvme_uspace_dev *dev, u16 qid, u16 qsize, u16 cqid, u8 prio)
{
    int ret;

    if (!dev || !dev->initialized || qid == 0 || qid >= MAX_QUEUE_PAIRS || cqid == 0 || cqid >= MAX_QUEUE_PAIRS) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&dev->lock, 0);

    /* Check if SQ already exists */
    if (dev->qmgr.io_sqs[qid].enabled) {
        ret = HFSSS_ERR_EXIST;
        goto out;
    }

    /* Check if CQ exists */
    if (!dev->qmgr.io_cqs[cqid].enabled) {
        ret = HFSSS_ERR_NOENT;
        goto out;
    }

    ret = nvme_sq_create(&dev->qmgr.io_sqs[qid], qid, 0, qsize, 64, cqid);
    if (ret == HFSSS_OK) {
        dev->qmgr.io_sqs[qid].priority = prio;
        dev->qmgr.num_io_sqs++;

        /* Associate SQ with CQ */
        dev->qmgr.io_cqs[cqid].associated_sqs[dev->qmgr.io_cqs[cqid].num_associated_sqs++] = qid;
    }

out:
    mutex_unlock(&dev->lock);
    return ret;
}

int nvme_uspace_delete_io_sq(struct nvme_uspace_dev *dev, u16 qid)
{
    if (!dev || !dev->initialized || qid == 0 || qid >= MAX_QUEUE_PAIRS) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&dev->lock, 0);

    if (!dev->qmgr.io_sqs[qid].enabled) {
        mutex_unlock(&dev->lock);
        return HFSSS_ERR_NOENT;
    }

    u16 cqid = dev->qmgr.io_sqs[qid].cqid;

    nvme_sq_destroy(&dev->qmgr.io_sqs[qid]);
    dev->qmgr.num_io_sqs--;

    /* Remove SQ from CQ's associated list */
    if (dev->qmgr.io_cqs[cqid].enabled) {
        for (u32 i = 0; i < dev->qmgr.io_cqs[cqid].num_associated_sqs; i++) {
            if (dev->qmgr.io_cqs[cqid].associated_sqs[i] == qid) {
                /* Shift remaining entries */
                for (u32 j = i; j < dev->qmgr.io_cqs[cqid].num_associated_sqs - 1; j++) {
                    dev->qmgr.io_cqs[cqid].associated_sqs[j] = dev->qmgr.io_cqs[cqid].associated_sqs[j + 1];
                }
                dev->qmgr.io_cqs[cqid].num_associated_sqs--;
                break;
            }
        }
    }

    mutex_unlock(&dev->lock);
    return HFSSS_OK;
}

int nvme_uspace_read(struct nvme_uspace_dev *dev, u32 nsid, u64 lba, u32 count, void *data)
{
    if (!dev || !dev->initialized || !data) {
        return HFSSS_ERR_INVAL;
    }

    if (nsid != 1) {
        return HFSSS_ERR_INVAL;
    }

    /* Check LBA range */
    if (lba + count > dev->sssim.config.total_lbas) {
        return HFSSS_ERR_INVAL;
    }

    return sssim_read(&dev->sssim, lba, count, data);
}

int nvme_uspace_write(struct nvme_uspace_dev *dev, u32 nsid, u64 lba, u32 count, const void *data)
{
    if (!dev || !dev->initialized || !data) {
        return HFSSS_ERR_INVAL;
    }

    if (nsid != 1) {
        return HFSSS_ERR_INVAL;
    }

    /* Check LBA range */
    if (lba + count > dev->sssim.config.total_lbas) {
        return HFSSS_ERR_INVAL;
    }

    return sssim_write(&dev->sssim, lba, count, data);
}

int nvme_uspace_flush(struct nvme_uspace_dev *dev, u32 nsid)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }

    if (nsid != 1 && nsid != 0) {
        return HFSSS_ERR_INVAL;
    }

    return sssim_flush(&dev->sssim);
}

int nvme_uspace_trim(struct nvme_uspace_dev *dev, u32 nsid, struct nvme_dsm_range *ranges, u32 nr_ranges)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }

    if (nsid != 1) {
        return HFSSS_ERR_INVAL;
    }

    if (!ranges || nr_ranges == 0) {
        return HFSSS_ERR_INVAL;
    }

    for (u32 i = 0; i < nr_ranges; i++) {
        if (ranges[i].nlb == 0) {
            continue;
        }
        int ret = sssim_trim(&dev->sssim, ranges[i].slba, ranges[i].nlb);
        if (ret != HFSSS_OK) {
            return ret;
        }
    }

    return HFSSS_OK;
}

/* Logically erase the entire namespace by trimming all LBAs then flushing. */
int nvme_uspace_format_nvm(struct nvme_uspace_dev *dev, u32 nsid)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }
    if (nsid != 1 && nsid != 0) {
        return HFSSS_ERR_INVAL;
    }

    u64 total_lbas = dev->sssim.config.total_lbas;
    int ret = sssim_trim(&dev->sssim, 0, (u32)total_lbas);
    if (ret != HFSSS_OK) {
        return ret;
    }
    return sssim_flush(&dev->sssim);
}

/* Sanitize: same semantics as format_nvm for simulation purposes. */
int nvme_uspace_sanitize(struct nvme_uspace_dev *dev, u32 sanact)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }
    /* sanact values 1=block-erase, 2=overwrite, 3=crypto-erase are all
     * treated identically: trim all LBAs + flush. */
    (void)sanact;

    u64 total_lbas = dev->sssim.config.total_lbas;
    int ret = sssim_trim(&dev->sssim, 0, (u32)total_lbas);
    if (ret != HFSSS_OK) {
        return ret;
    }
    return sssim_flush(&dev->sssim);
}

/* Stage firmware image bytes into dev->fw_staging_buf at the given offset. */
int nvme_uspace_fw_download(struct nvme_uspace_dev *dev, u32 offset, const void *data, u32 len)
{
    if (!dev || !dev->initialized || !data) {
        return HFSSS_ERR_INVAL;
    }

    u32 needed = offset + len;
    if (dev->fw_staging_buf == NULL || dev->fw_staging_size < needed) {
        void *nb = realloc(dev->fw_staging_buf, needed);
        if (!nb) {
            return HFSSS_ERR_NOMEM;
        }
        dev->fw_staging_buf = nb;
        dev->fw_staging_size = needed;
    }

    memcpy((u8 *)dev->fw_staging_buf + offset, data, len);
    return HFSSS_OK;
}

/* Activate staged firmware: copy first 8 bytes into fw_revision. */
int nvme_uspace_fw_commit(struct nvme_uspace_dev *dev, u32 slot, u32 action)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }
    (void)slot;
    (void)action;

    if (!dev->fw_staging_buf) {
        return HFSSS_ERR_INVAL;
    }

    u32 copy_len = dev->fw_staging_size < 8 ? dev->fw_staging_size : 8;
    memset(dev->fw_revision, 0, sizeof(dev->fw_revision));
    memcpy(dev->fw_revision, dev->fw_staging_buf, copy_len);

    free(dev->fw_staging_buf);
    dev->fw_staging_buf = NULL;
    dev->fw_staging_size = 0;

    return HFSSS_OK;
}

/* Get Log Page: only LID=2 (SMART/Health) is implemented. */
int nvme_uspace_get_log_page(struct nvme_uspace_dev *dev, u32 nsid, u8 lid, void *buf, u32 len)
{
    if (!dev || !dev->initialized || !buf) {
        return HFSSS_ERR_INVAL;
    }
    (void)nsid;

    if (lid != 2) {
        return HFSSS_ERR_NOTSUPP;
    }

    struct ftl_stats stats;
    sssim_get_stats(&dev->sssim, &stats);

    smart_log_t log;
    memset(&log, 0, sizeof(log));

    log.critical_warning = 0;
    log.temperature = 0x015E; /* 350 K */
    log.avail_spare = 100;
    log.avail_spare_thresh = 10;
    log.percent_used = 0;
    log.data_units_read = stats.read_count;
    log.data_units_written = stats.write_count;
    log.host_read_cmds = stats.read_count;
    log.host_write_cmds = stats.write_count;
    log.ctrl_busy_time = 0;
    log.power_cycles = 1;
    log.power_on_hours = 0;
    log.unsafe_shutdowns = 0;
    log.media_errors = 0;
    log.num_err_log_entries = 0;

    u32 copy_len = len < (u32)sizeof(log) ? len : (u32)sizeof(log);
    memset(buf, 0, len);
    memcpy(buf, &log, copy_len);

    return HFSSS_OK;
}

/* Get Features: return simulated or previously-set feature value. */
int nvme_uspace_get_features(struct nvme_uspace_dev *dev, u8 fid, u32 *value)
{
    if (!dev || !dev->initialized || !value) {
        return HFSSS_ERR_INVAL;
    }

    /* If set_features was called, return the stored value. */
    if (dev->features[fid] != 0) {
        *value = dev->features[fid];
        return HFSSS_OK;
    }

    switch (fid) {
    case 0x02: /* Power Management: PS0 */
        *value = 0;
        return HFSSS_OK;
    case 0x04: /* Temperature Threshold: 350 K */
        *value = 0x015E;
        return HFSSS_OK;
    case 0x07: /* Number of Queues: 64 IO queues each direction */
        *value = 0x003F003F;
        return HFSSS_OK;
    default:
        return HFSSS_ERR_NOTSUPP;
    }
}

/* Set Features: persist a feature value; only FIDs 0x02 and 0x04 accepted. */
int nvme_uspace_set_features(struct nvme_uspace_dev *dev, u8 fid, u32 value)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }

    if (fid != 0x02 && fid != 0x04) {
        return HFSSS_ERR_NOTSUPP;
    }

    dev->features[fid] = value;
    return HFSSS_OK;
}

/* -------------------------------------------------------------------------
 * Full-path NVMe I/O command dispatch
 *
 * Routes through nvme_ctrl_process_io_cmd() for opcode validation, then
 * dispatches to the appropriate nvme_uspace_* function for FTL execution.
 * This ensures E2E coverage of the NVMe command processing layer.
 * ---------------------------------------------------------------------- */

int nvme_uspace_dispatch_io_cmd(struct nvme_uspace_dev *dev, struct nvme_sq_entry *cmd, struct nvme_cq_entry *cpl,
                                void *data, u32 data_len)
{
    if (!dev || !dev->initialized || !cmd || !cpl) {
        return HFSSS_ERR_INVAL;
    }

    /* Step 1: Validate opcode through NVMe command processing layer */
    int rc = nvme_ctrl_process_io_cmd(&dev->ctrl, cmd, cpl);
    if (rc != HFSSS_OK) {
        return rc;
    }

    /* If process_io_cmd rejected the opcode, return immediately */
    if (cpl->status != 0) {
        return HFSSS_OK;
    }

    /* Step 2: Extract command parameters and dispatch to uspace */
    u32 nsid = cmd->nsid;
    u64 slba = ((u64)cmd->cdw11 << 32) | cmd->cdw10;
    u32 nlb = (cmd->cdw12 & 0xFFFF) + 1;
    int result = HFSSS_OK;

    switch (cmd->opcode) {
    case NVME_NVM_READ:
        if (!data || data_len < (u64)nlb * dev->sssim.config.lba_size) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_FIELD, NVME_STATUS_TYPE_GENERIC);
            return HFSSS_OK;
        }
        result = nvme_uspace_read(dev, nsid, slba, nlb, data);
        break;

    case NVME_NVM_WRITE:
        if (!data || data_len < (u64)nlb * dev->sssim.config.lba_size) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_FIELD, NVME_STATUS_TYPE_GENERIC);
            return HFSSS_OK;
        }
        result = nvme_uspace_write(dev, nsid, slba, nlb, data);
        break;

    case NVME_NVM_FLUSH:
        result = nvme_uspace_flush(dev, nsid);
        break;

    case NVME_NVM_WRITE_ZEROES: {
        if (!data || data_len < (u64)nlb * dev->sssim.config.lba_size) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_FIELD, NVME_STATUS_TYPE_GENERIC);
            return HFSSS_OK;
        }
        memset(data, 0, (u64)nlb * dev->sssim.config.lba_size);
        result = nvme_uspace_write(dev, nsid, slba, nlb, data);
        break;
    }

    case NVME_NVM_DATASET_MANAGEMENT: {
        /* DSM range list is in the data buffer.
         * CDW10 bits 7:0 = NR (number of ranges - 1)
         * CDW11 bit 2 = AD (Attribute - Deallocate) */
        u32 nr_ranges = (cmd->cdw10 & 0xFF) + 1;
        bool deallocate = (cmd->cdw11 & NVME_DSM_ATTR_DEALLOCATE) != 0;

        if (!deallocate) {
            /* No deallocate requested — nothing to do */
            break;
        }

        if (!data || data_len < nr_ranges * sizeof(struct nvme_dsm_range)) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_FIELD, NVME_STATUS_TYPE_GENERIC);
            return HFSSS_OK;
        }

        result = nvme_uspace_trim(dev, nsid, (struct nvme_dsm_range *)data, nr_ranges);
        break;
    }

    default:
        cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_OPCODE, NVME_STATUS_TYPE_GENERIC);
        return HFSSS_OK;
    }

    if (result != HFSSS_OK) {
        cpl->status = NVME_BUILD_STATUS(NVME_SC_INTERNAL_DEVICE_ERROR, NVME_STATUS_TYPE_GENERIC);
    }

    return HFSSS_OK;
}

/* -------------------------------------------------------------------------
 * Full-path NVMe admin command dispatch
 * ---------------------------------------------------------------------- */

int nvme_uspace_dispatch_admin_cmd(struct nvme_uspace_dev *dev, struct nvme_sq_entry *cmd, struct nvme_cq_entry *cpl,
                                   void *data, u32 data_len)
{
    if (!dev || !dev->initialized || !cmd || !cpl) {
        return HFSSS_ERR_INVAL;
    }

    /* Step 1: Validate opcode through NVMe command processing layer */
    int rc = nvme_ctrl_process_admin_cmd(&dev->ctrl, cmd, cpl);
    if (rc != HFSSS_OK) {
        return rc;
    }

    /* If process_admin_cmd rejected the opcode, return immediately */
    if (cpl->status != 0) {
        return HFSSS_OK;
    }

    /* Step 2: Dispatch to the appropriate admin handler */
    int result = HFSSS_OK;

    switch (cmd->opcode) {
    case NVME_ADMIN_IDENTIFY:
        if (!data || data_len < 4096) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_FIELD, NVME_STATUS_TYPE_GENERIC);
            return HFSSS_OK;
        }
        result = nvme_ctrl_process_identify(cmd, data, data_len);
        if (result == NVME_SC_SUCCESS) {
            /* Enrich with actual device state for controller identify */
            u32 cns = cmd->cdw10 & 0xFF;
            if (cns == 0x01) {
                /* Overlay actual capacity/firmware from uspace device */
                struct nvme_identify_ctrl *id = (struct nvme_identify_ctrl *)data;
                if (dev->fw_revision[0] != 0) {
                    memcpy(id->fr, dev->fw_revision, 8);
                }
                u64 total_bytes = dev->sssim.config.total_lbas * dev->sssim.config.lba_size;
                id->tnvmcap[0] = (u8)(total_bytes & 0xFF);
                id->tnvmcap[1] = (u8)((total_bytes >> 8) & 0xFF);
                id->tnvmcap[2] = (u8)((total_bytes >> 16) & 0xFF);
                id->tnvmcap[3] = (u8)((total_bytes >> 24) & 0xFF);
                id->tnvmcap[4] = (u8)((total_bytes >> 32) & 0xFF);
                id->tnvmcap[5] = (u8)((total_bytes >> 40) & 0xFF);
                id->tnvmcap[6] = (u8)((total_bytes >> 48) & 0xFF);
                id->tnvmcap[7] = (u8)((total_bytes >> 56) & 0xFF);
                memcpy(id->unvmcap, id->tnvmcap, 16);
            } else if (cns == 0x00) {
                /* Overlay actual namespace size from simulator config */
                struct nvme_identify_ns *ns = (struct nvme_identify_ns *)data;
                ns->nsze = dev->sssim.config.total_lbas;
                ns->ncap = dev->sssim.config.total_lbas;
                int lbads = 0;
                u32 sz = dev->sssim.config.lba_size;
                while (sz > 1) {
                    sz >>= 1;
                    lbads++;
                }
                ns->lbaf[0].lbads = lbads;
            }
            result = HFSSS_OK;
        } else {
            cpl->status = NVME_BUILD_STATUS((u16)result, NVME_STATUS_TYPE_GENERIC);
            return HFSSS_OK;
        }
        break;

    case NVME_ADMIN_GET_FEATURES: {
        u8 fid = (u8)(cmd->cdw10 & 0xFF);
        u32 value = 0;
        result = nvme_uspace_get_features(dev, fid, &value);
        if (result == HFSSS_OK) {
            cpl->cdw0 = value;
        } else if (result == HFSSS_ERR_NOTSUPP) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_FIELD, NVME_STATUS_TYPE_GENERIC);
            return HFSSS_OK;
        }
        break;
    }

    case NVME_ADMIN_SET_FEATURES: {
        u8 fid = (u8)(cmd->cdw10 & 0xFF);
        u32 value = cmd->cdw11;
        result = nvme_uspace_set_features(dev, fid, value);
        if (result == HFSSS_ERR_NOTSUPP) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_FIELD, NVME_STATUS_TYPE_GENERIC);
            return HFSSS_OK;
        }
        break;
    }

    case NVME_ADMIN_GET_LOG_PAGE: {
        u8 lid = (u8)(cmd->cdw10 & 0xFF);
        if (!data || data_len == 0) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_FIELD, NVME_STATUS_TYPE_GENERIC);
            return HFSSS_OK;
        }
        result = nvme_uspace_get_log_page(dev, cmd->nsid, lid, data, data_len);
        if (result == HFSSS_ERR_NOTSUPP) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_FIELD, NVME_STATUS_TYPE_GENERIC);
            return HFSSS_OK;
        }
        break;
    }

    case NVME_ADMIN_CREATE_IO_CQ: {
        u16 qid = (u16)(cmd->cdw10 & 0xFFFF);
        u16 qsize = (u16)((cmd->cdw10 >> 16) & 0xFFFF) + 1;
        bool ien = (cmd->cdw11 & 0x02) != 0;
        result = nvme_uspace_create_io_cq(dev, qid, qsize, ien);
        if (result == HFSSS_ERR_EXIST) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_CMD_ID_CONFLICT, NVME_STATUS_TYPE_CMD_SPEC);
            return HFSSS_OK;
        }
        break;
    }

    case NVME_ADMIN_CREATE_IO_SQ: {
        u16 qid = (u16)(cmd->cdw10 & 0xFFFF);
        u16 qsize = (u16)((cmd->cdw10 >> 16) & 0xFFFF) + 1;
        u16 cqid = (u16)(cmd->cdw11 & 0xFFFF);
        u8 prio = (u8)((cmd->cdw11 >> 1) & 0x3);
        result = nvme_uspace_create_io_sq(dev, qid, qsize, cqid, prio);
        if (result == HFSSS_ERR_EXIST) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_CMD_ID_CONFLICT, NVME_STATUS_TYPE_CMD_SPEC);
            return HFSSS_OK;
        } else if (result == HFSSS_ERR_NOENT) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_FIELD, NVME_STATUS_TYPE_GENERIC);
            return HFSSS_OK;
        }
        break;
    }

    case NVME_ADMIN_DELETE_IO_SQ: {
        u16 qid = (u16)(cmd->cdw10 & 0xFFFF);
        result = nvme_uspace_delete_io_sq(dev, qid);
        if (result == HFSSS_ERR_NOENT) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_FIELD, NVME_STATUS_TYPE_GENERIC);
            return HFSSS_OK;
        }
        break;
    }

    case NVME_ADMIN_DELETE_IO_CQ: {
        u16 qid = (u16)(cmd->cdw10 & 0xFFFF);
        result = nvme_uspace_delete_io_cq(dev, qid);
        if (result == HFSSS_ERR_NOENT) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_FIELD, NVME_STATUS_TYPE_GENERIC);
            return HFSSS_OK;
        } else if (result == HFSSS_ERR_BUSY) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_FIELD, NVME_STATUS_TYPE_CMD_SPEC);
            return HFSSS_OK;
        }
        break;
    }

    case NVME_ADMIN_FORMAT_NVM:
        result = nvme_uspace_format_nvm(dev, cmd->nsid);
        break;

    case NVME_ADMIN_SANITIZE: {
        u32 sanact = cmd->cdw10 & 0x07;
        result = nvme_uspace_sanitize(dev, sanact);
        break;
    }

    case NVME_ADMIN_FW_DOWNLOAD: {
        u32 numd = cmd->cdw10;
        u32 ofst = cmd->cdw11;
        u32 len_dw = (numd + 1);
        u32 len_bytes = len_dw * 4;
        if (!data || data_len < len_bytes) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_FIELD, NVME_STATUS_TYPE_GENERIC);
            return HFSSS_OK;
        }
        result = nvme_uspace_fw_download(dev, ofst * 4, data, len_bytes);
        break;
    }

    case NVME_ADMIN_FW_ACTIVATE: {
        u32 slot = cmd->cdw10 & 0x07;
        u32 action = (cmd->cdw10 >> 3) & 0x07;
        result = nvme_uspace_fw_commit(dev, slot, action);
        break;
    }

    case NVME_ADMIN_ASYNC_EVENT:
    case NVME_ADMIN_KEEP_ALIVE:
        /* Accepted but no real action needed */
        break;

    default:
        cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_OPCODE, NVME_STATUS_TYPE_GENERIC);
        return HFSSS_OK;
    }

    if (result != HFSSS_OK && cpl->status == 0) {
        cpl->status = NVME_BUILD_STATUS(NVME_SC_INTERNAL_DEVICE_ERROR, NVME_STATUS_TYPE_GENERIC);
    }

    return HFSSS_OK;
}

/* -------------------------------------------------------------------------
 * Exercise admin command path for E2E coverage
 *
 * Runs a series of admin commands through the full dispatch path to ensure
 * the NVMe command processing layer gets coverage during E2E tests.
 * ---------------------------------------------------------------------- */

int nvme_uspace_exercise_admin_path(struct nvme_uspace_dev *dev)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }

    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cpl;
    u8 *buf = calloc(1, 4096);
    if (!buf) {
        return HFSSS_ERR_NOMEM;
    }
    int rc;

    fprintf(stderr, "[NVMe] Exercising admin command path...\n");

    /* 1. Identify Controller (CNS=0x01) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.nsid = 0;
    cmd.cdw10 = 0x01;
    rc = nvme_uspace_dispatch_admin_cmd(dev, &cmd, &cpl, buf, 4096);
    fprintf(stderr, "[NVMe]   Identify Controller: rc=%d status=0x%04x\n", rc, cpl.status);

    /* 2. Identify Namespace (CNS=0x00, NSID=1) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.nsid = 1;
    cmd.cdw10 = 0x00;
    rc = nvme_uspace_dispatch_admin_cmd(dev, &cmd, &cpl, buf, 4096);
    fprintf(stderr, "[NVMe]   Identify Namespace:  rc=%d status=0x%04x\n", rc, cpl.status);

    /* 3. Active Namespace List (CNS=0x02) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.nsid = 0;
    cmd.cdw10 = 0x02;
    rc = nvme_uspace_dispatch_admin_cmd(dev, &cmd, &cpl, buf, 4096);
    fprintf(stderr, "[NVMe]   Active NS List:      rc=%d status=0x%04x\n", rc, cpl.status);

    /* 4. Get Features - Number of Queues (FID=0x07) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_GET_FEATURES;
    cmd.cdw10 = 0x07;
    rc = nvme_uspace_dispatch_admin_cmd(dev, &cmd, &cpl, NULL, 0);
    fprintf(stderr, "[NVMe]   Get Features NQ:     rc=%d status=0x%04x cdw0=0x%08x\n", rc, cpl.status, cpl.cdw0);

    /* 5. Get Features - Power Management (FID=0x02) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_GET_FEATURES;
    cmd.cdw10 = 0x02;
    rc = nvme_uspace_dispatch_admin_cmd(dev, &cmd, &cpl, NULL, 0);
    fprintf(stderr, "[NVMe]   Get Features PM:     rc=%d status=0x%04x\n", rc, cpl.status);

    /* 6. Set Features - Temperature Threshold (FID=0x04) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_SET_FEATURES;
    cmd.cdw10 = 0x04;
    cmd.cdw11 = 0x0160; /* 352 K */
    rc = nvme_uspace_dispatch_admin_cmd(dev, &cmd, &cpl, NULL, 0);
    fprintf(stderr, "[NVMe]   Set Features Temp:   rc=%d status=0x%04x\n", rc, cpl.status);

    /* 7. Get Log Page - SMART/Health (LID=2) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_GET_LOG_PAGE;
    cmd.nsid = 1;
    cmd.cdw10 = 0x02; /* LID=2 */
    rc = nvme_uspace_dispatch_admin_cmd(dev, &cmd, &cpl, buf, 512);
    fprintf(stderr, "[NVMe]   Get Log SMART:       rc=%d status=0x%04x\n", rc, cpl.status);

    /* 8. Create IO CQ (QID=1, size=32) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_CREATE_IO_CQ;
    cmd.cdw10 = 1 | (31u << 16); /* QID=1, QSIZE=31 (means 32 entries) */
    cmd.cdw11 = 0x02;            /* IEN=1 */
    rc = nvme_uspace_dispatch_admin_cmd(dev, &cmd, &cpl, NULL, 0);
    fprintf(stderr, "[NVMe]   Create IO CQ 1:      rc=%d status=0x%04x\n", rc, cpl.status);

    /* 9. Create IO SQ (QID=1, size=32, CQID=1) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_CREATE_IO_SQ;
    cmd.cdw10 = 1 | (31u << 16); /* QID=1, QSIZE=31 */
    cmd.cdw11 = 1;               /* CQID=1 */
    rc = nvme_uspace_dispatch_admin_cmd(dev, &cmd, &cpl, NULL, 0);
    fprintf(stderr, "[NVMe]   Create IO SQ 1:      rc=%d status=0x%04x\n", rc, cpl.status);

    /* 10. Delete IO SQ (QID=1) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_DELETE_IO_SQ;
    cmd.cdw10 = 1;
    rc = nvme_uspace_dispatch_admin_cmd(dev, &cmd, &cpl, NULL, 0);
    fprintf(stderr, "[NVMe]   Delete IO SQ 1:      rc=%d status=0x%04x\n", rc, cpl.status);

    /* 11. Delete IO CQ (QID=1) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_DELETE_IO_CQ;
    cmd.cdw10 = 1;
    rc = nvme_uspace_dispatch_admin_cmd(dev, &cmd, &cpl, NULL, 0);
    fprintf(stderr, "[NVMe]   Delete IO CQ 1:      rc=%d status=0x%04x\n", rc, cpl.status);

    /* 12. Keep Alive */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_KEEP_ALIVE;
    rc = nvme_uspace_dispatch_admin_cmd(dev, &cmd, &cpl, NULL, 0);
    fprintf(stderr, "[NVMe]   Keep Alive:           rc=%d status=0x%04x\n", rc, cpl.status);

    /* 13. Unsupported opcode (negative test) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_ABORT; /* not implemented */
    rc = nvme_uspace_dispatch_admin_cmd(dev, &cmd, &cpl, NULL, 0);
    fprintf(stderr, "[NVMe]   Abort (unsupported):  rc=%d status=0x%04x\n", rc, cpl.status);

    fprintf(stderr, "[NVMe] Admin command exercise complete.\n");

    free(buf);
    return HFSSS_OK;
}
