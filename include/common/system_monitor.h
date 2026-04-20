#ifndef __HFSSS_SYSTEM_MONITOR_H
#define __HFSSS_SYSTEM_MONITOR_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include "common.h"
#include "mutex.h"

/*
 * REQ-087: Periodic system resource monitor.
 *
 * A lightweight background sampler that reads CPU / memory / thread
 * metrics from caller-supplied callbacks at a configurable interval
 * and exposes the latest snapshot via lock-guarded accessors. Tests
 * drive it synchronously via system_monitor_poll_once; production
 * uses system_monitor_start to spin a daemon thread.
 *
 * CPU percentage is computed from the delta of cumulative CPU time
 * (nanoseconds) divided by wall-clock delta between consecutive
 * polls. The first poll seeds the baseline and reports 0 so the
 * first reading isn't a nonsense spike.
 *
 * Default callbacks reading getrusage(RUSAGE_SELF) are available
 * for real production use (see system_monitor_default_*). Tests
 * inject mock callbacks for deterministic behavior.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef u64 (*sm_cpu_time_ns_fn) (void *ctx);
typedef u64 (*sm_mem_bytes_fn)   (void *ctx);
typedef u32 (*sm_thread_count_fn)(void *ctx);

struct system_monitor_config {
    u32 poll_interval_ms;         /* 0 disables the auto-sleep in start() */
    sm_cpu_time_ns_fn   get_cpu_time_ns;
    sm_mem_bytes_fn     get_mem_bytes;
    sm_thread_count_fn  get_thread_count;
    void               *cb_ctx;
};

struct system_monitor {
    struct system_monitor_config cfg;

    pthread_t    thread;
    atomic_bool  stop;
    bool         running;
    bool         initialized;

    struct mutex lock;            /* guards the latest sample + deltas */

    /* Latest sample snapshot. */
    double       cpu_pct;
    u64          mem_bytes;
    u32          thread_count;
    u64          sample_count;

    /* Previous sample for delta computation. */
    u64          prev_cpu_ns;
    u64          prev_wall_ns;
    bool         have_baseline;
};

int  system_monitor_init     (struct system_monitor *m,
                              const struct system_monitor_config *cfg);
void system_monitor_poll_once(struct system_monitor *m);
int  system_monitor_start    (struct system_monitor *m);
void system_monitor_stop     (struct system_monitor *m);
void system_monitor_cleanup  (struct system_monitor *m);

/* Accessors — take the mutex briefly. */
double system_monitor_cpu_pct     (struct system_monitor *m);
u64    system_monitor_mem_bytes   (struct system_monitor *m);
u32    system_monitor_thread_count(struct system_monitor *m);
u64    system_monitor_sample_count(struct system_monitor *m);

/* POSIX defaults backed by getrusage(RUSAGE_SELF). `ctx` is ignored. */
u64 system_monitor_default_cpu_time_ns(void *ctx);
u64 system_monitor_default_mem_bytes   (void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* __HFSSS_SYSTEM_MONITOR_H */
