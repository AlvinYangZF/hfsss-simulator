# 高保真全栈SSD模拟器（HFSSS）详细设计文档

**文档名称**：实时服务层详细设计
**文档版本**：V1.0
**编制日期**：2026-03-15
**设计阶段**：V2.0 (Beta)
**密级**：内部资料

---

## 目录

1. [模块概述](#1-模块概述)
2. [功能需求详细分解](#2-功能需求详细分解)
3. [数据结构详细设计](#3-数据结构详细设计)
4. [头文件设计](#4-头文件设计)
5. [函数接口详细设计](#5-函数接口详细设计)
6. [流程图](#6-流程图)
7. [平台兼容性说明](#7-平台兼容性说明)
8. [测试要点](#8-测试要点)

---

## 1. 模块概述

实时服务层（Real-Time Services Layer）为仿真器固件线程提供确定性调度、核间通信和系统资源可观测性能力。该层独立于主机I/O数据通路之外，但为所有热路径线程提供底层支撑。

实时服务层覆盖以下五项能力：

1. **CPU核心亲和性绑定（REQ-074）**：通过 `pthread_setaffinity_np` 将每个固件线程静态绑定到特定CPU核心，消除调度抖动对延迟的影响；
2. **实时调度策略（REQ-075）**：对NVMe派发、FTL、媒体通道等关键线程设置 `SCHED_FIFO` 优先级50–90，确保低延迟响应；
3. **核间通信（REQ-085）**：基于SPSC（单生产者/单消费者）无锁环形队列 + `eventfd` 通知机制，实现跨核消息传递；
4. **系统资源监控（REQ-087）**：以1Hz采样频率追踪每个线程CPU占用率、内存分区使用量和NAND通道队列深度历史趋势；
5. **性能异常检测与温度仿真（REQ-088）**：P99.9延迟超阈值时触发告警；温度仿真模型为 `T = T_ambient + IOPS × c_i + BW × c_b`，温度超限时自动激活限速。

实时服务层以独立的监控线程运行，所有热路径操作均为无锁或低竞争设计，对主I/O路径的影响控制在纳秒量级。

**覆盖需求**：REQ-074、REQ-075、REQ-085、REQ-087、REQ-088。

---

## 2. 功能需求详细分解

| 需求ID | 需求描述 | 优先级 | 目标版本 |
|--------|----------|--------|----------|
| REQ-074 | CPU核心亲和性绑定 — 每个固件线程通过 `pthread_setaffinity_np` 静态绑定到指定核心 | P0 | V2.0 |
| REQ-075 | 实时调度策略 — NVMe派发、FTL、媒体通道线程设置 `SCHED_FIFO`，优先级范围50–90 | P0 | V2.0 |
| REQ-085 | 核间通信（IPC）— SPSC无锁环形队列 + `eventfd` 通知，支持跨CPU核心低延迟消息传递 | P0 | V2.0 |
| REQ-087 | 系统资源监控 — 每线程CPU占用率、内存分区利用率、NAND通道队列深度趋势（1Hz采样） | P0 | V2.0 |
| REQ-088 | 性能异常检测 + 温度仿真 — P99.9延迟超阈值告警；温度线性模型；超限自动限速 | P0 | V2.0 |

---

## 3. 数据结构详细设计

### 3.1 CPU亲和性与实时调度配置（REQ-074, REQ-075）

```c
#define HFSSS_THREAD_NAME_MAX 32

/*
 * thread_role – logical role of each firmware thread.
 * Each role maps to a dedicated CPU core range and SCHED_FIFO priority.
 * THREAD_ROLE_OOB runs at SCHED_OTHER (non-RT) to avoid starving monitors.
 */
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

/*
 * thread_affinity_cfg – static binding and scheduling parameters for one thread.
 * Populated from the global affinity table at startup; never modified at runtime.
 */
struct thread_affinity_cfg {
    enum thread_role role;
    char             name[HFSSS_THREAD_NAME_MAX]; /* human-readable label */
    int              cpu_ids[8];    /* list of eligible CPU IDs for this role */
    int              cpu_count;     /* number of valid entries in cpu_ids */
    int              sched_policy;  /* SCHED_FIFO or SCHED_OTHER */
    int              sched_priority; /* 0 when sched_policy == SCHED_OTHER */
};

/*
 * rt_thread_ctx – runtime state of one real-time firmware thread.
 * Tracks whether affinity and priority were successfully applied.
 */
struct rt_thread_ctx {
    pthread_t              tid;
    struct thread_affinity_cfg cfg;
    bool                   pinned;    /* true after pthread_setaffinity_np succeeds */
    bool                   rt_active; /* true after pthread_setschedparam succeeds */
};
```

### 3.2 IPC SPSC环形队列（REQ-085）

```c
#define IPC_RING_CAPACITY   4096         /* must be a power of 2 */
#define IPC_MSG_PAYLOAD_MAX 256

/*
 * ipc_msg – fixed-size message envelope for inter-core communication.
 * Aligned to 64 bytes (one cache line) to prevent false sharing between
 * adjacent slots in the ring buffer.
 */
struct ipc_msg {
    uint32_t type;                        /* application-defined message type */
    uint32_t len;                         /* valid bytes in payload */
    uint8_t  payload[IPC_MSG_PAYLOAD_MAX]; /* message body */
} __attribute__((aligned(64)));

/*
 * ipc_ring – single-producer / single-consumer lock-free ring buffer.
 *
 * head and tail are placed on separate cache lines to eliminate false sharing
 * between the producer (which writes head) and the consumer (which writes tail).
 * eventfd is used to block the consumer when the ring is empty.
 */
struct ipc_ring {
    /* Producer-owned field — cache line 0 */
    _Atomic uint64_t  head __attribute__((aligned(64)));
    /* Consumer-owned field — cache line 1 */
    _Atomic uint64_t  tail __attribute__((aligned(64)));
    /* Shared payload array */
    struct ipc_msg    msgs[IPC_RING_CAPACITY];
    int               eventfd;      /* eventfd descriptor for consumer wakeup */
    uint32_t          src_core_id;  /* logical ID of the producing core */
    uint32_t          dst_core_id;  /* logical ID of the consuming core */
};
```

### 3.3 系统资源监控（REQ-087）

```c
#define RESOURCE_HISTORY_DEPTH 60  /* rolling window of 60 one-second samples */

/*
 * thread_stats – per-thread CPU utilisation history.
 * Written by the monitor thread; read by OOB queries.
 */
struct thread_stats {
    pid_t    tid;
    char     name[HFSSS_THREAD_NAME_MAX];
    double   cpu_pct_history[RESOURCE_HISTORY_DEPTH]; /* circular buffer */
    uint32_t write_idx;  /* next slot to write; wraps at RESOURCE_HISTORY_DEPTH */
};

/*
 * mem_partition_stats – utilisation of a named DRAM region.
 * used_bytes is updated atomically by the allocation path.
 */
struct mem_partition_stats {
    const char    *name;         /* e.g. "write_buffer", "read_cache" */
    size_t         total_bytes;
    _Atomic size_t used_bytes;
};

/*
 * channel_queue_stats – queue depth history for one NAND channel.
 * depth_history is a circular buffer updated once per second.
 */
struct channel_queue_stats {
    uint8_t  channel_id;
    uint32_t depth_max;  /* maximum depth observed since startup */
    uint32_t depth_history[RESOURCE_HISTORY_DEPTH];
    uint32_t write_idx;
};

/*
 * resource_monitor – top-level context for the resource monitoring subsystem.
 * The monitor thread samples all sub-structures once per second and updates
 * the circular history buffers.  lock protects snapshot reads by OOB.
 */
struct resource_monitor {
    struct thread_stats        *threads;        /* array, thread_count entries */
    uint32_t                    thread_count;
    struct mem_partition_stats *partitions;     /* array, partition_count entries */
    uint32_t                    partition_count;
    struct channel_queue_stats  channels[MAX_CHANNELS];
    pthread_t                   monitor_tid;
    bool                        running;
    pthread_mutex_t             lock;
};
```

### 3.4 性能异常检测与温度仿真（REQ-088）

```c
#define ANOMALY_HIST_BUCKETS            64
#define ANOMALY_P999_THRESH_DEFAULT_US  2000

/*
 * anomaly_detector – tracks latency histograms and temperature model state.
 *
 * Latency histograms use the same exponential-bucket scheme as perf_counters
 * (bucket k covers [2^(k-1) µs, 2^k µs)).  Bucket index is computed in O(1)
 * using __builtin_clz.
 *
 * Temperature model:
 *   T = temp_ambient_celsius + iops * coeff_iops + bw_mbps * coeff_bw
 *
 * When current_temp_celsius >= TEMP_CRIT_CELSIUS, throttle_active is set and
 * the controller caps IOPS at 50% of the configured maximum.
 *
 * lock is a reader-writer lock: hot-path histogram updates take a read lock
 * (concurrent writers are safe via atomic increments); OOB snapshot queries
 * take a write lock to read a consistent view.
 */
struct anomaly_detector {
    uint64_t  lat_hist_read[ANOMALY_HIST_BUCKETS];   /* read latency histogram */
    uint64_t  lat_hist_write[ANOMALY_HIST_BUCKETS];  /* write latency histogram */
    uint64_t  p999_alert_threshold_us;  /* alert when P99.9 exceeds this value */
    uint64_t  alert_count;              /* cumulative number of P99.9 alerts */
    uint64_t  last_alert_ns;            /* timestamp of the most recent alert */
    /* Temperature model coefficients */
    double    temp_ambient_celsius;     /* baseline ambient temperature */
    double    coeff_iops;               /* degrees Celsius per IOPS */
    double    coeff_bw;                 /* degrees Celsius per MB/s */
    double    current_temp_celsius;     /* last computed temperature */
    bool      throttle_active;          /* true when T >= TEMP_CRIT_CELSIUS */
    pthread_rwlock_t lock;
};
```

---

## 4. 头文件设计

```c
/* include/common/rt_services.h */
#ifndef HFSSS_RT_SERVICES_H
#define HFSSS_RT_SERVICES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

/* Forward declarations */
struct sssim_ctx;
struct nvme_smart_log;
struct resource_monitor;
struct anomaly_detector;
struct rt_thread_ctx;
struct ipc_ring;
struct ipc_msg;

/* ------------------------------------------------------------------ */
/* RT thread lifecycle (REQ-074, REQ-075)                              */
/* ------------------------------------------------------------------ */

/*
 * rt_thread_create – create a firmware thread with affinity and real-time
 * scheduling applied before the entry function begins executing.
 *
 * ctx:   caller-supplied context; populated with tid, cfg, pinned, rt_active.
 * cfg:   affinity and scheduling parameters for this thread.
 * entry: thread entry function.
 * arg:   opaque argument passed to entry.
 *
 * Returns 0 on success, negative errno on pthread failure.
 * Affinity and priority failures are non-fatal; caller should inspect
 * ctx->pinned and ctx->rt_active to determine degraded-mode status.
 */
int rt_thread_create(struct rt_thread_ctx *ctx,
                     const struct thread_affinity_cfg *cfg,
                     void *(*entry)(void *), void *arg);

/*
 * rt_thread_pin_cpu – apply CPU affinity to the calling thread using
 * pthread_setaffinity_np.  Sets ctx->pinned on success.
 *
 * On non-Linux platforms this is a best-effort operation via
 * thread_policy_set; failure is logged as WARN and is non-fatal.
 */
int rt_thread_pin_cpu(struct rt_thread_ctx *ctx);

/*
 * rt_set_realtime_priority – set SCHED_FIFO scheduling policy and priority
 * for the calling thread via pthread_setschedparam.
 * Sets ctx->rt_active on success.
 *
 * If the process lacks CAP_SYS_NICE (EPERM), logs WARN and degrades
 * gracefully to SCHED_OTHER; rt_active remains false.
 */
int rt_set_realtime_priority(struct rt_thread_ctx *ctx);

/*
 * rt_thread_join – join the thread and release rt_thread_ctx resources.
 */
int rt_thread_join(struct rt_thread_ctx *ctx);

/*
 * rt_affinity_table_get – return the read-only global affinity configuration
 * table for a given role.  Returns NULL if role >= THREAD_ROLE_COUNT.
 */
const struct thread_affinity_cfg *rt_affinity_table_get(enum thread_role role);

/* ------------------------------------------------------------------ */
/* IPC SPSC ring buffer (REQ-085)                                      */
/* ------------------------------------------------------------------ */

/*
 * ipc_ring_init – initialise a ring buffer between src_core and dst_core.
 * Allocates the msg array, creates an eventfd descriptor, and zeroes
 * head/tail.  Returns 0 on success, negative errno on failure.
 */
int ipc_ring_init(struct ipc_ring *ring, uint32_t src_core_id,
                  uint32_t dst_core_id);

/*
 * ipc_ring_destroy – close the eventfd and release all resources.
 */
void ipc_ring_destroy(struct ipc_ring *ring);

/*
 * ipc_send – enqueue one message into the ring (producer side).
 * Uses a CAS loop on head to claim a slot; copies payload; writes
 * to eventfd to wake the consumer.
 *
 * Returns 0 on success, -EAGAIN if the ring is full.
 * Must be called from the designated producer thread only.
 */
int ipc_send(struct ipc_ring *ring, uint32_t type,
             const void *payload, uint32_t len);

/*
 * ipc_recv – dequeue one message from the ring (consumer side).
 * Blocks on epoll_wait(eventfd) for up to timeout_us microseconds.
 * Copies the message into msg_buf on success.
 *
 * Returns 0 on success, -ETIMEDOUT if no message arrived within the
 * timeout, negative errno on system error.
 * Must be called from the designated consumer thread only.
 */
int ipc_recv(struct ipc_ring *ring, struct ipc_msg *msg_buf,
             uint64_t timeout_us);

/*
 * ipc_ring_depth – return the current number of unconsumed messages.
 * Approximate (no lock); suitable for monitoring only.
 */
uint32_t ipc_ring_depth(const struct ipc_ring *ring);

/* ------------------------------------------------------------------ */
/* Resource monitoring (REQ-087)                                       */
/* ------------------------------------------------------------------ */

/*
 * resource_monitor_init – initialise the monitor context.
 * Allocates thread_stats and mem_partition_stats arrays.
 * Returns 0 on success, negative errno on allocation failure.
 */
int resource_monitor_init(struct resource_monitor *mon,
                          uint32_t thread_count,
                          uint32_t partition_count);

/*
 * resource_monitor_start – spawn the monitor thread at SCHED_OTHER
 * priority 50 bound to cores 62-63.
 * The monitor thread calls resource_monitor_sample every 1 second.
 */
int resource_monitor_start(struct resource_monitor *mon,
                           struct sssim_ctx *sssim);

/*
 * resource_monitor_stop – signal the monitor thread to exit and join it.
 */
void resource_monitor_stop(struct resource_monitor *mon);

/*
 * resource_monitor_sample – collect one sample of all monitored resources.
 * Reads /proc/self/task/[tid]/stat for CPU ticks, queries mempool used
 * bytes, and reads current NAND channel queue depths.
 * Updates the circular history buffers under mon->lock.
 */
void resource_monitor_sample(struct resource_monitor *mon);

/*
 * resource_monitor_get_snapshot – serialise current resource statistics
 * to a JSON string suitable for the OOB interface.
 * Caller must free() the returned buffer.
 * Returns NULL on allocation failure.
 */
char *resource_monitor_get_snapshot(struct resource_monitor *mon);

/*
 * resource_monitor_destroy – free all memory allocated by
 * resource_monitor_init.
 */
void resource_monitor_destroy(struct resource_monitor *mon);

/* ------------------------------------------------------------------ */
/* Performance anomaly detection & temperature simulation (REQ-088)   */
/* ------------------------------------------------------------------ */

/*
 * anomaly_detector_init – initialise an anomaly_detector with default
 * thresholds and the given ambient temperature and model coefficients.
 */
int anomaly_detector_init(struct anomaly_detector *det,
                          double temp_ambient_celsius,
                          double coeff_iops, double coeff_bw);

/*
 * anomaly_detector_destroy – release lock resources.
 */
void anomaly_detector_destroy(struct anomaly_detector *det);

/*
 * anomaly_record_latency – record one I/O completion in the latency
 * histogram.  Computes bucket index using __builtin_clz(lat_us) and
 * increments the corresponding counter.  Hot-path safe; takes no lock.
 *
 * is_read: true = read histogram, false = write histogram.
 * lat_ns:  end-to-end latency in nanoseconds.
 */
void anomaly_record_latency(struct anomaly_detector *det,
                            bool is_read, uint64_t lat_ns);

/*
 * anomaly_check_p999 – compute P99.9 latency from the histogram using
 * prefix-sum and compare against p999_alert_threshold_us.
 * If exceeded: logs WARN with the observed value and increments alert_count.
 * Called by the monitoring thread once per second.
 */
void anomaly_check_p999(struct anomaly_detector *det);

/*
 * anomaly_update_temperature – compute the current temperature from the
 * linear model, update current_temp_celsius, set or clear throttle_active,
 * and update the SMART log temperature and critical_warning fields.
 *
 * iops:      total IOPS (read + write) in the last sampling interval.
 * bw_mbps:   total bandwidth (read + write) in MB/s.
 * smart_log: pointer to the NVMe SMART log to update in place; may be NULL.
 *
 * Returns true if the throttle state changed (caller should log the event).
 */
bool anomaly_update_temperature(struct anomaly_detector *det,
                                double iops, double bw_mbps,
                                struct nvme_smart_log *smart_log);

/*
 * anomaly_get_stats_json – serialise anomaly detector state to JSON.
 * Caller must free() the returned buffer.
 */
char *anomaly_get_stats_json(const struct anomaly_detector *det);

#endif /* HFSSS_RT_SERVICES_H */
```

---

## 5. 函数接口详细设计

### 5.1 `rt_thread_create` — 创建RT线程并应用亲和性与优先级

```
rt_thread_create(ctx, cfg, entry, arg):
  1. ctx->cfg = *cfg
  2. 创建内部启动参数 wrapper_arg = {ctx, entry, arg}
  3. pthread_create(&ctx->tid, NULL, rt_thread_wrapper, wrapper_arg)
     → 失败：return -errno

rt_thread_wrapper(wrapper_arg):
  4. 提取 ctx, entry, arg
  5. rt_thread_pin_cpu(ctx)        // 非致命
  6. rt_set_realtime_priority(ctx) // 非致命
  7. return entry(arg)
```

### 5.2 `rt_thread_pin_cpu` — CPU核心亲和性绑定

```
rt_thread_pin_cpu(ctx):
  1. 构造 cpu_set_t mask
     for i in 0..ctx->cfg.cpu_count-1:
         CPU_SET(ctx->cfg.cpu_ids[i], &mask)
  2. ret = pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask)
  3. if ret == 0:
         ctx->pinned = true
         log INFO "thread [%s] pinned to cores [%s]"
     else:
         ctx->pinned = false
         log WARN "affinity bind failed for [%s]: %s (non-fatal)"
         // macOS: 尝试 thread_policy_set(THREAD_AFFINITY_POLICY)
  4. return ret  // 调用方忽略此返回值；仅供诊断
```

### 5.3 `rt_set_realtime_priority` — 设置SCHED_FIFO优先级

```
rt_set_realtime_priority(ctx):
  1. if ctx->cfg.sched_policy != SCHED_FIFO:
         ctx->rt_active = false
         return 0  // SCHED_OTHER 线程不需要提权
  2. param.sched_priority = ctx->cfg.sched_priority
  3. ret = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param)
  4. if ret == 0:
         ctx->rt_active = true
         log INFO "thread [%s] set SCHED_FIFO prio %d"
     else if errno == EPERM:
         ctx->rt_active = false
         log WARN "CAP_SYS_NICE required for SCHED_FIFO; degrading to SCHED_OTHER (non-fatal)"
     else:
         log WARN "pthread_setschedparam failed: %s"
  5. return ret
```

### 5.4 `ipc_ring_init` — 初始化SPSC环形队列

```
ipc_ring_init(ring, src_core_id, dst_core_id):
  1. assert IPC_RING_CAPACITY is power-of-2  // 编译期检查
  2. memset(ring->msgs, 0, sizeof(ring->msgs))
  3. atomic_store(&ring->head, 0)
  4. atomic_store(&ring->tail, 0)
  5. ring->src_core_id = src_core_id
  6. ring->dst_core_id = dst_core_id
  7. ring->eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)
     → Linux失败或macOS: 使用 pipe(2) 替代，log WARN
  8. return (ring->eventfd >= 0) ? 0 : -errno
```

### 5.5 `ipc_send` — 生产者发送消息

```
ipc_send(ring, type, payload, len):
  1. assert len <= IPC_MSG_PAYLOAD_MAX
  2. head = atomic_load(&ring->head, memory_order_relaxed)
  3. tail = atomic_load(&ring->tail, memory_order_acquire)
  4. if (head - tail) >= IPC_RING_CAPACITY: return -EAGAIN  // 队列已满
  5. slot = head & (IPC_RING_CAPACITY - 1)     // power-of-2 取模
  6. ring->msgs[slot].type = type
     ring->msgs[slot].len  = len
     memcpy(ring->msgs[slot].payload, payload, len)
  7. atomic_store(&ring->head, head + 1, memory_order_release)
  8. val = 1; write(ring->eventfd, &val, 8)    // 唤醒消费者
  9. return 0
```

### 5.6 `ipc_recv` — 消费者接收消息

```
ipc_recv(ring, msg_buf, timeout_us):
  1. 构造 epoll 实例（或 poll），监听 ring->eventfd 的 EPOLLIN 事件
  2. timeout_ms = timeout_us / 1000
  3. ret = epoll_wait(epfd, &ev, 1, timeout_ms)
     → ret == 0: return -ETIMEDOUT
     → ret < 0:  return -errno
  4. // 消耗 eventfd 计数
     read(ring->eventfd, &val, 8)
  5. tail = atomic_load(&ring->tail, memory_order_relaxed)
  6. head = atomic_load(&ring->head, memory_order_acquire)
  7. if head == tail: return -EAGAIN  // 虚假唤醒（理论上不应发生）
  8. slot = tail & (IPC_RING_CAPACITY - 1)
  9. *msg_buf = ring->msgs[slot]      // 复制消息
 10. atomic_store(&ring->tail, tail + 1, memory_order_release)
 11. return 0
```

### 5.7 `resource_monitor_start` — 启动资源监控线程

```
resource_monitor_start(mon, sssim):
  1. mon->running = true
  2. 构造线程亲和性配置：role = THREAD_ROLE_MONITORING
     cpu_ids = {62, 63}, sched_policy = SCHED_OTHER, sched_priority = 0
     （注：监控线程不使用 SCHED_FIFO，避免饿死其他 SCHED_OTHER 任务）
  3. rt_thread_create(&monitor_rt_ctx, &monitor_cfg,
                      resource_monitor_thread, mon)
  4. mon->monitor_tid = monitor_rt_ctx.tid
  5. return 0 on success
```

### 5.8 `resource_monitor_sample` — 执行一次资源采样

```
resource_monitor_sample(mon):
  1. for each thread in mon->threads[0..thread_count-1]:
       a. 读取 /proc/self/task/[tid]/stat 的 utime + stime（Linux）
          macOS: 调用 proc_pidinfo(PROC_PIDTHREADINFO)
       b. 计算与上次采样的 tick 差值，换算为 cpu_pct
       c. pthread_mutex_lock(&mon->lock)
          thread.cpu_pct_history[thread.write_idx % RESOURCE_HISTORY_DEPTH] = cpu_pct
          thread.write_idx++
          pthread_mutex_unlock(&mon->lock)

  2. for each partition in mon->partitions[0..partition_count-1]:
       a. used = atomic_load(&partition.used_bytes)
       b. （直接可读，无需采样历史，监控快照时取即时值）

  3. for each channel in mon->channels[0..MAX_CHANNELS-1]:
       a. depth = channel_queue_depth(sssim, channel_id)  // from channel ctx
       b. pthread_mutex_lock(&mon->lock)
          channels[i].depth_history[write_idx % RESOURCE_HISTORY_DEPTH] = depth
          if depth > channels[i].depth_max: channels[i].depth_max = depth
          channels[i].write_idx++
          pthread_mutex_unlock(&mon->lock)
```

### 5.9 `resource_monitor_get_snapshot` — 序列化资源快照为JSON

```
resource_monitor_get_snapshot(mon):
  1. pthread_mutex_lock(&mon->lock)
  2. 分配输出缓冲区
  3. 构造 JSON 对象：
     {
       "threads": [ { "name": ..., "cpu_pct_avg_1m": ... }, ... ],
       "partitions": [ { "name": ..., "used_pct": ... }, ... ],
       "channels": [ { "id": ..., "queue_depth": ..., "depth_max": ... }, ... ]
     }
     cpu_pct_avg_1m 取最近 60 个样本的均值
  4. pthread_mutex_unlock(&mon->lock)
  5. return json_string  // 调用方 free()
```

### 5.10 `anomaly_record_latency` — 记录延迟直方图

```
anomaly_record_latency(det, is_read, lat_ns):
  1. lat_us = lat_ns / 1000
  2. if lat_us == 0: bucket = 0
     else: bucket = 63 - __builtin_clzll(lat_us)
     bucket = min(bucket, ANOMALY_HIST_BUCKETS - 1)
  3. if is_read:
         __atomic_fetch_add(&det->lat_hist_read[bucket], 1, __ATOMIC_RELAXED)
     else:
         __atomic_fetch_add(&det->lat_hist_write[bucket], 1, __ATOMIC_RELAXED)
```

### 5.11 `anomaly_check_p999` — 计算P99.9延迟并告警

```
anomaly_check_p999(det):
  1. pthread_rwlock_rdlock(&det->lock)
  2. 遍历 lat_hist_read[0..63]，累加 total_read
     以相同方式计算 lat_hist_write → total_write
  3. 计算 P99.9 目标计数 = total_read * 0.999
     前缀和遍历直方图，找到第一个累计 >= 目标计数的 bucket k
     p999_us = (k == 0) ? 0 : (1ULL << (k - 1))
  4. pthread_rwlock_unlock(&det->lock)
  5. if p999_us > det->p999_alert_threshold_us:
         det->alert_count++
         det->last_alert_ns = clock_gettime(CLOCK_MONOTONIC)
         log WARN "P99.9 read latency anomaly: %llu µs > threshold %llu µs"
```

### 5.12 `anomaly_update_temperature` — 更新温度并决定限速

```
anomaly_update_temperature(det, iops, bw_mbps, smart_log):
  1. T = det->temp_ambient_celsius
         + iops   * det->coeff_iops
         + bw_mbps * det->coeff_bw
  2. pthread_rwlock_wrlock(&det->lock)
  3. prev_throttle = det->throttle_active
     det->current_temp_celsius = T
  4. if T >= TEMP_CRIT_CELSIUS:
         det->throttle_active = true
     else:
         det->throttle_active = false
  5. changed = (det->throttle_active != prev_throttle)
  6. if smart_log != NULL:
         smart_log->temperature = (uint16_t)(T + 273)
         if T >= TEMP_WARN_CELSIUS:
             smart_log->critical_warning |= SMART_CRIT_TEMPERATURE
         else:
             smart_log->critical_warning &= ~SMART_CRIT_TEMPERATURE
  7. pthread_rwlock_unlock(&det->lock)
  8. if changed:
         log WARN/INFO "throttle_active changed to %s at T=%.1f°C"
  9. return changed
```

---

## 6. 流程图

### 6.1 RT线程创建流程

```
rt_thread_create(ctx, cfg, entry, arg)
        │
        ▼
pthread_create(rt_thread_wrapper)
        │
        ▼
rt_thread_wrapper():
        │
        ├──► rt_thread_pin_cpu(ctx)
        │         │
        │         ├─ 成功 ──► ctx->pinned = true
        │         │           log INFO "pinned to cores [...]"
        │         │
        │         └─ 失败 ──► ctx->pinned = false
        │                     log WARN "affinity failed (non-fatal)"
        │
        ├──► rt_set_realtime_priority(ctx)
        │         │
        │         ├─ 成功 ──► ctx->rt_active = true
        │         │           log INFO "SCHED_FIFO prio N"
        │         │
        │         ├─ EPERM ──► ctx->rt_active = false
        │         │            log WARN "CAP_SYS_NICE missing; SCHED_OTHER"
        │         │
        │         └─ 其他错误 ──► log WARN; rt_active = false
        │
        └──► entry(arg)  ← 线程进入实际工作函数
```

### 6.2 IPC发送/接收流程

```
生产者（src_core）                        消费者（dst_core）
─────────────────                        ─────────────────
ipc_send(ring, type, payload, len)       ipc_recv(ring, msg_buf, timeout_us)
   │                                        │
   ▼                                        ▼
读取 head, tail (atomic_load)           epoll_wait(eventfd, timeout_ms)
   │                                        │
   ├─ (head - tail) >= CAPACITY            ├─ 超时 ──► return -ETIMEDOUT
   │   return -EAGAIN (队列满)              │
   │                                        ▼
   ▼                                     read(eventfd, &val, 8)  // 消耗通知
slot = head & (CAPACITY - 1)               │
   │                                        ▼
   ▼                                     读取 tail (atomic_load)
填写 msgs[slot].type/len/payload           │
   │                                        ▼
   ▼                                     slot = tail & (CAPACITY - 1)
atomic_store(head + 1, release)            │
   │                                        ▼
   ▼                                     *msg_buf = msgs[slot]  (复制)
write(eventfd, 1)  // 唤醒消费者           │
                                            ▼
                                        atomic_store(tail + 1, release)
                                            │
                                            ▼
                                        return 0 → 调用方处理消息
```

### 6.3 资源监控1秒采样循环

```
resource_monitor_thread(mon):
    ┌──────────────────────────────────────────────────────────────┐
    │  while (mon->running):                                        │
    │                                                              │
    │  resource_monitor_sample(mon)                                │
    │       │                                                      │
    │       ├─ 读取各线程 /proc/self/task/[tid]/stat               │
    │       │   → 计算 cpu_pct，写入循环历史缓冲区                  │
    │       │                                                      │
    │       ├─ 读取各内存分区 atomic used_bytes                     │
    │       │   → 即时值（无需历史）                                 │
    │       │                                                      │
    │       └─ 读取各 NAND 通道队列深度                             │
    │           → 写入循环历史缓冲区；更新 depth_max                │
    │                                                              │
    │  anomaly_check_p999(det)                                     │
    │       │                                                      │
    │       ├─ P99.9 > 阈值 ──► log WARN; alert_count++            │
    │       └─ 正常      ──► 无操作                                 │
    │                                                              │
    │  clock_nanosleep(1s)                                         │
    └──────────────────────────────────────────────────────────────┘
```

### 6.4 异常检测：延迟记录 → 告警 → 温度计算 → 限速决策

```
[热路径] I/O完成
        │
        ▼
anomaly_record_latency(det, is_read, lat_ns)
  bucket = 63 - __builtin_clzll(lat_us)
  atomic_fetch_add(lat_hist[bucket])
        │
        │ （每秒由监控线程触发）
        ▼
anomaly_check_p999(det)
        │
        ├─ P99.9 > threshold ──► log WARN "P99.9 anomaly: %llu µs"
        │                         alert_count++; last_alert_ns = now
        └─ 正常 ──► 无操作
        │
        ▼
anomaly_update_temperature(det, iops, bw_mbps, smart_log)
        │
        ▼
  T = T_ambient + iops × c_i + bw × c_b
        │
        ├─ T < TEMP_WARN (70°C)  ──► 清除 SMART_CRIT_TEMPERATURE
        │                             throttle_active = false
        │
        ├─ TEMP_WARN ≤ T < TEMP_CRIT ──► 置位 SMART_CRIT_TEMPERATURE
        │   (70–75°C)                     warn_temp_time++
        │                                 log WARN "temperature warning"
        │
        └─ T ≥ TEMP_CRIT (75°C)  ──► 置位 SMART_CRIT_TEMPERATURE
                                      throttle_active = true
                                      log ERROR "temperature critical – IOPS throttled to 50%"
                                      更新 smart_log->temperature
```

---

## 7. 平台兼容性说明

### 7.1 CPU核心亲和性

| 平台 | 实现方式 | 降级行为 |
|------|----------|----------|
| Linux | `pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &mask)` | 失败时 log WARN，`ctx->pinned = false`；线程继续运行 |
| macOS | `thread_policy_set(mach_thread_self(), THREAD_AFFINITY_POLICY, ...)` | macOS内核不保证亲和性约束，仅作性能提示；失败时 log WARN，非致命 |

### 7.2 SCHED_FIFO实时调度

| 条件 | 行为 |
|------|------|
| Linux + `CAP_SYS_NICE` 或 `sudo` | `pthread_setschedparam(SCHED_FIFO, prio)` 成功；`ctx->rt_active = true` |
| Linux，无 `CAP_SYS_NICE` | 返回 `EPERM`；降级到 `SCHED_OTHER`；log WARN；`ctx->rt_active = false`；性能测试结果仅供参考 |
| macOS | macOS不支持 `SCHED_FIFO`；调用 `pthread_setschedparam` 会返回错误；降级到 `SCHED_OTHER`；log WARN |

### 7.3 `/proc/self/task/[tid]/stat` CPU采样

| 平台 | 实现方式 |
|------|----------|
| Linux | 解析 `/proc/self/task/[tid]/stat` 的第14/15字段（`utime`/`stime`），计算两次采样之间的tick差值并换算百分比 |
| macOS | 调用 `proc_pidinfo(pid, PROC_PIDTHREADINFO, tid, &tinfo, sizeof(tinfo))`，读取 `pth_user_time` + `pth_system_time` |

若 `proc_pidinfo` 不可用或权限不足，`cpu_pct_history` 填充 `-1.0` 以标记无效样本；监控快照在序列化时跳过无效值。

### 7.4 `eventfd` 通知机制

| 平台 | 实现方式 |
|------|----------|
| Linux | `eventfd(0, EFD_NONBLOCK \| EFD_CLOEXEC)`；`epoll_wait` 等待消费者唤醒 |
| macOS | `pipe(2)` 替代：向写端写1字节触发通知，`poll` 等待读端可读；功能等价，延迟略高 |

`ipc_ring_init` 在创建时自动检测平台并选择对应实现；上层代码无需感知差异。

---

## 8. 测试要点

| 测试ID | 测试描述 | 验证点 |
|--------|----------|--------|
| RT-001 | 线程创建并绑定亲和性 | 创建后调用 `sched_getaffinity` 验证线程仅在配置的核心上运行 |
| RT-002 | SCHED_FIFO优先级设置 | 调用 `sched_getscheduler` 验证返回 `SCHED_FIFO`；`sched_getparam` 验证优先级数值匹配（需 `CAP_SYS_NICE`） |
| RT-003 | 无 `CAP_SYS_NICE` 降级 | 在无权限环境下创建RT线程；验证 `ctx->rt_active == false`，线程正常启动，log含WARN |
| RT-004 | NVME_DISPATCHER角色核心验证 | 创建 `THREAD_ROLE_NVME_DISPATCHER` 线程；验证其运行在核心48或49 |
| RT-005 | MONITORING角色核心验证 | 创建 `THREAD_ROLE_MONITORING` 线程；验证其运行在核心62或63 |
| RT-006 | IPC单次发送/接收往返 | 生产者发送1条消息；消费者接收并校验type、len、payload字节完整性 |
| RT-007 | IPC 1M消息吞吐 | 收发100万条消息；验证零丢失；测量平均往返延迟 < 500ns |
| RT-008 | IPC队列满 `-EAGAIN` | 填满4096个槽位后再发送；验证返回 `-EAGAIN`，ring状态不损坏 |
| RT-009 | eventfd消费者阻塞唤醒 | 消费者先行等待；生产者发送后验证消费者在1ms内被唤醒 |
| RT-010 | IPC消息无竞争腐败 | 2个生产者线程交替发送到同一ring（注意：SPSC设计；此测试验证文档约束被遵守，单生产者不会产生竞争） |
| RT-011 | 资源监控CPU采样准确性 | 启动一个忙循环线程；1秒后读取 `cpu_pct_history`；验证最新样本 > 90% |
| RT-012 | 资源监控内存分区 | 分配已知大小的内存分区；调用 `resource_monitor_get_snapshot`；验证 `used_pct` 匹配 |
| RT-013 | 通道队列深度历史 | 注入固定深度的通道队列；60秒后验证 `depth_history` 全部样本接近注入值 |
| RT-014 | 异常检测P99.9告警触发 | 向直方图注入1000个高延迟样本（> 阈值）；调用 `anomaly_check_p999`；验证 `alert_count` 递增，log含WARN |
| RT-015 | 异常检测P99.9正常不告警 | 注入均匀低延迟样本；验证 `alert_count` 保持为0 |
| RT-016 | 温度模型线性公式验证 | 设定 `coeff_iops=0.00004`，`coeff_bw=0.002`，`T_ambient=30`，输入 `IOPS=1000000`，`BW=3000 MB/s`；验证 `current_temp_celsius ≈ 76.0` |
| RT-017 | 温度超阈值触发限速 | 输入触发 T ≥ 75°C 的IOPS/BW；调用 `anomaly_update_temperature`；验证 `throttle_active == true`，SMART `critical_warning` 置位 |
| RT-018 | 温度恢复解除限速 | 先触发限速；再输入低IOPS/BW使 T < 70°C；验证 `throttle_active == false`，`critical_warning` 清位，函数返回 `true`（状态改变） |
| RT-019 | SMART温度字段更新 | 调用 `anomaly_update_temperature` 传入有效 `smart_log`；验证 `smart_log->temperature == (uint16_t)(T + 273)` |
| RT-020 | macOS亲和性非致命降级 | 在macOS环境下运行 `rt_thread_pin_cpu`；验证函数返回非零但进程不崩溃，`ctx->pinned == false`，log含WARN |
| RT-021 | macOS pipe替代eventfd | 在macOS环境下调用 `ipc_ring_init`；验证可正常收发消息，无功能缺失 |
| RT-022 | `resource_monitor_get_snapshot` JSON格式 | 调用后解析返回字符串为JSON；验证 `threads`、`partitions`、`channels` 三个顶层键均存在且为数组 |
| RT-023 | 监控线程启停 | 调用 `resource_monitor_start`；sleep 3秒；调用 `resource_monitor_stop`；验证线程已退出，无资源泄漏 |
| RT-024 | 延迟桶边界（bucket计算） | 注入 lat_ns = 1000（1µs）；验证落入 bucket 0；注入 1023000ns（1023µs）；验证落入 bucket 9（[512µs,1024µs)） |
| RT-025 | `anomaly_get_stats_json` 字段完整性 | 调用后解析JSON；验证 `alert_count`、`current_temp_celsius`、`throttle_active`、`p999_alert_threshold_us` 字段均存在 |

---

**文档统计**：
- 覆盖需求：5个（REQ-074、REQ-075、REQ-085、REQ-087、REQ-088）
- 新增头文件：`include/common/rt_services.h`
- 新增源文件：`src/common/rt_thread.c`、`src/common/ipc_ring.c`、`src/common/resource_monitor.c`、`src/common/anomaly_detector.c`
- 函数接口数：30+
- 测试用例：25个
