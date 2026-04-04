#ifndef __HFSSS_GC_H
#define __HFSSS_GC_H

#include "common/common.h"
#include "common/mutex.h"
#include "ftl/block.h"

/* GC Policy */
enum gc_policy {
    GC_POLICY_GREEDY = 0,
    GC_POLICY_COST_BENEFIT = 1,
    GC_POLICY_FIFO = 2,
};

/* GC Context */
struct gc_ctx {
    enum gc_policy policy;
    u32 threshold;      /* Trigger GC when free blocks < threshold */
    u32 hiwater;        /* High watermark */
    u32 lowater;        /* Low watermark */
    struct block_desc *victim;
    bool running;
    u64 gc_count;
    u64 moved_pages;
    u64 reclaimed_blocks;
    u64 gc_write_pages;   /* Pages written by GC */
    struct mutex lock;
    /*
     * Persistent destination block: kept open across multiple GC cycles so
     * that successive victims can share one destination block.  This allows
     * net-positive block reclamation even at high utilization ratios.
     */
    struct block_desc *dst_block;
    u32               dst_page;
};

/* Function Prototypes */
int gc_init(struct gc_ctx *ctx, enum gc_policy policy, u32 threshold, u32 hiwater, u32 lowater);
void gc_cleanup(struct gc_ctx *ctx);
bool gc_should_trigger(struct gc_ctx *ctx, u64 free_blocks);
int gc_run(struct gc_ctx *ctx, struct block_mgr *block_mgr, struct mapping_ctx *mapping_ctx,
           void *hal_ctx);
void gc_get_stats(struct gc_ctx *ctx, u64 *gc_count, u64 *moved_pages, u64 *reclaimed_blocks, u64 *gc_write_pages);
void gc_print_debug_stats(void);
void gc_flush_dst(struct gc_ctx *ctx, struct block_mgr *block_mgr);

/* MT-aware GC: uses TAA for valid page lookup instead of global mapping */
struct taa_ctx;  /* forward declaration */
int gc_run_mt(struct gc_ctx *ctx, struct block_mgr *block_mgr,
              struct taa_ctx *taa, void *hal_ctx);

#endif /* __HFSSS_GC_H */
