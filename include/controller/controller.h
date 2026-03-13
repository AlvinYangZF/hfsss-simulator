#ifndef __HFSSS_CONTROLLER_H
#define __HFSSS_CONTROLLER_H

#include "common/common.h"
#include "controller/shmem_if.h"
#include "controller/arbiter.h"
#include "controller/scheduler.h"
#include "controller/write_buffer.h"
#include "controller/read_cache.h"
#include "controller/channel.h"
#include "controller/resource.h"
#include "controller/flow_control.h"

/* Controller Configuration */
struct controller_config {
    u64 sched_period_ns;
    u32 max_concurrent_cmds;
    enum sched_policy sched_policy;
    u32 wb_max_entries;
    u32 rc_max_entries;
    u32 channel_count;
    bool flow_ctrl_enabled;
    u64 read_rate_limit;
    u64 write_rate_limit;
    const char *shmem_path;
};

/* Controller Context */
struct controller_ctx {
    struct controller_config config;
    struct shmem_layout *shmem;
    int shmem_fd;

    struct arbiter_ctx arbiter;
    struct scheduler_ctx scheduler;
    struct write_buffer_ctx wb;
    struct read_cache_ctx rc;
    struct channel_mgr channel_mgr;
    struct resource_mgr resource_mgr;
    struct flow_ctrl_ctx flow_ctrl;

    void *thread;  /* Placeholder for thread handle */
    bool running;
    u64 loop_count;
    u64 last_loop_ts;

    void *ftl_ctx;
    void *hal_ctx;
    struct mutex lock;
    bool initialized;
};

/* Function Prototypes */
int controller_init(struct controller_ctx *ctx, struct controller_config *config);
void controller_cleanup(struct controller_ctx *ctx);
int controller_start(struct controller_ctx *ctx);
void controller_stop(struct controller_ctx *ctx);

/* Default configuration helper */
void controller_config_default(struct controller_config *config);

#endif /* __HFSSS_CONTROLLER_H */
