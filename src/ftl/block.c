#include "ftl/block.h"
#include "ftl/gc.h"
#include <stdlib.h>
#include <string.h>

#ifndef U64_MAX
#define U64_MAX ((u64)-1)
#endif

/* Internal helper functions */
static void block_list_add_head(struct block_desc **list, struct block_desc *block);
static void block_list_remove(struct block_desc **list, struct block_desc *block);

static void block_list_add_head(struct block_desc **list, struct block_desc *block)
{
    if (!list || !block) {
        return;
    }

    block->next = *list;
    block->prev = NULL;

    if (*list) {
        (*list)->prev = block;
    }

    *list = block;
}


static void block_list_remove(struct block_desc **list, struct block_desc *block)
{
    if (!list || !block) {
        return;
    }

    if (block->prev) {
        block->prev->next = block->next;
    } else {
        /* Block was head of list */
        *list = block->next;
    }

    if (block->next) {
        block->next->prev = block->prev;
    }

    block->next = NULL;
    block->prev = NULL;
}

int block_mgr_init(struct block_mgr *mgr, u32 channel_count, u32 chips_per_channel,
                   u32 dies_per_chip, u32 planes_per_die, u32 blocks_per_plane)
{
    u64 total_blocks;
    u32 ch, chip, die, plane, blk;
    u64 idx = 0;
    int ret;

    if (!mgr) {
        return HFSSS_ERR_INVAL;
    }

    memset(mgr, 0, sizeof(*mgr));

    /* Initialize lock */
    ret = mutex_init(&mgr->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Store geometry for O(1) coordinate lookups */
    mgr->channel_count      = channel_count;
    mgr->chips_per_channel  = chips_per_channel;
    mgr->dies_per_chip      = dies_per_chip;
    mgr->planes_per_die     = planes_per_die;
    mgr->blocks_per_plane   = blocks_per_plane;

    /* Calculate total blocks */
    total_blocks = (u64)channel_count * chips_per_channel * dies_per_chip * planes_per_die * blocks_per_plane;
    mgr->total_blocks = total_blocks;

    /* Allocate block descriptors */
    mgr->blocks = (struct block_desc *)calloc(total_blocks, sizeof(struct block_desc));
    if (!mgr->blocks) {
        mutex_cleanup(&mgr->lock);
        return HFSSS_ERR_NOMEM;
    }

    /* Initialize all blocks */
    for (ch = 0; ch < channel_count; ch++) {
        for (chip = 0; chip < chips_per_channel; chip++) {
            for (die = 0; die < dies_per_chip; die++) {
                for (plane = 0; plane < planes_per_die; plane++) {
                    for (blk = 0; blk < blocks_per_plane; blk++) {
                        struct block_desc *block = &mgr->blocks[idx];

                        block->channel = ch;
                        block->chip = chip;
                        block->die = die;
                        block->plane = plane;
                        block->block_id = blk;
                        block->state = FTL_BLOCK_FREE;
                        block->valid_page_count = 0;
                        block->invalid_page_count = 0;
                        block->erase_count = 0;
                        block->last_write_ts = 0;
                        block->cost = 0;

                        /* Add to free list */
                        block_list_add_head(&mgr->free_list, block);
                        mgr->free_blocks++;

                        idx++;
                    }
                }
            }
        }
    }

    return HFSSS_OK;
}

void block_mgr_cleanup(struct block_mgr *mgr)
{
    if (!mgr) {
        return;
    }

    mutex_lock(&mgr->lock, 0);

    free(mgr->blocks);

    mutex_unlock(&mgr->lock);
    mutex_cleanup(&mgr->lock);

    memset(mgr, 0, sizeof(*mgr));
}

struct block_desc *block_alloc(struct block_mgr *mgr)
{
    struct block_desc *block;

    if (!mgr) {
        return NULL;
    }

    mutex_lock(&mgr->lock, 0);

    if (!mgr->free_list) {
        mutex_unlock(&mgr->lock);
        return NULL;
    }

    /* Get first free block */
    block = mgr->free_list;
    block_list_remove(&mgr->free_list, block);
    mgr->free_blocks--;

    /* Move to open list */
    block->state = FTL_BLOCK_OPEN;
    block->valid_page_count = 0;
    block->invalid_page_count = 0;
    block_list_add_head(&mgr->open_list, block);
    mgr->open_blocks++;

    mutex_unlock(&mgr->lock);

    return block;
}

int block_free(struct block_mgr *mgr, struct block_desc *block)
{
    if (!mgr || !block) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&mgr->lock, 0);

    /* Remove from current list */
    switch (block->state) {
    case FTL_BLOCK_OPEN:
        block_list_remove(&mgr->open_list, block);
        mgr->open_blocks--;
        break;
    case FTL_BLOCK_CLOSED:
        block_list_remove(&mgr->closed_list, block);
        mgr->closed_blocks--;
        break;
    case FTL_BLOCK_GC:
        /* GC blocks are not in any list, just change state */
        break;
    case FTL_BLOCK_FREE:
    case FTL_BLOCK_BAD:
    default:
        break;
    }

    /* Increment erase count */
    block->erase_count++;

    /* Move to free list. */
    block->state = FTL_BLOCK_FREE;
    block->valid_page_count = 0;
    block->invalid_page_count = 0;
    block_list_add_head(&mgr->free_list, block);
    mgr->free_blocks++;

    mutex_unlock(&mgr->lock);

    return HFSSS_OK;
}

int block_mark_closed(struct block_mgr *mgr, struct block_desc *block)
{
    if (!mgr || !block) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&mgr->lock, 0);

    if (block->state != FTL_BLOCK_OPEN) {
        mutex_unlock(&mgr->lock);
        return HFSSS_ERR_INVAL;
    }

    /* Remove from open list */
    block_list_remove(&mgr->open_list, block);
    mgr->open_blocks--;

    /* Move to closed list */
    block->state = FTL_BLOCK_CLOSED;
    block_list_add_head(&mgr->closed_list, block);
    mgr->closed_blocks++;

    mutex_unlock(&mgr->lock);

    return HFSSS_OK;
}

int block_mark_gc(struct block_mgr *mgr, struct block_desc *block)
{
    if (!mgr || !block) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&mgr->lock, 0);

    if (block->state != FTL_BLOCK_CLOSED) {
        mutex_unlock(&mgr->lock);
        return HFSSS_ERR_INVAL;
    }

    /* Remove from closed list */
    block_list_remove(&mgr->closed_list, block);
    mgr->closed_blocks--;

    /* Mark as GC */
    block->state = FTL_BLOCK_GC;

    mutex_unlock(&mgr->lock);

    return HFSSS_OK;
}

int block_unmark_gc(struct block_mgr *mgr, struct block_desc *block)
{
    if (!mgr || !block) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&mgr->lock, 0);

    if (block->state != FTL_BLOCK_GC) {
        mutex_unlock(&mgr->lock);
        return HFSSS_ERR_INVAL;
    }

    /* GC blocks are not on any list; restore to closed list. */
    block->state = FTL_BLOCK_CLOSED;
    block_list_add_head(&mgr->closed_list, block);
    mgr->closed_blocks++;

    mutex_unlock(&mgr->lock);

    return HFSSS_OK;
}

struct block_desc *block_find_by_coords(struct block_mgr *mgr,
                                         u32 ch, u32 chip, u32 die,
                                         u32 plane, u32 block_id)
{
    u64 idx;

    if (!mgr || !mgr->blocks) {
        return NULL;
    }

    idx = (u64)ch    * (mgr->chips_per_channel * mgr->dies_per_chip *
                        mgr->planes_per_die    * mgr->blocks_per_plane)
        + (u64)chip  * (mgr->dies_per_chip * mgr->planes_per_die * mgr->blocks_per_plane)
        + (u64)die   * (mgr->planes_per_die * mgr->blocks_per_plane)
        + (u64)plane * mgr->blocks_per_plane
        + block_id;

    if (idx >= mgr->total_blocks) {
        return NULL;
    }

    return &mgr->blocks[idx];
}

void block_mark_page_invalid(struct block_mgr *mgr,
                              u32 ch, u32 chip, u32 die,
                              u32 plane, u32 block_id)
{
    struct block_desc *block = block_find_by_coords(mgr, ch, chip, die, plane, block_id);

    if (!block) {
        return;
    }

    mutex_lock(&mgr->lock, 0);

    if (block->valid_page_count > 0) {
        block->valid_page_count--;
    }
    block->invalid_page_count++;

    mutex_unlock(&mgr->lock);
}

struct block_desc *block_find_victim(struct block_mgr *mgr, int policy)
{
    struct block_desc *block;
    struct block_desc *victim = NULL;
    u64 best_score = 0;

    if (!mgr) {
        return NULL;
    }

    mutex_lock(&mgr->lock, 0);

    switch (policy) {
    case GC_POLICY_GREEDY:
        /* Greedy: pick block with most invalid pages */
        for (block = mgr->closed_list; block != NULL; block = block->next) {
            u64 invalid = block->invalid_page_count;
            if (!victim || invalid > best_score) {
                victim = block;
                best_score = invalid;
            }
        }
        break;

    case GC_POLICY_COST_BENEFIT:
        /* Cost-Benefit: (1 - u) * 2^(-age / T) or similar heuristic
         * We'll use (invalid_pages) / (erase_count + 1) as a simple heuristic
         * Higher invalid pages and lower erase count = better victim
         */
        best_score = U64_MAX;
        for (block = mgr->closed_list; block != NULL; block = block->next) {
            u64 valid = block->valid_page_count;
            u64 erase = block->erase_count + 1; /* Avoid division by zero */
            u64 score = (valid + 1) * erase; /* Lower is better */
            if (!victim || score < best_score) {
                victim = block;
                best_score = score;
            }
        }
        break;

    case GC_POLICY_FIFO:
        /* FIFO: pick oldest closed block (by last_write_ts) */
        best_score = U64_MAX;
        for (block = mgr->closed_list; block != NULL; block = block->next) {
            if (!victim || block->last_write_ts < best_score) {
                victim = block;
                best_score = block->last_write_ts;
            }
        }
        break;

    default:
        /* Default to first closed block */
        victim = mgr->closed_list;
        break;
    }

    mutex_unlock(&mgr->lock);

    return victim;
}

u64 block_get_free_count(struct block_mgr *mgr)
{
    if (!mgr) {
        return 0;
    }

    mutex_lock(&mgr->lock, 0);
    u64 count = mgr->free_blocks;
    mutex_unlock(&mgr->lock);

    return count;
}

u64 block_get_valid_page_count(struct block_desc *block)
{
    if (!block) {
        return 0;
    }

    return block->valid_page_count;
}

u64 block_get_invalid_page_count(struct block_desc *block)
{
    if (!block) {
        return 0;
    }

    return block->invalid_page_count;
}

/*
 * Permanently retire a block that has failed at the media layer.  The block
 * is removed from whichever list it currently occupies and its state is set to
 * FTL_BLOCK_BAD.  It is never placed back on any list and will not be
 * allocated or GC'd again.
 */
int block_mark_bad(struct block_mgr *mgr, struct block_desc *block)
{
    if (!mgr || !block) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&mgr->lock, 0);

    switch (block->state) {
    case FTL_BLOCK_OPEN:
        block_list_remove(&mgr->open_list, block);
        mgr->open_blocks--;
        break;
    case FTL_BLOCK_CLOSED:
        block_list_remove(&mgr->closed_list, block);
        mgr->closed_blocks--;
        break;
    case FTL_BLOCK_FREE:
        block_list_remove(&mgr->free_list, block);
        mgr->free_blocks--;
        break;
    case FTL_BLOCK_GC:
        /* GC blocks are not on any list. */
        break;
    case FTL_BLOCK_BAD:
        /* Already bad — nothing to do. */
        mutex_unlock(&mgr->lock);
        return HFSSS_OK;
    default:
        break;
    }

    block->state = FTL_BLOCK_BAD;

    mutex_unlock(&mgr->lock);

    return HFSSS_OK;
}
