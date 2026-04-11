#ifndef __HFSSS_MEMPOOL_H
#define __HFSSS_MEMPOOL_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Memory Pool Block Metadata.
 * Kept in a parallel array, NOT embedded in user memory, so that
 * user writes to allocated blocks cannot corrupt allocator state. */
struct mem_block {
    struct mem_block *next;
    int in_use;
};

/* Minimum alignment for user-data pointers returned by mem_pool_alloc.
 * Matches the effective guarantee glibc malloc() provides for any
 * standard type (max_align_t is 16 bytes on x86-64 and aarch64). This
 * also preserves the historical property of the old implementation,
 * which spaced slots by sizeof(struct mem_block) == 16 bytes. */
#define MEMPOOL_MIN_ALIGN 16

/* Memory Pool Context */
struct mem_pool {
    u32 block_size;             /* caller's requested size (for stats) */
    u32 slot_size;              /* effective stride: block_size rounded
                                 * up to MEMPOOL_MIN_ALIGN. Used for all
                                 * address math so that returned pointers
                                 * stay aligned even when block_size is
                                 * small (e.g. 1). */
    u32 block_count;
    u32 free_count;
    u32 used_count;
    void *memory;               /* user data area: block_count * slot_size */
    struct mem_block *blocks;   /* metadata array: block_count entries */
    struct mem_block *free_list;
    void *lock;
    u64 alloc_count;
    u64 free_count_total;
};

/* Function Prototypes */
int mem_pool_init(struct mem_pool *pool, u32 block_size, u32 block_count);
void mem_pool_cleanup(struct mem_pool *pool);
void *mem_pool_alloc(struct mem_pool *pool);
void mem_pool_free(struct mem_pool *pool, void *ptr);
void mem_pool_stats(struct mem_pool *pool, u32 *used, u32 *free, u64 *alloc_total, u64 *free_total);

#ifdef __cplusplus
}
#endif

#endif /* __HFSSS_MEMPOOL_H */
