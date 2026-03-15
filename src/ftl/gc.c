#include "ftl/gc.h"
#include <string.h>

int gc_init(struct gc_ctx *ctx, enum gc_policy policy, u32 threshold, u32 hiwater, u32 lowater)
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

    ctx->policy = policy;
    ctx->threshold = threshold;
    ctx->hiwater = hiwater;
    ctx->lowater = lowater;
    ctx->victim = NULL;
    ctx->running = false;
    ctx->gc_count = 0;
    ctx->moved_pages = 0;
    ctx->reclaimed_blocks = 0;
    ctx->gc_write_pages = 0;

    return HFSSS_OK;
}

void gc_cleanup(struct gc_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->lock, 0);

    mutex_unlock(&ctx->lock);
    mutex_cleanup(&ctx->lock);

    memset(ctx, 0, sizeof(*ctx));
}

bool gc_should_trigger(struct gc_ctx *ctx, u64 free_blocks)
{
    if (!ctx) {
        return false;
    }

    mutex_lock(&ctx->lock, 0);

    bool should_trigger = (free_blocks <= ctx->threshold);

    mutex_unlock(&ctx->lock);

    return should_trigger;
}

int gc_run(struct gc_ctx *ctx, struct block_mgr *block_mgr, struct mapping_ctx *mapping_ctx,
           void *hal_ctx)
{
    struct block_desc *victim;
    u64 moved = 0;
    u64 reclaimed = 0;
    (void)mapping_ctx;
    (void)hal_ctx;

    if (!ctx || !block_mgr) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    if (ctx->running) {
        mutex_unlock(&ctx->lock);
        return HFSSS_ERR_BUSY;
    }

    ctx->running = true;
    mutex_unlock(&ctx->lock);

    /* Find victim block */
    victim = block_find_victim(block_mgr, ctx->policy);
    if (!victim) {
        mutex_lock(&ctx->lock, 0);
        ctx->running = false;
        mutex_unlock(&ctx->lock);
        return HFSSS_ERR_NOENT;
    }

    /* Mark block for GC */
    int ret = block_mark_gc(block_mgr, victim);
    if (ret != HFSSS_OK) {
        mutex_lock(&ctx->lock, 0);
        ctx->running = false;
        mutex_unlock(&ctx->lock);
        return ret;
    }

    /* In a real implementation, we would:
     * 1. Iterate through all valid pages in the victim block
     * 2. For each valid page, read it from media
     * 3. Allocate a new page in an open block
     * 4. Write the page to the new location
     * 5. Update the L2P mapping
     * 6. Count the moved pages
     */

    /* For this placeholder implementation, just mark the pages as invalid
     * and reclaim the block directly.
     */
    moved = victim->valid_page_count;
    victim->valid_page_count = 0;
    victim->invalid_page_count = 0;

    /* Track GC write pages for WAF */
    ctx->gc_write_pages += moved;

    /* Free the victim block */
    ret = block_free(block_mgr, victim);
    if (ret == HFSSS_OK) {
        reclaimed = 1;
    }

    /* Update GC stats */
    mutex_lock(&ctx->lock, 0);
    ctx->gc_count++;
    ctx->moved_pages += moved;
    ctx->reclaimed_blocks += reclaimed;
    ctx->running = false;
    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

void gc_get_stats(struct gc_ctx *ctx, u64 *gc_count, u64 *moved_pages, u64 *reclaimed_blocks, u64 *gc_write_pages)
{
    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->lock, 0);

    if (gc_count) *gc_count = ctx->gc_count;
    if (moved_pages) *moved_pages = ctx->moved_pages;
    if (reclaimed_blocks) *reclaimed_blocks = ctx->reclaimed_blocks;
    if (gc_write_pages) *gc_write_pages = ctx->gc_write_pages;

    mutex_unlock(&ctx->lock);
}
