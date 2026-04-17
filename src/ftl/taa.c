#include "ftl/taa.h"
#include <stdlib.h>
#include <string.h>

static inline u32 taa_shard_id(struct taa_ctx *ctx, u64 lba)
{
    return (u32)(lba / ctx->lbas_per_shard);
}

static inline u64 p2l_idx(union ppn ppn, u64 table_size)
{
    return (ppn.raw ^ (ppn.raw >> 24)) % table_size;
}

int taa_init(struct taa_ctx *ctx, u64 total_lbas, u64 total_pages,
             u32 num_shards)
{
    u32 i;

    if (!ctx || total_lbas == 0 || num_shards == 0) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    ctx->num_shards = num_shards;
    ctx->total_lbas = total_lbas;
    ctx->total_pages = total_pages;
    ctx->lbas_per_shard = (total_lbas + num_shards - 1) / num_shards;

    ctx->shards = (struct taa_shard *)calloc(num_shards,
                                              sizeof(struct taa_shard));
    if (!ctx->shards) {
        return HFSSS_ERR_NOMEM;
    }

    u64 p2l_per_shard = (total_pages + num_shards - 1) / num_shards;

    for (i = 0; i < num_shards; i++) {
        struct taa_shard *s = &ctx->shards[i];
        int ret = mutex_init(&s->lock);
        if (ret != HFSSS_OK) {
            goto fail;
        }

        s->base_lba = (u64)i * ctx->lbas_per_shard;
        s->lba_count = ctx->lbas_per_shard;
        if (s->base_lba + s->lba_count > total_lbas) {
            s->lba_count = total_lbas - s->base_lba;
        }

        s->p2l_base = (u64)i * p2l_per_shard;
        s->p2l_count = p2l_per_shard;

        s->l2p = (struct l2p_entry *)calloc(s->lba_count,
                                             sizeof(struct l2p_entry));
        if (!s->l2p) {
            goto fail;
        }

        s->p2l = (struct p2l_entry *)calloc(s->p2l_count,
                                             sizeof(struct p2l_entry));
        if (!s->p2l) {
            goto fail;
        }
    }

    ctx->initialized = true;
    return HFSSS_OK;

fail:
    taa_cleanup(ctx);
    return HFSSS_ERR_NOMEM;
}

void taa_cleanup(struct taa_ctx *ctx)
{
    u32 i;
    if (!ctx || !ctx->shards) {
        return;
    }
    for (i = 0; i < ctx->num_shards; i++) {
        free(ctx->shards[i].l2p);
        free(ctx->shards[i].p2l);
        mutex_cleanup(&ctx->shards[i].lock);
    }
    free(ctx->shards);
    memset(ctx, 0, sizeof(*ctx));
}

int taa_lookup(struct taa_ctx *ctx, u64 lba, union ppn *ppn_out)
{
    if (!ctx || !ctx->initialized || !ppn_out) {
        return HFSSS_ERR_INVAL;
    }
    if (lba >= ctx->total_lbas) {
        return HFSSS_ERR_INVAL;
    }

    u32 sid = taa_shard_id(ctx, lba);
    struct taa_shard *s = &ctx->shards[sid];
    u64 local_lba = lba - s->base_lba;

    mutex_lock(&s->lock, 0);
    s->lookup_count++;

    if (!s->l2p[local_lba].valid) {
        mutex_unlock(&s->lock);
        return HFSSS_ERR_NOENT;
    }

    *ppn_out = s->l2p[local_lba].ppn;
    mutex_unlock(&s->lock);
    return HFSSS_OK;
}

int taa_insert(struct taa_ctx *ctx, u64 lba, union ppn ppn)
{
    if (!ctx || !ctx->initialized || lba >= ctx->total_lbas) {
        return HFSSS_ERR_INVAL;
    }

    u32 sid = taa_shard_id(ctx, lba);
    struct taa_shard *s = &ctx->shards[sid];
    u64 local_lba = lba - s->base_lba;

    mutex_lock(&s->lock, 0);

    s->l2p[local_lba].ppn = ppn;
    s->l2p[local_lba].valid = true;

    u64 idx = p2l_idx(ppn, s->p2l_count);
    s->p2l[idx].lba = lba;
    s->p2l[idx].valid = true;
    s->valid_count++;

    mutex_unlock(&s->lock);
    return HFSSS_OK;
}

int taa_remove(struct taa_ctx *ctx, u64 lba)
{
    if (!ctx || !ctx->initialized || lba >= ctx->total_lbas) {
        return HFSSS_ERR_INVAL;
    }

    u32 sid = taa_shard_id(ctx, lba);
    struct taa_shard *s = &ctx->shards[sid];
    u64 local_lba = lba - s->base_lba;

    mutex_lock(&s->lock, 0);

    if (!s->l2p[local_lba].valid) {
        mutex_unlock(&s->lock);
        return HFSSS_ERR_NOENT;
    }

    union ppn ppn = s->l2p[local_lba].ppn;
    s->l2p[local_lba].valid = false;
    memset(&s->l2p[local_lba].ppn, 0, sizeof(union ppn));

    u64 idx = p2l_idx(ppn, s->p2l_count);
    s->p2l[idx].valid = false;
    s->p2l[idx].lba = 0;
    s->valid_count--;

    mutex_unlock(&s->lock);
    return HFSSS_OK;
}

int taa_update(struct taa_ctx *ctx, u64 lba, union ppn new_ppn,
               union ppn *old_ppn)
{
    if (!ctx || !ctx->initialized || lba >= ctx->total_lbas) {
        return HFSSS_ERR_INVAL;
    }

    u32 sid = taa_shard_id(ctx, lba);
    struct taa_shard *s = &ctx->shards[sid];
    u64 local_lba = lba - s->base_lba;

    mutex_lock(&s->lock, 0);

    if (old_ppn && s->l2p[local_lba].valid) {
        *old_ppn = s->l2p[local_lba].ppn;
    }

    /* Remove old P2L entry */
    if (s->l2p[local_lba].valid) {
        union ppn old = s->l2p[local_lba].ppn;
        u64 old_idx = p2l_idx(old, s->p2l_count);
        s->p2l[old_idx].valid = false;
        s->valid_count--;
    }

    /* Insert new mapping */
    s->l2p[local_lba].ppn = new_ppn;
    s->l2p[local_lba].valid = true;

    u64 new_idx = p2l_idx(new_ppn, s->p2l_count);
    s->p2l[new_idx].lba = lba;
    s->p2l[new_idx].valid = true;
    s->valid_count++;

    mutex_unlock(&s->lock);
    return HFSSS_OK;
}

int taa_update_if_equal(struct taa_ctx *ctx, u64 lba, union ppn expected_old,
                        union ppn new_ppn, bool *updated_out)
{
    if (updated_out) {
        *updated_out = false;
    }
    if (!ctx || !ctx->initialized || lba >= ctx->total_lbas) {
        return HFSSS_ERR_INVAL;
    }

    u32 sid = taa_shard_id(ctx, lba);
    struct taa_shard *s = &ctx->shards[sid];
    u64 local_lba = lba - s->base_lba;

    mutex_lock(&s->lock, 0);

    /*
     * Reject the swap if the slot is empty, holds a different mapping, or
     * the expected mapping is zero (uninitialized). A zero expected_old
     * means the caller is trying to conditionally install a brand-new
     * mapping — use taa_insert for that path, not this one.
     */
    if (!s->l2p[local_lba].valid || s->l2p[local_lba].ppn.raw != expected_old.raw) {
        mutex_unlock(&s->lock);
        return HFSSS_OK;
    }

    /* Remove P2L entry that matched expected_old. */
    u64 old_p2l_idx = p2l_idx(expected_old, s->p2l_count);
    s->p2l[old_p2l_idx].valid = false;
    s->valid_count--;

    /* Install the new mapping. */
    s->l2p[local_lba].ppn = new_ppn;
    s->l2p[local_lba].valid = true;

    u64 new_idx = p2l_idx(new_ppn, s->p2l_count);
    s->p2l[new_idx].lba = lba;
    s->p2l[new_idx].valid = true;
    s->valid_count++;

    mutex_unlock(&s->lock);

    if (updated_out) {
        *updated_out = true;
    }
    return HFSSS_OK;
}

int taa_reverse_lookup(struct taa_ctx *ctx, union ppn ppn, u64 *lba_out)
{
    if (!ctx || !ctx->initialized || !lba_out) {
        return HFSSS_ERR_INVAL;
    }

    /* P2L entries are distributed across shards — scan all shards */
    for (u32 i = 0; i < ctx->num_shards; i++) {
        struct taa_shard *s = &ctx->shards[i];
        u64 idx = p2l_idx(ppn, s->p2l_count);

        mutex_lock(&s->lock, 0);
        if (s->p2l[idx].valid) {
            *lba_out = s->p2l[idx].lba;
            mutex_unlock(&s->lock);
            return HFSSS_OK;
        }
        mutex_unlock(&s->lock);
    }
    return HFSSS_ERR_NOENT;
}

int taa_direct_set(struct taa_ctx *ctx, u64 lba, union ppn ppn)
{
    if (!ctx || !ctx->initialized || lba >= ctx->total_lbas) {
        return HFSSS_ERR_INVAL;
    }
    u32 sid = taa_shard_id(ctx, lba);
    struct taa_shard *s = &ctx->shards[sid];
    u64 local_lba = lba - s->base_lba;
    s->l2p[local_lba].ppn = ppn;
    s->l2p[local_lba].valid = true;
    return HFSSS_OK;
}

int taa_direct_clear(struct taa_ctx *ctx, u64 lba)
{
    if (!ctx || !ctx->initialized || lba >= ctx->total_lbas) {
        return HFSSS_ERR_INVAL;
    }
    u32 sid = taa_shard_id(ctx, lba);
    struct taa_shard *s = &ctx->shards[sid];
    u64 local_lba = lba - s->base_lba;
    s->l2p[local_lba].valid = false;
    memset(&s->l2p[local_lba].ppn, 0, sizeof(union ppn));
    return HFSSS_OK;
}

int taa_rebuild_p2l(struct taa_ctx *ctx)
{
    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    for (u32 i = 0; i < ctx->num_shards; i++) {
        struct taa_shard *s = &ctx->shards[i];
        mutex_lock(&s->lock, 0);

        memset(s->p2l, 0, s->p2l_count * sizeof(struct p2l_entry));
        s->valid_count = 0;

        for (u64 j = 0; j < s->lba_count; j++) {
            if (s->l2p[j].valid) {
                union ppn ppn = s->l2p[j].ppn;
                u64 idx = p2l_idx(ppn, s->p2l_count);
                s->p2l[idx].lba = s->base_lba + j;
                s->p2l[idx].valid = true;
                s->valid_count++;
            }
        }

        mutex_unlock(&s->lock);
    }
    return HFSSS_OK;
}

u64 taa_get_valid_count(struct taa_ctx *ctx)
{
    if (!ctx || !ctx->initialized) {
        return 0;
    }
    u64 total = 0;
    for (u32 i = 0; i < ctx->num_shards; i++) {
        total += ctx->shards[i].valid_count;
    }
    return total;
}

void taa_get_stats(struct taa_ctx *ctx, u64 *total_lookups,
                   u64 *total_conflicts)
{
    if (!ctx || !ctx->initialized) {
        return;
    }
    u64 lookups = 0, conflicts = 0;
    for (u32 i = 0; i < ctx->num_shards; i++) {
        lookups += ctx->shards[i].lookup_count;
        conflicts += ctx->shards[i].conflict_count;
    }
    if (total_lookups) *total_lookups = lookups;
    if (total_conflicts) *total_conflicts = conflicts;
}
