#ifndef __HFSSS_WEAR_LEVEL_H
#define __HFSSS_WEAR_LEVEL_H

#include "common/common.h"
#include "ftl/block.h"
#include "ftl/mapping.h"

/* Wear Leveling Context */
struct wear_level_ctx {
    u32 enabled;
    u32 static_enabled;         /* Static wear leveling enabled */
    u64 move_count;
    u64 static_move_count;
    u32 static_threshold;       /* Erase count difference threshold for static WL */
    u64 last_static_check_ts;   /* Timestamp of last static WL check */
};

/* Function Prototypes */
int wear_level_init(struct wear_level_ctx *ctx);
void wear_level_cleanup(struct wear_level_ctx *ctx);
int wear_level_set_static_threshold(struct wear_level_ctx *ctx, u32 threshold);
bool wear_level_should_run_static(struct wear_level_ctx *ctx, u64 current_ts, u32 max_erase, u32 min_erase);
int wear_level_run_static(struct wear_level_ctx *ctx, struct block_mgr *block_mgr,
                          struct mapping_ctx *mapping_ctx, void *hal_ctx);

#endif /* __HFSSS_WEAR_LEVEL_H */
