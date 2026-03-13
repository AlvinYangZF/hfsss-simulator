#ifndef __HFSSS_FLOW_CONTROL_H
#define __HFSSS_FLOW_CONTROL_H

#include "common/common.h"
#include "common/mutex.h"

/* Flow Type */
enum flow_type {
    FLOW_READ = 0,
    FLOW_WRITE = 1,
    FLOW_ADMIN = 2,
    FLOW_MAX = 3,
};

/* Token Bucket */
struct token_bucket {
    u64 tokens;
    u64 max_tokens;
    u64 rate;
    u64 last_refill_ts;
    struct mutex lock;
};

/* Flow Control Context */
struct flow_ctrl_ctx {
    struct token_bucket buckets[FLOW_MAX];
    bool enabled;
    u64 total_allowed[FLOW_MAX];
    u64 total_throttled[FLOW_MAX];
};

/* Function Prototypes */
int flow_ctrl_init(struct flow_ctrl_ctx *ctx);
void flow_ctrl_cleanup(struct flow_ctrl_ctx *ctx);
bool flow_ctrl_check(struct flow_ctrl_ctx *ctx, enum flow_type type, u64 tokens);
void flow_ctrl_refill(struct flow_ctrl_ctx *ctx);

#endif /* __HFSSS_FLOW_CONTROL_H */
