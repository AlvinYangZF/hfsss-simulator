#include "pcie/msix.h"
#include <stdlib.h>
#include <string.h>

int msix_init(struct msix_ctx *ctx, u32 num_vectors)
{
    int ret;

    if (!ctx || num_vectors > MSIX_MAX_VECTORS) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    ret = mutex_init(&ctx->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    ctx->table_size = num_vectors;
    ctx->pba_size = (num_vectors + 63) / 64;

    /* Allocate MSI-X Table */
    ctx->table = calloc(num_vectors, sizeof(struct msix_table_entry));
    if (!ctx->table) {
        mutex_cleanup(&ctx->lock);
        return HFSSS_ERR_NOMEM;
    }

    /* Allocate PBA */
    ctx->pba = calloc(1, sizeof(struct msix_pba));
    if (!ctx->pba) {
        free(ctx->table);
        mutex_cleanup(&ctx->lock);
        return HFSSS_ERR_NOMEM;
    }

    ctx->enabled = false;
    ctx->function_mask = false;

    return HFSSS_OK;
}

void msix_cleanup(struct msix_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->table) {
        free(ctx->table);
    }

    if (ctx->pba) {
        free(ctx->pba);
    }

    mutex_cleanup(&ctx->lock);
    memset(ctx, 0, sizeof(*ctx));
}

int msix_enable(struct msix_ctx *ctx)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);
    ctx->enabled = true;
    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

void msix_disable(struct msix_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->lock, 0);
    ctx->enabled = false;
    mutex_unlock(&ctx->lock);
}

int msix_trigger_irq(struct msix_ctx *ctx, u32 vector)
{
    if (!ctx || vector >= MSIX_MAX_VECTORS) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    if (ctx->enabled && !ctx->function_mask && !ctx->masked[vector]) {
        ctx->pending[vector] = true;
        ctx->irq_count[vector]++;

        /* Set PBA bit */
        if (ctx->pba) {
            ctx->pba->pending[vector / 64] |= (1ULL << (vector % 64));
        }

        /* Call IRQ callback if registered */
        if (ctx->irq_callback) {
            ctx->irq_callback(ctx->private_data, vector);
        }
    }

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int msix_table_read(struct msix_ctx *ctx, u32 offset, u64 *value, u32 size)
{
    if (!ctx || !value) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    u32 entry_idx = offset / MSIX_TABLE_ENTRY_SIZE;
    u32 entry_offset = offset % MSIX_TABLE_ENTRY_SIZE;

    if (entry_idx < ctx->table_size) {
        u8 *entry_ptr = (u8 *)&ctx->table[entry_idx];
        *value = 0;

        if (entry_offset + size <= MSIX_TABLE_ENTRY_SIZE) {
            memcpy(value, entry_ptr + entry_offset, size);
        }
    } else {
        *value = 0;
    }

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int msix_table_write(struct msix_ctx *ctx, u32 offset, u64 value, u32 size)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    u32 entry_idx = offset / MSIX_TABLE_ENTRY_SIZE;
    u32 entry_offset = offset % MSIX_TABLE_ENTRY_SIZE;

    if (entry_idx < ctx->table_size) {
        u8 *entry_ptr = (u8 *)&ctx->table[entry_idx];

        if (entry_offset + size <= MSIX_TABLE_ENTRY_SIZE) {
            memcpy(entry_ptr + entry_offset, &value, size);
        }

        /* Check for Vector Control write */
        if (entry_offset == 12 && size >= 4) {
            if (value & MSIX_VECTOR_CTRL_MASK) {
                ctx->masked[entry_idx] = true;
            } else {
                ctx->masked[entry_idx] = false;
            }
        }
    }

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int msix_pba_read(struct msix_ctx *ctx, u32 offset, u64 *value, u32 size)
{
    if (!ctx || !value || !ctx->pba) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    u32 pba_idx = offset / 8;
    if (pba_idx < ctx->pba_size) {
        *value = ctx->pba->pending[pba_idx];
    } else {
        *value = 0;
    }

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int msix_pba_write(struct msix_ctx *ctx, u32 offset, u64 value, u32 size)
{
    /* PBA is read-only */
    return HFSSS_OK;
}
