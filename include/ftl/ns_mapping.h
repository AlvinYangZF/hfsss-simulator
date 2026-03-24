#ifndef __HFSSS_NS_MAPPING_H
#define __HFSSS_NS_MAPPING_H

#include "common/common.h"
#include "common/mutex.h"
#include "ftl/mapping.h"

#define NS_MAX_NAMESPACES 32

/* Block pool allocation mode */
enum ns_pool_mode {
    NS_POOL_SHARED    = 0,   /* All NS share one block pool */
    NS_POOL_DEDICATED = 1,   /* Each NS has reserved blocks */
};

/* Per-namespace mapping context */
struct ns_mapping_ctx {
    u32 nsid;
    u64 lba_start;         /* Starting LBA for this NS */
    u64 lba_count;         /* Number of LBAs in this NS */
    enum ns_pool_mode pool_mode;
    u64 allocated_blocks;  /* Blocks assigned to this NS */
    u64 valid_pages;
    u64 invalid_pages;
    bool active;
};

/* Namespace mapping manager */
struct ns_mapping_mgr {
    struct ns_mapping_ctx namespaces[NS_MAX_NAMESPACES];
    u32 active_count;
    u64 total_lbas;        /* Total LBA capacity */
    u64 allocated_lbas;    /* LBAs allocated to namespaces */
    enum ns_pool_mode default_mode;
    struct mutex lock;
    bool initialized;
};

/* Initialize the namespace mapping manager. */
int ns_mapping_mgr_init(struct ns_mapping_mgr *mgr, u64 total_lbas,
                        enum ns_pool_mode default_mode);

/* Clean up the namespace mapping manager. */
void ns_mapping_mgr_cleanup(struct ns_mapping_mgr *mgr);

/* Create a new namespace with the given NSID and LBA count. */
int ns_mapping_create(struct ns_mapping_mgr *mgr, u32 nsid, u64 lba_count);

/* Delete an existing namespace. */
int ns_mapping_delete(struct ns_mapping_mgr *mgr, u32 nsid);

/* Retrieve information about a namespace. */
int ns_mapping_get_info(const struct ns_mapping_mgr *mgr, u32 nsid,
                        struct ns_mapping_ctx *info);

/* Format a namespace: reset stats but preserve allocation. */
int ns_mapping_format(struct ns_mapping_mgr *mgr, u32 nsid);

/* Return the number of active namespaces. */
u32 ns_mapping_get_active_count(const struct ns_mapping_mgr *mgr);

/* Return the number of unallocated LBAs. */
u64 ns_mapping_get_free_lbas(const struct ns_mapping_mgr *mgr);

#endif /* __HFSSS_NS_MAPPING_H */
