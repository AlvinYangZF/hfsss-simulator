#ifndef HFSSS_FTL_RELIABILITY_H
#define HFSSS_FTL_RELIABILITY_H

#include <stdint.h>
#include <stdbool.h>
#include "common/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * FTL Reliability: PE cycle monitoring, spare block management,
 * and write-cliff protection (REQ-110 – REQ-115)
 * ------------------------------------------------------------------ */

/* Default thresholds */
#define FTL_REL_MAX_PE_CYCLES_DEFAULT    3000u
#define FTL_REL_SPARE_LOW_WARN_RATIO     10u   /* warn when spare < 10% */
#define FTL_REL_SPARE_CRIT_RATIO         3u    /* critical when spare < 3% */
#define FTL_REL_HOT_BLOCK_PE_RATIO       200u  /* mark hot if PE > 200% of avg */

/* Health state of the FTL */
enum ftl_health_state {
    FTL_HEALTH_GOOD     = 0,  /* normal operation */
    FTL_HEALTH_DEGRADED = 1,  /* spare blocks low or PE variance high */
    FTL_HEALTH_CRITICAL = 2,  /* spare critically low, write cliff risk */
    FTL_HEALTH_FAILED   = 3,  /* cannot guarantee data integrity */
};

/* Per-block wear record */
struct block_wear_entry {
    uint32_t block_id;
    uint32_t pe_count;
};

/* FTL reliability configuration */
struct ftl_reliability_cfg {
    uint32_t total_blocks;          /* total physical blocks */
    uint32_t spare_blocks;          /* over-provisioned spare count */
    uint32_t max_pe_cycles;         /* hard limit per block */
    uint32_t spare_low_warn_pct;    /* % spare below which WARN fires */
    uint32_t spare_crit_pct;        /* % spare below which CRITICAL fires */
    uint32_t hot_block_ratio_pct;   /* % above avg PE to flag hot block */
};

/* FTL reliability statistics (feeds into SMART, REQ-115) */
struct ftl_reliability_stats {
    uint32_t max_pe_seen;           /* maximum PE count seen across all blocks */
    uint32_t avg_pe;                /* average PE count */
    uint32_t hot_block_count;       /* blocks above hot threshold */
    uint32_t worn_block_count;      /* blocks at or above max_pe_cycles */
    uint32_t spare_remaining;       /* spare blocks still available */
    uint32_t spare_remaining_pct;   /* spare as % of total */
    enum ftl_health_state health;
};

/* FTL reliability context */
struct ftl_reliability_ctx {
    struct ftl_reliability_cfg cfg;
    struct ftl_reliability_stats stats;

    /* Wear table: one entry per block */
    struct block_wear_entry *wear_table;
    uint32_t wear_table_len;

    bool initialized;
};

/* ------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------ */

/* Lifecycle */
int  ftl_rel_init(struct ftl_reliability_ctx *ctx,
                  const struct ftl_reliability_cfg *cfg);
void ftl_rel_cleanup(struct ftl_reliability_ctx *ctx);

/* Per-erase notification — call after every block erase */
int  ftl_rel_notify_erase(struct ftl_reliability_ctx *ctx, uint32_t block_id);

/* Periodic or on-demand health check — updates ctx->stats */
int  ftl_rel_check_health(struct ftl_reliability_ctx *ctx);

/* Query */
enum ftl_health_state ftl_rel_get_health(const struct ftl_reliability_ctx *ctx);
void ftl_rel_get_stats(const struct ftl_reliability_ctx *ctx,
                       struct ftl_reliability_stats *out);

/* Block-level accessors */
uint32_t ftl_rel_get_pe_count(const struct ftl_reliability_ctx *ctx,
                               uint32_t block_id);
bool     ftl_rel_is_worn(const struct ftl_reliability_ctx *ctx,
                          uint32_t block_id);
bool     ftl_rel_is_hot(const struct ftl_reliability_ctx *ctx,
                         uint32_t block_id);

/* Spare management */
int  ftl_rel_consume_spare(struct ftl_reliability_ctx *ctx);
int  ftl_rel_return_spare(struct ftl_reliability_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* HFSSS_FTL_RELIABILITY_H */
