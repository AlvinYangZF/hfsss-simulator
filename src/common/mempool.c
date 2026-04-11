#include "common/mempool.h"
#include <pthread.h>

struct mempool_lock {
    pthread_mutex_t mutex;
};

int mem_pool_init(struct mem_pool *pool, u32 block_size, u32 block_count)
{
    if (!pool || block_size == 0 || block_count == 0) {
        return HFSSS_ERR_INVAL;
    }

    memset(pool, 0, sizeof(*pool));

    /* User data area: exactly block_size per block. Allocator metadata
     * lives in a parallel blocks[] array so user writes to allocated
     * memory cannot corrupt allocator state. */
    u64 total_data = (u64)block_size * block_count;
    pool->memory = calloc(1, total_data);
    if (!pool->memory) {
        return HFSSS_ERR_NOMEM;
    }

    pool->blocks = (struct mem_block *)calloc(block_count,
                                               sizeof(struct mem_block));
    if (!pool->blocks) {
        free(pool->memory);
        pool->memory = NULL;
        return HFSSS_ERR_NOMEM;
    }

    pool->block_size = block_size;
    pool->block_count = block_count;
    pool->free_count = block_count;
    pool->used_count = 0;
    pool->alloc_count = 0;
    pool->free_count_total = 0;

    /* Build free list over the out-of-band metadata array */
    pool->free_list = NULL;
    for (u32 i = 0; i < block_count; i++) {
        pool->blocks[i].in_use = 0;
        pool->blocks[i].next = pool->free_list;
        pool->free_list = &pool->blocks[i];
    }

    struct mempool_lock *lock = (struct mempool_lock *)malloc(sizeof(struct mempool_lock));
    if (!lock) {
        free(pool->blocks);
        free(pool->memory);
        pool->blocks = NULL;
        pool->memory = NULL;
        return HFSSS_ERR_NOMEM;
    }
    pthread_mutex_init(&lock->mutex, NULL);
    pool->lock = lock;

    return HFSSS_OK;
}

void mem_pool_cleanup(struct mem_pool *pool)
{
    if (!pool) {
        return;
    }

    if (pool->lock) {
        struct mempool_lock *lock = (struct mempool_lock *)pool->lock;
        pthread_mutex_destroy(&lock->mutex);
        free(lock);
    }

    if (pool->blocks) {
        free(pool->blocks);
    }

    if (pool->memory) {
        free(pool->memory);
    }

    memset(pool, 0, sizeof(*pool));
}

void *mem_pool_alloc(struct mem_pool *pool)
{
    if (!pool) {
        return NULL;
    }

    struct mempool_lock *lock = (struct mempool_lock *)pool->lock;
    pthread_mutex_lock(&lock->mutex);

    void *ptr = NULL;
    if (pool->free_list) {
        struct mem_block *block = pool->free_list;
        pool->free_list = block->next;
        block->in_use = 1;
        block->next = NULL;
        /* Translate metadata index to user data address */
        u32 idx = (u32)(block - pool->blocks);
        ptr = (char *)pool->memory + (u64)idx * pool->block_size;
        pool->free_count--;
        pool->used_count++;
        pool->alloc_count++;
    }

    pthread_mutex_unlock(&lock->mutex);
    return ptr;
}

void mem_pool_free(struct mem_pool *pool, void *ptr)
{
    if (!pool || !ptr) {
        return;
    }

    /* Check if pointer is within pool memory and block-aligned */
    char *base = (char *)pool->memory;
    char *end  = base + (u64)pool->block_size * pool->block_count;
    if ((char *)ptr < base || (char *)ptr >= end) {
        return;
    }
    u64 offset = (char *)ptr - base;
    if (offset % pool->block_size != 0) {
        return;
    }
    u32 idx = (u32)(offset / pool->block_size);

    struct mempool_lock *lock = (struct mempool_lock *)pool->lock;
    pthread_mutex_lock(&lock->mutex);

    struct mem_block *block = &pool->blocks[idx];
    if (block->in_use) {
        block->in_use = 0;
        block->next = pool->free_list;
        pool->free_list = block;
        pool->free_count++;
        pool->used_count--;
        pool->free_count_total++;
    }

    pthread_mutex_unlock(&lock->mutex);
}

void mem_pool_stats(struct mem_pool *pool, u32 *used, u32 *free, u64 *alloc_total, u64 *free_total)
{
    if (!pool) {
        return;
    }

    struct mempool_lock *lock = (struct mempool_lock *)pool->lock;
    pthread_mutex_lock(&lock->mutex);

    if (used) *used = pool->used_count;
    if (free) *free = pool->free_count;
    if (alloc_total) *alloc_total = pool->alloc_count;
    if (free_total) *free_total = pool->free_count_total;

    pthread_mutex_unlock(&lock->mutex);
}
