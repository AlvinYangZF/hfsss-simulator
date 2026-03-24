/*
 * ns_mapping.c - Multi-namespace mapping manager (REQ-166 through REQ-170)
 *
 * Manages per-namespace LBA ranges, allocation tracking, and format operations.
 * This layer wraps on top of the existing mapping.c L2P/P2L tables.
 */

#include "ftl/ns_mapping.h"

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/* Find the slot index for a given NSID.  Returns -1 if not found. */
static int find_ns_slot(const struct ns_mapping_mgr *mgr, u32 nsid) {
    for (u32 i = 0; i < NS_MAX_NAMESPACES; i++) {
        if (mgr->namespaces[i].active && mgr->namespaces[i].nsid == nsid) {
            return (int)i;
        }
    }
    return -1;
}

/* Find the first free (inactive) slot.  Returns -1 if full. */
static int find_free_slot(const struct ns_mapping_mgr *mgr) {
    for (u32 i = 0; i < NS_MAX_NAMESPACES; i++) {
        if (!mgr->namespaces[i].active) {
            return (int)i;
        }
    }
    return -1;
}

/* Compute the next available LBA start address (contiguous allocation). */
static u64 compute_next_lba_start(const struct ns_mapping_mgr *mgr) {
    u64 max_end = 0;
    for (u32 i = 0; i < NS_MAX_NAMESPACES; i++) {
        if (mgr->namespaces[i].active) {
            u64 end = mgr->namespaces[i].lba_start +
                      mgr->namespaces[i].lba_count;
            if (end > max_end) {
                max_end = end;
            }
        }
    }
    return max_end;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int ns_mapping_mgr_init(struct ns_mapping_mgr *mgr, u64 total_lbas,
                        enum ns_pool_mode default_mode) {
    if (!mgr) {
        return HFSSS_ERR_INVAL;
    }
    if (total_lbas == 0) {
        return HFSSS_ERR_INVAL;
    }

    memset(mgr, 0, sizeof(*mgr));
    mgr->total_lbas = total_lbas;
    mgr->allocated_lbas = 0;
    mgr->active_count = 0;
    mgr->default_mode = default_mode;

    int ret = mutex_init(&mgr->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    for (u32 i = 0; i < NS_MAX_NAMESPACES; i++) {
        memset(&mgr->namespaces[i], 0, sizeof(mgr->namespaces[i]));
        mgr->namespaces[i].active = false;
    }

    mgr->initialized = true;
    return HFSSS_OK;
}

void ns_mapping_mgr_cleanup(struct ns_mapping_mgr *mgr) {
    if (!mgr || !mgr->initialized) {
        return;
    }

    mutex_lock(&mgr->lock, 0);

    for (u32 i = 0; i < NS_MAX_NAMESPACES; i++) {
        mgr->namespaces[i].active = false;
    }
    mgr->active_count = 0;
    mgr->allocated_lbas = 0;

    mutex_unlock(&mgr->lock);
    mutex_cleanup(&mgr->lock);
    mgr->initialized = false;
}

int ns_mapping_create(struct ns_mapping_mgr *mgr, u32 nsid, u64 lba_count) {
    if (!mgr || !mgr->initialized) {
        return HFSSS_ERR_INVAL;
    }
    if (nsid == 0 || lba_count == 0) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&mgr->lock, 0);

    /* Check for duplicate NSID */
    if (find_ns_slot(mgr, nsid) >= 0) {
        mutex_unlock(&mgr->lock);
        return HFSSS_ERR_EXIST;
    }

    /* Check capacity */
    if (mgr->allocated_lbas + lba_count > mgr->total_lbas) {
        mutex_unlock(&mgr->lock);
        return HFSSS_ERR_NOSPC;
    }

    /* Find a free slot */
    int slot = find_free_slot(mgr);
    if (slot < 0) {
        mutex_unlock(&mgr->lock);
        return HFSSS_ERR_NOSPC;
    }

    /* Compute LBA start for this namespace */
    u64 lba_start = compute_next_lba_start(mgr);

    struct ns_mapping_ctx *ns = &mgr->namespaces[slot];
    ns->nsid = nsid;
    ns->lba_start = lba_start;
    ns->lba_count = lba_count;
    ns->pool_mode = mgr->default_mode;
    ns->allocated_blocks = 0;
    ns->valid_pages = 0;
    ns->invalid_pages = 0;
    ns->active = true;

    mgr->allocated_lbas += lba_count;
    mgr->active_count++;

    mutex_unlock(&mgr->lock);
    return HFSSS_OK;
}

int ns_mapping_delete(struct ns_mapping_mgr *mgr, u32 nsid) {
    if (!mgr || !mgr->initialized) {
        return HFSSS_ERR_INVAL;
    }
    if (nsid == 0) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&mgr->lock, 0);

    int slot = find_ns_slot(mgr, nsid);
    if (slot < 0) {
        mutex_unlock(&mgr->lock);
        return HFSSS_ERR_NOENT;
    }

    struct ns_mapping_ctx *ns = &mgr->namespaces[slot];
    u64 freed_lbas = ns->lba_count;

    memset(ns, 0, sizeof(*ns));
    ns->active = false;

    mgr->allocated_lbas -= freed_lbas;
    mgr->active_count--;

    mutex_unlock(&mgr->lock);
    return HFSSS_OK;
}

int ns_mapping_get_info(const struct ns_mapping_mgr *mgr, u32 nsid,
                        struct ns_mapping_ctx *info) {
    if (!mgr || !mgr->initialized || !info) {
        return HFSSS_ERR_INVAL;
    }
    if (nsid == 0) {
        return HFSSS_ERR_INVAL;
    }

    /* Cast away const for lock -- read-side locking pattern */
    struct ns_mapping_mgr *m = (struct ns_mapping_mgr *)mgr;
    mutex_lock(&m->lock, 0);

    int slot = find_ns_slot(mgr, nsid);
    if (slot < 0) {
        mutex_unlock(&m->lock);
        return HFSSS_ERR_NOENT;
    }

    memcpy(info, &mgr->namespaces[slot], sizeof(*info));

    mutex_unlock(&m->lock);
    return HFSSS_OK;
}

int ns_mapping_format(struct ns_mapping_mgr *mgr, u32 nsid) {
    if (!mgr || !mgr->initialized) {
        return HFSSS_ERR_INVAL;
    }
    if (nsid == 0) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&mgr->lock, 0);

    int slot = find_ns_slot(mgr, nsid);
    if (slot < 0) {
        mutex_unlock(&mgr->lock);
        return HFSSS_ERR_NOENT;
    }

    struct ns_mapping_ctx *ns = &mgr->namespaces[slot];

    /* Reset statistics but preserve allocation (nsid, lba_start, lba_count,
     * pool_mode, allocated_blocks, active). */
    ns->valid_pages = 0;
    ns->invalid_pages = 0;

    mutex_unlock(&mgr->lock);
    return HFSSS_OK;
}

u32 ns_mapping_get_active_count(const struct ns_mapping_mgr *mgr) {
    if (!mgr || !mgr->initialized) {
        return 0;
    }
    return mgr->active_count;
}

u64 ns_mapping_get_free_lbas(const struct ns_mapping_mgr *mgr) {
    if (!mgr || !mgr->initialized) {
        return 0;
    }
    if (mgr->allocated_lbas >= mgr->total_lbas) {
        return 0;
    }
    return mgr->total_lbas - mgr->allocated_lbas;
}
