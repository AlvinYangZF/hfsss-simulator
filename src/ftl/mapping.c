#include "ftl/mapping.h"
#include <stdlib.h>
#include <string.h>

int mapping_init(struct mapping_ctx *ctx, u64 l2p_size, u64 p2l_size)
{
    int ret;

    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    /* Initialize lock */
    ret = mutex_init(&ctx->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Clamp sizes to reasonable values */
    if (l2p_size > L2P_TABLE_SIZE) {
        l2p_size = L2P_TABLE_SIZE;
    }
    if (p2l_size > P2L_TABLE_SIZE) {
        p2l_size = P2L_TABLE_SIZE;
    }

    ctx->l2p_size = l2p_size;
    ctx->p2l_size = p2l_size;

    /* Allocate L2P table */
    ctx->l2p_table = (struct l2p_entry *)calloc(l2p_size, sizeof(struct l2p_entry));
    if (!ctx->l2p_table) {
        mutex_cleanup(&ctx->lock);
        return HFSSS_ERR_NOMEM;
    }

    /* Allocate P2L table */
    ctx->p2l_table = (struct p2l_entry *)calloc(p2l_size, sizeof(struct p2l_entry));
    if (!ctx->p2l_table) {
        free(ctx->l2p_table);
        mutex_cleanup(&ctx->lock);
        return HFSSS_ERR_NOMEM;
    }

    ctx->valid_count = 0;

    return HFSSS_OK;
}

void mapping_cleanup(struct mapping_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->lock, 0);

    free(ctx->l2p_table);
    free(ctx->p2l_table);

    mutex_unlock(&ctx->lock);
    mutex_cleanup(&ctx->lock);

    memset(ctx, 0, sizeof(*ctx));
}

int mapping_l2p(struct mapping_ctx *ctx, u64 lba, union ppn *ppn)
{
    if (!ctx || !ppn) {
        return HFSSS_ERR_INVAL;
    }

    if (lba >= ctx->l2p_size) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    if (!ctx->l2p_table[lba].valid) {
        mutex_unlock(&ctx->lock);
        return HFSSS_ERR_NOENT;
    }

    *ppn = ctx->l2p_table[lba].ppn;

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int mapping_p2l(struct mapping_ctx *ctx, union ppn ppn, u64 *lba)
{
    if (!ctx || !lba) {
        return HFSSS_ERR_INVAL;
    }

    u64 idx = ppn.raw % ctx->p2l_size;

    mutex_lock(&ctx->lock, 0);

    if (!ctx->p2l_table[idx].valid) {
        mutex_unlock(&ctx->lock);
        return HFSSS_ERR_NOENT;
    }

    *lba = ctx->p2l_table[idx].lba;

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int mapping_insert(struct mapping_ctx *ctx, u64 lba, union ppn ppn)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    if (lba >= ctx->l2p_size) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    /* Insert into L2P */
    ctx->l2p_table[lba].ppn = ppn;
    ctx->l2p_table[lba].valid = true;

    /* Insert into P2L (simple hash) */
    u64 idx = ppn.raw % ctx->p2l_size;
    ctx->p2l_table[idx].lba = lba;
    ctx->p2l_table[idx].valid = true;

    ctx->valid_count++;

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int mapping_remove(struct mapping_ctx *ctx, u64 lba)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    if (lba >= ctx->l2p_size) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    if (!ctx->l2p_table[lba].valid) {
        mutex_unlock(&ctx->lock);
        return HFSSS_ERR_NOENT;
    }

    /* Get PPN for P2L removal */
    union ppn ppn = ctx->l2p_table[lba].ppn;

    /* Remove from L2P */
    ctx->l2p_table[lba].valid = false;
    memset(&ctx->l2p_table[lba].ppn, 0, sizeof(ppn));

    /* Remove from P2L */
    u64 idx = ppn.raw % ctx->p2l_size;
    ctx->p2l_table[idx].valid = false;
    ctx->p2l_table[idx].lba = 0;

    ctx->valid_count--;

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int mapping_update(struct mapping_ctx *ctx, u64 lba, union ppn new_ppn, union ppn *old_ppn)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    if (lba >= ctx->l2p_size) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    /* Get old PPN if requested */
    if (old_ppn && ctx->l2p_table[lba].valid) {
        *old_ppn = ctx->l2p_table[lba].ppn;
    }

    /* If there was an old mapping, remove from P2L first */
    if (ctx->l2p_table[lba].valid) {
        union ppn old = ctx->l2p_table[lba].ppn;
        u64 old_idx = old.raw % ctx->p2l_size;
        ctx->p2l_table[old_idx].valid = false;
        ctx->valid_count--;
    }

    /* Insert new mapping */
    ctx->l2p_table[lba].ppn = new_ppn;
    ctx->l2p_table[lba].valid = true;

    u64 new_idx = new_ppn.raw % ctx->p2l_size;
    ctx->p2l_table[new_idx].lba = lba;
    ctx->p2l_table[new_idx].valid = true;
    ctx->valid_count++;

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

u64 mapping_get_valid_count(struct mapping_ctx *ctx)
{
    if (!ctx) {
        return 0;
    }

    mutex_lock(&ctx->lock, 0);
    u64 count = ctx->valid_count;
    mutex_unlock(&ctx->lock);

    return count;
}
