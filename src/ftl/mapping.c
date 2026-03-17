#include "ftl/mapping.h"
#include <stdlib.h>
#include <string.h>

/*
 * p2l_idx — compute the P2L table index for a given PPN.
 *
 * The PPN raw value encodes page bits at positions 27-36, which lie above
 * the 2^24 boundary.  A plain modulo by P2L_TABLE_SIZE (2^24) would mask
 * out the page field entirely, causing every page in the same block to
 * collide in the P2L table.  The XOR fold brings bits 24 and above back
 * into the lower 24 bits before taking the modulo, ensuring that distinct
 * pages within the same block produce distinct table indices.
 */
static inline u64 p2l_idx(union ppn ppn, u64 table_size)
{
    return (ppn.raw ^ (ppn.raw >> 24)) % table_size;
}

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

    u64 idx = p2l_idx(ppn, ctx->p2l_size);

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
    u64 idx = p2l_idx(ppn, ctx->p2l_size);
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
    u64 idx = p2l_idx(ppn, ctx->p2l_size);
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
        u64 old_idx = p2l_idx(old, ctx->p2l_size);
        ctx->p2l_table[old_idx].valid = false;
        ctx->valid_count--;
    }

    /* Insert new mapping */
    ctx->l2p_table[lba].ppn = new_ppn;
    ctx->l2p_table[lba].valid = true;

    u64 new_idx = p2l_idx(new_ppn, ctx->p2l_size);
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

/* Set L2P entry directly without touching P2L or valid_count.
 * Used during checkpoint recovery to bulk-load L2P before rebuilding P2L. */
int mapping_direct_set(struct mapping_ctx *ctx, u64 lba, union ppn ppn)
{
    if (!ctx || lba >= ctx->l2p_size) {
        return HFSSS_ERR_INVAL;
    }
    ctx->l2p_table[lba].ppn = ppn;
    ctx->l2p_table[lba].valid = true;
    return HFSSS_OK;
}

/* Clear L2P entry directly without touching P2L or valid_count.
 * Used during journal replay for TRIM operations. */
int mapping_direct_clear(struct mapping_ctx *ctx, u64 lba)
{
    if (!ctx || lba >= ctx->l2p_size) {
        return HFSSS_ERR_INVAL;
    }
    ctx->l2p_table[lba].valid = false;
    memset(&ctx->l2p_table[lba].ppn, 0, sizeof(union ppn));
    return HFSSS_OK;
}

/* Rebuild P2L table and valid_count from the current L2P table.
 * Called after checkpoint load + journal replay to reconstruct derived state. */
int mapping_rebuild_p2l(struct mapping_ctx *ctx)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    /* Clear P2L table */
    memset(ctx->p2l_table, 0, ctx->p2l_size * sizeof(struct p2l_entry));
    ctx->valid_count = 0;

    /* Scan L2P and populate P2L */
    for (u64 lba = 0; lba < ctx->l2p_size; lba++) {
        if (ctx->l2p_table[lba].valid) {
            union ppn ppn = ctx->l2p_table[lba].ppn;
            u64 idx = p2l_idx(ppn, ctx->p2l_size);
            ctx->p2l_table[idx].lba = lba;
            ctx->p2l_table[idx].valid = true;
            ctx->valid_count++;
        }
    }

    mutex_unlock(&ctx->lock);
    return HFSSS_OK;
}
