#ifndef __HFSSS_RESOURCE_H
#define __HFSSS_RESOURCE_H

#include "common/common.h"
#include "common/mutex.h"

/* Resource Type */
enum resource_type {
    RESOURCE_CMD_SLOT = 0,
    RESOURCE_DATA_BUFFER = 1,
    RESOURCE_DMA_DESC = 2,
    RESOURCE_MEDIA_CMD = 3,
    RESOURCE_MAX = 4,
};

/* Idle Block Pool Entry */
struct idle_block_entry {
    u32 channel;
    u32 chip;
    u32 die;
    u32 plane;
    u32 block;
    u64 erase_count;
    bool is_bad;
    struct idle_block_entry *next;
};

/* Idle Block Pool */
struct idle_block_pool {
    struct idle_block_entry *free_list;
    struct idle_block_entry *used_list;
    u32 total;
    u32 free;
    u32 used;
    u32 low_watermark;
    u32 high_watermark;
    struct mutex lock;
};

/* Resource Pool */
struct resource_pool {
    enum resource_type type;
    u32 total;
    u32 used;
    u32 free;
    void **free_list;
    void *data_pool;
    struct mutex lock;
};

/* Resource Manager */
struct resource_mgr {
    struct resource_pool pools[RESOURCE_MAX];
    u64 alloc_count[RESOURCE_MAX];
    u64 free_count[RESOURCE_MAX];
    struct mutex lock;

    /* Idle Block Pool */
    struct idle_block_pool idle_blocks;
};

/* Function Prototypes */
int resource_mgr_init(struct resource_mgr *mgr);
void resource_mgr_cleanup(struct resource_mgr *mgr);
void *resource_alloc(struct resource_mgr *mgr, enum resource_type type);
void resource_free(struct resource_mgr *mgr, enum resource_type type, void *ptr);

/* Idle Block Pool Functions */
int idle_block_pool_init(struct idle_block_pool *pool, u32 total_blocks, u32 low_watermark, u32 high_watermark);
void idle_block_pool_cleanup(struct idle_block_pool *pool);
struct idle_block_entry *idle_block_alloc(struct resource_mgr *mgr);
void idle_block_free(struct resource_mgr *mgr, struct idle_block_entry *block);
u32 idle_block_get_free_count(struct resource_mgr *mgr);
bool idle_block_needs_gc(struct resource_mgr *mgr);

#endif /* __HFSSS_RESOURCE_H */
