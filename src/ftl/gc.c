#include "ftl/gc.h"
#include "ftl/taa.h"
#include "hal/hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static u64 gc_dbg_read_fail_removes = 0;
static u64 gc_dbg_write_fail_removes = 0;

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
    ctx->dst_block = NULL;
    ctx->dst_page = 0;

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
    return ctx && free_blocks <= ctx->threshold;
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
    u64 moved = 0;
    u64 reclaimed = 0;
    u8 *page_buf = NULL;
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
     * The destination block (ctx->dst_block) is kept open across GC cycles
     * so that multiple victims can share a single destination block.  This
     * achieves net-positive block reclamation even at high utilisation
     * ratios: if each victim contributes V valid pages (V < pages_per_block),
     * ceil(pages_per_block / V) victims fill one destination block and free
     * the same number of victim blocks, for a net gain of
     * ceil(pages_per_block / V) - 1 blocks per destination cycle.
     *
     * On resource exhaustion (no free block for a new destination) the
     * relocation stops after partially processing the current victim.  The
     * L2P updates already applied remain valid; the victim is restored to the
     * closed list with fewer live pages than before and will be retried on
     * the next GC call.
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

        bool reloc_aborted = false;

        /*
         * Scan the L2P table to discover live pages in the victim block.
         * The L2P table is authoritative; P2L reverse lookups are skipped
         * because the flat hash table has no collision resolution.
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

            /* Page is live.  Read it from NAND (spare area carries PI metadata). */
            u8 spare_buf[64];
            memset(spare_buf, 0xFF, sizeof(spare_buf));
            ret = hal_nand_read_sync(hal,
                                     victim->channel, victim->chip,
                                     victim->die, victim->plane,
                                     victim->block_id, pg,
                                     page_buf, spare_buf);
            if (ret != HFSSS_OK) {
                gc_dbg_read_fail_removes++;
                if (gc_dbg_read_fail_removes <= 3) {
                    fprintf(stderr, "[GC_DBG] read fail: lba=%llu pg=%u "
                            "victim(ch=%u blk=%u) ret=%d\n",
                            (unsigned long long)lba, pg,
                            victim->channel, victim->block_id, ret);
                }
                mapping_remove(mapping_ctx, lba);
                continue;
            }

            /* Ensure the persistent destination block has room. */
            if (!ctx->dst_block || ctx->dst_page >= pages_per_block) {
                if (ctx->dst_block) {
                    /* Current destination is full; close it. */
                    block_mark_closed(block_mgr, ctx->dst_block);
                    ctx->dst_block = NULL;
                }
                ctx->dst_block = block_alloc_for_channel_plane(block_mgr,
                                                               victim->channel,
                                                               victim->plane);
                if (!ctx->dst_block) {
                    /*
                     * No free blocks available for a new destination.
                     * L2P updates applied so far remain valid.  Restore
                     * the victim to CLOSED so it can be retried later.
                     */
                    reloc_aborted = true;
                    break;
                }
                ctx->dst_page = 0;

                /* Erase the newly allocated destination block. */
                ret = hal_nand_erase_sync(hal,
                                          ctx->dst_block->channel,
                                          ctx->dst_block->chip,
                                          ctx->dst_block->die,
                                          ctx->dst_block->plane,
                                          ctx->dst_block->block_id);
                if (ret != HFSSS_OK) {
                    block_mark_bad(block_mgr, ctx->dst_block);
                    ctx->dst_block = NULL;
                    reloc_aborted = true;
                    break;
                }
            }

            /* Write the live page to the destination block.
             * Pass through spare_buf to preserve T10 PI metadata. */
            ret = hal_nand_program_sync(hal,
                                        ctx->dst_block->channel,
                                        ctx->dst_block->chip,
                                        ctx->dst_block->die,
                                        ctx->dst_block->plane,
                                        ctx->dst_block->block_id,
                                        ctx->dst_page,
                                        page_buf, spare_buf);
            if (ret != HFSSS_OK) {
                gc_dbg_write_fail_removes++;
                if (gc_dbg_write_fail_removes <= 3) {
                    fprintf(stderr, "[GC_DBG] write fail: lba=%llu dst_pg=%u "
                            "dst(ch=%u blk=%u) ret=%d\n",
                            (unsigned long long)lba, ctx->dst_page,
                            ctx->dst_block->channel, ctx->dst_block->block_id,
                            ret);
                }
                mapping_remove(mapping_ctx, lba);
                ctx->dst_page++;
                continue;
            }

            /* Update L2P to point to the new physical location. */
            union ppn dst_ppn = gc_encode_ppn(ctx->dst_block->channel,
                                               ctx->dst_block->chip,
                                               ctx->dst_block->die,
                                               ctx->dst_block->plane,
                                               ctx->dst_block->block_id,
                                               ctx->dst_page);
            union ppn old_ppn;
            mapping_update(mapping_ctx, lba, dst_ppn, &old_ppn);

            atomic_fetch_add_explicit(&ctx->dst_block->valid_page_count, 1,
                                      memory_order_relaxed);
            ctx->dst_page++;
            moved++;
        }

        free(page_buf);
        page_buf = NULL;

        if (reloc_aborted) {
            /*
             * Partial relocation: L2P is consistent for the pages already
             * moved.  Restore the victim to CLOSED so it can be picked up
             * again once more space is available.
             */
            block_unmark_gc(block_mgr, victim);
            mutex_lock(&ctx->lock, 0);
            ctx->gc_write_pages += moved;
            ctx->gc_count++;
            ctx->moved_pages += moved;
            ctx->running = false;
            mutex_unlock(&ctx->lock);
            return HFSSS_ERR_NOSPC;
        }

        /*
         * Do NOT close ctx->dst_block here — it stays open so the next GC
         * cycle can continue filling it, achieving net-positive reclamation.
         */
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
    /* Erase the victim block on NAND.  If the erase fails the block has
     * worn out; permanently retire it rather than returning it to the free
     * pool where it would cause repeated IO errors on future writes. */
    if (hal) {
        ret = hal_nand_erase_sync(hal,
                                  victim->channel, victim->chip,
                                  victim->die, victim->plane,
                                  victim->block_id);
        if (ret != HFSSS_OK) {
            block_mark_bad(block_mgr, victim);
            /* Track GC write pages for WAF */
            ctx->gc_write_pages += moved;
            mutex_lock(&ctx->lock, 0);
            ctx->gc_count++;
            ctx->moved_pages += moved;
            ctx->running = false;
            mutex_unlock(&ctx->lock);
            return HFSSS_OK;
        }
    }

    /* Track GC write pages for WAF */
    ctx->gc_write_pages += moved;

    /* Return victim block to the free pool. */
    atomic_store_explicit(&victim->valid_page_count, 0, memory_order_relaxed);
    atomic_store_explicit(&victim->invalid_page_count, 0, memory_order_relaxed);
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

int gc_run_mt(struct gc_ctx *ctx, struct block_mgr *block_mgr,
              struct taa_ctx *taa, void *hal_ctx)
{
    struct hal_ctx *hal = (struct hal_ctx *)hal_ctx;
    struct block_desc *victim;
    u64 moved = 0;
    u64 reclaimed = 0;
    u8 *page_buf = NULL;
    int ret;

    if (!ctx || !block_mgr || !taa) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);
    if (ctx->running) {
        mutex_unlock(&ctx->lock);
        return HFSSS_ERR_BUSY;
    }
    ctx->running = true;
    mutex_unlock(&ctx->lock);

    victim = block_find_victim(block_mgr, ctx->policy);
    if (!victim) {
        mutex_lock(&ctx->lock, 0);
        ctx->running = false;
        mutex_unlock(&ctx->lock);
        return HFSSS_ERR_NOENT;
    }

    ret = block_mark_gc(block_mgr, victim);
    if (ret != HFSSS_OK) {
        mutex_lock(&ctx->lock, 0);
        ctx->running = false;
        mutex_unlock(&ctx->lock);
        return ret;
    }

    if (!hal || !hal->nand) {
        goto erase_and_free_mt;
    }

    u32 pages_per_block = hal->nand->pages_per_block;
    u32 page_size       = hal->nand->page_size;

    page_buf = (u8 *)malloc(page_size);
    if (!page_buf) {
        goto erase_and_free_mt;
    }

    bool reloc_aborted = false;

    for (u64 lba = 0; lba < taa->total_lbas; lba++) {
        union ppn src_ppn;

        if (taa_lookup(taa, lba, &src_ppn) != HFSSS_OK) {
            continue;
        }
        if (src_ppn.bits.channel != victim->channel ||
            src_ppn.bits.chip    != victim->chip    ||
            src_ppn.bits.die     != victim->die     ||
            src_ppn.bits.plane   != victim->plane   ||
            src_ppn.bits.block   != victim->block_id) {
            continue;
        }

        u8 spare_buf[64];
        memset(spare_buf, 0xFF, sizeof(spare_buf));
        ret = hal_nand_read_sync(hal,
                                 victim->channel, victim->chip,
                                 victim->die, victim->plane,
                                 victim->block_id, src_ppn.bits.page,
                                 page_buf, spare_buf);
        if (ret != HFSSS_OK) {
            taa_remove(taa, lba);
            continue;
        }

        if (!ctx->dst_block || ctx->dst_page >= pages_per_block) {
            if (ctx->dst_block) {
                block_mark_closed(block_mgr, ctx->dst_block);
                ctx->dst_block = NULL;
            }
            ctx->dst_block = block_alloc_for_channel_plane(block_mgr,
                                                           victim->channel,
                                                           victim->plane);
            if (!ctx->dst_block) {
                reloc_aborted = true;
                break;
            }
            ctx->dst_page = 0;

            ret = hal_nand_erase_sync(hal,
                                      ctx->dst_block->channel,
                                      ctx->dst_block->chip,
                                      ctx->dst_block->die,
                                      ctx->dst_block->plane,
                                      ctx->dst_block->block_id);
            if (ret != HFSSS_OK) {
                block_mark_bad(block_mgr, ctx->dst_block);
                ctx->dst_block = NULL;
                reloc_aborted = true;
                break;
            }
        }

        ret = hal_nand_program_sync(hal,
                                    ctx->dst_block->channel,
                                    ctx->dst_block->chip,
                                    ctx->dst_block->die,
                                    ctx->dst_block->plane,
                                    ctx->dst_block->block_id,
                                    ctx->dst_page,
                                    page_buf, spare_buf);
        if (ret != HFSSS_OK) {
            taa_remove(taa, lba);
            ctx->dst_page++;
            continue;
        }

        union ppn dst_ppn;
        dst_ppn.raw = 0;
        dst_ppn.bits.channel = ctx->dst_block->channel;
        dst_ppn.bits.chip    = ctx->dst_block->chip;
        dst_ppn.bits.die     = ctx->dst_block->die;
        dst_ppn.bits.plane   = ctx->dst_block->plane;
        dst_ppn.bits.block   = ctx->dst_block->block_id;
        dst_ppn.bits.page    = ctx->dst_page;

        union ppn old_ppn;
        taa_update(taa, lba, dst_ppn, &old_ppn);

        atomic_fetch_add_explicit(&ctx->dst_block->valid_page_count, 1,
                                  memory_order_relaxed);
        ctx->dst_page++;
        moved++;
    }

    free(page_buf);

    if (reloc_aborted) {
        block_unmark_gc(block_mgr, victim);
        mutex_lock(&ctx->lock, 0);
        ctx->gc_write_pages += moved;
        ctx->gc_count++;
        ctx->moved_pages += moved;
        ctx->running = false;
        mutex_unlock(&ctx->lock);
        return HFSSS_ERR_NOSPC;
    }

erase_and_free_mt:
    if (hal) {
        ret = hal_nand_erase_sync(hal,
                                  victim->channel, victim->chip,
                                  victim->die, victim->plane,
                                  victim->block_id);
        if (ret != HFSSS_OK) {
            block_mark_bad(block_mgr, victim);
            ctx->gc_write_pages += moved;
            mutex_lock(&ctx->lock, 0);
            ctx->gc_count++;
            ctx->moved_pages += moved;
            ctx->running = false;
            mutex_unlock(&ctx->lock);
            return HFSSS_OK;
        }
    }

    ctx->gc_write_pages += moved;
    atomic_store_explicit(&victim->valid_page_count, 0, memory_order_relaxed);
    atomic_store_explicit(&victim->invalid_page_count, 0, memory_order_relaxed);
    ret = block_free(block_mgr, victim);
    if (ret == HFSSS_OK) {
        reclaimed = 1;
    }

    mutex_lock(&ctx->lock, 0);
    ctx->gc_count++;
    ctx->moved_pages += moved;
    ctx->reclaimed_blocks += reclaimed;
    ctx->running = false;
    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

void gc_print_debug_stats(void)
{
    fprintf(stderr, "[GC_DBG] mapping_remove: read_fail=%llu write_fail=%llu\n",
            (unsigned long long)gc_dbg_read_fail_removes,
            (unsigned long long)gc_dbg_write_fail_removes);
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

/*
 * Close the persistent GC destination block if one is currently open.
 * Must be called during flush and cleanup so the block enters the closed
 * list and becomes eligible for future GC victim selection.
 */
void gc_flush_dst(struct gc_ctx *ctx, struct block_mgr *block_mgr)
{
    if (!ctx || !block_mgr || !ctx->dst_block) {
        return;
    }

    if (ctx->dst_block->state == FTL_BLOCK_OPEN) {
        block_mark_closed(block_mgr, ctx->dst_block);
    }

    ctx->dst_block = NULL;
    ctx->dst_page  = 0;
}
