#include "pcie/dma.h"
#include <stdlib.h>
#include <string.h>

int dma_init(struct dma_ctx *ctx, u64 buffer_size)
{
    int ret;

    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    ret = mutex_init(&ctx->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    ctx->buffer_size = buffer_size;
    ctx->buffer = calloc(1, buffer_size);
    if (!ctx->buffer) {
        mutex_cleanup(&ctx->lock);
        return HFSSS_ERR_NOMEM;
    }

    return HFSSS_OK;
}

void dma_cleanup(struct dma_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->buffer) {
        free(ctx->buffer);
    }

    mutex_cleanup(&ctx->lock);
    memset(ctx, 0, sizeof(*ctx));
}

int dma_copy_from_prp(struct dma_ctx *ctx, u8 *dst, u64 prp1, u64 prp2, u32 len, u32 page_size)
{
    if (!ctx || !dst) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    /* For user-space emulation: just copy from our local buffer */
    if (len <= ctx->buffer_size) {
        memcpy(dst, ctx->buffer, len);
        ctx->transfer_count++;
        ctx->bytes_transferred += len;
    } else {
        ctx->error_count++;
    }

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int dma_copy_to_prp(struct dma_ctx *ctx, u64 prp1, u64 prp2, const u8 *src, u32 len, u32 page_size)
{
    if (!ctx || !src) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    /* For user-space emulation: just copy to our local buffer */
    if (len <= ctx->buffer_size) {
        memcpy(ctx->buffer, src, len);
        ctx->transfer_count++;
        ctx->bytes_transferred += len;
    } else {
        ctx->error_count++;
    }

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int dma_read_cmd_data(struct dma_ctx *ctx, struct nvme_sq_entry *cmd, u8 *buffer, u32 len, u32 page_size)
{
    if (!ctx || !cmd || !buffer) {
        return HFSSS_ERR_INVAL;
    }

    return dma_copy_from_prp(ctx, buffer, cmd->dp.prp.prp1, cmd->dp.prp.prp2, len, page_size);
}

int dma_write_cmd_data(struct dma_ctx *ctx, struct nvme_sq_entry *cmd, const u8 *buffer, u32 len, u32 page_size)
{
    if (!ctx || !cmd || !buffer) {
        return HFSSS_ERR_INVAL;
    }

    return dma_copy_to_prp(ctx, cmd->dp.prp.prp1, cmd->dp.prp.prp2, buffer, len, page_size);
}
