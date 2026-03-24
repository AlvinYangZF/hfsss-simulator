#ifndef __HFSSS_NS_GC_H
#define __HFSSS_NS_GC_H

#include "common/common.h"

#define NS_GC_MAX_NS 32

/* Per-namespace GC urgency */
struct ns_gc_urgency {
    u32 nsid;
    double score;          /* 0.0 (no urgency) to 1.0 (critical) */
    u64 free_blocks;
    u64 total_blocks;
    u64 valid_pages;
    u64 invalid_pages;
    bool active;
};

/* Per-namespace WAF tracker */
struct ns_waf_tracker {
    u32 nsid;
    u64 host_write_pages;
    u64 nand_write_pages;  /* includes GC writes */
    bool active;
};

/* Multi-NS GC coordinator */
struct ns_gc_coordinator {
    struct ns_gc_urgency urgency[NS_GC_MAX_NS];
    struct ns_waf_tracker waf[NS_GC_MAX_NS];
    u32 total_gc_budget;    /* pages per GC cycle */
    double urgency_threshold;
    double critical_threshold;
    u32 min_budget_per_ns;
    bool initialized;
};

/* Initialize the multi-NS GC coordinator. */
int ns_gc_coordinator_init(struct ns_gc_coordinator *coord, u32 total_budget,
                           double urgency_thresh, double critical_thresh,
                           u32 min_budget);

/* Clean up the GC coordinator. */
void ns_gc_coordinator_cleanup(struct ns_gc_coordinator *coord);

/* Register a namespace with the GC coordinator. */
int ns_gc_register_ns(struct ns_gc_coordinator *coord, u32 nsid,
                      u64 total_blocks);

/* Unregister a namespace from the GC coordinator. */
int ns_gc_unregister_ns(struct ns_gc_coordinator *coord, u32 nsid);

/* Calculate urgency score for given block/page statistics. */
double ns_gc_calculate_urgency(u64 free_blocks, u64 total_blocks,
                               u64 invalid_pages, u64 valid_pages);

/* Update urgency for a specific namespace. */
int ns_gc_update_urgency(struct ns_gc_coordinator *coord, u32 nsid,
                         u64 free_blocks, u64 total_blocks,
                         u64 invalid_pages, u64 valid_pages);

/* Allocate GC budget across namespaces proportional to urgency. */
int ns_gc_allocate_budget(struct ns_gc_coordinator *coord, u32 *budgets_out);

/* Record host and NAND write pages for WAF tracking. */
void ns_gc_record_write(struct ns_gc_coordinator *coord, u32 nsid,
                        u64 host_pages, u64 nand_pages);

/* Get the write amplification factor for a namespace. */
double ns_gc_get_waf(const struct ns_gc_coordinator *coord, u32 nsid);

#endif /* __HFSSS_NS_GC_H */
