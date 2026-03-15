#include "hal/hal_power.h"
#include <string.h>

/* Default NVMe power state descriptors */
static const struct hal_power_state_desc default_power_states[HAL_POWER_STATE_MAX] = {
    [HAL_POWER_PS0] = { .max_power = 10000, .entry_latency_us = 0, .exit_latency_us = 0, .non_operational = false },
    [HAL_POWER_PS1] = { .max_power = 8000,  .entry_latency_us = 100, .exit_latency_us = 100, .non_operational = false },
    [HAL_POWER_PS2] = { .max_power = 5000,  .entry_latency_us = 500, .exit_latency_us = 500, .non_operational = false },
    [HAL_POWER_PS3] = { .max_power = 2000,  .entry_latency_us = 10000, .exit_latency_us = 50000, .non_operational = true },
    [HAL_POWER_PS4] = { .max_power = 100,   .entry_latency_us = 50000, .exit_latency_us = 500000, .non_operational = true },
};

int hal_power_init(struct hal_power_ctx *ctx)
{
    int ret;

    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    /* Initialize default power states */
    memcpy(ctx->states, default_power_states, sizeof(default_power_states));

    /* Initialize to PS0 (full power) */
    ctx->current_state = HAL_POWER_PS0;
    ctx->previous_state = HAL_POWER_PS0;
    ctx->state_entry_time_ns = get_time_ns();

    ret = mutex_init(&ctx->lock);
    if (ret != HFSSS_OK) {
        memset(ctx, 0, sizeof(*ctx));
        return ret;
    }

    ctx->initialized = true;
    return HFSSS_OK;
}

void hal_power_cleanup(struct hal_power_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->lock, 0);
    ctx->initialized = false;
    mutex_unlock(&ctx->lock);
    mutex_cleanup(&ctx->lock);

    memset(ctx, 0, sizeof(*ctx));
}

int hal_power_set_state(struct hal_power_ctx *ctx, enum hal_nvme_power_state state)
{
    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    if (state >= HAL_POWER_STATE_MAX) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    if (state != ctx->current_state) {
        /* Simulate entry latency */
        u64 entry_latency_ns = (u64)ctx->states[state].entry_latency_us * 1000;
        u64 start = get_time_ns();
        while (get_time_ns() - start < entry_latency_ns) {
            /* Busy wait for simulation */
        }

        ctx->previous_state = ctx->current_state;
        ctx->current_state = state;
        ctx->state_entry_time_ns = get_time_ns();
    }

    mutex_unlock(&ctx->lock);
    return HFSSS_OK;
}

enum hal_nvme_power_state hal_power_get_state(struct hal_power_ctx *ctx)
{
    if (!ctx || !ctx->initialized) {
        return HAL_POWER_PS4;
    }

    return ctx->current_state;
}

int hal_power_get_state_desc(struct hal_power_ctx *ctx, enum hal_nvme_power_state state,
                             struct hal_power_state_desc *desc)
{
    if (!ctx || !ctx->initialized || !desc) {
        return HFSSS_ERR_INVAL;
    }

    if (state >= HAL_POWER_STATE_MAX) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);
    *desc = ctx->states[state];
    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

u64 hal_power_get_state_residency_ns(struct hal_power_ctx *ctx, enum hal_nvme_power_state state)
{
    if (!ctx || !ctx->initialized) {
        return 0;
    }

    if (state >= HAL_POWER_STATE_MAX) {
        return 0;
    }

    mutex_lock(&ctx->lock, 0);
    u64 residency = 0;
    if (state == ctx->current_state) {
        residency = get_time_ns() - ctx->state_entry_time_ns;
    }
    mutex_unlock(&ctx->lock);

    return residency;
}

int hal_power_set_performance_mode(struct hal_power_ctx *ctx, bool high_performance)
{
    enum hal_nvme_power_state target_state;

    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    target_state = high_performance ? HAL_POWER_PS0 : HAL_POWER_PS2;
    return hal_power_set_state(ctx, target_state);
}
