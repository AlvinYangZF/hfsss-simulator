#include "controller/qos.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Map latency in nanoseconds to histogram bucket index.
 * Bucket[i] covers [1us * 2^i, 1us * 2^(i+1)).
 * base_us = 1, so bucket 0 = [1us, 2us), bucket 1 = [2us, 4us), etc.
 * Latencies below 1us go to bucket 0. */
static u32 latency_to_bucket(u64 latency_ns)
{
    u64 latency_us = latency_ns / 1000;

    if (latency_us == 0) {
        return 0;
    }

    /* Find highest set bit position (floor of log2) */
    u32 bucket = 0;
    u64 val = latency_us;
    while (val > 1 && bucket < QOS_HIST_BUCKETS - 1) {
        val >>= 1;
        bucket++;
    }

    return bucket;
}

/* Convert bucket index back to the lower bound in nanoseconds */
static u64 bucket_to_latency_ns(u32 bucket)
{
    if (bucket == 0) {
        return 0;
    }
    return (1ULL << bucket) * 1000ULL;  /* 2^bucket microseconds in ns */
}

int lat_monitor_init(struct ns_latency_monitor *mon, u32 nsid, u32 target_us)
{
    if (!mon) {
        return HFSSS_ERR_INVAL;
    }

    memset(mon, 0, sizeof(*mon));
    mon->nsid = nsid;
    mon->target_us = target_us;
    mon->initialized = true;

    return HFSSS_OK;
}

void lat_monitor_cleanup(struct ns_latency_monitor *mon)
{
    if (!mon) {
        return;
    }
    memset(mon, 0, sizeof(*mon));
}

void lat_monitor_record(struct ns_latency_monitor *mon, u64 latency_ns)
{
    if (!mon || !mon->initialized) {
        return;
    }

    u32 bucket = latency_to_bucket(latency_ns);
    mon->buckets[bucket]++;
    mon->total_samples++;
    mon->total_latency_ns += latency_ns;
}

u64 lat_monitor_percentile(const struct ns_latency_monitor *mon,
                           u32 percentile_x10)
{
    if (!mon || !mon->initialized || mon->total_samples == 0) {
        return 0;
    }

    /* Target count: how many samples must be at or below this percentile.
     * percentile_x10: 990 = P99, 999 = P99.9, 500 = P50 */
    u64 target = (mon->total_samples * percentile_x10 + 999) / 1000;
    if (target > mon->total_samples) {
        target = mon->total_samples;
    }

    u64 cumulative = 0;
    for (u32 i = 0; i < QOS_HIST_BUCKETS; i++) {
        cumulative += mon->buckets[i];
        if (cumulative >= target) {
            /* Return upper bound of this bucket in microseconds.
             * Clamp the shift: at i == QOS_HIST_BUCKETS - 1 (last bucket)
             * a shift by QOS_HIST_BUCKETS would be UB on a 64-bit int. */
            if (i + 1 >= QOS_HIST_BUCKETS) {
                return UINT64_MAX;
            }
            return (1ULL << (i + 1));
        }
    }

    /* Exhausted all buckets without reaching target: return saturated max */
    return UINT64_MAX;
}

bool lat_monitor_check_sla(struct ns_latency_monitor *mon)
{
    if (!mon || !mon->initialized || mon->target_us == 0) {
        return false;
    }

    u64 p99_us = lat_monitor_percentile(mon, 990);

    if (p99_us > mon->target_us) {
        mon->sla_violations++;
        mon->consecutive_violations++;
        return true;
    }

    mon->consecutive_violations = 0;
    return false;
}

void lat_monitor_reset(struct ns_latency_monitor *mon)
{
    if (!mon || !mon->initialized) {
        return;
    }

    u32 nsid = mon->nsid;
    u32 target_us = mon->target_us;
    bool initialized = mon->initialized;

    memset(mon->buckets, 0, sizeof(mon->buckets));
    mon->total_samples = 0;
    mon->total_latency_ns = 0;
    mon->sla_violations = 0;
    mon->consecutive_violations = 0;

    mon->nsid = nsid;
    mon->target_us = target_us;
    mon->initialized = initialized;
}
