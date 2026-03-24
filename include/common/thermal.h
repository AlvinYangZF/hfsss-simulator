#ifndef __HFSSS_THERMAL_H
#define __HFSSS_THERMAL_H

#include "common/common.h"

/*
 * Thermal Management Module (REQ-171 through REQ-174)
 *
 * Provides progressive thermal throttling with hysteresis and
 * thermal shutdown detection. The throttle_factor output is a
 * standalone value (0.0-1.0) consumed by the caller.
 */

/* Thermal throttle levels */
#define THERMAL_LEVEL_NONE     0   /* < 75C, factor 1.00 */
#define THERMAL_LEVEL_LIGHT    1   /* >= 75C, factor 0.80 */
#define THERMAL_LEVEL_MODERATE 2   /* >= 80C, factor 0.50 */
#define THERMAL_LEVEL_HEAVY    3   /* >= 85C, factor 0.20 */
#define THERMAL_LEVEL_SHUTDOWN 4   /* >= 90C, factor 0.00 */

/* Thresholds in Celsius */
#define THERMAL_THRESH_LIGHT    75
#define THERMAL_THRESH_MODERATE 80
#define THERMAL_THRESH_HEAVY    85
#define THERMAL_THRESH_SHUTDOWN 90
#define THERMAL_HYSTERESIS      3

/* Throttle factors per level */
static const double thermal_factors[] = { 1.0, 0.80, 0.50, 0.20, 0.0 };

/* Thermal throttle context */
struct thermal_throttle_ctx {
    u8  current_level;      /* THERMAL_LEVEL_* */
    double throttle_factor; /* 0.0-1.0 */
    u64 level_enter_ns;     /* timestamp of current level entry */
    u64 total_throttle_ns;  /* cumulative time in throttled states */
    u32 warn_temp_minutes;  /* minutes above 70C */
    u32 crit_temp_minutes;  /* minutes above 85C */
    u64 last_minute_ns;     /* last minute boundary timestamp */
    bool initialized;
};

/* Initialize thermal throttle context */
int thermal_throttle_init(struct thermal_throttle_ctx *ctx);

/* Clean up thermal throttle context */
void thermal_throttle_cleanup(struct thermal_throttle_ctx *ctx);

/* Compute thermal level with 3C hysteresis to prevent oscillation */
u8 thermal_compute_level(double temp_c, u8 current_level);

/* Get throttle factor for a given level */
double thermal_get_factor(u8 level);

/* Update warn/crit time counters based on temperature and elapsed time */
void thermal_update_counters(struct thermal_throttle_ctx *ctx,
                             double temp_c, u64 now_ns);

/* Check if a level represents thermal shutdown */
bool thermal_is_shutdown(u8 level);

#endif /* __HFSSS_THERMAL_H */
