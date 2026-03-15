#ifndef HFSSS_COMMON_RT_SERVICES_H
#define HFSSS_COMMON_RT_SERVICES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include "common/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * RT Services: CPU affinity, IPC channels, and lock-free trace ring
 * (REQ-074, REQ-075, REQ-085, REQ-087, REQ-088)
 * ------------------------------------------------------------------ */

/* ---- CPU affinity (REQ-074, REQ-075) ---- */

/* Maximum CPUs supported */
#define RT_MAX_CPUS  64

/* Thread role — maps to CPU pinning policy */
enum rt_thread_role {
    RT_ROLE_IO_FAST    = 0,  /* NVMe I/O completion path, pinned to core 0 */
    RT_ROLE_GC         = 1,  /* GC worker, pinned to core 1 */
    RT_ROLE_MONITOR    = 2,  /* health monitor, pinned to core 2 */
    RT_ROLE_OOB        = 3,  /* out-of-band listener, pinned to core 3 */
    RT_ROLE_GENERIC    = 4,  /* no hard pinning */
    RT_ROLE_COUNT      = 5,
};

int  rt_pin_thread(pthread_t thread, enum rt_thread_role role);
int  rt_pin_self(enum rt_thread_role role);
int  rt_get_affinity_mask(enum rt_thread_role role, uint64_t *mask_out);

/* ---- IPC message channel (REQ-085) ---- */

#define RT_IPC_MAX_MSG_SIZE   256u
#define RT_IPC_CHANNEL_DEPTH  64u

struct rt_ipc_msg {
    uint32_t type;
    uint32_t len;
    uint8_t  data[RT_IPC_MAX_MSG_SIZE];
};

struct rt_ipc_channel {
    struct rt_ipc_msg  ring[RT_IPC_CHANNEL_DEPTH];
    volatile uint32_t  head;      /* producer writes here */
    volatile uint32_t  tail;      /* consumer reads here */
    pthread_mutex_t    lock;
    pthread_cond_t     not_empty;
    bool               initialized;
};

int  rt_ipc_init(struct rt_ipc_channel *ch);
void rt_ipc_cleanup(struct rt_ipc_channel *ch);
int  rt_ipc_send(struct rt_ipc_channel *ch, const struct rt_ipc_msg *msg);
int  rt_ipc_recv(struct rt_ipc_channel *ch, struct rt_ipc_msg *msg);
int  rt_ipc_recv_timeout(struct rt_ipc_channel *ch, struct rt_ipc_msg *msg,
                          uint32_t timeout_ms);
bool rt_ipc_is_empty(const struct rt_ipc_channel *ch);
uint32_t rt_ipc_pending(const struct rt_ipc_channel *ch);

/* ---- Lock-free trace ring (REQ-087, REQ-088) ---- */

/* Use a small ring in test mode to avoid 8 MB allocation per ring */
#ifdef HFSSS_TRACE_TEST_MODE
#define TRACE_RING_CAPACITY  256u
#else
#define TRACE_RING_CAPACITY  131072u  /* 128K entries */
#endif

#define TRACE_MSG_LEN        96u

enum trace_level {
    TRACE_LEVEL_DEBUG = 0,
    TRACE_LEVEL_INFO  = 1,
    TRACE_LEVEL_WARN  = 2,
    TRACE_LEVEL_ERROR = 3,
};

struct trace_entry {
    uint64_t    timestamp_ns;
    uint32_t    seq;
    uint8_t     level;
    uint8_t     subsystem;   /* opaque subsystem tag */
    uint8_t     _pad[2];
    char        msg[TRACE_MSG_LEN];
};

struct trace_ring {
    struct trace_entry  entries[TRACE_RING_CAPACITY];
    volatile uint32_t   write_idx;   /* monotonically increasing */
    uint32_t            read_idx;    /* consumer read cursor */
    uint32_t            dropped;     /* entries dropped when full */
    bool                initialized;
};

int      trace_ring_init(struct trace_ring *ring);
void     trace_ring_cleanup(struct trace_ring *ring);
int      trace_ring_write(struct trace_ring *ring, enum trace_level level,
                           uint8_t subsys, const char *msg);
int      trace_ring_read(struct trace_ring *ring, struct trace_entry *out);
uint32_t trace_ring_pending(const struct trace_ring *ring);
uint32_t trace_ring_dropped(const struct trace_ring *ring);

/* ---- Resource monitor (REQ-087, REQ-088) ---- */

#define RT_MON_HISTORY_LEN  16u

struct rt_resource_sample {
    uint64_t timestamp_ns;
    uint32_t cpu_util_pct;    /* 0–100 */
    uint32_t mem_used_kb;
    uint32_t iops;
    uint32_t latency_us_p99;
};

struct rt_resource_monitor {
    struct rt_resource_sample history[RT_MON_HISTORY_LEN];
    uint32_t  history_idx;
    uint32_t  sample_count;

    /* Anomaly thresholds */
    uint32_t  cpu_warn_pct;
    uint32_t  latency_warn_us;

    /* Counters */
    uint32_t  cpu_anomaly_count;
    uint32_t  latency_anomaly_count;

    pthread_mutex_t lock;
    bool initialized;
};

int  rt_mon_init(struct rt_resource_monitor *mon,
                 uint32_t cpu_warn_pct, uint32_t latency_warn_us);
void rt_mon_cleanup(struct rt_resource_monitor *mon);
int  rt_mon_record(struct rt_resource_monitor *mon,
                   const struct rt_resource_sample *s);
int  rt_mon_get_latest(const struct rt_resource_monitor *mon,
                        struct rt_resource_sample *out);
uint32_t rt_mon_cpu_anomalies(const struct rt_resource_monitor *mon);
uint32_t rt_mon_latency_anomalies(const struct rt_resource_monitor *mon);

#ifdef __cplusplus
}
#endif

#endif /* HFSSS_COMMON_RT_SERVICES_H */
