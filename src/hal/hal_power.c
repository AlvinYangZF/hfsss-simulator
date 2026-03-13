#include "hal/hal_power.h"
#include <string.h>

int hal_power_init(struct hal_power_ctx *ctx)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->state = HAL_POWER_ACTIVE;

    return HFSSS_OK;
}

void hal_power_cleanup(struct hal_power_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
}

int hal_power_set_state(struct hal_power_ctx *ctx, enum hal_power_state state)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    ctx->state = state;
    return HFSSS_OK;
}

enum hal_power_state hal_power_get_state(struct hal_power_ctx *ctx)
{
    if (!ctx) {
        return HAL_POWER_OFF;
    }

    return ctx->state;
}
