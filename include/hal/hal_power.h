#ifndef __HFSSS_HAL_POWER_H
#define __HFSSS_HAL_POWER_H

#include "common/common.h"
#include "common/mutex.h"

/* NVMe Power States (PS0 - PS4) */
enum hal_nvme_power_state {
    HAL_POWER_PS0 = 0,  /* Full power, maximum performance */
    HAL_POWER_PS1 = 1,  /* Reduced power, high performance */
    HAL_POWER_PS2 = 2,  /* Medium power, medium performance */
    HAL_POWER_PS3 = 3,  /* Low power, low performance */
    HAL_POWER_PS4 = 4,  /* Minimum power, standby */
    HAL_POWER_STATE_MAX = 5
};

/* Power State Descriptor (per NVMe spec) */
struct hal_power_state_desc {
    u32 max_power;        /* Maximum power in milliwatts */
    u32 entry_latency_us; /* Entry latency in microseconds */
    u32 exit_latency_us;  /* Exit latency in microseconds */
    bool non_operational; /* True if state is non-operational */
};

/* HAL Power Context */
struct hal_power_ctx {
    enum hal_nvme_power_state current_state;
    enum hal_nvme_power_state previous_state;
    struct hal_power_state_desc states[HAL_POWER_STATE_MAX];
    u64 state_entry_time_ns;
    struct mutex lock;
    bool initialized;
};

/* Function Prototypes */
int hal_power_init(struct hal_power_ctx *ctx);
void hal_power_cleanup(struct hal_power_ctx *ctx);
int hal_power_set_state(struct hal_power_ctx *ctx, enum hal_nvme_power_state state);
enum hal_nvme_power_state hal_power_get_state(struct hal_power_ctx *ctx);
int hal_power_get_state_desc(struct hal_power_ctx *ctx, enum hal_nvme_power_state state,
                             struct hal_power_state_desc *desc);
u64 hal_power_get_state_residency_ns(struct hal_power_ctx *ctx, enum hal_nvme_power_state state);
int hal_power_set_performance_mode(struct hal_power_ctx *ctx, bool high_performance);

#endif /* __HFSSS_HAL_POWER_H */
