# Real-Time Services Layer Low-Level Design

## Revision History

| Version | Date       | Author | Description                          |
|---------|------------|--------|--------------------------------------|
| V1.0    | 2026-03-15 | HFSSS  | Initial release                      |
| V1.1    | 2026-03-23 | HFSSS  | Added Thermal Throttle Enforcement   |

## Table of Contents

1. [Module Overview](#1-module-overview)
2. [Requirements Traceability](#2-requirements-traceability)
3. [Data Structure Design](#3-data-structure-design)
4. [Header File Design](#4-header-file-design)
5. [Function Interface Design](#5-function-interface-design)
6. [Flow Diagrams](#6-flow-diagrams)
7. [Thermal Throttle Enforcement](#7-thermal-throttle-enforcement)
8. [Platform Compatibility](#8-platform-compatibility)
9. [Test Plan](#9-test-plan)

---

## 1. Module Overview

The Real-Time Services Layer provides deterministic scheduling, inter-core communication, and system resource observability capabilities for firmware threads. This layer is independent of the host I/O data path but provides underlying support for all hot-path threads.

The layer covers five capabilities:

1. **CPU Core Affinity Binding (REQ-074)**: Static binding of each firmware thread to specific CPU cores via `pthread_setaffinity_np`, eliminating scheduling jitter;
2. **Real-Time Scheduling Policy (REQ-075)**: Setting `SCHED_FIFO` priority 50-90 for critical threads (NVMe dispatch, FTL, media channel);
3. **Inter-Core Communication (REQ-085)**: SPSC (Single-Producer/Single-Consumer) lock-free ring queue + `eventfd` notification for low-latency cross-core messaging;
4. **System Resource Monitoring (REQ-087)**: 1Hz sampling of per-thread CPU utilization, memory partition usage, and NAND channel queue depth trends;
5. **Performance Anomaly Detection and Temperature Simulation (REQ-088)**: P99.9 latency threshold alerts; linear temperature model `T = T_ambient + IOPS * c_i + BW * c_b`; automatic throttling on temperature exceedance.

**Requirements Coverage**: REQ-074, REQ-075, REQ-085, REQ-087, REQ-088.

---

## 2. Requirements Traceability

| Req ID  | Description | Priority | Target |
|---------|-------------|----------|--------|
| REQ-074 | CPU core affinity binding via pthread_setaffinity_np | P0 | V2.0 |
| REQ-075 | Real-time scheduling: SCHED_FIFO priority 50-90 for critical threads | P0 | V2.0 |
| REQ-085 | IPC: SPSC lock-free ring + eventfd notification | P0 | V2.0 |
| REQ-087 | Resource monitoring: per-thread CPU, memory partitions, channel queue depth (1Hz) | P0 | V2.0 |
| REQ-088 | Performance anomaly detection + temperature simulation + auto-throttle | P0 | V2.0 |

---

## 3. Data Structure Design

### 3.1 CPU Affinity and Real-Time Scheduling (REQ-074, REQ-075)

```c
enum thread_role {
    THREAD_ROLE_NVME_DISPATCHER,  /* cores 48-49, SCHED_FIFO prio 90 */
    THREAD_ROLE_NVME_WORKER,      /* cores 50-53, SCHED_FIFO prio 85 */
    THREAD_ROLE_CONTROLLER,       /* cores 40-47, SCHED_FIFO prio 80 */
    THREAD_ROLE_FTL,              /* cores 32-39, SCHED_FIFO prio 75 */
    THREAD_ROLE_NAND_CHANNEL,     /* cores  0-31, SCHED_FIFO prio 70 */
    THREAD_ROLE_GC,               /* cores  4-7,  SCHED_FIFO prio 60 */
    THREAD_ROLE_MONITORING,       /* cores 62-63, SCHED_FIFO prio 50 */
    THREAD_ROLE_OOB,              /* any core,    SCHED_OTHER prio  0 */
    THREAD_ROLE_COUNT
};

struct thread_affinity_cfg {
    enum thread_role role;
    char name[32];
    int  cpu_ids[8];
    int  cpu_count;
    int  sched_policy;
    int  sched_priority;
};

struct rt_thread_ctx {
    pthread_t tid;
    struct thread_affinity_cfg cfg;
    bool pinned;
    bool rt_active;
};
```

### 3.2 IPC SPSC Ring Buffer (REQ-085)

```c
#define IPC_RING_CAPACITY   4096
#define IPC_MSG_PAYLOAD_MAX 256

struct ipc_msg {
    uint32_t type;
    uint32_t len;
    uint8_t  payload[IPC_MSG_PAYLOAD_MAX];
} __attribute__((aligned(64)));

struct ipc_ring {
    _Atomic uint64_t head __attribute__((aligned(64)));
    _Atomic uint64_t tail __attribute__((aligned(64)));
    struct ipc_msg   msgs[IPC_RING_CAPACITY];
    int              eventfd;
    uint32_t         src_core_id;
    uint32_t         dst_core_id;
};
```

### 3.3 System Resource Monitoring (REQ-087)

```c
#define RESOURCE_HISTORY_DEPTH 60

struct thread_stats {
    pid_t    tid;
    char     name[32];
    double   cpu_pct_history[RESOURCE_HISTORY_DEPTH];
    uint32_t write_idx;
};

struct mem_partition_stats {
    const char    *name;
    size_t         total_bytes;
    _Atomic size_t used_bytes;
};

struct channel_queue_stats {
    uint8_t  channel_id;
    uint32_t depth_max;
    uint32_t depth_history[RESOURCE_HISTORY_DEPTH];
    uint32_t write_idx;
};

struct resource_monitor {
    struct thread_stats        *threads;
    uint32_t                    thread_count;
    struct mem_partition_stats *partitions;
    uint32_t                    partition_count;
    struct channel_queue_stats  channels[MAX_CHANNELS];
    pthread_t                   monitor_tid;
    bool                        running;
    pthread_mutex_t             lock;
};
```

### 3.4 Performance Anomaly Detection and Temperature Simulation (REQ-088)

```c
#define ANOMALY_HIST_BUCKETS 64

struct anomaly_detector {
    uint64_t lat_hist_read[ANOMALY_HIST_BUCKETS];
    uint64_t lat_hist_write[ANOMALY_HIST_BUCKETS];
    uint64_t p999_alert_threshold_us;
    uint64_t alert_count;
    uint64_t last_alert_ns;
    double   temp_ambient_celsius;
    double   coeff_iops;
    double   coeff_bw;
    double   current_temp_celsius;
    bool     throttle_active;
    pthread_rwlock_t lock;
};
```

---

## 4. Header File Design

```c
/* include/common/rt_services.h */
#ifndef HFSSS_RT_SERVICES_H
#define HFSSS_RT_SERVICES_H

/* RT thread lifecycle */
int rt_thread_create(struct rt_thread_ctx *ctx, const struct thread_affinity_cfg *cfg,
                     void *(*entry)(void *), void *arg);
int rt_thread_pin_cpu(struct rt_thread_ctx *ctx);
int rt_set_realtime_priority(struct rt_thread_ctx *ctx);
int rt_thread_join(struct rt_thread_ctx *ctx);
const struct thread_affinity_cfg *rt_affinity_table_get(enum thread_role role);

/* IPC SPSC ring buffer */
int ipc_ring_init(struct ipc_ring *ring, uint32_t src_core_id, uint32_t dst_core_id);
void ipc_ring_destroy(struct ipc_ring *ring);
int ipc_send(struct ipc_ring *ring, uint32_t type, const void *payload, uint32_t len);
int ipc_recv(struct ipc_ring *ring, struct ipc_msg *msg_buf, uint64_t timeout_us);
uint32_t ipc_ring_depth(const struct ipc_ring *ring);

/* Resource monitoring */
int resource_monitor_init(struct resource_monitor *mon, uint32_t thread_count, uint32_t partition_count);
int resource_monitor_start(struct resource_monitor *mon, struct sssim_ctx *sssim);
void resource_monitor_stop(struct resource_monitor *mon);
void resource_monitor_sample(struct resource_monitor *mon);
char *resource_monitor_get_snapshot(struct resource_monitor *mon);
void resource_monitor_destroy(struct resource_monitor *mon);

/* Anomaly detection */
int anomaly_detector_init(struct anomaly_detector *det, double temp_ambient, double coeff_iops, double coeff_bw);
void anomaly_detector_destroy(struct anomaly_detector *det);
void anomaly_record_latency(struct anomaly_detector *det, bool is_read, uint64_t lat_ns);
void anomaly_check_p999(struct anomaly_detector *det);
bool anomaly_update_temperature(struct anomaly_detector *det, double iops, double bw_mbps,
                                struct nvme_smart_log *smart_log);
char *anomaly_get_stats_json(const struct anomaly_detector *det);

#endif
```

---

## 5. Function Interface Design

### 5.1 rt_thread_create

Creates a firmware thread with affinity and real-time scheduling applied in a wrapper function before the entry function executes. Affinity and priority failures are non-fatal.

### 5.2 ipc_send / ipc_recv

Producer writes to `msgs[head & (CAPACITY-1)]`, increments head with release semantics, writes to eventfd to wake consumer. Consumer polls eventfd, reads from `msgs[tail & (CAPACITY-1)]`, increments tail with release semantics.

### 5.3 resource_monitor_sample

Reads `/proc/self/task/[tid]/stat` for CPU ticks (Linux) or `proc_pidinfo` (macOS), queries mempool used bytes, reads NAND channel queue depths. Updates circular history buffers under lock.

### 5.4 anomaly_record_latency

Hot-path safe: computes bucket index as `63 - __builtin_clzll(lat_us)` and atomically increments the corresponding histogram counter. No lock required.

### 5.5 anomaly_update_temperature

```
T = temp_ambient + iops * coeff_iops + bw_mbps * coeff_bw
if T >= TEMP_CRIT: throttle_active = true
else: throttle_active = false
Update smart_log->temperature and critical_warning bits
Return true if throttle state changed
```

---

## 6. Flow Diagrams

### 6.1 RT Thread Creation Flow

```
rt_thread_create() -> pthread_create(rt_thread_wrapper)
    -> rt_thread_pin_cpu() [non-fatal]
    -> rt_set_realtime_priority() [non-fatal]
    -> entry(arg)
```

### 6.2 IPC Send/Receive Flow

```
Producer: check capacity -> fill slot -> atomic_store(head+1) -> write(eventfd)
Consumer: epoll_wait(eventfd) -> read(eventfd) -> copy msg -> atomic_store(tail+1)
```

### 6.3 Resource Monitor 1-Second Sampling Loop

```
while (running):
  resource_monitor_sample(mon)  // CPU, memory, channel depth
  anomaly_check_p999(det)       // P99.9 latency alert check
  clock_nanosleep(1s)
```

---

## 7. Thermal Throttle Enforcement

### 7.1 Overview

Thermal throttle enforcement implements progressive performance reduction as temperature rises, protecting the simulated SSD from thermal damage while maintaining data integrity.

### 7.2 Throttle Action Implementation

When thermal throttle is activated, the simulator:

1. **Reduces max outstanding NAND commands**: Limits the number of concurrent commands issued to the NAND media layer by scaling `channel_flow_ctrl.queue_depth_max`
2. **Delays command submission**: Introduces artificial delay before dispatching commands to the NAND scheduler

```c
struct thermal_throttle_ctx {
    uint8_t  current_level;        /* 0=none, 1-4=throttle levels */
    double   throttle_factor;      /* 1.0=full speed, 0.2=20% speed */
    uint64_t level_enter_ns;       /* timestamp when current level was entered */
    uint64_t total_throttle_ns;    /* cumulative time spent throttling */
};
```

### 7.3 Integration with Controller Scheduler

The `throttle_factor` is applied to the DWRR (Deficit Weighted Round Robin) scheduler quantum:

```
effective_quantum = base_quantum * throttle_factor
```

When `throttle_factor < 1.0`, each scheduling round dispatches fewer commands, proportionally reducing throughput while maintaining fair scheduling across namespaces.

### 7.4 Progressive Throttle Levels

| Level | Temperature Threshold | Performance Factor | Action |
|-------|----------------------|-------------------|--------|
| 0 (None) | < 75 deg C | 1.00 (100%) | Normal operation |
| 1 (Light) | >= 75 deg C | 0.80 (80%) | Reduce NAND queue depth to 80%; log INFO |
| 2 (Moderate) | >= 80 deg C | 0.50 (50%) | Reduce NAND queue depth to 50%; log WARN |
| 3 (Heavy) | >= 85 deg C | 0.20 (20%) | Reduce NAND queue depth to 20%; log ERROR |
| 4 (Critical) | >= 90 deg C | 0.00 (0%) | Initiate thermal shutdown; log CRITICAL |

### 7.5 Hysteresis

To prevent rapid oscillation between throttle levels, the system uses a 3 deg C hysteresis band:

- **Entering a level**: Temperature must reach or exceed the level's threshold
- **Exiting a level**: Temperature must drop 3 deg C below the threshold before resuming the previous level

Example: Level 2 activates at 80 deg C, but only deactivates when temperature drops to 77 deg C.

```c
static uint8_t thermal_compute_level(double temp_c, uint8_t current_level) {
    /* Ascending thresholds */
    const double thresholds[] = {75.0, 80.0, 85.0, 90.0};
    const double hysteresis = 3.0;

    /* Check for level increase */
    for (int l = 3; l >= 0; l--) {
        if (temp_c >= thresholds[l] && current_level < l + 1)
            return l + 1;
    }
    /* Check for level decrease (with hysteresis) */
    if (current_level > 0 && temp_c < thresholds[current_level - 1] - hysteresis)
        return current_level - 1;

    return current_level;
}
```

### 7.6 SMART Log Updates

When thermal throttle is active, the following SMART log fields are updated:

- **`warn_temp_time`**: Incremented every minute while temperature >= 70 deg C (warning threshold). Represents cumulative minutes above warning temperature.
- **`crit_comp_temp_time`**: Incremented every minute while temperature >= 85 deg C (critical composite temperature). Represents cumulative minutes above critical temperature.

Both counters persist across power cycles via NOR Flash SysInfo partition.

```c
void thermal_update_smart_counters(struct nvme_smart_log *log,
                                    struct thermal_throttle_ctx *ctx,
                                    double current_temp_c) {
    if (current_temp_c >= 70.0)
        log->warn_temp_time++;      /* minutes above warning */
    if (current_temp_c >= 85.0)
        log->crit_comp_temp_time++; /* minutes above critical */
}
```

### 7.7 Thermal Shutdown (Level 4)

At Level 4 (>= 90 deg C), the simulator initiates a controlled thermal shutdown:

1. Stop accepting new host I/O commands
2. Complete in-flight NAND operations (with 5-second timeout)
3. Flush Write Buffer to NAND (best-effort)
4. Write thermal shutdown marker to SysInfo
5. Set CSTS.SHST = 0x02 (shutdown complete)
6. Halt simulator with thermal shutdown reason

---

## 8. Platform Compatibility

### 8.1 CPU Core Affinity

| Platform | Implementation | Degraded Behavior |
|----------|---------------|-------------------|
| Linux | `pthread_setaffinity_np` | Failure logged as WARN, non-fatal |
| macOS | `thread_policy_set(THREAD_AFFINITY_POLICY)` | Hint only, not guaranteed; WARN on failure |

### 8.2 SCHED_FIFO Real-Time Scheduling

| Condition | Behavior |
|-----------|----------|
| Linux + CAP_SYS_NICE | SCHED_FIFO succeeds; rt_active=true |
| Linux, no CAP_SYS_NICE | EPERM; degrade to SCHED_OTHER; WARN |
| macOS | SCHED_FIFO not supported; degrade to SCHED_OTHER |

### 8.3 eventfd Notification

| Platform | Implementation |
|----------|---------------|
| Linux | `eventfd(0, EFD_NONBLOCK\|EFD_CLOEXEC)` + `epoll_wait` |
| macOS | `pipe(2)` fallback; functionally equivalent, slightly higher latency |

---

## 9. Test Plan

| Test ID | Description | Verification Point |
|---------|-------------|-------------------|
| RT-001 | Thread affinity binding | sched_getaffinity confirms correct cores |
| RT-002 | SCHED_FIFO priority | sched_getscheduler returns SCHED_FIFO |
| RT-003 | No CAP_SYS_NICE degradation | rt_active==false, thread runs normally |
| RT-004 | NVME_DISPATCHER role verification | Runs on cores 48-49 |
| RT-005 | IPC single send/receive | Type, len, payload integrity verified |
| RT-006 | IPC 1M message throughput | Zero loss; avg RTT < 500ns |
| RT-007 | IPC queue full -EAGAIN | Returns -EAGAIN; ring state intact |
| RT-008 | eventfd consumer wakeup | Consumer woken within 1ms |
| RT-009 | Resource monitor CPU sampling | Busy-loop thread shows > 90% CPU |
| RT-010 | Resource monitor memory partition | used_pct matches allocated amount |
| RT-011 | P99.9 alert trigger | alert_count incremented; WARN logged |
| RT-012 | P99.9 normal no alert | alert_count remains 0 |
| RT-013 | Temperature model formula | T=76.0 for IOPS=1M, BW=3000, ambient=30 |
| RT-014 | Temperature triggers throttle | throttle_active==true; SMART critical_warning set |
| RT-015 | Temperature recovery clears throttle | throttle_active==false; critical_warning cleared |
| RT-016 | SMART temperature field update | smart_log->temperature == (uint16_t)(T+273) |
| RT-017 | Thermal throttle Level 1 (75 deg C) | throttle_factor == 0.80; performance reduced to 80% |
| RT-018 | Thermal throttle Level 2 (80 deg C) | throttle_factor == 0.50; queue depth halved |
| RT-019 | Thermal throttle Level 3 (85 deg C) | throttle_factor == 0.20; crit_comp_temp_time incrementing |
| RT-020 | Thermal hysteresis | Level 2 at 80 deg C; does not drop to Level 1 until 77 deg C |
| RT-021 | Thermal shutdown (Level 4, 90 deg C) | Controlled shutdown; thermal marker written |
| RT-022 | warn_temp_time SMART counter | Increments each minute while T >= 70 deg C |
| RT-023 | crit_comp_temp_time SMART counter | Increments each minute while T >= 85 deg C |
| RT-024 | macOS pipe fallback for eventfd | IPC works correctly on macOS |
| RT-025 | Monitor get_snapshot JSON format | threads, partitions, channels arrays present |

---

**Document Statistics**:
- Requirements covered: 5 (REQ-074, REQ-075, REQ-085, REQ-087, REQ-088) + Thermal Throttle Enforcement
- New header files: `include/common/rt_services.h`
- New source files: `src/common/rt_thread.c`, `src/common/ipc_ring.c`, `src/common/resource_monitor.c`, `src/common/anomaly_detector.c`
- Function interfaces: 30+
- Test cases: 25

## Appendix: Cross-References

| Reference | Document |
|-----------|----------|
| OOB temperature alerts | LLD_07_OOB_MANAGEMENT |
| QoS scheduler integration | LLD_18_QOS_DETERMINISM |
| Power loss on thermal shutdown | LLD_17_POWER_LOSS_PROTECTION |
