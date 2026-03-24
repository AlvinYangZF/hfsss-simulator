#include "controller/qos.h"
#include <stdlib.h>
#include <string.h>

/* Initialize a token bucket from rate and burst parameters */
static void token_bucket_init(struct qos_token_bucket *tb, u64 rate,
                              u64 burst, u64 now_ns)
{
    tb->refill_rate = rate;
    tb->max_tokens = burst > 0 ? burst : rate;
    tb->tokens = tb->max_tokens;
    tb->last_refill_ns = now_ns;
}

int qos_ctx_init(struct ns_qos_ctx *ctx, u32 nsid,
                 const struct ns_qos_policy *policy)
{
    u64 now_ns;

    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->nsid = nsid;

    now_ns = get_time_ns();

    if (policy) {
        ctx->policy = *policy;
        ctx->policy.nsid = nsid;
    } else {
        /* Default: unlimited */
        ctx->policy.nsid = nsid;
        ctx->policy.iops_limit = 0;
        ctx->policy.bw_limit_mbps = 0;
        ctx->policy.latency_target_us = 0;
        ctx->policy.burst_allowance = 0;
        ctx->policy.enforced = false;
    }

    /* Initialize IOPS token bucket */
    if (ctx->policy.iops_limit > 0) {
        u64 burst = ctx->policy.iops_limit + ctx->policy.burst_allowance;
        token_bucket_init(&ctx->iops_bucket, ctx->policy.iops_limit,
                          burst, now_ns);
    } else {
        token_bucket_init(&ctx->iops_bucket, 0, 0, now_ns);
    }

    /* Initialize BW token bucket (tokens = bytes per second) */
    if (ctx->policy.bw_limit_mbps > 0) {
        u64 bw_bps = (u64)ctx->policy.bw_limit_mbps * 1024ULL * 1024ULL;
        u64 burst = bw_bps + (u64)ctx->policy.burst_allowance * 1024ULL;
        token_bucket_init(&ctx->bw_bucket, bw_bps, burst, now_ns);
    } else {
        token_bucket_init(&ctx->bw_bucket, 0, 0, now_ns);
    }

    ctx->initialized = true;
    return HFSSS_OK;
}

void qos_ctx_cleanup(struct ns_qos_ctx *ctx)
{
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
}

/* Refill a single token bucket based on elapsed time */
static void refill_bucket(struct qos_token_bucket *tb, u64 now_ns)
{
    if (tb->refill_rate == 0) {
        return;
    }

    if (now_ns <= tb->last_refill_ns) {
        return;
    }

    u64 elapsed_ns = now_ns - tb->last_refill_ns;
    u64 tokens_to_add = (tb->refill_rate * elapsed_ns) / 1000000000ULL;

    if (tokens_to_add > 0) {
        tb->tokens += tokens_to_add;
        if (tb->tokens > tb->max_tokens) {
            tb->tokens = tb->max_tokens;
        }
        tb->last_refill_ns = now_ns;
    }
}

void qos_refill_tokens(struct ns_qos_ctx *ctx, u64 now_ns)
{
    if (!ctx || !ctx->initialized) {
        return;
    }

    refill_bucket(&ctx->iops_bucket, now_ns);
    refill_bucket(&ctx->bw_bucket, now_ns);
}

bool qos_acquire_tokens(struct ns_qos_ctx *ctx, bool is_write, u32 io_size)
{
    if (!ctx || !ctx->initialized) {
        return true;  /* allow if not initialized */
    }

    if (!ctx->policy.enforced) {
        return true;
    }

    /* Check IOPS bucket */
    if (ctx->policy.iops_limit > 0) {
        u64 cost = is_write ? 2 : 1;
        if (ctx->iops_bucket.tokens < cost) {
            return false;
        }
        ctx->iops_bucket.tokens -= cost;
    }

    /* Check BW bucket */
    if (ctx->policy.bw_limit_mbps > 0) {
        u64 bw_cost = (u64)io_size;
        if (ctx->bw_bucket.tokens < bw_cost) {
            /* Restore IOPS tokens if BW check fails */
            if (ctx->policy.iops_limit > 0) {
                u64 cost = is_write ? 2 : 1;
                ctx->iops_bucket.tokens += cost;
            }
            return false;
        }
        ctx->bw_bucket.tokens -= bw_cost;
    }

    return true;
}

int qos_set_policy(struct ns_qos_ctx *ctx, const struct ns_qos_policy *policy)
{
    u64 now_ns;

    if (!ctx || !policy) {
        return HFSSS_ERR_INVAL;
    }

    now_ns = get_time_ns();

    u32 nsid = ctx->nsid;
    ctx->policy = *policy;
    ctx->policy.nsid = nsid;

    /* Reinitialize IOPS bucket */
    if (ctx->policy.iops_limit > 0) {
        u64 burst = ctx->policy.iops_limit + ctx->policy.burst_allowance;
        token_bucket_init(&ctx->iops_bucket, ctx->policy.iops_limit,
                          burst, now_ns);
    } else {
        token_bucket_init(&ctx->iops_bucket, 0, 0, now_ns);
    }

    /* Reinitialize BW bucket */
    if (ctx->policy.bw_limit_mbps > 0) {
        u64 bw_bps = (u64)ctx->policy.bw_limit_mbps * 1024ULL * 1024ULL;
        u64 burst = bw_bps + (u64)ctx->policy.burst_allowance * 1024ULL;
        token_bucket_init(&ctx->bw_bucket, bw_bps, burst, now_ns);
    } else {
        token_bucket_init(&ctx->bw_bucket, 0, 0, now_ns);
    }

    return HFSSS_OK;
}

void qos_get_policy(const struct ns_qos_ctx *ctx, struct ns_qos_policy *out)
{
    if (!ctx || !out) {
        return;
    }
    *out = ctx->policy;
}
