#ifndef __HFSSS_FLOW_CONTROL_H
#define __HFSSS_FLOW_CONTROL_H

#include "common/common.h"
#include "common/mutex.h"

/* Flow Type */
enum flow_type {
    FLOW_READ = 0,
    FLOW_WRITE = 1,
    FLOW_ADMIN = 2,
    FLOW_GC = 3,
    FLOW_MAX = 4,
};

/* QoS Priority Levels */
enum qos_priority {
    QOS_URGENT = 0,
    QOS_HIGH = 1,
    QOS_NORMAL = 2,
    QOS_LOW = 3,
    QOS_MAX = 4,
};

/* Backpressure State */
enum backpressure_state {
    BACKPRESSURE_NONE = 0,
    BACKPRESSURE_LOW = 1,
    BACKPRESSURE_MEDIUM = 2,
    BACKPRESSURE_HIGH = 3,
    BACKPRESSURE_CRITICAL = 4,
};

/* Token Bucket */
struct token_bucket {
    u64 tokens;
    u64 max_tokens;
    u64 rate;
    u64 last_refill_ts;
    struct mutex lock;
};

/* QoS Configuration */
struct qos_config {
    u64 min_rate[QOS_MAX];    /* Minimum guaranteed rate */
    u64 max_rate[QOS_MAX];    /* Maximum allowed rate */
    u8 priority_weight[QOS_MAX]; /* Weight for weighted scheduling */
};

/* Flow Control Context */
struct flow_ctrl_ctx {
    struct token_bucket buckets[FLOW_MAX];
    bool enabled;
    u64 total_allowed[FLOW_MAX];
    u64 total_throttled[FLOW_MAX];

    /* Backpressure */
    enum backpressure_state bp_state;
    u32 wb_occupancy_percent;  /* Write buffer occupancy */
    u32 free_block_threshold;   /* Free block threshold for backpressure */
    struct mutex bp_lock;

    /* QoS */
    struct qos_config qos_cfg;
    struct token_bucket qos_buckets[QOS_MAX];
    u64 qos_allowed[QOS_MAX];
    u64 qos_throttled[QOS_MAX];

    /* GC Traffic Control */
    u64 gc_rate_limit;          /* GC rate limit */
    u64 gc_max_burst;           /* GC max burst */
    bool gc_throttled;           /* Is GC currently throttled? */
};

/* Function Prototypes */
int flow_ctrl_init(struct flow_ctrl_ctx *ctx);
void flow_ctrl_cleanup(struct flow_ctrl_ctx *ctx);
bool flow_ctrl_check(struct flow_ctrl_ctx *ctx, enum flow_type type, u64 tokens);
void flow_ctrl_refill(struct flow_ctrl_ctx *ctx);

/* Backpressure Functions */
void flow_ctrl_update_backpressure(struct flow_ctrl_ctx *ctx, u32 wb_occupancy, u32 free_blocks);
enum backpressure_state flow_ctrl_get_backpressure(struct flow_ctrl_ctx *ctx);
bool flow_ctrl_should_throttle(struct flow_ctrl_ctx *ctx, enum flow_type type);

/* QoS Functions */
int flow_ctrl_qos_init(struct flow_ctrl_ctx *ctx);
bool flow_ctrl_qos_check(struct flow_ctrl_ctx *ctx, enum qos_priority prio, u64 tokens);
void flow_ctrl_qos_refill(struct flow_ctrl_ctx *ctx);

/* GC Traffic Control Functions */
void flow_ctrl_set_gc_rate(struct flow_ctrl_ctx *ctx, u64 rate, u64 max_burst);
bool flow_ctrl_gc_check(struct flow_ctrl_ctx *ctx, u64 tokens);

#endif /* __HFSSS_FLOW_CONTROL_H */
