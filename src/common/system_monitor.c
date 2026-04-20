/*
 * REQ-087: periodic system resource monitor. See header for the
 * sampling semantics and the callback contract.
 */

#include "common/system_monitor.h"
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/time.h>

/* Returns monotonic wall time in nanoseconds, or 0 if the OS clock
 * call fails. Callers treat 0 as "no usable sample" so a transient
 * clock failure never poisons cpu_pct with a bogus delta. */
static u64 now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

int system_monitor_init(struct system_monitor *m,
                        const struct system_monitor_config *cfg)
{
    if (!m || !cfg ||
        !cfg->get_cpu_time_ns || !cfg->get_mem_bytes ||
        !cfg->get_thread_count) {
        return HFSSS_ERR_INVAL;
    }
    memset(m, 0, sizeof(*m));
    m->cfg = *cfg;
    atomic_store(&m->stop, false);
    int rc = mutex_init(&m->lock);
    if (rc != HFSSS_OK) {
        return rc;
    }
    m->initialized = true;
    return HFSSS_OK;
}

void system_monitor_poll_once(struct system_monitor *m)
{
    if (!m || !m->initialized) return;

    u64 cpu_ns  = m->cfg.get_cpu_time_ns (m->cfg.cb_ctx);
    u64 mem     = m->cfg.get_mem_bytes   (m->cfg.cb_ctx);
    u32 threads = m->cfg.get_thread_count(m->cfg.cb_ctx);
    u64 wall_ns = now_ns();

    mutex_lock(&m->lock, 0);
    if (!m->have_baseline) {
        /* First sample: seed prev_* and report nominal 0% CPU. The
         * next poll computes a real delta against this baseline. */
        m->prev_cpu_ns  = cpu_ns;
        m->prev_wall_ns = wall_ns;
        m->cpu_pct      = 0.0;
        m->have_baseline = true;
    } else {
        /* u64 subtraction underflows silently if wall_ns <= prev —
         * that would feed a huge bogus delta into the division and
         * hide a real clock regression behind a tiny cpu_pct.
         * Signed compare first; `0.0` here means "no usable wall
         * delta this poll" and keeps cpu_pct stable. */
        if (wall_ns > m->prev_wall_ns && cpu_ns >= m->prev_cpu_ns) {
            u64 cpu_delta  = cpu_ns  - m->prev_cpu_ns;
            u64 wall_delta = wall_ns - m->prev_wall_ns;
            m->cpu_pct = 100.0 * (double)cpu_delta / (double)wall_delta;
        } else {
            m->cpu_pct = 0.0;
        }
        m->prev_cpu_ns  = cpu_ns;
        m->prev_wall_ns = wall_ns;
    }
    m->mem_bytes     = mem;
    m->thread_count  = threads;
    m->sample_count += 1;
    mutex_unlock(&m->lock);
}

static void monitor_sleep_ms(u32 ms)
{
    if (ms == 0) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        return;
    }
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000 * 1000;
    nanosleep(&ts, NULL);
}

static void *monitor_thread_main(void *arg)
{
    struct system_monitor *m = (struct system_monitor *)arg;
    while (!atomic_load(&m->stop)) {
        system_monitor_poll_once(m);
        monitor_sleep_ms(m->cfg.poll_interval_ms);
    }
    return NULL;
}

int system_monitor_start(struct system_monitor *m)
{
    if (!m || !m->initialized) return HFSSS_ERR_INVAL;
    if (m->running) return HFSSS_OK;
    atomic_store(&m->stop, false);
    int rc = pthread_create(&m->thread, NULL, monitor_thread_main, m);
    if (rc != 0) return HFSSS_ERR_IO;
    m->running = true;
    return HFSSS_OK;
}

void system_monitor_stop(struct system_monitor *m)
{
    if (!m || !m->running) return;
    atomic_store(&m->stop, true);
    pthread_join(m->thread, NULL);
    m->running = false;
}

void system_monitor_cleanup(struct system_monitor *m)
{
    if (!m) return;
    if (m->running) system_monitor_stop(m);
    if (m->initialized) mutex_cleanup(&m->lock);
    memset(m, 0, sizeof(*m));
}

double system_monitor_cpu_pct(struct system_monitor *m)
{
    if (!m || !m->initialized) return 0.0;
    mutex_lock(&m->lock, 0);
    double v = m->cpu_pct;
    mutex_unlock(&m->lock);
    return v;
}

u64 system_monitor_mem_bytes(struct system_monitor *m)
{
    if (!m || !m->initialized) return 0;
    mutex_lock(&m->lock, 0);
    u64 v = m->mem_bytes;
    mutex_unlock(&m->lock);
    return v;
}

u32 system_monitor_thread_count(struct system_monitor *m)
{
    if (!m || !m->initialized) return 0;
    mutex_lock(&m->lock, 0);
    u32 v = m->thread_count;
    mutex_unlock(&m->lock);
    return v;
}

u64 system_monitor_sample_count(struct system_monitor *m)
{
    if (!m || !m->initialized) return 0;
    mutex_lock(&m->lock, 0);
    u64 v = m->sample_count;
    mutex_unlock(&m->lock);
    return v;
}

u64 system_monitor_default_cpu_time_ns(void *ctx)
{
    (void)ctx;
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    /* User + system CPU time, converted to nanoseconds. */
    u64 user_ns = (u64)ru.ru_utime.tv_sec * 1000000000ULL +
                  (u64)ru.ru_utime.tv_usec * 1000ULL;
    u64 sys_ns  = (u64)ru.ru_stime.tv_sec * 1000000000ULL +
                  (u64)ru.ru_stime.tv_usec * 1000ULL;
    return user_ns + sys_ns;
}

u64 system_monitor_default_mem_bytes(void *ctx)
{
    (void)ctx;
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    /* ru_maxrss is bytes on macOS, kilobytes on Linux. Apply the
     * kB -> bytes conversion everywhere except macOS. */
#if defined(__APPLE__)
    return (u64)ru.ru_maxrss;
#else
    return (u64)ru.ru_maxrss * 1024ULL;
#endif
}
