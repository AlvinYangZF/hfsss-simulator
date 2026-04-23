#ifndef __HFSSS_GC_H
#define __HFSSS_GC_H

#include "common/common.h"
#include "common/mutex.h"
#include "ftl/block.h"

/*
 * Optional duty-cycle admission callback. The FTL does not link the
 * controller's det_window directly (that would pull controller into
 * the FTL lib); instead a caller can register a callback that
 * gc_run_mt consults before each page move. Return true to permit
 * the move, false to bail out of the remaining moves in this run.
 * The callback is expected to do any stats / accounting itself.
 */
typedef bool (*gc_admit_gc_fn)(void *ctx, u64 now_ns);

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

    /*
     * Optional duty-cycle admission hook. When set, gc_run_mt
     * consults admit_gc_fn(admit_gc_ctx, now_ns) once per candidate
     * page and breaks out of the remaining moves if the callback
     * returns false. Matches HLD_02 §11.3 "No GC page moves occur
     * during HOST_IO window" at the finest available granularity.
     * The callback indirection keeps libhfsss-ftl free of a direct
     * link dependency on libhfsss-controller (where det_window
     * lives). Test / production callers bind the callback to
     * det_window_admit_gc via gc_attach_admit_cb.
     */
    gc_admit_gc_fn   admit_gc_fn;
    void            *admit_gc_ctx;

    /*
     * Count of times the admit callback rejected a GC attempt.
     * Incremented under ctx->lock whenever gc_run_mt bails its
     * per-move loop. Useful for tests and for hfsss-ctrl to
     * report how often GC was gated.
     */
    u64 det_window_rejects;
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

/*
 * Attach an optional duty-cycle admission callback. Pass fn=NULL
 * to detach. Lifetime of the callback + ctx must outlive the
 * gc_ctx, or detach before freeing them.
 */
void gc_attach_admit_cb(struct gc_ctx *ctx,
                        gc_admit_gc_fn fn, void *cb_ctx);

/* MT-aware GC: uses TAA for valid page lookup instead of global mapping */
struct taa_ctx;  /* forward declaration */
int gc_run_mt(struct gc_ctx *ctx, struct block_mgr *block_mgr,
              struct taa_ctx *taa, void *hal_ctx);

#endif /* __HFSSS_GC_H */
