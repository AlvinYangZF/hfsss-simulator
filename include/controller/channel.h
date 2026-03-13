#ifndef __HFSSS_CHANNEL_H
#define __HFSSS_CHANNEL_H

#include "common/common.h"
#include "common/mutex.h"

#define MAX_CHANNELS 32
#define MAX_CHIPS_PER_CHANNEL 8
#define MAX_DIES_PER_CHIP 4

/* Channel State */
enum channel_state {
    CHANNEL_IDLE = 0,
    CHANNEL_BUSY = 1,
    CHANNEL_ERROR = 2,
};

/* Channel Statistics */
struct channel_stats {
    u64 cmd_count;
    u64 read_count;
    u64 write_count;
    u64 busy_time_ns;
    u64 idle_time_ns;
};

/* Channel Context */
struct channel_ctx {
    u32 channel_id;
    enum channel_state state;
    u32 chip_count;
    u32 die_count;
    u64 next_available_ts;
    struct channel_stats stats;
    void *private_data;
};

/* Channel Manager */
struct channel_mgr {
    struct channel_ctx channels[MAX_CHANNELS];
    u32 channel_count;
    u64 total_busy_time;
    u64 last_balance_ts;
    u64 balance_interval_ns;
    struct mutex lock;
};

/* Function Prototypes */
int channel_mgr_init(struct channel_mgr *mgr, u32 channel_count);
void channel_mgr_cleanup(struct channel_mgr *mgr);
int channel_mgr_select(struct channel_mgr *mgr, u64 lba);
int channel_mgr_balance(struct channel_mgr *mgr);

#endif /* __HFSSS_CHANNEL_H */
