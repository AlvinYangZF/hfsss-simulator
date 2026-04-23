#include "controller/resource.h"
#include <stdlib.h>
#include <string.h>

#define RESOURCE_POOL_SIZE 4096
#define RESOURCE_SLOT_SIZE 64
#define DEFAULT_IDLE_BLOCKS 1024
#define DEFAULT_LOW_WATERMARK 128
#define DEFAULT_HIGH_WATERMARK 512

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

    /* Initialize idle block pool */
    ret = idle_block_pool_init(&mgr->idle_blocks, DEFAULT_IDLE_BLOCKS,
                                DEFAULT_LOW_WATERMARK, DEFAULT_HIGH_WATERMARK);
    if (ret != HFSSS_OK) {
        for (j = 0; j < RESOURCE_MAX; j++) {
            free(mgr->pools[j].data_pool);
            free(mgr->pools[j].free_list);
            mutex_cleanup(&mgr->pools[j].lock);
        }
        mutex_cleanup(&mgr->lock);
        return ret;
    }

    return HFSSS_OK;
}

void resource_mgr_cleanup(struct resource_mgr *mgr)
{
    u32 i;

    if (!mgr) {
        return;
    }

    /* REQ-134: detach any attached fault registry first so no
     * in-flight resource_alloc can dereference a soon-to-be-freed
     * registry. The per-pool locks + mgr->lock below drain any
     * mid-call readers. */
    resource_mgr_attach_faults(mgr, NULL);

    mutex_lock(&mgr->lock, 0);

    /* Cleanup idle block pool */
    idle_block_pool_cleanup(&mgr->idle_blocks);

    /* Cleanup resource pools */
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

    /*
     * REQ-134 fault-injection hook. Checked under the pool lock so the
     * faults-pointer read is serialized against
     * resource_mgr_attach_faults / resource_mgr_cleanup (both store
     * under mgr->lock but also need to be ordered against readers).
     * fault_check itself is internally thread-safe on the registry.
     * Hit returns NULL without consuming a slot.
     */
    if (mgr->faults) {
        struct fault_addr faddr = {
            FAULT_WILDCARD, FAULT_WILDCARD, FAULT_WILDCARD,
            FAULT_WILDCARD, FAULT_WILDCARD, FAULT_WILDCARD,
        };
        if (fault_check(mgr->faults, FAULT_POOL_EXHAUST, &faddr)) {
            mutex_unlock(&pool->lock);
            return NULL;
        }
    }

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

void resource_mgr_attach_faults(struct resource_mgr *mgr,
                                struct fault_registry *faults)
{
    if (!mgr) {
        return;
    }
    /*
     * Store under mgr->lock so concurrent readers in resource_alloc
     * (which take their per-pool lock) + idle_block_alloc (idle
     * pool lock) observe a consistent pointer. See resource_mgr_cleanup.
     */
    mutex_lock(&mgr->lock, 0);
    mgr->faults = faults;
    mutex_unlock(&mgr->lock);
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

/* Idle Block Pool Functions */
int idle_block_pool_init(struct idle_block_pool *pool, u32 total_blocks,
                          u32 low_watermark, u32 high_watermark)
{
    u32 i;
    struct idle_block_entry *entry;

    if (!pool) {
        return HFSSS_ERR_INVAL;
    }

    memset(pool, 0, sizeof(*pool));

    pool->total = total_blocks;
    pool->free = total_blocks;
    pool->used = 0;
    pool->low_watermark = low_watermark;
    pool->high_watermark = high_watermark;

    int ret = mutex_init(&pool->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Allocate all entries */
    pool->free_list = NULL;
    for (i = 0; i < total_blocks; i++) {
        entry = (struct idle_block_entry *)malloc(sizeof(struct idle_block_entry));
        if (!entry) {
            /* Cleanup */
            idle_block_pool_cleanup(pool);
            return HFSSS_ERR_NOMEM;
        }
        memset(entry, 0, sizeof(*entry));
        entry->next = pool->free_list;
        pool->free_list = entry;
    }

    pool->used_list = NULL;

    return HFSSS_OK;
}

void idle_block_pool_cleanup(struct idle_block_pool *pool)
{
    struct idle_block_entry *entry, *next;

    if (!pool) {
        return;
    }

    mutex_lock(&pool->lock, 0);

    /* Free free list */
    entry = pool->free_list;
    while (entry) {
        next = entry->next;
        free(entry);
        entry = next;
    }

    /* Free used list */
    entry = pool->used_list;
    while (entry) {
        next = entry->next;
        free(entry);
        entry = next;
    }

    mutex_unlock(&pool->lock);
    mutex_cleanup(&pool->lock);

    memset(pool, 0, sizeof(*pool));
}

struct idle_block_entry *idle_block_alloc(struct resource_mgr *mgr)
{
    struct idle_block_entry *entry = NULL;
    struct idle_block_pool *pool;

    if (!mgr) {
        return NULL;
    }

    pool = &mgr->idle_blocks;

    mutex_lock(&pool->lock, 0);

    /* REQ-134 fault-injection hook — idle-block pool exhaustion.
     * Under pool->lock for the same ordering reason as resource_alloc. */
    if (mgr->faults) {
        struct fault_addr faddr = {
            FAULT_WILDCARD, FAULT_WILDCARD, FAULT_WILDCARD,
            FAULT_WILDCARD, FAULT_WILDCARD, FAULT_WILDCARD,
        };
        if (fault_check(mgr->faults, FAULT_POOL_EXHAUST, &faddr)) {
            mutex_unlock(&pool->lock);
            return NULL;
        }
    }

    if (pool->free > 0) {
        /* Take from head of free list */
        entry = pool->free_list;
        pool->free_list = entry->next;
        entry->next = pool->used_list;
        pool->used_list = entry;
        pool->free--;
        pool->used++;
    }

    mutex_unlock(&pool->lock);

    return entry;
}

void idle_block_free(struct resource_mgr *mgr, struct idle_block_entry *block)
{
    struct idle_block_pool *pool;
    struct idle_block_entry **prev;
    struct idle_block_entry *curr;

    if (!mgr || !block) {
        return;
    }

    pool = &mgr->idle_blocks;

    mutex_lock(&pool->lock, 0);

    /* Remove from used list */
    prev = &pool->used_list;
    curr = pool->used_list;
    while (curr) {
        if (curr == block) {
            *prev = curr->next;
            break;
        }
        prev = &curr->next;
        curr = curr->next;
    }

    if (curr == block) {
        /* Add to free list */
        block->next = pool->free_list;
        pool->free_list = block;
        pool->used--;
        pool->free++;
    }

    mutex_unlock(&pool->lock);
}

u32 idle_block_get_free_count(struct resource_mgr *mgr)
{
    u32 count;
    struct idle_block_pool *pool;

    if (!mgr) {
        return 0;
    }

    pool = &mgr->idle_blocks;

    mutex_lock(&pool->lock, 0);
    count = pool->free;
    mutex_unlock(&pool->lock);

    return count;
}

bool idle_block_needs_gc(struct resource_mgr *mgr)
{
    bool needs_gc;
    struct idle_block_pool *pool;

    if (!mgr) {
        return false;
    }

    pool = &mgr->idle_blocks;

    mutex_lock(&pool->lock, 0);
    needs_gc = (pool->free <= pool->low_watermark);
    mutex_unlock(&pool->lock);

    return needs_gc;
}

void resource_cpu_record(struct resource_mgr *mgr, enum cpu_role role, u64 cycles)
{
    if (!mgr || role >= CPU_ROLE_MAX) return;
    mutex_lock(&mgr->lock, 0);
    mgr->cpu.cycle_count[role] += cycles;
    mgr->cpu.total_cycles += cycles;
    mutex_unlock(&mgr->lock);
}

void resource_cpu_get_stats(const struct resource_mgr *mgr, struct cpu_stats *out)
{
    if (!mgr || !out) return;
    struct resource_mgr *m = (struct resource_mgr *)mgr;
    mutex_lock(&m->lock, 0);
    *out = mgr->cpu;
    mutex_unlock(&m->lock);
}

double resource_cpu_utilization(const struct resource_mgr *mgr, enum cpu_role role)
{
    if (!mgr || role >= CPU_ROLE_MAX || mgr->cpu.total_cycles == 0) return 0.0;
    return (double)mgr->cpu.cycle_count[role] / (double)mgr->cpu.total_cycles;
}
