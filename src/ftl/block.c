#include "ftl/block.h"
#include "ftl/gc.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifndef U64_MAX
#define U64_MAX ((u64)-1)
#endif

/* Internal helper functions */
static void block_list_add_head(struct block_desc **list, struct block_desc *block);
static void block_list_remove(struct block_desc **list, struct block_desc *block);
static u32 block_free_shard_index(struct block_mgr *mgr, u32 channel, u32 plane);
static struct block_free_shard *block_get_free_shard(struct block_mgr *mgr,
                                                     u32 channel, u32 plane);
static struct block_free_shard *block_get_free_shard_for_block(struct block_mgr *mgr,
                                                               struct block_desc *block);
static struct block_desc *block_alloc_from_shard(struct block_mgr *mgr,
                                                 struct block_free_shard *shard);

static inline u64 block_mgr_count_load(_Atomic u64 *counter)
{
    return atomic_load_explicit(counter, memory_order_relaxed);
}

static inline void block_mgr_count_inc(_Atomic u64 *counter)
{
    atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
}

static inline void block_mgr_count_dec(_Atomic u64 *counter)
{
    atomic_fetch_sub_explicit(counter, 1, memory_order_relaxed);
}

static inline u32 block_page_count_load(_Atomic u32 *counter)
{
    return atomic_load_explicit(counter, memory_order_relaxed);
}

static inline void block_page_count_store(_Atomic u32 *counter, u32 value)
{
    atomic_store_explicit(counter, value, memory_order_relaxed);
}

static inline void block_page_count_inc(_Atomic u32 *counter)
{
    atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
}

static u32 block_free_shard_index(struct block_mgr *mgr, u32 channel, u32 plane)
{
    return channel * mgr->planes_per_die + plane;
}

static struct block_free_shard *block_get_free_shard(struct block_mgr *mgr,
                                                     u32 channel, u32 plane)
{
    u32 idx;

    if (!mgr || !mgr->free_shards) {
        return NULL;
    }
    if (channel >= mgr->channel_count || plane >= mgr->planes_per_die) {
        return NULL;
    }

    idx = block_free_shard_index(mgr, channel, plane);
    if (idx >= mgr->free_shard_count) {
        return NULL;
    }

    return &mgr->free_shards[idx];
}

static struct block_free_shard *block_get_free_shard_for_block(struct block_mgr *mgr,
                                                               struct block_desc *block)
{
    if (!mgr || !block) {
        return NULL;
    }

    return block_get_free_shard(mgr, block->channel, block->plane);
}

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

static struct block_desc *block_alloc_from_shard(struct block_mgr *mgr,
                                                 struct block_free_shard *shard)
{
    struct block_desc *block;

    if (!mgr || !shard) {
        return NULL;
    }

    mutex_lock(&shard->lock, 0);
    if (!shard->free_list) {
        mutex_unlock(&shard->lock);
        return NULL;
    }

    block = shard->free_list;
    block_list_remove(&shard->free_list, block);
    block_mgr_count_dec(&shard->free_blocks);
    block_mgr_count_dec(&mgr->free_blocks);
    block->state = FTL_BLOCK_OPEN;
    block_page_count_store(&block->valid_page_count, 0);
    block_page_count_store(&block->invalid_page_count, 0);
    mutex_unlock(&shard->lock);

    mutex_lock(&mgr->lock, 0);
    block_list_add_head(&mgr->open_list, block);
    block_mgr_count_inc(&mgr->open_blocks);
    mutex_unlock(&mgr->lock);

    return block;
}

int block_mgr_init(struct block_mgr *mgr, u32 channel_count, u32 chips_per_channel,
                   u32 dies_per_chip, u32 planes_per_die, u32 blocks_per_plane)
{
    u64 total_blocks;
    u32 free_shard_count;
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
    free_shard_count = channel_count * planes_per_die;
    mgr->free_shard_count = free_shard_count;

    mgr->free_shards = (struct block_free_shard *)calloc(free_shard_count,
                                                         sizeof(struct block_free_shard));
    if (!mgr->free_shards) {
        mutex_cleanup(&mgr->lock);
        return HFSSS_ERR_NOMEM;
    }

    for (ch = 0; ch < channel_count; ch++) {
        for (plane = 0; plane < planes_per_die; plane++) {
            struct block_free_shard *shard =
                block_get_free_shard(mgr, ch, plane);

            shard->channel = ch;
            shard->plane = plane;
            ret = mutex_init(&shard->lock);
            if (ret != HFSSS_OK) {
                u32 cleanup_ch, cleanup_plane;

                for (cleanup_ch = 0; cleanup_ch <= ch; cleanup_ch++) {
                    u32 plane_limit = (cleanup_ch == ch) ? plane : planes_per_die;
                    for (cleanup_plane = 0; cleanup_plane < plane_limit; cleanup_plane++) {
                        struct block_free_shard *cleanup_shard =
                            block_get_free_shard(mgr, cleanup_ch, cleanup_plane);
                        mutex_cleanup(&cleanup_shard->lock);
                    }
                }
                free(mgr->free_shards);
                mgr->free_shards = NULL;
                mgr->free_shard_count = 0;
                mutex_cleanup(&mgr->lock);
                return ret;
            }
        }
    }

    /* Allocate block descriptors */
    mgr->blocks = (struct block_desc *)calloc(total_blocks, sizeof(struct block_desc));
    if (!mgr->blocks) {
        for (ch = 0; ch < channel_count; ch++) {
            for (plane = 0; plane < planes_per_die; plane++) {
                struct block_free_shard *shard = block_get_free_shard(mgr, ch, plane);
                mutex_cleanup(&shard->lock);
            }
        }
        free(mgr->free_shards);
        mgr->free_shards = NULL;
        mgr->free_shard_count = 0;
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
                        block_page_count_store(&block->valid_page_count, 0);
                        block_page_count_store(&block->invalid_page_count, 0);
                        block->erase_count = 0;
                        block->last_write_ts = 0;
                        block->cost = 0;

                        /* Add to the channel/plane-local free list. */
                        struct block_free_shard *shard =
                            block_get_free_shard_for_block(mgr, block);
                        block_list_add_head(&shard->free_list, block);
                        block_mgr_count_inc(&shard->free_blocks);
                        block_mgr_count_inc(&mgr->free_blocks);

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
    u32 idx;

    if (!mgr) {
        return;
    }

    mutex_lock(&mgr->lock, 0);

    free(mgr->blocks);
    mgr->blocks = NULL;

    mutex_unlock(&mgr->lock);
    mutex_cleanup(&mgr->lock);

    for (idx = 0; idx < mgr->free_shard_count; idx++) {
        mutex_cleanup(&mgr->free_shards[idx].lock);
    }
    free(mgr->free_shards);

    memset(mgr, 0, sizeof(*mgr));
}

struct block_desc *block_alloc(struct block_mgr *mgr)
{
    u32 idx;

    if (!mgr) {
        return NULL;
    }

    for (idx = 0; idx < mgr->free_shard_count; idx++) {
        struct block_desc *block =
            block_alloc_from_shard(mgr, &mgr->free_shards[idx]);
        if (block) {
            return block;
        }
    }

    return NULL;
}

struct block_desc *block_alloc_for_channel_plane(struct block_mgr *mgr,
                                                 u32 channel, u32 plane)
{
    struct block_free_shard *shard;
    struct block_desc *block;

    if (!mgr) {
        return NULL;
    }

    shard = block_get_free_shard(mgr, channel, plane);
    block = block_alloc_from_shard(mgr, shard);
    if (block) {
        return block;
    }

    return block_alloc(mgr);
}

int block_free(struct block_mgr *mgr, struct block_desc *block)
{
    struct block_free_shard *shard;

    if (!mgr || !block) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&mgr->lock, 0);

    /* Remove from current list */
    switch (block->state) {
    case FTL_BLOCK_OPEN:
        block_list_remove(&mgr->open_list, block);
        block_mgr_count_dec(&mgr->open_blocks);
        break;
    case FTL_BLOCK_CLOSED:
        block_list_remove(&mgr->closed_list, block);
        block_mgr_count_dec(&mgr->closed_blocks);
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
    block_page_count_store(&block->valid_page_count, 0);
    block_page_count_store(&block->invalid_page_count, 0);

    mutex_unlock(&mgr->lock);

    shard = block_get_free_shard_for_block(mgr, block);
    if (!shard) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&shard->lock, 0);
    block_list_add_head(&shard->free_list, block);
    block_mgr_count_inc(&shard->free_blocks);
    block_mgr_count_inc(&mgr->free_blocks);
    mutex_unlock(&shard->lock);

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
    block_mgr_count_dec(&mgr->open_blocks);

    /* Move to closed list */
    block->state = FTL_BLOCK_CLOSED;
    block_list_add_head(&mgr->closed_list, block);
    block_mgr_count_inc(&mgr->closed_blocks);

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
    block_mgr_count_dec(&mgr->closed_blocks);

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
    block_mgr_count_inc(&mgr->closed_blocks);

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

    u32 current_valid = block_page_count_load(&block->valid_page_count);
    while (current_valid > 0) {
        if (atomic_compare_exchange_weak_explicit(&block->valid_page_count,
                                                  &current_valid,
                                                  current_valid - 1,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
            break;
        }
    }
    block_page_count_inc(&block->invalid_page_count);
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
            u64 invalid = block_get_invalid_page_count(block);
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
            u64 valid = block_get_valid_page_count(block);
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
    return block_mgr_count_load(&mgr->free_blocks);
}

u64 block_get_valid_page_count(struct block_desc *block)
{
    if (!block) {
        return 0;
    }

    return block_page_count_load(&block->valid_page_count);
}

u64 block_get_invalid_page_count(struct block_desc *block)
{
    if (!block) {
        return 0;
    }

    return block_page_count_load(&block->invalid_page_count);
}

/*
 * Permanently retire a block that has failed at the media layer.  The block
 * is removed from whichever list it currently occupies and its state is set to
 * FTL_BLOCK_BAD.  It is never placed back on any list and will not be
 * allocated or GC'd again.
 */
int block_mark_bad(struct block_mgr *mgr, struct block_desc *block)
{
    struct block_free_shard *shard;

    if (!mgr || !block) {
        return HFSSS_ERR_INVAL;
    }

    if (block->state == FTL_BLOCK_FREE) {
        shard = block_get_free_shard_for_block(mgr, block);
        if (!shard) {
            return HFSSS_ERR_INVAL;
        }

        mutex_lock(&shard->lock, 0);
        if (block->state == FTL_BLOCK_FREE) {
            block_list_remove(&shard->free_list, block);
            block_mgr_count_dec(&shard->free_blocks);
            block_mgr_count_dec(&mgr->free_blocks);
            block->state = FTL_BLOCK_BAD;
        }
        mutex_unlock(&shard->lock);

        return HFSSS_OK;
    }

    mutex_lock(&mgr->lock, 0);

    switch (block->state) {
    case FTL_BLOCK_OPEN:
        block_list_remove(&mgr->open_list, block);
        block_mgr_count_dec(&mgr->open_blocks);
        break;
    case FTL_BLOCK_CLOSED:
        block_list_remove(&mgr->closed_list, block);
        block_mgr_count_dec(&mgr->closed_blocks);
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

/*
 * Reserve a block for metadata (superblock) use.  The block is removed from
 * the free list and will never be allocated by block_alloc() or selected
 * as a GC victim.
 */
int block_mark_reserved(struct block_mgr *mgr, struct block_desc *block)
{
    struct block_free_shard *shard;

    if (!mgr || !block) {
        return HFSSS_ERR_INVAL;
    }

    if (block->state == FTL_BLOCK_FREE) {
        shard = block_get_free_shard_for_block(mgr, block);
        if (!shard) {
            return HFSSS_ERR_INVAL;
        }

        mutex_lock(&shard->lock, 0);
        if (block->state == FTL_BLOCK_RESERVED) {
            mutex_unlock(&shard->lock);
            return HFSSS_OK;
        }
        if (block->state == FTL_BLOCK_FREE) {
            block_list_remove(&shard->free_list, block);
            block_mgr_count_dec(&shard->free_blocks);
            block_mgr_count_dec(&mgr->free_blocks);
            block->state = FTL_BLOCK_RESERVED;
            mutex_unlock(&shard->lock);
            return HFSSS_OK;
        }
        mutex_unlock(&shard->lock);
    }

    mutex_lock(&mgr->lock, 0);

    if (block->state == FTL_BLOCK_RESERVED) {
        mutex_unlock(&mgr->lock);
        return HFSSS_OK;
    }

    switch (block->state) {
    case FTL_BLOCK_OPEN:
        block_list_remove(&mgr->open_list, block);
        block_mgr_count_dec(&mgr->open_blocks);
        break;
    case FTL_BLOCK_CLOSED:
        block_list_remove(&mgr->closed_list, block);
        block_mgr_count_dec(&mgr->closed_blocks);
        break;
    default:
        break;
    }

    block->state = FTL_BLOCK_RESERVED;

    mutex_unlock(&mgr->lock);

    return HFSSS_OK;
}
