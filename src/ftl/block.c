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

    /* Move to free list */
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

struct block_desc *block_find_victim(struct block_mgr *mgr, int policy)
{
    struct block_desc *block;
    struct block_desc *victim = NULL;
    u64 max_cost = 0;
    (void)policy;

    if (!mgr) {
        return NULL;
    }

    mutex_lock(&mgr->lock, 0);

    /* For testing: just return the first closed block */
    victim = mgr->closed_list;

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
