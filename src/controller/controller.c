#include "controller/controller.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

void controller_config_default(struct controller_config *config)
{
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));

    config->sched_period_ns = 10000;  /* 10us */
    config->max_concurrent_cmds = 65536;
    config->sched_policy = SCHED_FIFO;
    config->wb_max_entries = WB_MAX_ENTRIES;
    config->rc_max_entries = RC_MAX_ENTRIES;
    config->channel_count = 8;
    config->flow_ctrl_enabled = true;
    config->read_rate_limit = 1000000;
    config->write_rate_limit = 500000;
    config->shmem_path = "/hfsss_shmem";
}

int controller_init(struct controller_ctx *ctx, struct controller_config *config)
{
    int ret;

    if (!ctx || !config) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    memcpy(&ctx->config, config, sizeof(*config));

    ret = mutex_init(&ctx->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Initialize arbiter */
    ret = arbiter_init(&ctx->arbiter, config->max_concurrent_cmds);
    if (ret != HFSSS_OK) {
        mutex_cleanup(&ctx->lock);
        return ret;
    }

    /* Initialize scheduler */
    ret = scheduler_init(&ctx->scheduler, config->sched_policy);
    if (ret != HFSSS_OK) {
        arbiter_cleanup(&ctx->arbiter);
        mutex_cleanup(&ctx->lock);
        return ret;
    }

    /* Initialize write buffer */
    ret = wb_init(&ctx->wb, config->wb_max_entries);
    if (ret != HFSSS_OK) {
        scheduler_cleanup(&ctx->scheduler);
        arbiter_cleanup(&ctx->arbiter);
        mutex_cleanup(&ctx->lock);
        return ret;
    }

    /* Initialize read cache */
    ret = rc_init(&ctx->rc, config->rc_max_entries);
    if (ret != HFSSS_OK) {
        wb_cleanup(&ctx->wb);
        scheduler_cleanup(&ctx->scheduler);
        arbiter_cleanup(&ctx->arbiter);
        mutex_cleanup(&ctx->lock);
        return ret;
    }

    /* Initialize channel manager */
    ret = channel_mgr_init(&ctx->channel_mgr, config->channel_count);
    if (ret != HFSSS_OK) {
        rc_cleanup(&ctx->rc);
        wb_cleanup(&ctx->wb);
        scheduler_cleanup(&ctx->scheduler);
        arbiter_cleanup(&ctx->arbiter);
        mutex_cleanup(&ctx->lock);
        return ret;
    }

    /* Initialize resource manager */
    ret = resource_mgr_init(&ctx->resource_mgr);
    if (ret != HFSSS_OK) {
        channel_mgr_cleanup(&ctx->channel_mgr);
        rc_cleanup(&ctx->rc);
        wb_cleanup(&ctx->wb);
        scheduler_cleanup(&ctx->scheduler);
        arbiter_cleanup(&ctx->arbiter);
        mutex_cleanup(&ctx->lock);
        return ret;
    }

    /* Initialize flow control */
    ret = flow_ctrl_init(&ctx->flow_ctrl);
    if (ret != HFSSS_OK) {
        resource_mgr_cleanup(&ctx->resource_mgr);
        channel_mgr_cleanup(&ctx->channel_mgr);
        rc_cleanup(&ctx->rc);
        wb_cleanup(&ctx->wb);
        scheduler_cleanup(&ctx->scheduler);
        arbiter_cleanup(&ctx->arbiter);
        mutex_cleanup(&ctx->lock);
        return ret;
    }

    ctx->initialized = true;
    return HFSSS_OK;
}

void controller_cleanup(struct controller_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    if (!ctx->initialized) {
        return;
    }

    if (ctx->running) {
        controller_stop(ctx);
    }

    if (ctx->shmem) {
        shmem_if_close(ctx->shmem, ctx->shmem_fd);
        ctx->shmem = NULL;
        ctx->shmem_fd = -1;
    }

    flow_ctrl_cleanup(&ctx->flow_ctrl);
    resource_mgr_cleanup(&ctx->resource_mgr);
    channel_mgr_cleanup(&ctx->channel_mgr);
    rc_cleanup(&ctx->rc);
    wb_cleanup(&ctx->wb);
    scheduler_cleanup(&ctx->scheduler);
    arbiter_cleanup(&ctx->arbiter);

    mutex_cleanup(&ctx->lock);

    memset(ctx, 0, sizeof(*ctx));
}

int controller_start(struct controller_ctx *ctx)
{
    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    if (ctx->running) {
        return HFSSS_OK;
    }

    /* Shared memory is optional, skip for now */
    ctx->shmem = NULL;
    ctx->shmem_fd = -1;

    ctx->running = true;
    return HFSSS_OK;
}

void controller_stop(struct controller_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    if (!ctx->running) {
        return;
    }

    ctx->running = false;

    if (ctx->shmem) {
        shmem_if_close(ctx->shmem, ctx->shmem_fd);
        ctx->shmem = NULL;
        ctx->shmem_fd = -1;
    }
}
