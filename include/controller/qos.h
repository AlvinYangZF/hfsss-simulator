#ifndef __HFSSS_QOS_H
#define __HFSSS_QOS_H

#include "common/common.h"
#include "common/mutex.h"

#define QOS_MAX_NAMESPACES 32
#define QOS_HIST_BUCKETS   64
#define QOS_DEFAULT_WEIGHT 100

/* DWRR queue per namespace */
struct dwrr_queue {
    u32 nsid;
    u32 weight;
    s32 deficit;
    u32 pending_cmds;
    u32 dispatched_cmds;
    bool active;
};

/* DWRR scheduler */
struct dwrr_scheduler {
    struct dwrr_queue queues[QOS_MAX_NAMESPACES];
    u32 active_count;
    u32 base_quantum;
    u32 max_outstanding;
    u32 current_idx;
    double throttle_factor;  /* 0.0-1.0, from thermal */
    struct mutex lock;
    bool initialized;
};

/* Per-namespace QoS policy */
struct ns_qos_policy {
    u32 nsid;
    u32 iops_limit;        /* 0 = unlimited */
    u32 bw_limit_mbps;     /* 0 = unlimited */
    u32 latency_target_us; /* P99 target, 0 = no target */
    u32 burst_allowance;   /* extra tokens for burst */
    bool enforced;
};

/* Token bucket for rate limiting */
struct qos_token_bucket {
    u64 tokens;
    u64 max_tokens;
    u64 refill_rate;   /* tokens per second */
    u64 last_refill_ns;
};

/* Per-namespace QoS context */
struct ns_qos_ctx {
    u32 nsid;
    struct ns_qos_policy policy;
    struct qos_token_bucket iops_bucket;
    struct qos_token_bucket bw_bucket;
    bool initialized;
};

/* REQ-088: caller-supplied alert fired when the P99.9 anomaly
 * detector observes a breach. Receives the offending nsid, the
 * current P99.9 reading (microseconds), and the caller ctx. */
typedef void (*lat_anomaly_fn)(u32 nsid, u64 p999_us, void *ctx);

/* Latency monitor per namespace */
struct ns_latency_monitor {
    u32 nsid;
    u64 buckets[QOS_HIST_BUCKETS];
    u64 total_samples;
    u64 total_latency_ns;
    u32 target_us;
    u32 sla_violations;
    u32 consecutive_violations;
    bool initialized;

    /* REQ-088: P99.9 anomaly detector. threshold_us==0 keeps the
     * detector disabled; any non-zero value arms it. `p999_cb` may
     * be NULL — then only the counter advances on breach. */
    u32             p999_threshold_us;
    u32             p999_anomalies;
    lat_anomaly_fn  p999_cb;
    void           *p999_cb_ctx;
};

/* Deterministic window configuration */
enum det_window_phase {
    DW_HOST_IO    = 0,
    DW_GC_ALLOWED = 1,
    DW_GC_ONLY    = 2,
};

struct det_window_config {
    u32 host_io_pct;      /* e.g., 80 */
    u32 gc_allowed_pct;   /* e.g., 15 */
    u32 gc_only_pct;      /* e.g., 5 */
    u32 cycle_ms;         /* full cycle duration */
    u64 cycle_start_ns;
    bool enabled;
};

/* DWRR scheduler functions */
int dwrr_init(struct dwrr_scheduler *sched, u32 max_outstanding);
void dwrr_cleanup(struct dwrr_scheduler *sched);
int dwrr_queue_create(struct dwrr_scheduler *sched, u32 nsid, u32 weight);
int dwrr_queue_delete(struct dwrr_scheduler *sched, u32 nsid);
int dwrr_enqueue(struct dwrr_scheduler *sched, u32 nsid);
int dwrr_dequeue(struct dwrr_scheduler *sched, u32 *nsid_out);
u32 dwrr_command_cost(bool is_write);
void dwrr_set_throttle_factor(struct dwrr_scheduler *sched, double factor);
bool dwrr_has_pending(struct dwrr_scheduler *sched);
void dwrr_get_stats(struct dwrr_scheduler *sched, u32 nsid,
                    u32 *pending, u32 *dispatched);

/* Per-namespace QoS policy functions */
int qos_ctx_init(struct ns_qos_ctx *ctx, u32 nsid,
                 const struct ns_qos_policy *policy);
void qos_ctx_cleanup(struct ns_qos_ctx *ctx);
bool qos_acquire_tokens(struct ns_qos_ctx *ctx, bool is_write, u32 io_size);
void qos_refill_tokens(struct ns_qos_ctx *ctx, u64 now_ns);
int qos_set_policy(struct ns_qos_ctx *ctx, const struct ns_qos_policy *policy);
void qos_get_policy(const struct ns_qos_ctx *ctx, struct ns_qos_policy *out);

/* Latency monitor functions */
int lat_monitor_init(struct ns_latency_monitor *mon, u32 nsid, u32 target_us);
void lat_monitor_cleanup(struct ns_latency_monitor *mon);
void lat_monitor_record(struct ns_latency_monitor *mon, u64 latency_ns);
u64 lat_monitor_percentile(const struct ns_latency_monitor *mon,
                           u32 percentile_x10);
bool lat_monitor_check_sla(struct ns_latency_monitor *mon);
void lat_monitor_reset(struct ns_latency_monitor *mon);

/* REQ-088: P99.9 anomaly detector. set_p999_anomaly installs a
 * threshold + optional alert callback; passing threshold_us=0
 * disables the detector without touching the histogram.
 * check_p999_anomaly computes the current P99.9 from the backing
 * histogram, compares against the threshold, and on breach bumps
 * the p999_anomalies counter and fires the callback (if non-NULL).
 * Returns true on breach, false otherwise. */
int  lat_monitor_set_p999_anomaly(struct ns_latency_monitor *mon,
                                  u32 threshold_us,
                                  lat_anomaly_fn cb, void *cb_ctx);
bool lat_monitor_check_p999_anomaly(struct ns_latency_monitor *mon);
u32  lat_monitor_p999_anomaly_count(const struct ns_latency_monitor *mon);

/* Deterministic window functions */
int det_window_init(struct det_window_config *cfg, u32 host_pct,
                    u32 gc_allowed_pct, u32 gc_only_pct, u32 cycle_ms);
enum det_window_phase det_window_get_phase(const struct det_window_config *cfg,
                                           u64 now_ns);
bool det_window_allow_gc(const struct det_window_config *cfg, u64 now_ns);
bool det_window_allow_host_io(const struct det_window_config *cfg, u64 now_ns);

#endif /* __HFSSS_QOS_H */
