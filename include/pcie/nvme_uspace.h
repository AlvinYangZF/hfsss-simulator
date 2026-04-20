#ifndef __HFSSS_NVME_USPACE_H
#define __HFSSS_NVME_USPACE_H

#include "pcie/nvme.h"
#include "pcie/queue.h"
#include "pcie/smart_monitor.h"
#include "sssim.h"
#include "common/telemetry.h"
#include "controller/security.h"
#include "controller/qos.h"
#include "hal/hal_aer.h"

/* REQ-150: callback fired when the latency monitor observes
 * `trigger_count` consecutive P99 SLA violations on a namespace.
 * Receives the nsid, the current P99 reading (microseconds), and
 * the caller ctx. Declared here so struct nvme_uspace_dev below
 * can embed a per-ns slot without a forward-decl dance. */
typedef void (*sla_rollback_fn)(u32 nsid, u64 p99_us, void *ctx);

struct nvme_sla_slot {
    sla_rollback_fn cb;
    void           *cb_ctx;
    u32             trigger_count;   /* 0 disables the rollback */
    u32             fire_count;      /* total times cb has fired */
};

/* Simulator Opal command frame layout used on Security Send / Recv
 * data buffers (REQ-161). 37 bytes total:
 *   byte 0:   opcode    (OPAL_CMD_*)
 *   bytes 1-4: nsid     (little-endian)
 *   bytes 5-36: auth    (SEC_KEY_LEN = 32 bytes; unused by LOCK/STATUS)
 * Security Recv writes the status byte at offset 5 when opcode=STATUS. */
#define OPAL_CMD_LOCK      0x01
#define OPAL_CMD_UNLOCK    0x02
#define OPAL_CMD_STATUS    0x03
#define OPAL_CMD_FRAME_LEN 37

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

    /* Firmware staging buffer (fw_download/fw_commit) */
    void *fw_staging_buf;
    u32 fw_staging_size;
    char fw_revision[8];

    /* Per-FID feature storage (set_features/get_features) */
    u32 features[256];

    /* NVMe Error Information Log Page ring (REQ-115 / REQ-158).
     * `head` is the next write position; `count` is the total entries
     * ever appended (exposed as SMART num_err_log_entries). Entries
     * older than NVME_ERROR_LOG_ENTRIES are overwritten. */
    struct nvme_error_log_entry error_log[NVME_ERROR_LOG_ENTRIES];
    u32  error_log_head;
    u64  error_log_count;
    struct mutex error_log_lock;

    /* NVMe Telemetry Log Page backing ring (REQ-174 / REQ-175 / REQ-176).
     * `telemetry` holds all recorded events. The generation numbers are
     * stamped into the Telemetry Log header so the host can detect new
     * data across polls. `last_ctrl_gen_total_events` tracks the
     * telemetry count at the most recent controller-initiated (LID 0x08)
     * read so the controller-initiated generation number only advances
     * when genuinely new events have been appended. */
    struct telemetry_log telemetry;
    struct mutex         telemetry_lock;
    u8                   telemetry_host_gen;
    u8                   telemetry_ctrl_gen;
    u64                  last_ctrl_gen_total_events;

    /* NVMe AER framework (REQ-063). Holds outstanding Async Event
     * Request CIDs and pending events produced by subsystems like
     * thermal / wear / spare. */
    struct hal_aer_ctx   aer;

    /* Device-wide key table (REQ-161). Security Send / Recv dispatch
     * mutates this via opal_lock_ns / opal_unlock_ns, and the I/O
     * path consults opal_is_locked before accepting read / write.
     * The all-zero `opal_mk` is the device-side master key used by
     * opal_unlock_ns when verifying a host-supplied auth token; the
     * simulator keeps it deterministic so tests can derive a matching
     * auth with `opal_derive_auth(opal_mk, nsid, ...)`. */
    struct key_table     keys;
    u8                   opal_mk[SEC_KEY_LEN];
    /* Guards every runtime access to `keys`. The I/O path
     * (opal_is_locked in nvme_uspace_read/write), the admin path
     * (opal_lock_ns/unlock_ns in SECURITY_SEND dispatch), and the
     * SECURITY_RECV STATUS probe all now acquire this mutex; init-
     * time seeding (key_table_init / key_table_register_ns) runs
     * before any other thread can reach the dev, so those paths
     * intentionally stay unlocked. Leaf lock — never held across
     * another lock acquisition, so ordering against dev->lock is
     * unconstrained. */
    struct mutex         keys_lock;

    /* SMART live state (REQ-174/REQ-178 consistency). The AER notifier
     * bridges update these fields so Get Log Page LID=0x02 reflects the
     * same state a preceding async event told the host to inspect.
     * Defaults: level=0 (nominal), spare=100%, used=0%. */
    u8                   thermal_level;       /* 0..4 per common/thermal.h */
    u8                   avail_spare_pct;     /* 0..100 */
    u8                   percent_used_pct;    /* 0..100 */

    /* REQ-178: runtime producer that polls caller-supplied sources
     * and fires the aer_notify_* bridges on threshold crossings.
     * Default callbacks return the nominal device state (0 / 100 /
     * 100) so the monitor is silent until a real data source is
     * attached via nvme_uspace_dev_set_smart_source(). */
    struct smart_monitor smart_mon;

    /* REQ-148 / REQ-149 / REQ-151: per-namespace QoS. Indexed by
     * nsid-1 (Identify advertises NN=1 so only slot 0 is used today,
     * but the table size matches the spec's QOS_MAX_NAMESPACES so
     * future multi-NS work has room). `qos_by_ns[i].initialized`
     * gates the acquire-tokens check on the I/O path; an
     * unenforced policy passes through cleanly. */
    struct ns_qos_ctx         qos_by_ns[QOS_MAX_NAMESPACES];

    /* REQ-150: latency monitor used for P99 SLA tracking +
     * rollback callback registered by the caller. */
    struct ns_latency_monitor lat_by_ns[QOS_MAX_NAMESPACES];
    struct nvme_sla_slot      sla_by_ns[QOS_MAX_NAMESPACES];
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
int nvme_uspace_trim(struct nvme_uspace_dev *dev, u32 nsid, struct nvme_dsm_range *ranges, u32 nr_ranges);

/* Default Config Helper */
void nvme_uspace_config_default(struct nvme_uspace_config *config);

/* Admin Commands */
int nvme_uspace_format_nvm(struct nvme_uspace_dev *dev, u32 nsid);
int nvme_uspace_sanitize(struct nvme_uspace_dev *dev, u32 sanact);
int nvme_uspace_fw_download(struct nvme_uspace_dev *dev, u32 offset, const void *data, u32 len);
int nvme_uspace_fw_commit(struct nvme_uspace_dev *dev, u32 slot, u32 action);
int nvme_uspace_get_log_page(struct nvme_uspace_dev *dev, u32 nsid, u8 lid, void *buf, u32 len);

/* Append one entry to the NVMe Error Information Log ring (REQ-115/158).
 * Call sites: read retry exhausted → UCE, write verify failure,
 * internal device error, etc. Fills error_count from the device
 * monotonic counter, stamps the caller-supplied fields. */
void nvme_uspace_report_error(struct nvme_uspace_dev *dev,
                              u16 sq_id, u16 cmd_id, u16 status_field,
                              u64 lba, u32 nsid);

/* Post an async event into the device's AER framework (REQ-063).
 * If the host had an outstanding AER, it consumes the event and
 * `*out_delivered` becomes true; `*out_cid` / `*out_cqe` receive the
 * completion to post on the admin CQ. Otherwise the event is buffered
 * in the pending ring. */
int nvme_uspace_aer_post_event(struct nvme_uspace_dev *dev,
                               enum nvme_async_event_type type,
                               enum nvme_async_event_info info,
                               u8 log_page_id,
                               bool *out_delivered,
                               u16 *out_cid,
                               struct nvme_cq_entry *out_cqe);

/* Controller-side convenience bridges (REQ-178). These record a
 * telemetry event into dev->telemetry AND post the appropriate AER,
 * matching NVMe §5.2 semantics for SMART/Health asynchronous events.
 *
 *   aer_notify_thermal(dev, level)  – SMART temperature threshold AER
 *   aer_notify_wear   (dev, pct)    – NVM subsystem reliability AER
 *   aer_notify_spare  (dev, pct)    – Spare below threshold AER
 *
 * `level` uses THERMAL_LEVEL_* from common/thermal.h. `pct` is the
 * current remaining-life / spare percentage (0-100). The optional
 * `out_*` parameters mirror nvme_uspace_aer_post_event. */
int nvme_uspace_aer_notify_thermal(struct nvme_uspace_dev *dev,
                                   u8 thermal_level,
                                   bool *out_delivered,
                                   u16 *out_cid,
                                   struct nvme_cq_entry *out_cqe);
int nvme_uspace_aer_notify_wear(struct nvme_uspace_dev *dev,
                                u8 remaining_life_pct,
                                bool *out_delivered,
                                u16 *out_cid,
                                struct nvme_cq_entry *out_cqe);
int nvme_uspace_aer_notify_spare(struct nvme_uspace_dev *dev,
                                 u8 spare_pct,
                                 bool *out_delivered,
                                 u16 *out_cid,
                                 struct nvme_cq_entry *out_cqe);

/* Attach a live data source to the embedded smart_monitor. Any of
 * the callbacks may be NULL, in which case the matching default
 * (always-nominal) is kept. This is the integration point callers
 * (NBD / vhost / exporter main) use to plug real thermal / wear /
 * spare producers into the AER path without restarting the dev. */
int nvme_uspace_dev_set_smart_source(struct nvme_uspace_dev *dev,
                                     smart_thermal_level_fn  get_thermal,
                                     smart_remaining_life_fn get_remaining_life,
                                     smart_spare_fn          get_spare,
                                     void                   *cb_ctx);

/* REQ-151: hot-reconfigure the per-namespace QoS policy at runtime.
 * Applies `policy` to `nsid` without stopping or draining the I/O
 * path; the next qos_acquire_tokens call sees the new caps. Pass
 * policy->enforced=false to disable throttling without tearing
 * down the ctx. Returns HFSSS_ERR_INVAL for nsid outside the
 * advertised range, HFSSS_ERR_NOENT if the slot hasn't been armed
 * yet. */
int nvme_uspace_dev_set_qos_policy(struct nvme_uspace_dev *dev,
                                   u32 nsid,
                                   const struct ns_qos_policy *policy);

/* REQ-150: install a P99 SLA rollback callback for a namespace.
 * The existing ns_latency_monitor counts consecutive_violations on
 * every SLA breach; when that counter reaches `trigger_count`, the
 * callback fires with the offending nsid + the current P99 reading
 * so the caller can tighten caps, flip to a stricter deterministic
 * window, or log a telemetry event. `trigger_count == 0` disables
 * the rollback without touching the monitor. */
int nvme_uspace_dev_set_sla_rollback(struct nvme_uspace_dev *dev,
                                     u32 nsid,
                                     u32 target_us,
                                     u32 trigger_count,
                                     sla_rollback_fn cb,
                                     void *cb_ctx);

/* Drive the SLA check for `nsid`. Usually called after recording a
 * batch of latency samples via lat_monitor_record; on the sim test
 * path it's invoked synchronously. Returns true if the rollback
 * callback fired on this call. */
bool nvme_uspace_dev_check_sla(struct nvme_uspace_dev *dev, u32 nsid);

/* Record a single completion latency into the per-ns monitor so the
 * P99 / P99.9 detectors have data. Used by the dispatch wiring + by
 * tests. */
void nvme_uspace_dev_record_latency(struct nvme_uspace_dev *dev,
                                    u32 nsid, u64 latency_ns);
int nvme_uspace_get_features(struct nvme_uspace_dev *dev, u8 fid, u32 *value);
int nvme_uspace_set_features(struct nvme_uspace_dev *dev, u8 fid, u32 value);

/*
 * Full-path NVMe command dispatch.
 *
 * These functions route commands through nvme_ctrl_process_{io,admin}_cmd()
 * for opcode validation and completion entry construction, then dispatch to
 * the appropriate nvme_uspace_* implementation for actual FTL execution.
 *
 * This exercises the complete NVMe command processing pipeline end-to-end:
 *   SQE construction → opcode validation → uspace dispatch → FTL → CQE
 */
int nvme_uspace_dispatch_io_cmd(struct nvme_uspace_dev *dev, struct nvme_sq_entry *cmd, struct nvme_cq_entry *cpl,
                                void *data, u32 data_len);

int nvme_uspace_dispatch_admin_cmd(struct nvme_uspace_dev *dev, struct nvme_sq_entry *cmd, struct nvme_cq_entry *cpl,
                                   void *data, u32 data_len);

/* Exercise admin commands to validate the NVMe command processing path.
 * Called during NBD server startup to generate E2E admin command coverage. */
int nvme_uspace_exercise_admin_path(struct nvme_uspace_dev *dev);

#endif /* __HFSSS_NVME_USPACE_H */
