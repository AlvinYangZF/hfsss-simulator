#define _GNU_SOURCE
#include "common/rt_services.h"
#include "common/log.h"
#include "common/common.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

#ifdef __APPLE__
#  include <sys/types.h>
#  include <sys/sysctl.h>
#  include <mach/thread_policy.h>
#  include <mach/thread_act.h>
#else
#  include <sched.h>
#  include <unistd.h>
#endif

/* ------------------------------------------------------------------
 * CPU affinity
 * ------------------------------------------------------------------ */

/* Logical CPU assignment per role */
static const int role_cpu_map[RT_ROLE_COUNT] = {
    [RT_ROLE_IO_FAST]  = 0,
    [RT_ROLE_GC]       = 1,
    [RT_ROLE_MONITOR]  = 2,
    [RT_ROLE_OOB]      = 3,
    [RT_ROLE_GENERIC]  = -1,  /* no pinning */
};

int rt_pin_thread(pthread_t thread, enum rt_thread_role role) {
    if (role >= RT_ROLE_COUNT) return HFSSS_ERR_INVAL;

    int cpu = role_cpu_map[role];
    if (cpu < 0) return HFSSS_OK;  /* GENERIC: no-op */

#ifdef __APPLE__
    /* macOS does not expose CPU pinning via POSIX; use thread affinity tags
     * as best-effort grouping hint.  This avoids a hard compile error while
     * still exercising the code path in tests.                              */
    thread_affinity_policy_data_t pol = { .affinity_tag = (integer_t)(cpu + 1) };
    mach_port_t mach_thread = pthread_mach_thread_np(thread);
    kern_return_t kr = thread_policy_set(mach_thread,
                                          THREAD_AFFINITY_POLICY,
                                          (thread_policy_t)&pol,
                                          THREAD_AFFINITY_POLICY_COUNT);
    if (kr != KERN_SUCCESS) {
        HFSSS_LOG_WARN("RT", "thread_policy_set failed for role %d (non-fatal on macOS)", role);
    }
    (void)thread;
    return HFSSS_OK;
#else
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    int ret = pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset);
    if (ret != 0) {
        HFSSS_LOG_WARN("RT", "pthread_setaffinity_np failed for role %d: %d", role, ret);
        return HFSSS_ERR_IO;
    }
    return HFSSS_OK;
#endif
}

int rt_pin_self(enum rt_thread_role role) {
    return rt_pin_thread(pthread_self(), role);
}

int rt_get_affinity_mask(enum rt_thread_role role, uint64_t *mask_out) {
    if (role >= RT_ROLE_COUNT || !mask_out) return HFSSS_ERR_INVAL;
    int cpu = role_cpu_map[role];
    if (cpu < 0) {
        *mask_out = 0;  /* GENERIC: any CPU */
        return HFSSS_OK;
    }
    *mask_out = (uint64_t)1 << cpu;
    return HFSSS_OK;
}

/* ------------------------------------------------------------------
 * IPC channel
 * ------------------------------------------------------------------ */
int rt_ipc_init(struct rt_ipc_channel *ch) {
    if (!ch) return HFSSS_ERR_INVAL;
    memset(ch, 0, sizeof(*ch));
    pthread_mutex_init(&ch->lock, NULL);
    pthread_cond_init(&ch->not_empty, NULL);
    ch->initialized = true;
    return HFSSS_OK;
}

void rt_ipc_cleanup(struct rt_ipc_channel *ch) {
    if (!ch || !ch->initialized) return;
    pthread_cond_destroy(&ch->not_empty);
    pthread_mutex_destroy(&ch->lock);
    ch->initialized = false;
}

int rt_ipc_send(struct rt_ipc_channel *ch, const struct rt_ipc_msg *msg) {
    if (!ch || !ch->initialized || !msg) return HFSSS_ERR_INVAL;

    pthread_mutex_lock(&ch->lock);
    uint32_t next_head = (ch->head + 1) % RT_IPC_CHANNEL_DEPTH;
    if (next_head == ch->tail) {
        /* Ring full */
        pthread_mutex_unlock(&ch->lock);
        return HFSSS_ERR_NOSPC;
    }
    ch->ring[ch->head] = *msg;
    ch->head = next_head;
    pthread_cond_signal(&ch->not_empty);
    pthread_mutex_unlock(&ch->lock);
    return HFSSS_OK;
}

int rt_ipc_recv(struct rt_ipc_channel *ch, struct rt_ipc_msg *msg) {
    if (!ch || !ch->initialized || !msg) return HFSSS_ERR_INVAL;

    pthread_mutex_lock(&ch->lock);
    while (ch->head == ch->tail) {
        pthread_cond_wait(&ch->not_empty, &ch->lock);
    }
    *msg = ch->ring[ch->tail];
    ch->tail = (ch->tail + 1) % RT_IPC_CHANNEL_DEPTH;
    pthread_mutex_unlock(&ch->lock);
    return HFSSS_OK;
}

int rt_ipc_recv_timeout(struct rt_ipc_channel *ch, struct rt_ipc_msg *msg,
                         uint32_t timeout_ms) {
    if (!ch || !ch->initialized || !msg) return HFSSS_ERR_INVAL;

    struct timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_sec  += timeout_ms / 1000;
    abs.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (abs.tv_nsec >= 1000000000L) {
        abs.tv_sec++;
        abs.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&ch->lock);
    int ret = 0;
    while (ch->head == ch->tail && ret == 0) {
        ret = pthread_cond_timedwait(&ch->not_empty, &ch->lock, &abs);
    }
    if (ch->head == ch->tail) {
        pthread_mutex_unlock(&ch->lock);
        return HFSSS_ERR_TIMEOUT;
    }
    *msg = ch->ring[ch->tail];
    ch->tail = (ch->tail + 1) % RT_IPC_CHANNEL_DEPTH;
    pthread_mutex_unlock(&ch->lock);
    return HFSSS_OK;
}

bool rt_ipc_is_empty(const struct rt_ipc_channel *ch) {
    if (!ch || !ch->initialized) return true;
    return ch->head == ch->tail;
}

uint32_t rt_ipc_pending(const struct rt_ipc_channel *ch) {
    if (!ch || !ch->initialized) return 0;
    uint32_t h = ch->head;
    uint32_t t = ch->tail;
    if (h >= t) return h - t;
    return RT_IPC_CHANNEL_DEPTH - t + h;
}

/* ------------------------------------------------------------------
 * Trace ring
 * ------------------------------------------------------------------ */
int trace_ring_init(struct trace_ring *ring) {
    if (!ring) return HFSSS_ERR_INVAL;
    memset(ring, 0, sizeof(*ring));
    ring->initialized = true;
    return HFSSS_OK;
}

void trace_ring_cleanup(struct trace_ring *ring) {
    if (!ring) return;
    ring->initialized = false;
}

int trace_ring_write(struct trace_ring *ring, enum trace_level level,
                      uint8_t subsys, const char *msg) {
    if (!ring || !ring->initialized || !msg) return HFSSS_ERR_INVAL;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    /* Claim a slot using a CAS loop. The previous implementation did
     * an unconditional fetch_and_add on write_idx and only then checked
     * fullness, which left write_idx advanced even when the write was
     * rejected as NOSPC -- making trace_ring_pending() report more
     * entries than were actually committed. */
    uint32_t seq;
    for (;;) {
        uint32_t w = __sync_fetch_and_add(&ring->write_idx, 0);
        uint32_t r = __sync_fetch_and_add(&ring->read_idx, 0);
        if ((w - r) >= TRACE_RING_CAPACITY) {
            __sync_fetch_and_add(&ring->dropped, 1);
            return HFSSS_ERR_NOSPC;
        }
        if (__sync_bool_compare_and_swap(&ring->write_idx, w, w + 1)) {
            seq = w;
            break;
        }
        /* Raced with another writer; retry */
    }
    uint32_t slot = seq % TRACE_RING_CAPACITY;

    struct trace_entry *e = &ring->entries[slot];
    e->timestamp_ns = now;
    e->seq          = seq;
    e->level        = (uint8_t)level;
    e->subsystem    = subsys;
    strncpy(e->msg, msg, TRACE_MSG_LEN - 1);
    e->msg[TRACE_MSG_LEN - 1] = '\0';

    return HFSSS_OK;
}

int trace_ring_read(struct trace_ring *ring, struct trace_entry *out) {
    if (!ring || !ring->initialized || !out) return HFSSS_ERR_INVAL;
    if (ring->read_idx >= ring->write_idx) return HFSSS_ERR_NOENT;

    uint32_t slot = ring->read_idx % TRACE_RING_CAPACITY;
    *out = ring->entries[slot];
    ring->read_idx++;
    return HFSSS_OK;
}

uint32_t trace_ring_pending(const struct trace_ring *ring) {
    if (!ring || !ring->initialized) return 0;
    uint32_t w = ring->write_idx;
    uint32_t r = ring->read_idx;
    return (w > r) ? (w - r) : 0;
}

uint32_t trace_ring_dropped(const struct trace_ring *ring) {
    if (!ring) return 0;
    return ring->dropped;
}

/* ------------------------------------------------------------------
 * Resource monitor
 * ------------------------------------------------------------------ */
int rt_mon_init(struct rt_resource_monitor *mon,
                uint32_t cpu_warn_pct, uint32_t latency_warn_us) {
    if (!mon) return HFSSS_ERR_INVAL;
    memset(mon, 0, sizeof(*mon));
    mon->cpu_warn_pct      = cpu_warn_pct;
    mon->latency_warn_us   = latency_warn_us;
    pthread_mutex_init(&mon->lock, NULL);
    mon->initialized = true;
    return HFSSS_OK;
}

void rt_mon_cleanup(struct rt_resource_monitor *mon) {
    if (!mon || !mon->initialized) return;
    pthread_mutex_destroy(&mon->lock);
    mon->initialized = false;
}

int rt_mon_record(struct rt_resource_monitor *mon,
                  const struct rt_resource_sample *s) {
    if (!mon || !mon->initialized || !s) return HFSSS_ERR_INVAL;

    pthread_mutex_lock(&mon->lock);

    uint32_t idx = mon->history_idx % RT_MON_HISTORY_LEN;
    mon->history[idx] = *s;
    mon->history_idx++;
    if (mon->sample_count < RT_MON_HISTORY_LEN)
        mon->sample_count++;

    /* Anomaly detection */
    if (mon->cpu_warn_pct > 0 && s->cpu_util_pct >= mon->cpu_warn_pct) {
        mon->cpu_anomaly_count++;
        HFSSS_LOG_WARN("RT-MON", "CPU anomaly: %u%%", s->cpu_util_pct);
    }
    if (mon->latency_warn_us > 0 && s->latency_us_p99 >= mon->latency_warn_us) {
        mon->latency_anomaly_count++;
        HFSSS_LOG_WARN("RT-MON", "latency anomaly: %u us P99", s->latency_us_p99);
    }

    pthread_mutex_unlock(&mon->lock);
    return HFSSS_OK;
}

int rt_mon_get_latest(const struct rt_resource_monitor *mon,
                       struct rt_resource_sample *out) {
    if (!mon || !mon->initialized || !out) return HFSSS_ERR_INVAL;
    if (mon->sample_count == 0) return HFSSS_ERR_NOENT;

    pthread_mutex_lock((pthread_mutex_t *)&mon->lock);
    uint32_t last_idx = (mon->history_idx - 1) % RT_MON_HISTORY_LEN;
    *out = mon->history[last_idx];
    pthread_mutex_unlock((pthread_mutex_t *)&mon->lock);
    return HFSSS_OK;
}

uint32_t rt_mon_cpu_anomalies(const struct rt_resource_monitor *mon) {
    if (!mon || !mon->initialized) return 0;
    return mon->cpu_anomaly_count;
}

uint32_t rt_mon_latency_anomalies(const struct rt_resource_monitor *mon) {
    if (!mon || !mon->initialized) return 0;
    return mon->latency_anomaly_count;
}
