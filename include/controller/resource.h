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
};

/* Function Prototypes */
int resource_mgr_init(struct resource_mgr *mgr);
void resource_mgr_cleanup(struct resource_mgr *mgr);
void *resource_alloc(struct resource_mgr *mgr, enum resource_type type);
void resource_free(struct resource_mgr *mgr, enum resource_type type, void *ptr);

#endif /* __HFSSS_RESOURCE_H */
