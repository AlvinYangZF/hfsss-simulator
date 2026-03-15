#include "ftl/ftl_reliability.h"
#include "common/log.h"
#include "common/common.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */
int ftl_rel_init(struct ftl_reliability_ctx *ctx,
                 const struct ftl_reliability_cfg *cfg) {
    if (!ctx || !cfg) return HFSSS_ERR_INVAL;
    if (cfg->total_blocks == 0) return HFSSS_ERR_INVAL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg = *cfg;

    /* Apply defaults for zero-valued optional fields */
    if (ctx->cfg.max_pe_cycles == 0)
        ctx->cfg.max_pe_cycles = FTL_REL_MAX_PE_CYCLES_DEFAULT;
    if (ctx->cfg.spare_low_warn_pct == 0)
        ctx->cfg.spare_low_warn_pct = FTL_REL_SPARE_LOW_WARN_RATIO;
    if (ctx->cfg.spare_crit_pct == 0)
        ctx->cfg.spare_crit_pct = FTL_REL_SPARE_CRIT_RATIO;
    if (ctx->cfg.hot_block_ratio_pct == 0)
        ctx->cfg.hot_block_ratio_pct = FTL_REL_HOT_BLOCK_PE_RATIO;

    ctx->wear_table_len = cfg->total_blocks;
    ctx->wear_table = (struct block_wear_entry *)calloc(
        ctx->wear_table_len, sizeof(struct block_wear_entry));
    if (!ctx->wear_table) return HFSSS_ERR_NOMEM;

    for (uint32_t i = 0; i < ctx->wear_table_len; i++) {
        ctx->wear_table[i].block_id = i;
        ctx->wear_table[i].pe_count = 0;
    }

    ctx->stats.spare_remaining     = cfg->spare_blocks;
    ctx->stats.spare_remaining_pct = (cfg->total_blocks > 0)
        ? (cfg->spare_blocks * 100u / cfg->total_blocks) : 0;
    ctx->stats.health = FTL_HEALTH_GOOD;
    ctx->initialized  = true;

    HFSSS_LOG_INFO("FTL-REL", "reliability ctx init: %u blocks, %u spare, max_pe=%u",
                   cfg->total_blocks, cfg->spare_blocks, ctx->cfg.max_pe_cycles);
    return HFSSS_OK;
}

void ftl_rel_cleanup(struct ftl_reliability_ctx *ctx) {
    if (!ctx || !ctx->initialized) return;
    free(ctx->wear_table);
    ctx->wear_table     = NULL;
    ctx->wear_table_len = 0;
    ctx->initialized    = false;
}

/* ------------------------------------------------------------------
 * Per-erase notification
 * ------------------------------------------------------------------ */
int ftl_rel_notify_erase(struct ftl_reliability_ctx *ctx, uint32_t block_id) {
    if (!ctx || !ctx->initialized) return HFSSS_ERR_INVAL;
    if (block_id >= ctx->wear_table_len)  return HFSSS_ERR_INVAL;

    ctx->wear_table[block_id].pe_count++;
    uint32_t pe = ctx->wear_table[block_id].pe_count;

    if (pe >= ctx->cfg.max_pe_cycles) {
        HFSSS_LOG_WARN("FTL-REL", "block %u reached max PE cycles (%u)",
                       block_id, pe);
        ctx->stats.worn_block_count++;
    }
    if (pe > ctx->stats.max_pe_seen)
        ctx->stats.max_pe_seen = pe;

    return HFSSS_OK;
}

/* ------------------------------------------------------------------
 * Health check — recomputes stats from the wear table
 * ------------------------------------------------------------------ */
int ftl_rel_check_health(struct ftl_reliability_ctx *ctx) {
    if (!ctx || !ctx->initialized) return HFSSS_ERR_INVAL;

    uint64_t sum = 0;
    uint32_t worn  = 0;
    uint32_t max_pe = 0;

    for (uint32_t i = 0; i < ctx->wear_table_len; i++) {
        uint32_t pe = ctx->wear_table[i].pe_count;
        sum += pe;
        if (pe > max_pe) max_pe = pe;
        if (pe >= ctx->cfg.max_pe_cycles) worn++;
    }

    uint32_t avg = (ctx->wear_table_len > 0)
        ? (uint32_t)(sum / ctx->wear_table_len) : 0;

    /* Count hot blocks */
    uint32_t hot = 0;
    if (avg > 0) {
        uint64_t threshold = (uint64_t)avg * ctx->cfg.hot_block_ratio_pct / 100u;
        for (uint32_t i = 0; i < ctx->wear_table_len; i++) {
            if (ctx->wear_table[i].pe_count > threshold) hot++;
        }
    }

    ctx->stats.max_pe_seen      = max_pe;
    ctx->stats.avg_pe           = avg;
    ctx->stats.hot_block_count  = hot;
    ctx->stats.worn_block_count = worn;

    uint32_t spare_pct = (ctx->cfg.total_blocks > 0)
        ? (ctx->stats.spare_remaining * 100u / ctx->cfg.total_blocks) : 0;
    ctx->stats.spare_remaining_pct = spare_pct;

    /* Derive health state */
    if (worn > 0 && ctx->stats.spare_remaining == 0) {
        ctx->stats.health = FTL_HEALTH_FAILED;
    } else if (spare_pct <= ctx->cfg.spare_crit_pct) {
        ctx->stats.health = FTL_HEALTH_CRITICAL;
    } else if (spare_pct <= ctx->cfg.spare_low_warn_pct || hot > 0) {
        ctx->stats.health = FTL_HEALTH_DEGRADED;
    } else {
        ctx->stats.health = FTL_HEALTH_GOOD;
    }

    HFSSS_LOG_INFO("FTL-REL",
        "health=%d max_pe=%u avg_pe=%u worn=%u hot=%u spare=%u(%u%%)",
        ctx->stats.health, max_pe, avg, worn, hot,
        ctx->stats.spare_remaining, spare_pct);

    return HFSSS_OK;
}

/* ------------------------------------------------------------------
 * Query helpers
 * ------------------------------------------------------------------ */
enum ftl_health_state ftl_rel_get_health(const struct ftl_reliability_ctx *ctx) {
    if (!ctx || !ctx->initialized) return FTL_HEALTH_FAILED;
    return ctx->stats.health;
}

void ftl_rel_get_stats(const struct ftl_reliability_ctx *ctx,
                       struct ftl_reliability_stats *out) {
    if (!ctx || !out) return;
    *out = ctx->stats;
}

uint32_t ftl_rel_get_pe_count(const struct ftl_reliability_ctx *ctx,
                               uint32_t block_id) {
    if (!ctx || !ctx->initialized) return 0;
    if (block_id >= ctx->wear_table_len) return 0;
    return ctx->wear_table[block_id].pe_count;
}

bool ftl_rel_is_worn(const struct ftl_reliability_ctx *ctx, uint32_t block_id) {
    if (!ctx || !ctx->initialized) return false;
    if (block_id >= ctx->wear_table_len) return false;
    return ctx->wear_table[block_id].pe_count >= ctx->cfg.max_pe_cycles;
}

bool ftl_rel_is_hot(const struct ftl_reliability_ctx *ctx, uint32_t block_id) {
    if (!ctx || !ctx->initialized) return false;
    if (block_id >= ctx->wear_table_len) return false;
    if (ctx->stats.avg_pe == 0) return false;
    uint64_t threshold = (uint64_t)ctx->stats.avg_pe *
                         ctx->cfg.hot_block_ratio_pct / 100u;
    return ctx->wear_table[block_id].pe_count > threshold;
}

/* ------------------------------------------------------------------
 * Spare block management
 * ------------------------------------------------------------------ */
int ftl_rel_consume_spare(struct ftl_reliability_ctx *ctx) {
    if (!ctx || !ctx->initialized) return HFSSS_ERR_INVAL;
    if (ctx->stats.spare_remaining == 0) {
        HFSSS_LOG_WARN("FTL-REL", "no spare blocks left");
        return HFSSS_ERR_NOSPC;
    }
    ctx->stats.spare_remaining--;
    ctx->stats.spare_remaining_pct = (ctx->cfg.total_blocks > 0)
        ? (ctx->stats.spare_remaining * 100u / ctx->cfg.total_blocks) : 0;

    if (ctx->stats.spare_remaining_pct <= ctx->cfg.spare_crit_pct) {
        HFSSS_LOG_WARN("FTL-REL", "spare critically low: %u blocks (%u%%)",
                       ctx->stats.spare_remaining, ctx->stats.spare_remaining_pct);
        ctx->stats.health = FTL_HEALTH_CRITICAL;
    } else if (ctx->stats.spare_remaining_pct <= ctx->cfg.spare_low_warn_pct) {
        HFSSS_LOG_WARN("FTL-REL", "spare low: %u blocks (%u%%)",
                       ctx->stats.spare_remaining, ctx->stats.spare_remaining_pct);
        if (ctx->stats.health < FTL_HEALTH_DEGRADED)
            ctx->stats.health = FTL_HEALTH_DEGRADED;
    }
    return HFSSS_OK;
}

int ftl_rel_return_spare(struct ftl_reliability_ctx *ctx) {
    if (!ctx || !ctx->initialized) return HFSSS_ERR_INVAL;
    if (ctx->stats.spare_remaining >= ctx->cfg.spare_blocks)
        return HFSSS_ERR_INVAL;  /* cannot exceed original spare count */
    ctx->stats.spare_remaining++;
    return HFSSS_OK;
}
