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

    /* Initialize Error Log ring lock (REQ-115/158) */
    ret = mutex_init(&dev->error_log_lock);
    if (ret != HFSSS_OK) {
        mutex_cleanup(&dev->lock);
        return ret;
    }

    /* Initialize Telemetry ring + lock (REQ-174/175/176) */
    ret = mutex_init(&dev->telemetry_lock);
    if (ret != HFSSS_OK) {
        mutex_cleanup(&dev->error_log_lock);
        mutex_cleanup(&dev->lock);
        return ret;
    }
    ret = telemetry_init(&dev->telemetry);
    if (ret != HFSSS_OK) {
        mutex_cleanup(&dev->telemetry_lock);
        mutex_cleanup(&dev->error_log_lock);
        mutex_cleanup(&dev->lock);
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
    telemetry_cleanup(&dev->telemetry);
    mutex_cleanup(&dev->telemetry_lock);
    mutex_cleanup(&dev->error_log_lock);
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

/* Sanitize: dispatch on Sanitize Action (SANACT) per NVMe spec §5.22.
 *  - EXIT_FAILURE   (0x01): no-op (no simulated failure-mode to clear)
 *  - BLOCK_ERASE    (0x02): trim every LBA then flush
 *  - OVERWRITE      (0x03): fill-zero pass across all LBAs then flush
 *  - CRYPTO_ERASE   (0x04): destroy user data by dropping the mapping
 *                           (simulated crypto erase), then flush
 *  - anything else : HFSSS_ERR_INVAL -- caller translates to INVALID_FIELD
 */
int nvme_uspace_sanitize(struct nvme_uspace_dev *dev, u32 sanact)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }

    switch (sanact) {
    case NVME_SANACT_EXIT_FAILURE:
        return HFSSS_OK;
    case NVME_SANACT_BLOCK_ERASE:
    case NVME_SANACT_CRYPTO_ERASE: {
        u64 total_lbas = dev->sssim.config.total_lbas;
        int ret = sssim_trim(&dev->sssim, 0, (u32)total_lbas);
        if (ret != HFSSS_OK) {
            return ret;
        }
        return sssim_flush(&dev->sssim);
    }
    case NVME_SANACT_OVERWRITE: {
        /* Overwrite every LBA with zeros. Simulator collapses this to a
         * trim plus a single zero-fill pass so the host observes all-
         * zero reads after completion. */
        u64 total_lbas = dev->sssim.config.total_lbas;
        u32 lba_size = dev->sssim.config.lba_size;
        int ret = sssim_trim(&dev->sssim, 0, (u32)total_lbas);
        if (ret != HFSSS_OK) {
            return ret;
        }
        u8 *zbuf = calloc(1, lba_size);
        if (!zbuf) {
            return HFSSS_ERR_NOMEM;
        }
        for (u64 lba = 0; lba < total_lbas; lba++) {
            ret = sssim_write(&dev->sssim, lba, 1, zbuf);
            if (ret != HFSSS_OK) {
                free(zbuf);
                return ret;
            }
        }
        free(zbuf);
        return sssim_flush(&dev->sssim);
    }
    default:
        return HFSSS_ERR_INVAL;
    }
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

/* Append an entry to the NVMe Error Information Log ring (REQ-115/158). */
void nvme_uspace_report_error(struct nvme_uspace_dev *dev,
                              u16 sq_id, u16 cmd_id, u16 status_field,
                              u64 lba, u32 nsid)
{
    if (!dev) {
        return;
    }
    mutex_lock(&dev->error_log_lock, 0);
    u32 slot = dev->error_log_head % NVME_ERROR_LOG_ENTRIES;
    struct nvme_error_log_entry *e = &dev->error_log[slot];
    memset(e, 0, sizeof(*e));
    dev->error_log_count++;
    e->error_count   = dev->error_log_count;
    e->sq_id         = sq_id;
    e->cmd_id        = cmd_id;
    e->status_field  = status_field;
    e->lba           = lba;
    e->nsid          = nsid;
    dev->error_log_head = (dev->error_log_head + 1) % NVME_ERROR_LOG_ENTRIES;
    mutex_unlock(&dev->error_log_lock);
}

/* Copy up to num_entries Error Log Page entries into caller-supplied buffer.
 * Entries are copied newest-first per NVMe spec §5.14.1.1. Returns the
 * number of entries actually written. */
static u32 error_log_copy_recent(struct nvme_uspace_dev *dev,
                                  struct nvme_error_log_entry *out,
                                  u32 num_entries)
{
    mutex_lock(&dev->error_log_lock, 0);
    u32 available = dev->error_log_count < NVME_ERROR_LOG_ENTRIES
        ? (u32)dev->error_log_count
        : NVME_ERROR_LOG_ENTRIES;
    u32 to_copy = available < num_entries ? available : num_entries;
    /* Newest-first: walk backwards from head-1. */
    for (u32 i = 0; i < to_copy; i++) {
        u32 idx = (dev->error_log_head + NVME_ERROR_LOG_ENTRIES - 1 - i)
                  % NVME_ERROR_LOG_ENTRIES;
        out[i] = dev->error_log[idx];
    }
    mutex_unlock(&dev->error_log_lock);
    return to_copy;
}

/* Serialize the telemetry ring into a caller-supplied buffer using the
 * NVMe Telemetry Log Page layout: a 512-byte header followed by Data
 * Area 1 carrying struct tel_event records in most-recent-first order.
 * `host_initiated` selects LID 0x07 (host) vs LID 0x08 (controller)
 * semantics for the generation-number and data-available fields. */
static int telemetry_fill_log_page(struct nvme_uspace_dev *dev,
                                   bool host_initiated,
                                   void *buf, u32 len)
{
    const u32 header_size = (u32)sizeof(struct nvme_telemetry_log_header);
    if (len < header_size) {
        return HFSSS_ERR_INVAL;
    }

    memset(buf, 0, len);
    struct nvme_telemetry_log_header *hdr =
        (struct nvme_telemetry_log_header *)buf;
    hdr->log_identifier = host_initiated
                          ? NVME_LID_TELEMETRY_HOST
                          : NVME_LID_TELEMETRY_CTRL;
    hdr->ieee_oui[0] = 0x48; /* 'H' */
    hdr->ieee_oui[1] = 0x46; /* 'F' */
    hdr->ieee_oui[2] = 0x53; /* 'S' */

    mutex_lock(&dev->telemetry_lock, 0);

    /* Generation-number policy differs between the two pages:
     *  - Host-initiated (0x07): every Get Log Page call starts a new
     *    snapshot, so the generation number advances unconditionally.
     *  - Controller-initiated (0x08): the controller only increments
     *    when genuinely new events have been appended since the last
     *    read, so back-to-back polls don't appear to churn. */
    if (host_initiated) {
        dev->telemetry_host_gen++;
        hdr->host_gen_number = dev->telemetry_host_gen;
    } else {
        u64 total_now = dev->telemetry.total_events;
        if (total_now > dev->last_ctrl_gen_total_events) {
            dev->telemetry_ctrl_gen++;
            dev->last_ctrl_gen_total_events = total_now;
        }
        hdr->ctrl_gen_number = dev->telemetry_ctrl_gen;
        hdr->ctrl_data_available = (dev->telemetry.count > 0) ? 1 : 0;
    }

    /* Fill Data Area 1 with the most recent events that fit. */
    u8 *area1 = (u8 *)buf + header_size;
    u32 area1_cap = len - header_size;
    u32 max_events = area1_cap / (u32)sizeof(struct tel_event);
    u32 actual = 0;
    if (max_events > 0) {
        telemetry_get_recent(&dev->telemetry,
                             (struct tel_event *)area1, max_events,
                             &actual);
    }

    /* data_area_1_last_block is in 512-byte units from the log start. */
    if (actual > 0) {
        u32 area1_bytes = actual * (u32)sizeof(struct tel_event);
        u32 total_bytes = header_size + area1_bytes;
        hdr->data_area_1_last_block = (u16)((total_bytes - 1) / 512);
    } else {
        hdr->data_area_1_last_block = 0;
    }

    mutex_unlock(&dev->telemetry_lock);
    return HFSSS_OK;
}

/* Populate a vendor-specific counter snapshot from the telemetry ring.
 * Used by hfsss-ctrl and the test harness to read device state without
 * parsing the per-event Data Area of the standard Telemetry Log pages. */
static int telemetry_fill_vendor_counters(struct nvme_uspace_dev *dev,
                                          void *buf, u32 len)
{
    if (len < (u32)sizeof(struct nvme_vendor_log_counters)) {
        return HFSSS_ERR_INVAL;
    }

    struct nvme_vendor_log_counters snap;
    memset(&snap, 0, sizeof(snap));
    snap.magic   = NVME_VENDOR_LOG_MAGIC;
    snap.version = 1;

    mutex_lock(&dev->telemetry_lock, 0);
    snap.total_events   = dev->telemetry.total_events;
    snap.events_in_ring = dev->telemetry.count;
    snap.ring_head      = dev->telemetry.head;

    /* Walk the live ring to produce per-type counts. Must go over
     * `count` entries starting at the oldest slot, not the whole
     * TEL_MAX_EVENTS array (stale slots could contain bogus type
     * values from previous wraps — though telemetry_record zeroes
     * slots before use, defending in depth is cheap here). */
    u32 count = dev->telemetry.count;
    u32 start = (count < TEL_MAX_EVENTS)
                ? 0
                : dev->telemetry.head; /* full ring → oldest at head */
    for (u32 i = 0; i < count; i++) {
        u32 idx = (start + i) % TEL_MAX_EVENTS;
        u32 t = dev->telemetry.events[idx].type;
        if (t < NVME_VENDOR_LOG_EVENT_TYPES) {
            snap.events_by_type[t]++;
        }
    }
    mutex_unlock(&dev->telemetry_lock);

    memcpy(buf, &snap, sizeof(snap));
    return HFSSS_OK;
}

/* Get Log Page: LID=0x01 (Error Info), 0x02 (SMART/Health),
 * 0x07 (Host-Initiated Telemetry, REQ-174),
 * 0x08 (Controller-Initiated Telemetry, REQ-175),
 * 0xC0 (Vendor-Specific Counters, REQ-176). */
int nvme_uspace_get_log_page(struct nvme_uspace_dev *dev, u32 nsid, u8 lid, void *buf, u32 len)
{
    if (!dev || !dev->initialized || !buf) {
        return HFSSS_ERR_INVAL;
    }
    (void)nsid;

    if (lid == NVME_LID_ERROR_INFO) {
        u32 max_entries = len / (u32)sizeof(struct nvme_error_log_entry);
        if (max_entries == 0) {
            return HFSSS_ERR_INVAL;
        }
        memset(buf, 0, len);
        error_log_copy_recent(dev,
                              (struct nvme_error_log_entry *)buf,
                              max_entries);
        return HFSSS_OK;
    }

    if (lid == NVME_LID_TELEMETRY_HOST) {
        return telemetry_fill_log_page(dev, true, buf, len);
    }

    if (lid == NVME_LID_TELEMETRY_CTRL) {
        return telemetry_fill_log_page(dev, false, buf, len);
    }

    if (lid == NVME_LID_VENDOR_COUNTERS) {
        return telemetry_fill_vendor_counters(dev, buf, len);
    }

    if (lid != NVME_LID_SMART) {
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
    /* Keep SMART num_err_log_entries in sync with the Error Information
     * Log ring counter so the two host-visible log pages agree. */
    mutex_lock(&dev->error_log_lock, 0);
    log.num_err_log_entries = dev->error_log_count;
    mutex_unlock(&dev->error_log_lock);

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
        u64 zero_len = (u64)nlb * dev->sssim.config.lba_size;
        void *zero_buf = calloc(1, zero_len);
        if (!zero_buf) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INTERNAL_DEVICE_ERROR, NVME_STATUS_TYPE_GENERIC);
            return HFSSS_OK;
        }
        result = nvme_uspace_write(dev, nsid, slba, nlb, zero_buf);
        free(zero_buf);
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
        u16 status_field = NVME_BUILD_STATUS(NVME_SC_INTERNAL_DEVICE_ERROR,
                                             NVME_STATUS_TYPE_GENERIC);
        cpl->status = status_field;
        /* Append the failure to the Error Information Log ring so host
         * Get Log Page (LID=0x01) surfaces it (REQ-115 / REQ-158). Only
         * LBA-bearing I/O opcodes carry a meaningful slba/nsid. */
        if (cmd->opcode == NVME_NVM_READ ||
            cmd->opcode == NVME_NVM_WRITE ||
            cmd->opcode == NVME_NVM_WRITE_ZEROES ||
            cmd->opcode == NVME_NVM_DATASET_MANAGEMENT) {
            nvme_uspace_report_error(dev, 0 /* sq_id unknown at this layer */,
                                     cmd->command_id, status_field,
                                     slba, nsid);
        } else {
            nvme_uspace_report_error(dev, 0, cmd->command_id, status_field,
                                     0, nsid);
        }
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

    /* 6. Set Features - Temperature Threshold (FID=0x04)
     *    Read current value first, then write it back unchanged so the
     *    dispatch path is exercised without mutating device state. */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_GET_FEATURES;
    cmd.cdw10 = 0x04;
    rc = nvme_uspace_dispatch_admin_cmd(dev, &cmd, &cpl, NULL, 0);
    u32 saved_temp_thresh = cpl.cdw0;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_SET_FEATURES;
    cmd.cdw10 = 0x04;
    cmd.cdw11 = saved_temp_thresh;
    rc = nvme_uspace_dispatch_admin_cmd(dev, &cmd, &cpl, NULL, 0);
    fprintf(stderr, "[NVMe]   Set Features Temp:   rc=%d status=0x%04x (restored 0x%04x)\n", rc, cpl.status,
            saved_temp_thresh);

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
