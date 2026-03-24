#include "common/thermal.h"
#include <string.h>

/*
 * Thermal Management Implementation (REQ-171 through REQ-174)
 *
 * Progressive thermal throttling with hysteresis to prevent
 * oscillation at threshold boundaries. Tracks cumulative time
 * spent above warning (70C) and critical (85C) temperatures.
 */

/* Minutes in nanoseconds */
#define NS_PER_MINUTE (60ULL * 1000000000ULL)

/* Warning and critical temperature thresholds for time counters */
#define TEMP_WARN_THRESHOLD  70
#define TEMP_CRIT_THRESHOLD  85

int thermal_throttle_init(struct thermal_throttle_ctx *ctx) {
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->current_level = THERMAL_LEVEL_NONE;
    ctx->throttle_factor = thermal_factors[THERMAL_LEVEL_NONE];
    ctx->level_enter_ns = get_time_ns();
    ctx->last_minute_ns = ctx->level_enter_ns;
    ctx->initialized = true;

    return HFSSS_OK;
}

void thermal_throttle_cleanup(struct thermal_throttle_ctx *ctx) {
    if (!ctx || !ctx->initialized) {
        return;
    }

    /* Accumulate final throttle time if we were in a throttled state */
    if (ctx->current_level > THERMAL_LEVEL_NONE) {
        u64 now = get_time_ns();
        if (now > ctx->level_enter_ns) {
            ctx->total_throttle_ns += (now - ctx->level_enter_ns);
        }
    }

    ctx->initialized = false;
}

u8 thermal_compute_level(double temp_c, u8 current_level) {
    /* De-escalation with hysteresis: check if temperature dropped below
     * the hysteresis band for the current level. Must be checked BEFORE
     * escalation to allow stepping down one level at a time. */
    if (current_level == THERMAL_LEVEL_SHUTDOWN &&
        temp_c < (THERMAL_THRESH_SHUTDOWN - THERMAL_HYSTERESIS)) {
        current_level = THERMAL_LEVEL_HEAVY;
    }
    if (current_level == THERMAL_LEVEL_HEAVY &&
        temp_c < (THERMAL_THRESH_HEAVY - THERMAL_HYSTERESIS)) {
        current_level = THERMAL_LEVEL_MODERATE;
    }
    if (current_level == THERMAL_LEVEL_MODERATE &&
        temp_c < (THERMAL_THRESH_MODERATE - THERMAL_HYSTERESIS)) {
        current_level = THERMAL_LEVEL_LIGHT;
    }
    if (current_level == THERMAL_LEVEL_LIGHT &&
        temp_c < (THERMAL_THRESH_LIGHT - THERMAL_HYSTERESIS)) {
        current_level = THERMAL_LEVEL_NONE;
    }

    /* Escalation: raise level if temperature exceeds a higher threshold */
    if (temp_c >= THERMAL_THRESH_SHUTDOWN) {
        return THERMAL_LEVEL_SHUTDOWN;
    }
    if (temp_c >= THERMAL_THRESH_HEAVY && current_level < THERMAL_LEVEL_HEAVY) {
        return THERMAL_LEVEL_HEAVY;
    }
    if (temp_c >= THERMAL_THRESH_MODERATE && current_level < THERMAL_LEVEL_MODERATE) {
        return THERMAL_LEVEL_MODERATE;
    }
    if (temp_c >= THERMAL_THRESH_LIGHT && current_level < THERMAL_LEVEL_LIGHT) {
        return THERMAL_LEVEL_LIGHT;
    }

    return current_level;
}

double thermal_get_factor(u8 level) {
    if (level > THERMAL_LEVEL_SHUTDOWN) {
        return 0.0;
    }
    return thermal_factors[level];
}

void thermal_update_counters(struct thermal_throttle_ctx *ctx,
                             double temp_c, u64 now_ns) {
    if (!ctx || !ctx->initialized) {
        return;
    }

    /* Compute new level */
    u8 new_level = thermal_compute_level(temp_c, ctx->current_level);

    /* If level changed, accumulate throttle time for the old level */
    if (new_level != ctx->current_level) {
        if (ctx->current_level > THERMAL_LEVEL_NONE &&
            now_ns > ctx->level_enter_ns) {
            ctx->total_throttle_ns += (now_ns - ctx->level_enter_ns);
        }
        ctx->current_level = new_level;
        ctx->throttle_factor = thermal_get_factor(new_level);
        ctx->level_enter_ns = now_ns;
    }

    /* Update warn/crit minute counters when a minute boundary passes */
    if (ctx->last_minute_ns == 0) {
        ctx->last_minute_ns = now_ns;
        return;
    }

    while (now_ns >= ctx->last_minute_ns + NS_PER_MINUTE) {
        ctx->last_minute_ns += NS_PER_MINUTE;
        if (temp_c >= TEMP_WARN_THRESHOLD) {
            ctx->warn_temp_minutes++;
        }
        if (temp_c >= TEMP_CRIT_THRESHOLD) {
            ctx->crit_temp_minutes++;
        }
    }
}

bool thermal_is_shutdown(u8 level) {
    return (level == THERMAL_LEVEL_SHUTDOWN);
}
