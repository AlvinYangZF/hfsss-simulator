#ifndef __HFSSS_SMART_MONITOR_H
#define __HFSSS_SMART_MONITOR_H

#include <pthread.h>
#include <stdatomic.h>
#include "common/common.h"
#include "pcie/nvme_uspace.h"

/*
 * REQ-178: SMART monitor runtime producer.
 *
 * The notifier bridges (nvme_uspace_aer_notify_{thermal,wear,spare})
 * only fire an AER when something external calls them. Without a
 * producer the ⚠️ status stuck: live state could never drift without
 * the host poking it.
 *
 * This monitor closes that loop. It polls caller-supplied callbacks
 * on a configurable interval and calls the matching notifier bridge
 * whenever a threshold crossing is observed:
 *
 *   - Thermal: any change in level (THERMAL_LEVEL_* 0..4).
 *   - Wear   : percent-used moves into a higher 10%-bucket.
 *   - Spare  : avail-spare moves into a lower 10%-bucket.
 *
 * A background thread drives the poll loop by default. Tests may
 * skip the thread and call smart_monitor_poll_once() directly for
 * deterministic single-cycle behavior.
 */

struct nvme_uspace_dev;

typedef u8 (*smart_thermal_level_fn)(void *ctx);
typedef u8 (*smart_remaining_life_fn)(void *ctx);
typedef u8 (*smart_spare_fn)(void *ctx);

struct smart_monitor_config {
    struct nvme_uspace_dev *dev;

    /* Milliseconds between background polls. 0 disables the loop
     * body when smart_monitor_start is invoked; tests then drive
     * behavior manually via smart_monitor_poll_once. */
    u32 poll_interval_ms;

    smart_thermal_level_fn  get_thermal;
    smart_remaining_life_fn get_remaining_life;
    smart_spare_fn          get_spare;
    void                   *cb_ctx;
};

struct smart_monitor {
    struct smart_monitor_config cfg;

    pthread_t       thread;
    atomic_bool     stop;
    bool            running;
    bool            initialized;

    /* Last values we emitted AERs for. Buckets are integer-divided
     * by 10 so we only fire on 10%-granularity watermark crossings. */
    u8   last_thermal;
    u8   last_wear_bucket;  /* (100 - remaining_life) / 10 */
    u8   last_spare_bucket; /* avail_spare / 10 */
    bool have_baseline;     /* first poll seeds last_* without emit */

    u64 notify_count_thermal;
    u64 notify_count_wear;
    u64 notify_count_spare;
};

int  smart_monitor_init     (struct smart_monitor *mon,
                             const struct smart_monitor_config *cfg);
void smart_monitor_poll_once(struct smart_monitor *mon);
int  smart_monitor_start    (struct smart_monitor *mon);
void smart_monitor_stop     (struct smart_monitor *mon);
void smart_monitor_cleanup  (struct smart_monitor *mon);

#endif /* __HFSSS_SMART_MONITOR_H */
