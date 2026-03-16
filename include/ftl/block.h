#ifndef __HFSSS_BLOCK_H
#define __HFSSS_BLOCK_H

#include "common/common.h"
#include "common/mutex.h"
#include "ftl/mapping.h"

/* Block State */
enum ftl_block_state {
    FTL_BLOCK_FREE = 0,
    FTL_BLOCK_OPEN = 1,
    FTL_BLOCK_CLOSED = 2,
    FTL_BLOCK_GC = 3,
    FTL_BLOCK_BAD = 4,
};

/* Block Descriptor */
struct block_desc {
    u32 channel;
    u32 chip;
    u32 die;
    u32 plane;
    u32 block_id;
    enum ftl_block_state state;
    u32 valid_page_count;
    u32 invalid_page_count;
    u32 erase_count;
    u64 last_write_ts;
    u64 cost;
    struct block_desc *next;
    struct block_desc *prev;
};

/* Block Manager */
struct block_mgr {
    struct block_desc *blocks;
    u64 total_blocks;
    u64 free_blocks;
    u64 open_blocks;
    u64 closed_blocks;
    struct block_desc *free_list;
    struct block_desc *open_list;
    struct block_desc *closed_list;
    /* Geometry — needed for O(1) coordinate-to-descriptor lookup */
    u32 channel_count;
    u32 chips_per_channel;
    u32 dies_per_chip;
    u32 planes_per_die;
    u32 blocks_per_plane;
    struct mutex lock;
};

/* Current Write Block */
struct cwb {
    struct block_desc *block;
    u32 current_page;
    u64 last_write_ts;
};

/* Function Prototypes */
int block_mgr_init(struct block_mgr *mgr, u32 channel_count, u32 chips_per_channel,
                   u32 dies_per_chip, u32 planes_per_die, u32 blocks_per_plane);
void block_mgr_cleanup(struct block_mgr *mgr);
struct block_desc *block_alloc(struct block_mgr *mgr);
int block_free(struct block_mgr *mgr, struct block_desc *block);
int block_mark_closed(struct block_mgr *mgr, struct block_desc *block);
int block_mark_gc(struct block_mgr *mgr, struct block_desc *block);
int block_unmark_gc(struct block_mgr *mgr, struct block_desc *block);
int block_mark_bad(struct block_mgr *mgr, struct block_desc *block);
struct block_desc *block_find_victim(struct block_mgr *mgr, int policy);
u64 block_get_free_count(struct block_mgr *mgr);
u64 block_get_free_count_unsafe(struct block_mgr *mgr);
u64 block_get_valid_page_count(struct block_desc *block);
u64 block_get_invalid_page_count(struct block_desc *block);
struct block_desc *block_find_by_coords(struct block_mgr *mgr,
                                         u32 ch, u32 chip, u32 die,
                                         u32 plane, u32 block_id);
void block_mark_page_invalid(struct block_mgr *mgr,
                              u32 ch, u32 chip, u32 die,
                              u32 plane, u32 block_id);

#endif /* __HFSSS_BLOCK_H */
