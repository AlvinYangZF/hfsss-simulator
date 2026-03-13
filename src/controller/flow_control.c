#include "controller/flow_control.h"
#include <stdlib.h>
#include <string.h>

#define DEFAULT_TOKEN_RATE 1000000
#define DEFAULT_MAX_TOKENS 1000000

int flow_ctrl_init(struct flow_ctrl_ctx *ctx)
{
    u32 i;
    int ret;

    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

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
