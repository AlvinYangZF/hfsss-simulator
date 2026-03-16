#include "pcie/nvme_uspace.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* SMART/Health log page (LID=2) layout used by get_log_page */
typedef struct {
    uint8_t  critical_warning;
    uint16_t temperature;
    uint8_t  avail_spare;
    uint8_t  avail_spare_thresh;
    uint8_t  percent_used;
    uint8_t  rsvd[26];
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

int nvme_uspace_trim(struct nvme_uspace_dev *dev, u32 nsid,
                     struct nvme_dsm_range *ranges, u32 nr_ranges)
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
int nvme_uspace_fw_download(struct nvme_uspace_dev *dev,
                             u32 offset, const void *data, u32 len)
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
int nvme_uspace_get_log_page(struct nvme_uspace_dev *dev, u32 nsid,
                              u8 lid, void *buf, u32 len)
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

    log.critical_warning   = 0;
    log.temperature        = 0x015E;  /* 350 K */
    log.avail_spare        = 100;
    log.avail_spare_thresh = 10;
    log.percent_used       = 0;
    log.data_units_read    = stats.read_count;
    log.data_units_written = stats.write_count;
    log.host_read_cmds     = stats.read_count;
    log.host_write_cmds    = stats.write_count;
    log.ctrl_busy_time     = 0;
    log.power_cycles       = 1;
    log.power_on_hours     = 0;
    log.unsafe_shutdowns   = 0;
    log.media_errors       = 0;
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
    case 0x02:  /* Power Management: PS0 */
        *value = 0;
        return HFSSS_OK;
    case 0x04:  /* Temperature Threshold: 350 K */
        *value = 0x015E;
        return HFSSS_OK;
    case 0x07:  /* Number of Queues: 64 IO queues each direction */
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
