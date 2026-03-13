#include "controller/channel.h"
#include <stdlib.h>
#include <string.h>

int channel_mgr_init(struct channel_mgr *mgr, u32 channel_count)
{
    u32 i;
    int ret;

    if (!mgr) {
        return HFSSS_ERR_INVAL;
    }

    if (channel_count > MAX_CHANNELS) {
        channel_count = MAX_CHANNELS;
    }

    memset(mgr, 0, sizeof(*mgr));

    ret = mutex_init(&mgr->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    mgr->channel_count = channel_count;
    mgr->balance_interval_ns = 10000000;  /* 10ms */

    for (i = 0; i < channel_count; i++) {
        mgr->channels[i].channel_id = i;
        mgr->channels[i].state = CHANNEL_IDLE;
        mgr->channels[i].chip_count = 4;
        mgr->channels[i].die_count = 2;
        mgr->channels[i].next_available_ts = 0;
    }

    return HFSSS_OK;
}

void channel_mgr_cleanup(struct channel_mgr *mgr)
{
    if (!mgr) {
        return;
    }

    mutex_lock(&mgr->lock, 0);
    mutex_unlock(&mgr->lock);

    mutex_cleanup(&mgr->lock);
    memset(mgr, 0, sizeof(*mgr));
}

int channel_mgr_select(struct channel_mgr *mgr, u64 lba)
{
    u32 i;
    u64 min_busy = U64_MAX;
    int selected_channel = 0;

    if (!mgr) {
        return 0;
    }

    mutex_lock(&mgr->lock, 0);

    /* Find channel with lowest usage */
    for (i = 0; i < mgr->channel_count; i++) {
        if (mgr->channels[i].stats.busy_time_ns < min_busy) {
            min_busy = mgr->channels[i].stats.busy_time_ns;
            selected_channel = i;
        }
    }

    /* Round-robin fallback */
    if (min_busy == 0) {
        selected_channel = lba % mgr->channel_count;
    }

    mutex_unlock(&mgr->lock);

    return selected_channel;
}

int channel_mgr_balance(struct channel_mgr *mgr)
{
    u32 i;
    u64 total_busy = 0;
    u64 avg_busy;

    if (!mgr) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&mgr->lock, 0);

    /* Calculate average busy time */
    for (i = 0; i < mgr->channel_count; i++) {
        total_busy += mgr->channels[i].stats.busy_time_ns;
    }
    mgr->total_busy_time = total_busy;

    if (mgr->channel_count > 0) {
        avg_busy = total_busy / mgr->channel_count;
        (void)avg_busy;
    }

    mgr->last_balance_ts = get_time_ns();

    mutex_unlock(&mgr->lock);

    return HFSSS_OK;
}
