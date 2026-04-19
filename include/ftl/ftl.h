#ifndef __HFSSS_FTL_H
#define __HFSSS_FTL_H

#include "common/common.h"
#include "common/mutex.h"
#include "ftl/mapping.h"
#include "ftl/block.h"
#include "ftl/gc.h"
#include "ftl/wear_level.h"
#include "ftl/ecc.h"
#include "ftl/error.h"
#include "hal/hal.h"
#include "ftl/superblock.h"
#include "media/cmd_state.h"

struct nand_profile;

/* FTL Configuration */
struct ftl_config {
    u64 total_lbas;
    u32 page_size;
    u32 pages_per_block;
    u32 blocks_per_plane;
    u32 planes_per_die;
    u32 dies_per_chip;
    u32 chips_per_channel;
    u32 channel_count;
    u32 op_ratio;         /* Over-provisioning ratio (percentage) */
    enum gc_policy gc_policy;
    u32 gc_threshold;
    u32 gc_hiwater;
    u32 gc_lowater;
};

/* FTL Statistics */
struct ftl_stats {
    u64 read_count;
    u64 write_count;
    u64 trim_count;
    u64 read_bytes;
    u64 write_bytes;
    u64 gc_count;
    u64 moved_pages;
    u64 reclaimed_blocks;
    u64 host_write_pages;     /* Pages written by host */
    u64 gc_write_pages;       /* Pages written by GC */
    double waf;              /* Write Amplification Factor */
};

/* FTL Context */
struct ftl_ctx {
    struct ftl_config config;
    struct mapping_ctx mapping;
    struct block_mgr block_mgr;
    struct cwb *cwbs;
    u32 cwb_count;
    struct gc_ctx gc;
    struct wear_level_ctx wl;
    struct ecc_ctx ecc;
    struct error_ctx error;
    struct superblock_ctx sb;
    struct hal_ctx *hal;
    /*
     * NAND profile resolved at ftl_init via hal_get_profile(). Read-only
     * snapshot for the lifetime of the FTL context. Never updated after
     * init (a profile switch would require rebuilding the stack).
     * NULL if HAL has no profile attached (legacy code paths).
     */
    const struct nand_profile *profile;
    struct ftl_stats stats;
    struct mutex lock;
    bool initialized;
};

/* Function Prototypes */
int ftl_init(struct ftl_ctx *ctx, struct ftl_config *config, struct hal_ctx *hal);
void ftl_cleanup(struct ftl_ctx *ctx);
int ftl_read(struct ftl_ctx *ctx, u64 lba, u32 len, void *data);
int ftl_write(struct ftl_ctx *ctx, u64 lba, u32 len, const void *data);
int ftl_trim(struct ftl_ctx *ctx, u64 lba, u32 len);
int ftl_flush(struct ftl_ctx *ctx);
int ftl_checkpoint(struct ftl_ctx *ctx);
void ftl_get_stats(struct ftl_ctx *ctx, struct ftl_stats *stats);
void ftl_reset_stats(struct ftl_ctx *ctx);

/* Internal functions for testing */
int ftl_map_l2p(struct ftl_ctx *ctx, u64 lba, union ppn *ppn);
int ftl_unmap_lba(struct ftl_ctx *ctx, u64 lba);
int ftl_gc_trigger(struct ftl_ctx *ctx);

/*
 * Profile query API. These all return safe defaults when ctx->profile is
 * NULL so legacy code paths that skip profile resolution keep working:
 *   - ftl_get_profile: returns the resolved profile pointer or NULL
 *   - ftl_preferred_plane_batch: the per-command plane cap the profile
 *     advertises (mp_rules.max_planes_per_cmd) or 1 if no profile
 *   - ftl_supports_op: true if the profile advertises the opcode or if
 *     no profile is attached (assume supported — matches pre-Phase-7
 *     behavior where every op was legal at the bitmap layer)
 */
const struct nand_profile *ftl_get_profile(struct ftl_ctx *ctx);
u8 ftl_preferred_plane_batch(struct ftl_ctx *ctx);
bool ftl_supports_op(struct ftl_ctx *ctx, enum nand_cmd_opcode op);


/* Multi-threaded page operations — use TAA shards instead of global lock */
struct taa_ctx;
int ftl_read_page_mt(struct ftl_ctx *ctx, struct taa_ctx *taa,
                     u64 lba, void *data);
int ftl_write_page_mt(struct ftl_ctx *ctx, struct taa_ctx *taa,
                      u64 lba, const void *data);
int ftl_trim_page_mt(struct ftl_ctx *ctx, struct taa_ctx *taa, u64 lba);
#endif /* __HFSSS_FTL_H */
