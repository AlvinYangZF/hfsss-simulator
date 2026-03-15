#include "ftl/gc.h"
#include "hal/hal.h"
#include <stdlib.h>
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

/*
 * Encode a physical page address into a PPN value matching the ftl.c bit layout:
 *   channel:6  chip:4  die:3  plane:2  block:12  page:10
 */
static union ppn gc_encode_ppn(u32 channel, u32 chip, u32 die, u32 plane,
                                u32 block, u32 page)
{
    union ppn ppn;
    ppn.raw = 0;
    ppn.bits.channel = channel;
    ppn.bits.chip    = chip;
    ppn.bits.die     = die;
    ppn.bits.plane   = plane;
    ppn.bits.block   = block;
    ppn.bits.page    = page;
    return ppn;
}

int gc_run(struct gc_ctx *ctx, struct block_mgr *block_mgr, struct mapping_ctx *mapping_ctx,
           void *hal_ctx)
{
    struct hal_ctx *hal = (struct hal_ctx *)hal_ctx;
    struct block_desc *victim;
    struct block_desc *dst_block = NULL;
    u32 dst_page = 0;
    u64 moved = 0;
    u64 reclaimed = 0;
    u8 *page_buf = NULL;
    bool reloc_aborted = false;
    int ret;

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
    ret = block_mark_gc(block_mgr, victim);
    if (ret != HFSSS_OK) {
        mutex_lock(&ctx->lock, 0);
        ctx->running = false;
        mutex_unlock(&ctx->lock);
        return ret;
    }

    /*
     * Relocate all live pages from the victim block before reclaiming it.
     *
     * For each page slot in the victim block:
     *   1. Derive its PPN and ask P2L for the associated LBA.
     *   2. Cross-check via L2P that the LBA still maps back to this exact
     *      PPN — if not, the page is stale (an earlier write superseded it)
     *      and can be skipped.
     *   3. For live pages: read from NAND, write to a freshly allocated
     *      destination block, and update the L2P mapping atomically.
     *
     * Only attempt relocation when both mapping and HAL contexts are
     * available (they are always passed from ftl_gc_trigger, but guard
     * against NULL for unit-test scenarios that call gc_run directly).
     */
    if (mapping_ctx && hal && hal->nand) {
        u32 pages_per_block = hal->nand->pages_per_block;
        u32 page_size       = hal->nand->page_size;

        page_buf = (u8 *)malloc(page_size);
        if (!page_buf) {
            /* Cannot relocate without a buffer — fall through to the
             * invalidation path below so we at least do not corrupt data
             * by silently reusing the block with stale L2P entries. */
            goto invalidate_mappings;
        }

        /*
         * Discover live pages by scanning the L2P table rather than relying
         * on P2L reverse lookups.  The P2L table is a flat hash table with no
         * collision resolution: two PPNs that hash to the same slot silently
         * overwrite each other, making P2L an unreliable source of truth.
         * The L2P table is authoritative — every valid entry here represents
         * a live host mapping.  We iterate all LBAs and relocate those whose
         * PPN falls inside the victim block.
         */
        for (u64 lba = 0; lba < mapping_ctx->l2p_size; lba++) {
            union ppn src_ppn;

            /* Check whether this LBA lives in the victim block. */
            if (mapping_l2p(mapping_ctx, lba, &src_ppn) != HFSSS_OK) {
                continue;
            }
            if (src_ppn.bits.channel != victim->channel ||
                src_ppn.bits.chip    != victim->chip    ||
                src_ppn.bits.die     != victim->die     ||
                src_ppn.bits.plane   != victim->plane   ||
                src_ppn.bits.block   != victim->block_id) {
                continue;
            }

            u32 pg = src_ppn.bits.page;

            /* Page is live.  Read it from NAND. */
            ret = hal_nand_read_sync(hal,
                                     victim->channel, victim->chip,
                                     victim->die, victim->plane,
                                     victim->block_id, pg,
                                     page_buf, NULL);
            if (ret != HFSSS_OK) {
                /* Unreadable page — invalidate the mapping entry so the
                 * host gets a clean error on the next read rather than
                 * silently wrong data. */
                mapping_remove(mapping_ctx, lba);
                continue;
            }

            /* Ensure we have a destination block with room. */
            if (!dst_block || dst_page >= pages_per_block) {
                if (dst_block) {
                    block_mark_closed(block_mgr, dst_block);
                }
                dst_block = block_alloc(block_mgr);
                if (!dst_block) {
                    /* Out of free blocks: abort relocation rather than
                     * discarding live mappings.  Restore the victim to
                     * CLOSED so GC can retry the block later when space
                     * becomes available. */
                    reloc_aborted = true;
                    break;
                }
                dst_page = 0;

                /* Erase the newly allocated block before writing. */
                hal_nand_erase_sync(hal,
                                    dst_block->channel, dst_block->chip,
                                    dst_block->die, dst_block->plane,
                                    dst_block->block_id);
            }

            /* Write the live page to the destination block. */
            ret = hal_nand_program_sync(hal,
                                        dst_block->channel, dst_block->chip,
                                        dst_block->die, dst_block->plane,
                                        dst_block->block_id, dst_page,
                                        page_buf, NULL);
            if (ret != HFSSS_OK) {
                mapping_remove(mapping_ctx, lba);
                dst_page++;
                continue;
            }

            /* Build the new PPN and update L2P atomically. */
            union ppn dst_ppn = gc_encode_ppn(dst_block->channel,
                                               dst_block->chip,
                                               dst_block->die,
                                               dst_block->plane,
                                               dst_block->block_id,
                                               dst_page);
            union ppn ignored_old;
            mapping_update(mapping_ctx, lba, dst_ppn, &ignored_old);

            dst_block->valid_page_count++;
            dst_page++;
            moved++;
        }

        free(page_buf);
        page_buf = NULL;

        if (reloc_aborted) {
            /* Close any partially-written destination block. */
            if (dst_block && dst_block->state == FTL_BLOCK_OPEN) {
                block_mark_closed(block_mgr, dst_block);
            }
            /* Restore victim to CLOSED — do not erase it; live data remains. */
            victim->state = FTL_BLOCK_CLOSED;
            mutex_lock(&ctx->lock, 0);
            ctx->running = false;
            mutex_unlock(&ctx->lock);
            return HFSSS_ERR_NOSPC;
        }

        /* Close the destination block if it was used. */
        if (dst_block && dst_block->state == FTL_BLOCK_OPEN) {
            block_mark_closed(block_mgr, dst_block);
        }

        goto erase_and_free;
    }

invalidate_mappings:
    /*
     * Fallback: no HAL or mapping available.  Scan the L2P table and
     * invalidate every entry that points into the victim block.  This
     * turns a silent corruption into a detectable NOENT on read.
     */
    if (mapping_ctx) {
        for (u64 lba = 0; lba < mapping_ctx->l2p_size; lba++) {
            union ppn ppn;
            if (mapping_l2p(mapping_ctx, lba, &ppn) != HFSSS_OK) {
                continue;
            }
            if (ppn.bits.channel == victim->channel &&
                ppn.bits.chip    == victim->chip    &&
                ppn.bits.die     == victim->die     &&
                ppn.bits.plane   == victim->plane   &&
                ppn.bits.block   == victim->block_id) {
                mapping_remove(mapping_ctx, lba);
            }
        }
    }

erase_and_free:
    /* Erase the victim block on NAND before returning it to the free pool. */
    if (hal) {
        hal_nand_erase_sync(hal,
                            victim->channel, victim->chip,
                            victim->die, victim->plane,
                            victim->block_id);
    }

    /* Track GC write pages for WAF */
    ctx->gc_write_pages += moved;

    /* Return victim block to the free pool. */
    victim->valid_page_count   = 0;
    victim->invalid_page_count = 0;
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
