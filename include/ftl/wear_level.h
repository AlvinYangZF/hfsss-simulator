#ifndef __HFSSS_WEAR_LEVEL_H
#define __HFSSS_WEAR_LEVEL_H

#include "common/common.h"
#include "ftl/block.h"
#include "ftl/mapping.h"

/* Wear Monitoring Configuration */
#define WEAR_MONITORING_ENABLED 1
#define WEAR_ALERT_THRESHOLD 90 /* 90% of max PE cycles */
#define WEAR_CRITICAL_THRESHOLD 95 /* 95% of max PE cycles */

/* Wear Leveling Context */
struct wear_level_ctx {
    u32 enabled;
    u32 static_enabled;         /* Static wear leveling enabled */
    u64 move_count;
    u64 static_move_count;
    u32 static_threshold;       /* Erase count difference threshold for static WL */
    u64 last_static_check_ts;   /* Timestamp of last static WL check */

    /* Wear Monitoring */
    bool wear_monitoring_enabled;
    u32 wear_alert_threshold;    /* Percentage of max PE cycles to trigger alert */
    u32 wear_critical_threshold; /* Percentage of max PE cycles for critical alert */
    u32 max_erase_count;         /* Maximum erase count across all blocks */
    u32 min_erase_count;         /* Minimum erase count across all blocks */
    u32 avg_erase_count;         /* Average erase count across all blocks */
    u64 alert_count;             /* Number of wear alerts issued */
    u64 critical_alert_count;    /* Number of critical wear alerts issued */
};

/* Wear Alert Types */
typedef enum {
    WEAR_ALERT_NONE = 0,
    WEAR_ALERT_WARNING,
    WEAR_ALERT_CRITICAL
} wear_alert_type;

/* Function Prototypes */
int wear_level_init(struct wear_level_ctx *ctx);
void wear_level_cleanup(struct wear_level_ctx *ctx);
int wear_level_set_static_threshold(struct wear_level_ctx *ctx, u32 threshold);
bool wear_level_should_run_static(struct wear_level_ctx *ctx, u64 current_ts, u32 max_erase, u32 min_erase);
int wear_level_run_static(struct wear_level_ctx *ctx, struct block_mgr *block_mgr,
                          struct mapping_ctx *mapping_ctx, void *hal_ctx);

/* Wear Monitoring Functions */
int wear_level_set_alert_threshold(struct wear_level_ctx *ctx, u32 alert_threshold, u32 critical_threshold);
wear_alert_type wear_level_check_wear(struct wear_level_ctx *ctx, struct block_mgr *block_mgr, u32 max_pe_cycles);
int wear_level_update_stats(struct wear_level_ctx *ctx, struct block_mgr *block_mgr);
u32 wear_level_get_max_erase(struct wear_level_ctx *ctx);
u32 wear_level_get_min_erase(struct wear_level_ctx *ctx);
u32 wear_level_get_avg_erase(struct wear_level_ctx *ctx);
u64 wear_level_get_alert_count(struct wear_level_ctx *ctx);
u64 wear_level_get_critical_alert_count(struct wear_level_ctx *ctx);

#endif /* __HFSSS_WEAR_LEVEL_H */
