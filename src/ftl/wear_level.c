#include "ftl/wear_level.h"
#include "ftl/block.h"
#include "ftl/mapping.h"
#include <string.h>

#define STATIC_WL_DEFAULT_THRESHOLD 100
#define STATIC_WL_CHECK_INTERVAL 100000000000ULL /* 100 seconds in ns */

int wear_level_init(struct wear_level_ctx *ctx)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->enabled = 1;
    ctx->static_enabled = 1;
    ctx->move_count = 0;
    ctx->static_move_count = 0;
    ctx->static_threshold = STATIC_WL_DEFAULT_THRESHOLD;
    ctx->last_static_check_ts = 0;

    return HFSSS_OK;
}

void wear_level_cleanup(struct wear_level_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
}

int wear_level_set_static_threshold(struct wear_level_ctx *ctx, u32 threshold)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    ctx->static_threshold = threshold;
    return HFSSS_OK;
}

bool wear_level_should_run_static(struct wear_level_ctx *ctx, u64 current_ts, u32 max_erase, u32 min_erase)
{
    if (!ctx || !ctx->enabled || !ctx->static_enabled) {
        return false;
    }

    /* Check if enough time has passed since last check */
    if (current_ts < ctx->last_static_check_ts + STATIC_WL_CHECK_INTERVAL) {
        return false;
    }

    /* Check if erase count difference exceeds threshold */
    if (max_erase > min_erase + ctx->static_threshold) {
        return true;
    }

    return false;
}

int wear_level_run_static(struct wear_level_ctx *ctx, struct block_mgr *block_mgr,
                          struct mapping_ctx *mapping_ctx, void *hal_ctx)
{
    /* Placeholder for static wear leveling implementation */
    /* In a real implementation, this would:
     * 1. Find a block with low erase count and static data (valid pages not modified for a long time)
     * 2. Move that static data to a block with high erase count
     * 3. Update mappings accordingly
     */
    (void)ctx;
    (void)block_mgr;
    (void)mapping_ctx;
    (void)hal_ctx;

    /* Increment static move count for demonstration */
    if (ctx) {
        ctx->static_move_count++;
        ctx->last_static_check_ts = get_time_ns();
    }

    return HFSSS_OK;
}
