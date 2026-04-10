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

    /* Wear monitoring defaults (from wear_level.h) */
    ctx->wear_monitoring_enabled = WEAR_MONITORING_ENABLED;
    ctx->wear_alert_threshold = WEAR_ALERT_THRESHOLD;
    ctx->wear_critical_threshold = WEAR_CRITICAL_THRESHOLD;

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

/* Wear Monitoring Functions */

int wear_level_update_stats(struct wear_level_ctx *ctx, struct block_mgr *block_mgr)
{
    if (!ctx || !block_mgr || !block_mgr->blocks || block_mgr->total_blocks == 0) {
        return HFSSS_ERR_INVAL;
    }

    u32 max_ec = 0;
    u32 min_ec = UINT32_MAX;
    u64 sum_ec = 0;

    for (u64 i = 0; i < block_mgr->total_blocks; i++) {
        u32 ec = block_mgr->blocks[i].erase_count;
        if (ec > max_ec) max_ec = ec;
        if (ec < min_ec) min_ec = ec;
        sum_ec += ec;
    }

    ctx->max_erase_count = max_ec;
    ctx->min_erase_count = min_ec;
    ctx->avg_erase_count = (u32)(sum_ec / block_mgr->total_blocks);

    return HFSSS_OK;
}

wear_alert_type wear_level_check_wear(struct wear_level_ctx *ctx,
                                      struct block_mgr *block_mgr,
                                      u32 max_pe_cycles)
{
    if (!ctx || !block_mgr || max_pe_cycles == 0) {
        return WEAR_ALERT_NONE;
    }

    /* Respect monitoring-disabled state */
    if (!ctx->wear_monitoring_enabled) {
        return WEAR_ALERT_NONE;
    }

    wear_level_update_stats(ctx, block_mgr);

    u32 pct = (ctx->max_erase_count * 100) / max_pe_cycles;

    if (pct >= ctx->wear_critical_threshold) {
        ctx->critical_alert_count++;
        return WEAR_ALERT_CRITICAL;
    }
    if (pct >= ctx->wear_alert_threshold) {
        ctx->alert_count++;
        return WEAR_ALERT_WARNING;
    }

    return WEAR_ALERT_NONE;
}

u32 wear_level_get_max_erase(struct wear_level_ctx *ctx)
{
    return ctx ? ctx->max_erase_count : 0;
}

u32 wear_level_get_min_erase(struct wear_level_ctx *ctx)
{
    return ctx ? ctx->min_erase_count : 0;
}

u32 wear_level_get_avg_erase(struct wear_level_ctx *ctx)
{
    return ctx ? ctx->avg_erase_count : 0;
}

u64 wear_level_get_alert_count(struct wear_level_ctx *ctx)
{
    return ctx ? ctx->alert_count : 0;
}

u64 wear_level_get_critical_alert_count(struct wear_level_ctx *ctx)
{
    return ctx ? ctx->critical_alert_count : 0;
}
