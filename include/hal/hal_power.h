#ifndef __HFSSS_HAL_POWER_H
#define __HFSSS_HAL_POWER_H

#include "common/common.h"

/* Power States */
enum hal_power_state {
    HAL_POWER_ACTIVE = 0,
    HAL_POWER_IDLE = 1,
    HAL_POWER_SLEEP = 2,
    HAL_POWER_OFF = 3,
};

/* HAL Power Context (Placeholder for future implementation) */
struct hal_power_ctx {
    enum hal_power_state state;
};

/* Function Prototypes (Placeholders) */
int hal_power_init(struct hal_power_ctx *ctx);
void hal_power_cleanup(struct hal_power_ctx *ctx);
int hal_power_set_state(struct hal_power_ctx *ctx, enum hal_power_state state);
enum hal_power_state hal_power_get_state(struct hal_power_ctx *ctx);

#endif /* __HFSSS_HAL_POWER_H */
