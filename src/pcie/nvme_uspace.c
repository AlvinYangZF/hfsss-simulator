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

/* Default smart_monitor callbacks: always report nominal state
 * (cool / 100% life / 100% spare). Keeps the monitor silent when
 * no real data source is attached. */
static u8 default_nominal_thermal(void *ctx) { (void)ctx; return 0; }
static u8 default_nominal_life   (void *ctx) { (void)ctx; return 100; }
static u8 default_nominal_spare  (void *ctx) { (void)ctx; return 100; }

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

    /* Single-exit init: on failure we `goto fail_<last-successful-stage>`,
     * which unwinds every prior step in strict reverse order. Earlier
     * reviewer flagged that newly added state (telemetry/aer) leaked
     * on mid-init errors; this pattern keeps the cleanup set in lockstep
     * with what's been constructed, no matter where we abort. */

    ret = mutex_init(&dev->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    ret = mutex_init(&dev->error_log_lock);
    if (ret != HFSSS_OK) goto fail_lock;

    ret = mutex_init(&dev->keys_lock);
    if (ret != HFSSS_OK) goto fail_error_log_lock;

    ret = mutex_init(&dev->telemetry_lock);
    if (ret != HFSSS_OK) goto fail_keys_lock;

    ret = telemetry_init(&dev->telemetry);
    if (ret != HFSSS_OK) goto fail_telemetry_lock;

    ret = hal_aer_init(&dev->aer);
    if (ret != HFSSS_OK) goto fail_telemetry;

    /* Device-wide Opal key table (REQ-161). key_table_init is
     * infallible so no rollback is needed beyond the later failure
     * legs that already wind this back down. Register the default
     * namespace so SECURITY_SEND/LOCK on nsid=1 (the only namespace
     * Identify Controller advertises here, NN=1) reaches a live
     * entry instead of being rejected as INVALID_FIELD. */
    key_table_init(&dev->keys);
    (void)key_table_register_ns(&dev->keys, 1);

    /* SMART live state defaults (REQ-174/178 consistency). */
    dev->thermal_level    = 0;
    dev->avail_spare_pct  = 100;
    dev->percent_used_pct = 0;

    ret = nvme_ctrl_init(&dev->ctrl);
    if (ret != HFSSS_OK) goto fail_aer;

    ret = nvme_queue_mgr_init(&dev->qmgr, dev);
    if (ret != HFSSS_OK) goto fail_ctrl;

    ret = sssim_init(&dev->sssim, &config->sssim_cfg);
    if (ret != HFSSS_OK) goto fail_qmgr;

    dev->data_buffer_size = config->data_buffer_size;
    dev->data_buffer      = malloc(config->data_buffer_size);
    if (!dev->data_buffer) {
        ret = HFSSS_ERR_NOMEM;
        goto fail_sssim;
    }

    /* REQ-178: embedded SMART monitor with default nominal
     * callbacks. The monitor stays silent (0 thermal / 100% life /
     * 100% spare) until a caller injects a real data source via
     * nvme_uspace_dev_set_smart_source. Kept last so init failures
     * don't leave a half-started monitor behind. */
    struct smart_monitor_config smcfg = {
        .dev                = dev,
        .poll_interval_ms   = 1000,
        .get_thermal        = default_nominal_thermal,
        .get_remaining_life = default_nominal_life,
        .get_spare          = default_nominal_spare,
        .cb_ctx             = NULL,
    };
    ret = smart_monitor_init(&dev->smart_mon, &smcfg);
    if (ret != HFSSS_OK) goto fail_data_buffer;

    dev->initialized = true;
    dev->running     = false;
    return HFSSS_OK;

fail_data_buffer:
    free(dev->data_buffer);
    dev->data_buffer = NULL;
fail_sssim:
    sssim_cleanup(&dev->sssim);
fail_qmgr:
    nvme_queue_mgr_cleanup(&dev->qmgr);
fail_ctrl:
    nvme_ctrl_cleanup(&dev->ctrl);
fail_aer:
    hal_aer_cleanup(&dev->aer);
fail_telemetry:
    telemetry_cleanup(&dev->telemetry);
fail_telemetry_lock:
    mutex_cleanup(&dev->telemetry_lock);
fail_keys_lock:
    mutex_cleanup(&dev->keys_lock);
fail_error_log_lock:
    mutex_cleanup(&dev->error_log_lock);
fail_lock:
    mutex_cleanup(&dev->lock);
    return ret;
}

void nvme_uspace_dev_cleanup(struct nvme_uspace_dev *dev)
{
    if (!dev) {
        return;
    }

    /* Ensure the monitor thread is joined before tearing down any
     * of the state it may still be reading (dev->aer, etc.). */
    smart_monitor_cleanup(&dev->smart_mon);

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
    hal_aer_cleanup(&dev->aer);
    telemetry_cleanup(&dev->telemetry);
    mutex_cleanup(&dev->telemetry_lock);
    mutex_cleanup(&dev->keys_lock);
    mutex_cleanup(&dev->error_log_lock);
    mutex_cleanup(&dev->lock);

    memset(dev, 0, sizeof(*dev));
}

int nvme_uspace_aer_post_event(struct nvme_uspace_dev *dev,
                               enum nvme_async_event_type type,
                               enum nvme_async_event_info info,
                               u8 log_page_id,
                               bool *out_delivered,
                               u16 *out_cid,
                               struct nvme_cq_entry *out_cqe)
{
    if (!dev || !dev->initialized || !out_delivered) {
        return HFSSS_ERR_INVAL;
    }

    struct nvme_aer_completion comp;
    int rc = hal_aer_post_event(&dev->aer, type, info, log_page_id,
                                out_delivered, &comp);
    if (rc != HFSSS_OK) {
        return rc;
    }
    if (*out_delivered) {
        if (out_cid) {
            *out_cid = comp.cid;
        }
        if (out_cqe) {
            *out_cqe = comp.cqe;
        }
    }
    return HFSSS_OK;
}

/* Record a telemetry event under dev->telemetry_lock so the Log Page
 * 07h/08h dispatch (REQ-174/175/176) sees the payload consistently. */
static void aer_record_telemetry(struct nvme_uspace_dev *dev,
                                 enum tel_event_type type, u8 severity,
                                 const void *payload, u32 payload_len)
{
    mutex_lock(&dev->telemetry_lock, 0);
    telemetry_record(&dev->telemetry, type, severity, payload, payload_len);
    mutex_unlock(&dev->telemetry_lock);
}

int nvme_uspace_aer_notify_thermal(struct nvme_uspace_dev *dev,
                                   u8 thermal_level,
                                   bool *out_delivered,
                                   u16 *out_cid,
                                   struct nvme_cq_entry *out_cqe)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }

    /* Map thermal level to telemetry severity. */
    u8 severity = 0;
    if (thermal_level >= 4) {        /* SHUTDOWN */
        severity = 3;
    } else if (thermal_level >= 3) { /* HEAVY */
        severity = 2;
    } else if (thermal_level >= 1) { /* LIGHT / MODERATE */
        severity = 1;
    }
    aer_record_telemetry(dev, TEL_EVENT_THERMAL, severity,
                         &thermal_level, sizeof(thermal_level));

    /* Keep SMART live state in sync (REQ-178 / Fix-3) so a host that
     * reads LID=0x02 after the AER sees the elevated temperature. */
    mutex_lock(&dev->lock, 0);
    dev->thermal_level = thermal_level;
    mutex_unlock(&dev->lock);

    bool local_delivered = false;
    u16  local_cid       = 0;
    struct nvme_cq_entry local_cqe;
    memset(&local_cqe, 0, sizeof(local_cqe));
    int rc = nvme_uspace_aer_post_event(dev,
                                        NVME_AER_TYPE_SMART_HEALTH,
                                        NVME_AEI_SMART_TEMPERATURE_THRESHOLD,
                                        NVME_LID_SMART,
                                        &local_delivered, &local_cid,
                                        &local_cqe);
    if (out_delivered) *out_delivered = local_delivered;
    if (out_cid && local_delivered) *out_cid = local_cid;
    if (out_cqe && local_delivered) *out_cqe = local_cqe;
    return rc;
}

int nvme_uspace_aer_notify_wear(struct nvme_uspace_dev *dev,
                                u8 remaining_life_pct,
                                bool *out_delivered,
                                u16 *out_cid,
                                struct nvme_cq_entry *out_cqe)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }

    /* Severity climbs as life drops. 3=critical (<5%), 2=warn, 1=info. */
    u8 severity = 0;
    if (remaining_life_pct < 5) {
        severity = 3;
    } else if (remaining_life_pct < 10) {
        severity = 2;
    } else if (remaining_life_pct < 25) {
        severity = 1;
    }
    aer_record_telemetry(dev, TEL_EVENT_WEAR, severity,
                         &remaining_life_pct, sizeof(remaining_life_pct));

    /* percent_used = 100 - remaining_life_pct, clamped at 100. */
    u8 used = (remaining_life_pct > 100) ? 0 : (u8)(100 - remaining_life_pct);
    mutex_lock(&dev->lock, 0);
    dev->percent_used_pct = used;
    mutex_unlock(&dev->lock);

    bool local_delivered = false;
    u16  local_cid       = 0;
    struct nvme_cq_entry local_cqe;
    memset(&local_cqe, 0, sizeof(local_cqe));
    int rc = nvme_uspace_aer_post_event(dev,
                                        NVME_AER_TYPE_SMART_HEALTH,
                                        NVME_AEI_SMART_NVM_SUBSYS_RELIABILITY,
                                        NVME_LID_SMART,
                                        &local_delivered, &local_cid,
                                        &local_cqe);
    if (out_delivered) *out_delivered = local_delivered;
    if (out_cid && local_delivered) *out_cid = local_cid;
    if (out_cqe && local_delivered) *out_cqe = local_cqe;
    return rc;
}

int nvme_uspace_aer_notify_spare(struct nvme_uspace_dev *dev,
                                 u8 spare_pct,
                                 bool *out_delivered,
                                 u16 *out_cid,
                                 struct nvme_cq_entry *out_cqe)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }

    u8 severity = (spare_pct < 10) ? 2 : 1;
    aer_record_telemetry(dev, TEL_EVENT_SPARE, severity,
                         &spare_pct, sizeof(spare_pct));

    u8 clamped = (spare_pct > 100) ? 100 : spare_pct;
    mutex_lock(&dev->lock, 0);
    dev->avail_spare_pct = clamped;
    mutex_unlock(&dev->lock);

    bool local_delivered = false;
    u16  local_cid       = 0;
    struct nvme_cq_entry local_cqe;
    memset(&local_cqe, 0, sizeof(local_cqe));
    int rc = nvme_uspace_aer_post_event(dev,
                                        NVME_AER_TYPE_SMART_HEALTH,
                                        NVME_AEI_SMART_SPARE_BELOW_THRESHOLD,
                                        NVME_LID_SMART,
                                        &local_delivered, &local_cid,
                                        &local_cqe);
    if (out_delivered) *out_delivered = local_delivered;
    if (out_cid && local_delivered) *out_cid = local_cid;
    if (out_cqe && local_delivered) *out_cqe = local_cqe;
    return rc;
}

int nvme_uspace_dev_start(struct nvme_uspace_dev *dev)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&dev->lock, 0);
    dev->running = true;
    mutex_unlock(&dev->lock);

    /* REQ-178: spin up the smart_monitor background thread. With
     * default nominal callbacks this is a no-op producer; real
     * callers plug in live sources afterwards via
     * nvme_uspace_dev_set_smart_source. */
    int sm_rc = smart_monitor_start(&dev->smart_mon);
    if (sm_rc != HFSSS_OK) {
        mutex_lock(&dev->lock, 0);
        dev->running = false;
        mutex_unlock(&dev->lock);
        return sm_rc;
    }
    return HFSSS_OK;
}

void nvme_uspace_dev_stop(struct nvme_uspace_dev *dev)
{
    if (!dev || !dev->initialized) {
        return;
    }

    /* Stop the monitor before marking the dev un-running so any
     * in-flight poll cycle completes against still-valid state. */
    smart_monitor_stop(&dev->smart_mon);

    mutex_lock(&dev->lock, 0);
    dev->running = false;
    mutex_unlock(&dev->lock);
}

int nvme_uspace_dev_set_smart_source(struct nvme_uspace_dev *dev,
                                     smart_thermal_level_fn  get_thermal,
                                     smart_remaining_life_fn get_remaining_life,
                                     smart_spare_fn          get_spare,
                                     void                   *cb_ctx)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }
    /* Any NULL leaves the current callback in place so callers
     * can override just one axis. The monitor config is written
     * outside the lock because callbacks are plain fn-pointers
     * — the running thread reads them atomically on each poll. */
    struct smart_monitor_config *c = &dev->smart_mon.cfg;
    if (get_thermal)        c->get_thermal        = get_thermal;
    if (get_remaining_life) c->get_remaining_life = get_remaining_life;
    if (get_spare)          c->get_spare          = get_spare;
    c->cb_ctx = cb_ctx;
    return HFSSS_OK;
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

    /* REQ-161: refuse I/O while the namespace is Opal-locked. The
     * dispatcher maps HFSSS_ERR_AUTH to NVMe SC=0x16 (OP_DENIED).
     * keys_lock guards against concurrent SECURITY_SEND dispatch
     * racing a host read. */
    mutex_lock(&dev->keys_lock, 0);
    bool locked = opal_is_locked(&dev->keys, nsid);
    mutex_unlock(&dev->keys_lock);
    if (locked) {
        return HFSSS_ERR_AUTH;
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

    /* REQ-161: refuse I/O while the namespace is Opal-locked. */
    mutex_lock(&dev->keys_lock, 0);
    bool locked = opal_is_locked(&dev->keys, nsid);
    mutex_unlock(&dev->keys_lock);
    if (locked) {
        return HFSSS_ERR_AUTH;
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

    /* Snapshot live thermal / spare / wear state under dev->lock so
     * SMART (REQ-174) matches what the most recent AER notifier told
     * the host to look for. Review of PR #91 flagged the previous
     * hard-coded payload as inconsistent with the AER story. */
    mutex_lock(&dev->lock, 0);
    u8 live_thermal  = dev->thermal_level;
    u8 live_spare    = dev->avail_spare_pct;
    u8 live_used_pct = dev->percent_used_pct;
    mutex_unlock(&dev->lock);

    /* Map thermal level to Kelvin. Level 0=nominal 27°C/300K;
     * each further level roughly +5°C up to SHUTDOWN=363K. */
    static const uint16_t thermal_to_kelvin[5] = {
        300,  /* NONE     */
        348,  /* LIGHT    (>=75C) */
        353,  /* MODERATE (>=80C) */
        358,  /* HEAVY    (>=85C) */
        363,  /* SHUTDOWN (>=90C) */
    };
    uint16_t temp_k = (live_thermal < 5) ? thermal_to_kelvin[live_thermal]
                                         : thermal_to_kelvin[4];

    /* Build critical_warning per NVMe §5.14.1.2: bit 0=spare below
     * threshold, bit 1=temperature above threshold, bit 2=NVM
     * subsystem reliability degraded (tied to percent_used>=90). */
    uint8_t crit = 0;
    if (live_spare < 10)   crit |= (1u << 0);
    if (live_thermal >= 1) crit |= (1u << 1);
    if (live_used_pct >= 90) crit |= (1u << 2);

    log.critical_warning = crit;
    log.temperature = temp_k;
    log.avail_spare = live_spare;
    log.avail_spare_thresh = 10;
    log.percent_used = live_used_pct;
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
        /* REQ-161: a locked namespace surfaces as SC=0x16 (OP_DENIED)
         * rather than the generic internal-device-error. All other
         * backend failures still map to INTERNAL_DEVICE_ERROR. */
        u8 sc = (result == HFSSS_ERR_AUTH) ? NVME_SC_OP_DENIED
                                           : NVME_SC_INTERNAL_DEVICE_ERROR;
        u16 status_field = NVME_BUILD_STATUS(sc, NVME_STATUS_TYPE_GENERIC);
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

    case NVME_ADMIN_ASYNC_EVENT: {
        /* Route through the AER framework (REQ-063). If the host's
         * outstanding queue still has room, the CID is parked there
         * and no completion is produced now — real NVMe would leave
         * the CQE pending. We signal that to the caller by leaving
         * cpl->cdw0 zero and returning a success status; the AER
         * framework will stamp the real DW0 when a matching event
         * arrives via nvme_uspace_aer_post_event(). If an event was
         * already waiting in the pending ring, the submit completes
         * immediately — fill cpl with the returned CQE now. */
        bool immediate = false;
        struct nvme_aer_completion comp;
        int aer_rc = hal_aer_submit_request(&dev->aer, cmd->command_id,
                                            &immediate, &comp);
        if (aer_rc == HFSSS_ERR_NOSPC) {
            /* NVMe spec §5.2: Asynchronous Event Request Limit
             * Exceeded = SCT=Command Specific, SC=0x05. */
            cpl->status = NVME_BUILD_STATUS(0x05,
                                            NVME_STATUS_TYPE_CMD_SPEC);
        } else if (immediate) {
            cpl->cdw0   = comp.cqe.cdw0;
            cpl->status = comp.cqe.status;
        }
        break;
    }

    case NVME_ADMIN_SECURITY_SEND: {
        /* REQ-161: Opal SSC lock / unlock over NVMe Security Send.
         * Data buffer is an OPAL_CMD_FRAME_LEN-byte frame (opcode,
         * nsid, auth). Lock returns SC=OP_DENIED if the NS isn't
         * currently ACTIVE; unlock with a wrong auth returns the
         * same. Everything else maps to INVALID_FIELD. */
        if (!data || data_len < OPAL_CMD_FRAME_LEN) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_FIELD,
                                            NVME_STATUS_TYPE_GENERIC);
            break;
        }
        const u8 *p = (const u8 *)data;
        u8  opal_op = p[0];
        u32 opal_ns = ((u32)p[1])       | ((u32)p[2] << 8)
                    | ((u32)p[3] << 16) | ((u32)p[4] << 24);
        const u8 *auth = &p[5];

        int opal_rc = HFSSS_OK;
        /* Serialize LOCK/UNLOCK against concurrent I/O-path
         * opal_is_locked() probes so a host can't observe a torn
         * key_state between the two callers. */
        mutex_lock(&dev->keys_lock, 0);
        switch (opal_op) {
        case OPAL_CMD_LOCK:
            opal_rc = opal_lock_ns(&dev->keys, opal_ns);
            break;
        case OPAL_CMD_UNLOCK:
            opal_rc = opal_unlock_ns(&dev->keys, dev->opal_mk,
                                     opal_ns, auth);
            break;
        default:
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_FIELD,
                                            NVME_STATUS_TYPE_GENERIC);
            break;
        }
        mutex_unlock(&dev->keys_lock);
        if (cpl->status != 0) {
            break;  /* already marked INVALID_FIELD above */
        }
        if (opal_rc == HFSSS_ERR_AUTH) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_OP_DENIED,
                                            NVME_STATUS_TYPE_GENERIC);
        } else if (opal_rc == HFSSS_ERR_NOENT ||
                   opal_rc == HFSSS_ERR_INVAL) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_FIELD,
                                            NVME_STATUS_TYPE_GENERIC);
        } else if (opal_rc != HFSSS_OK) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INTERNAL_DEVICE_ERROR,
                                            NVME_STATUS_TYPE_GENERIC);
        }
        break;
    }

    case NVME_ADMIN_SECURITY_RECV: {
        /* REQ-161: Security Receive. Currently supports
         * OPAL_CMD_STATUS which writes 1/0 at data[5] indicating
         * whether the namespace is locked. */
        if (!data || data_len < 6) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_FIELD,
                                            NVME_STATUS_TYPE_GENERIC);
            break;
        }
        u8  *pm      = (u8 *)data;
        u8   opal_op = pm[0];
        u32  opal_ns = ((u32)pm[1])       | ((u32)pm[2] << 8)
                     | ((u32)pm[3] << 16) | ((u32)pm[4] << 24);
        if (opal_op != OPAL_CMD_STATUS) {
            cpl->status = NVME_BUILD_STATUS(NVME_SC_INVALID_FIELD,
                                            NVME_STATUS_TYPE_GENERIC);
            break;
        }
        mutex_lock(&dev->keys_lock, 0);
        pm[5] = opal_is_locked(&dev->keys, opal_ns) ? 1 : 0;
        mutex_unlock(&dev->keys_lock);
        break;
    }

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
