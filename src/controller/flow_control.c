#include "controller/flow_control.h"
#include <stdlib.h>
#include <string.h>

#define DEFAULT_TOKEN_RATE 1000000
#define DEFAULT_MAX_TOKENS 1000000
#define DEFAULT_GC_RATE 200000
#define DEFAULT_GC_MAX_BURST 500000

int flow_ctrl_init(struct flow_ctrl_ctx *ctx)
{
    u32 i;
    int ret;

    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    /* Initialize flow buckets */
    for (i = 0; i < FLOW_MAX; i++) {
        ctx->buckets[i].tokens = DEFAULT_MAX_TOKENS;
        ctx->buckets[i].max_tokens = DEFAULT_MAX_TOKENS;
        ctx->buckets[i].rate = DEFAULT_TOKEN_RATE;
        ctx->buckets[i].last_refill_ts = get_time_ns();

        ret = mutex_init(&ctx->buckets[i].lock);
        if (ret != HFSSS_OK) {
            /* Cleanup and fail */
            for (u32 j = 0; j < i; j++) {
                mutex_cleanup(&ctx->buckets[j].lock);
            }
            return ret;
        }
    }

    /* Initialize backpressure */
    ctx->bp_state = BACKPRESSURE_NONE;
    ctx->wb_occupancy_percent = 0;
    ctx->free_block_threshold = 10;  /* 10% free blocks threshold */
    ret = mutex_init(&ctx->bp_lock);
    if (ret != HFSSS_OK) {
        for (u32 j = 0; j < FLOW_MAX; j++) {
            mutex_cleanup(&ctx->buckets[j].lock);
        }
        return ret;
    }

    /* Initialize QoS */
    ret = flow_ctrl_qos_init(ctx);
    if (ret != HFSSS_OK) {
        mutex_cleanup(&ctx->bp_lock);
        for (u32 j = 0; j < FLOW_MAX; j++) {
            mutex_cleanup(&ctx->buckets[j].lock);
        }
        return ret;
    }

    /* Initialize GC traffic control */
    ctx->gc_rate_limit = DEFAULT_GC_RATE;
    ctx->gc_max_burst = DEFAULT_GC_MAX_BURST;
    ctx->gc_throttled = false;

    ctx->enabled = true;

    return HFSSS_OK;
}

void flow_ctrl_cleanup(struct flow_ctrl_ctx *ctx)
{
    u32 i;

    if (!ctx) {
        return;
    }

    for (i = 0; i < FLOW_MAX; i++) {
        mutex_cleanup(&ctx->buckets[i].lock);
    }

    for (i = 0; i < QOS_MAX; i++) {
        mutex_cleanup(&ctx->qos_buckets[i].lock);
    }

    mutex_cleanup(&ctx->bp_lock);

    memset(ctx, 0, sizeof(*ctx));
}

void flow_ctrl_refill(struct flow_ctrl_ctx *ctx)
{
    u32 i;
    u64 now;
    u64 elapsed_ns;
    u64 tokens_to_add;

    if (!ctx) {
        return;
    }

    now = get_time_ns();

    for (i = 0; i < FLOW_MAX; i++) {
        struct token_bucket *bucket = &ctx->buckets[i];

        mutex_lock(&bucket->lock, 0);

        elapsed_ns = now - bucket->last_refill_ts;
        if (elapsed_ns > 0) {
            tokens_to_add = (bucket->rate * elapsed_ns) / 1000000000ULL;
            if (tokens_to_add > 0) {
                bucket->tokens += tokens_to_add;
                if (bucket->tokens > bucket->max_tokens) {
                    bucket->tokens = bucket->max_tokens;
                }
                bucket->last_refill_ts = now;
            }
        }

        mutex_unlock(&bucket->lock);
    }
}

bool flow_ctrl_check(struct flow_ctrl_ctx *ctx, enum flow_type type, u64 tokens)
{
    struct token_bucket *bucket;
    bool allowed = false;

    if (!ctx || type >= FLOW_MAX) {
        return true;  /* Allow if not initialized */
    }

    if (!ctx->enabled) {
        return true;
    }

    /* Check backpressure first */
    if (flow_ctrl_should_throttle(ctx, type)) {
        ctx->total_throttled[type]++;
        return false;
    }

    bucket = &ctx->buckets[type];

    /* First refill the bucket */
    flow_ctrl_refill(ctx);

    mutex_lock(&bucket->lock, 0);

    if (bucket->tokens >= tokens) {
        bucket->tokens -= tokens;
        allowed = true;
        ctx->total_allowed[type]++;
    } else {
        ctx->total_throttled[type]++;
    }

    mutex_unlock(&bucket->lock);

    return allowed;
}

/* Backpressure Functions */
void flow_ctrl_update_backpressure(struct flow_ctrl_ctx *ctx, u32 wb_occupancy, u32 free_blocks)
{
    enum backpressure_state new_state;

    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->bp_lock, 0);

    ctx->wb_occupancy_percent = wb_occupancy;

    /* Determine backpressure state based on write buffer occupancy and free blocks */
    if (wb_occupancy >= 95 || free_blocks < 2) {
        new_state = BACKPRESSURE_CRITICAL;
    } else if (wb_occupancy >= 85 || free_blocks < 5) {
        new_state = BACKPRESSURE_HIGH;
    } else if (wb_occupancy >= 70 || free_blocks < 10) {
        new_state = BACKPRESSURE_MEDIUM;
    } else if (wb_occupancy >= 50 || free_blocks < 20) {
        new_state = BACKPRESSURE_LOW;
    } else {
        new_state = BACKPRESSURE_NONE;
    }

    ctx->bp_state = new_state;

    mutex_unlock(&ctx->bp_lock);
}

enum backpressure_state flow_ctrl_get_backpressure(struct flow_ctrl_ctx *ctx)
{
    enum backpressure_state state;

    if (!ctx) {
        return BACKPRESSURE_NONE;
    }

    mutex_lock(&ctx->bp_lock, 0);
    state = ctx->bp_state;
    mutex_unlock(&ctx->bp_lock);

    return state;
}

bool flow_ctrl_should_throttle(struct flow_ctrl_ctx *ctx, enum flow_type type)
{
    enum backpressure_state state;
    bool should_throttle = false;

    if (!ctx) {
        return false;
    }

    state = flow_ctrl_get_backpressure(ctx);

    switch (state) {
        case BACKPRESSURE_NONE:
            should_throttle = false;
            break;
        case BACKPRESSURE_LOW:
            /* Throttle GC only */
            should_throttle = (type == FLOW_GC);
            break;
        case BACKPRESSURE_MEDIUM:
            /* Throttle writes and GC */
            should_throttle = (type == FLOW_WRITE || type == FLOW_GC);
            break;
        case BACKPRESSURE_HIGH:
        case BACKPRESSURE_CRITICAL:
            /* Throttle everything except admin */
            should_throttle = (type != FLOW_ADMIN);
            break;
        default:
            break;
    }

    return should_throttle;
}

/* QoS Functions */
int flow_ctrl_qos_init(struct flow_ctrl_ctx *ctx)
{
    u32 i;
    int ret;

    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    /* Default QoS configuration */
    ctx->qos_cfg.min_rate[QOS_URGENT] = 500000;
    ctx->qos_cfg.min_rate[QOS_HIGH] = 300000;
    ctx->qos_cfg.min_rate[QOS_NORMAL] = 150000;
    ctx->qos_cfg.min_rate[QOS_LOW] = 50000;

    ctx->qos_cfg.max_rate[QOS_URGENT] = DEFAULT_TOKEN_RATE * 2;
    ctx->qos_cfg.max_rate[QOS_HIGH] = DEFAULT_TOKEN_RATE * 1.5;
    ctx->qos_cfg.max_rate[QOS_NORMAL] = DEFAULT_TOKEN_RATE;
    ctx->qos_cfg.max_rate[QOS_LOW] = DEFAULT_TOKEN_RATE * 0.5;

    ctx->qos_cfg.priority_weight[QOS_URGENT] = 8;
    ctx->qos_cfg.priority_weight[QOS_HIGH] = 4;
    ctx->qos_cfg.priority_weight[QOS_NORMAL] = 2;
    ctx->qos_cfg.priority_weight[QOS_LOW] = 1;

    /* Initialize QoS buckets */
    for (i = 0; i < QOS_MAX; i++) {
        ctx->qos_buckets[i].tokens = ctx->qos_cfg.max_rate[i];
        ctx->qos_buckets[i].max_tokens = ctx->qos_cfg.max_rate[i];
        ctx->qos_buckets[i].rate = ctx->qos_cfg.max_rate[i];
        ctx->qos_buckets[i].last_refill_ts = get_time_ns();

        ret = mutex_init(&ctx->qos_buckets[i].lock);
        if (ret != HFSSS_OK) {
            /* Cleanup and fail */
            for (u32 j = 0; j < i; j++) {
                mutex_cleanup(&ctx->qos_buckets[j].lock);
            }
            return ret;
        }
    }

    return HFSSS_OK;
}

bool flow_ctrl_qos_check(struct flow_ctrl_ctx *ctx, enum qos_priority prio, u64 tokens)
{
    struct token_bucket *bucket;
    bool allowed = false;
    u64 now;
    u64 elapsed_ns;
    u64 tokens_to_add;

    if (!ctx || prio >= QOS_MAX) {
        return true;
    }

    if (!ctx->enabled) {
        return true;
    }

    bucket = &ctx->qos_buckets[prio];
    now = get_time_ns();

    mutex_lock(&bucket->lock, 0);

    /* Refill */
    elapsed_ns = now - bucket->last_refill_ts;
    if (elapsed_ns > 0) {
        tokens_to_add = (bucket->rate * elapsed_ns) / 1000000000ULL;
        if (tokens_to_add > 0) {
            bucket->tokens += tokens_to_add;
            if (bucket->tokens > bucket->max_tokens) {
                bucket->tokens = bucket->max_tokens;
            }
            bucket->last_refill_ts = now;
        }
    }

    /* Check tokens */
    if (bucket->tokens >= tokens) {
        bucket->tokens -= tokens;
        allowed = true;
        ctx->qos_allowed[prio]++;
    } else {
        ctx->qos_throttled[prio]++;
    }

    mutex_unlock(&bucket->lock);

    return allowed;
}

void flow_ctrl_qos_refill(struct flow_ctrl_ctx *ctx)
{
    u32 i;
    u64 now;
    u64 elapsed_ns;
    u64 tokens_to_add;

    if (!ctx) {
        return;
    }

    now = get_time_ns();

    for (i = 0; i < QOS_MAX; i++) {
        struct token_bucket *bucket = &ctx->qos_buckets[i];

        mutex_lock(&bucket->lock, 0);

        elapsed_ns = now - bucket->last_refill_ts;
        if (elapsed_ns > 0) {
            tokens_to_add = (bucket->rate * elapsed_ns) / 1000000000ULL;
            if (tokens_to_add > 0) {
                bucket->tokens += tokens_to_add;
                if (bucket->tokens > bucket->max_tokens) {
                    bucket->tokens = bucket->max_tokens;
                }
                bucket->last_refill_ts = now;
            }
        }

        mutex_unlock(&bucket->lock);
    }
}

/* GC Traffic Control Functions */
void flow_ctrl_set_gc_rate(struct flow_ctrl_ctx *ctx, u64 rate, u64 max_burst)
{
    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->buckets[FLOW_GC].lock, 0);

    ctx->gc_rate_limit = rate;
    ctx->gc_max_burst = max_burst;
    ctx->buckets[FLOW_GC].rate = rate;
    ctx->buckets[FLOW_GC].max_tokens = max_burst;
    if (ctx->buckets[FLOW_GC].tokens > max_burst) {
        ctx->buckets[FLOW_GC].tokens = max_burst;
    }

    mutex_unlock(&ctx->buckets[FLOW_GC].lock);
}

bool flow_ctrl_gc_check(struct flow_ctrl_ctx *ctx, u64 tokens)
{
    return flow_ctrl_check(ctx, FLOW_GC, tokens);
}
