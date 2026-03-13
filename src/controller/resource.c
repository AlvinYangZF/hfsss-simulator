#include "controller/resource.h"
#include <stdlib.h>
#include <string.h>

#define RESOURCE_POOL_SIZE 4096
#define RESOURCE_SLOT_SIZE 64

int resource_mgr_init(struct resource_mgr *mgr)
{
    u32 i, j;
    int ret;

    if (!mgr) {
        return HFSSS_ERR_INVAL;
    }

    memset(mgr, 0, sizeof(*mgr));

    ret = mutex_init(&mgr->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Initialize each resource pool */
    for (i = 0; i < RESOURCE_MAX; i++) {
        mgr->pools[i].type = (enum resource_type)i;
        mgr->pools[i].total = RESOURCE_POOL_SIZE;
        mgr->pools[i].used = 0;
        mgr->pools[i].free = RESOURCE_POOL_SIZE;

        /* Allocate free list */
        mgr->pools[i].free_list = (void **)calloc(RESOURCE_POOL_SIZE, sizeof(void *));
        if (!mgr->pools[i].free_list) {
            /* Cleanup and fail */
            for (j = 0; j < i; j++) {
                free(mgr->pools[j].data_pool);
                free(mgr->pools[j].free_list);
            }
            mutex_cleanup(&mgr->lock);
            return HFSSS_ERR_NOMEM;
        }

        /* Allocate data pool for actual resources */
        mgr->pools[i].data_pool = calloc(RESOURCE_POOL_SIZE, RESOURCE_SLOT_SIZE);
        if (!mgr->pools[i].data_pool) {
            /* Cleanup and fail */
            free(mgr->pools[i].free_list);
            for (j = 0; j < i; j++) {
                free(mgr->pools[j].data_pool);
                free(mgr->pools[j].free_list);
            }
            mutex_cleanup(&mgr->lock);
            return HFSSS_ERR_NOMEM;
        }

        /* Initialize free list with actual pointers */
        for (j = 0; j < RESOURCE_POOL_SIZE; j++) {
            mgr->pools[i].free_list[j] = (u8 *)mgr->pools[i].data_pool + j * RESOURCE_SLOT_SIZE;
        }

        ret = mutex_init(&mgr->pools[i].lock);
        if (ret != HFSSS_OK) {
            /* Cleanup and fail */
            free(mgr->pools[i].data_pool);
            free(mgr->pools[i].free_list);
            for (j = 0; j < i; j++) {
                free(mgr->pools[j].data_pool);
                free(mgr->pools[j].free_list);
                mutex_cleanup(&mgr->pools[j].lock);
            }
            mutex_cleanup(&mgr->lock);
            return ret;
        }
    }

    return HFSSS_OK;
}

void resource_mgr_cleanup(struct resource_mgr *mgr)
{
    u32 i;

    if (!mgr) {
        return;
    }

    mutex_lock(&mgr->lock, 0);

    for (i = 0; i < RESOURCE_MAX; i++) {
        free(mgr->pools[i].data_pool);
        free(mgr->pools[i].free_list);
        mutex_cleanup(&mgr->pools[i].lock);
    }

    mutex_unlock(&mgr->lock);
    mutex_cleanup(&mgr->lock);

    memset(mgr, 0, sizeof(*mgr));
}

void *resource_alloc(struct resource_mgr *mgr, enum resource_type type)
{
    void *ptr = NULL;
    struct resource_pool *pool;

    if (!mgr || type >= RESOURCE_MAX) {
        return NULL;
    }

    pool = &mgr->pools[type];

    mutex_lock(&pool->lock, 0);

    if (pool->free > 0) {
        /* Take from the end of the free list */
        ptr = pool->free_list[pool->total - pool->free];
        pool->used++;
        pool->free--;
        mgr->alloc_count[type]++;
    }

    mutex_unlock(&pool->lock);

    return ptr;
}

void resource_free(struct resource_mgr *mgr, enum resource_type type, void *ptr)
{
    struct resource_pool *pool;

    if (!mgr || type >= RESOURCE_MAX || !ptr) {
        return;
    }

    pool = &mgr->pools[type];

    mutex_lock(&pool->lock, 0);

    if (pool->used > 0) {
        pool->used--;
        pool->free++;
        /* Put back at the end of the free list */
        pool->free_list[pool->total - pool->free] = ptr;
        mgr->free_count[type]++;
    }

    mutex_unlock(&pool->lock);
}
