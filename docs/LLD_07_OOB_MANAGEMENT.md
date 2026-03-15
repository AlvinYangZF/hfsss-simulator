# 高保真全栈SSD模拟器（HFSSS）详细设计文档

**文档名称**：带外管理详细设计
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
7. [/proc文件系统接口设计](#7-proc文件系统接口设计)
8. [CLI工具设计](#8-cli工具设计)
9. [YAML配置文件设计](#9-yaml配置文件设计)
10. [测试要点](#10-测试要点)

---

## 1. 模块概述

带外管理模块（Out-Of-Band Management，OOB）提供仿真器运行时的外部观测与控制能力，与主机I/O数据通路完全隔离。用户和运维工具可在仿真器运行期间通过三种接口与其交互：

1. **Unix Domain Socket（JSON-RPC 2.0）**：读写控制的主要接口，支持查询、配置、故障注入和快照等操作；
2. **/proc文件系统接口**：只读统计信息，适合监控脚本轮询；
3. **hfsss-ctrl CLI工具**：对上述Socket接口的命令行封装，面向开发者和调试场景。

OOB模块以独立守护线程运行，不阻塞任何主I/O路径。内部采用读写锁（`pthread_rwlock_t`）访问共享统计数据，对主路径的影响控制在纳秒级别。

**覆盖需求**：REQ-077、REQ-078、REQ-079、REQ-083（温度与性能异常告警）、REQ-086（Debug Trace导出）、REQ-123、REQ-124、REQ-125、REQ-126。

---

## 2. 功能需求详细分解

| 需求ID | 需求描述 | 优先级 | 目标版本 |
|--------|----------|--------|----------|
| REQ-077 | Unix Domain Socket JSON-RPC + /proc接口 | P0 | V2.0 |
| REQ-078 | 状态/SMART/性能/配置/故障/GC/快照/日志/通道/Die接口 | P0 | V2.0 |
| REQ-079 | NVMe SMART/Health Log Page（0x02）字段仿真 | P0 | V2.0 |
| REQ-083 | 延迟异常告警 + 温度仿真模型 | P0 | V2.0 |
| REQ-086 | 命令Trace/NAND Trace/FTL Trace + 性能计数器 | P0 | V2.0 |
| REQ-123 | JSON-RPC接口详细规范 | P0 | V2.0 |
| REQ-124 | /proc/hfsss/目录结构 | P0 | V2.0 |
| REQ-125 | hfsss-ctrl CLI工具 | P0 | V2.0 |
| REQ-126 | YAML配置文件加载 | P0 | V1.0 |

---

## 3. 数据结构详细设计

### 3.1 OOB上下文

```c
#ifndef __HFSSS_OOB_H
#define __HFSSS_OOB_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

#define OOB_SOCK_PATH_MAX    108
#define OOB_RECV_BUF_SIZE    65536
#define OOB_SEND_BUF_SIZE    65536
#define OOB_MAX_CLIENTS      16
#define OOB_JSONRPC_VERSION  "2.0"

/* OOB server state */
enum oob_state {
    OOB_STATE_STOPPED  = 0,
    OOB_STATE_STARTING = 1,
    OOB_STATE_RUNNING  = 2,
    OOB_STATE_STOPPING = 3,
};

/* Connected client descriptor */
struct oob_client {
    int fd;
    bool active;
    char recv_buf[OOB_RECV_BUF_SIZE];
    uint32_t recv_len;
};

/* OOB server context */
struct oob_ctx {
    int                  server_fd;
    char                 sock_path[OOB_SOCK_PATH_MAX];
    enum oob_state       state;
    pthread_t            thread;
    pthread_mutex_t      lock;
    struct oob_client    clients[OOB_MAX_CLIENTS];
    uint32_t             client_count;

    /* back-pointer to simulator root context for data access */
    void                *sssim_ctx;
};
```

### 3.2 SMART/Health Log Page

```c
/*
 * NVMe SMART/Health Information Log Page (Log Page ID = 0x02).
 * Field layout follows NVMe Base Specification 2.0, Section 5.14.1.2.
 * All multi-byte integer fields are little-endian.
 * 128-bit counters are represented as two consecutive uint64_t (lo, hi).
 */
struct nvme_smart_log {
    uint8_t  critical_warning;        /* byte 0   – bitfield, see SMART_CRIT_* */
    uint16_t temperature;             /* byte 1-2 – Kelvin (273 + Celsius) */
    uint8_t  available_spare;         /* byte 3   – 0-100 % */
    uint8_t  available_spare_thresh;  /* byte 4   – threshold for warning */
    uint8_t  percent_used;            /* byte 5   – 0-255, >100 means worn out */
    uint8_t  endurance_crit_warn;     /* byte 6   */
    uint8_t  reserved0[25];           /* byte 7-31 */
    uint64_t data_units_read[2];      /* byte 32-47  – units of 512kB */
    uint64_t data_units_written[2];   /* byte 48-63  – units of 512kB */
    uint64_t host_read_commands[2];   /* byte 64-79  */
    uint64_t host_write_commands[2];  /* byte 80-95  */
    uint64_t ctrl_busy_time[2];       /* byte 96-111 – minutes */
    uint64_t power_cycles[2];         /* byte 112-127 */
    uint64_t power_on_hours[2];       /* byte 128-143 */
    uint64_t unsafe_shutdowns[2];     /* byte 144-159 */
    uint64_t media_errors[2];         /* byte 160-175 */
    uint64_t num_err_log_entries[2];  /* byte 176-191 */
    uint32_t warn_temp_time;          /* byte 192-195 – minutes above warning temp */
    uint32_t crit_comp_temp_time;     /* byte 196-199 – minutes above critical temp */
    uint16_t temp_sensor[8];          /* byte 200-215 – per-sensor temperatures */
    uint32_t thm_temp1_trans_count;   /* byte 216-219 */
    uint32_t thm_temp2_trans_count;   /* byte 220-223 */
    uint32_t thm_temp1_total_time;    /* byte 224-227 */
    uint32_t thm_temp2_total_time;    /* byte 228-231 */
    uint8_t  reserved1[280];          /* byte 232-511 */
} __attribute__((packed));

/* critical_warning bit definitions */
#define SMART_CRIT_SPARE        (1u << 0)  /* available spare < threshold */
#define SMART_CRIT_TEMPERATURE  (1u << 1)  /* temperature >= warning threshold */
#define SMART_CRIT_DEGRADED     (1u << 2)  /* NVM subsystem reliability degraded */
#define SMART_CRIT_READONLY     (1u << 3)  /* media placed in read-only mode */
#define SMART_CRIT_VOLATILE_MEM (1u << 4)  /* volatile memory backup device failed */
```

### 3.3 性能统计

```c
#define LATENCY_HIST_BUCKETS  64   /* exponential buckets: 1µs, 2µs, 4µs … */

/* Per-namespace or global I/O performance counters.
 * Updated by the controller path under an atomic or the stats_lock. */
struct perf_counters {
    /* Throughput */
    uint64_t total_read_ios;
    uint64_t total_write_ios;
    uint64_t total_read_bytes;
    uint64_t total_write_bytes;

    /* Latency histogram (read and write separate) */
    uint64_t read_lat_hist[LATENCY_HIST_BUCKETS];
    uint64_t write_lat_hist[LATENCY_HIST_BUCKETS];

    /* Snapshot window (reset-able via OOB) */
    uint64_t window_read_ios;
    uint64_t window_write_ios;
    uint64_t window_start_ns;

    /* Write amplification */
    uint64_t nand_write_pages;  /* NAND-level pages written (GC + host) */
    uint64_t host_write_pages;  /* host-driven logical pages written */

    /* GC */
    uint64_t gc_runs;
    uint64_t gc_pages_moved;
    uint64_t gc_blocks_erased;

    pthread_rwlock_t lock;
};

/* Derived / computed metrics (populated on each OOB query, not hot path) */
struct perf_snapshot {
    double  read_iops;
    double  write_iops;
    double  read_bw_mbps;
    double  write_bw_mbps;
    double  waf;                         /* write amplification factor */
    uint64_t lat_read_p50_us;
    uint64_t lat_read_p99_us;
    uint64_t lat_read_p999_us;
    uint64_t lat_write_p50_us;
    uint64_t lat_write_p99_us;
    uint64_t lat_write_p999_us;
};
```

### 3.4 温度模型

```c
#define TEMP_WARN_CELSIUS    70
#define TEMP_CRIT_CELSIUS    75
#define TEMP_AMBIENT_CELSIUS 30

/* Linear model: T = T_ambient + IOPS * COEFF_I + BW_MBps * COEFF_B */
#define TEMP_COEFF_IOPS     0.00004   /* °C per IOPS */
#define TEMP_COEFF_BW       0.002     /* °C per MB/s */

struct temp_model {
    double   current_celsius;
    double   ambient_celsius;
    bool     throttle_active;  /* true when T >= TEMP_CRIT_CELSIUS */
    uint32_t warn_minutes;     /* cumulative minutes above warn threshold */
    uint32_t crit_minutes;     /* cumulative minutes above crit threshold */
};
```

### 3.5 命令Trace

```c
#define TRACE_RING_CAPACITY  100000

enum trace_op_type {
    TRACE_OP_HOST_READ   = 0,
    TRACE_OP_HOST_WRITE  = 1,
    TRACE_OP_HOST_FLUSH  = 2,
    TRACE_OP_GC_READ     = 3,
    TRACE_OP_GC_WRITE    = 4,
    TRACE_OP_GC_ERASE    = 5,
    TRACE_OP_WL_MIGRATE  = 6,
};

/* Single trace record – 64 bytes */
struct trace_entry {
    uint64_t submit_ns;     /* absolute timestamp of command submission */
    uint64_t complete_ns;   /* absolute timestamp of completion */
    uint64_t lba;           /* starting LBA (host commands) or PPN (NAND commands) */
    uint32_t len_sectors;   /* transfer length in 512-byte sectors */
    uint32_t cmd_id;        /* NVMe command identifier */
    uint16_t sq_id;         /* NVMe SQ ID */
    uint8_t  op_type;       /* enum trace_op_type */
    uint8_t  status;        /* NVMe status code (0 = success) */
    uint8_t  channel;
    uint8_t  chip;
    uint8_t  die;
    uint8_t  reserved[13];
} __attribute__((packed));

struct trace_ring {
    struct trace_entry  entries[TRACE_RING_CAPACITY];
    uint64_t            head;        /* next write position (mod capacity) */
    uint64_t            total_count; /* total records ever written */
    bool                enabled;
    pthread_spinlock_t  lock;
};
```

---

## 4. 头文件设计

```c
/* include/common/oob.h */
#ifndef __HFSSS_OOB_FULL_H
#define __HFSSS_OOB_FULL_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

/* Forward declarations */
struct sssim_ctx;
struct perf_counters;
struct nvme_smart_log;
struct trace_ring;

/* ------------------------------------------------------------------ */
/* OOB server lifecycle                                                 */
/* ------------------------------------------------------------------ */

/*
 * oob_init – initialise and launch the OOB listener thread.
 *
 * sock_path: Unix domain socket path (e.g. "/var/run/hfsss/hfsss.sock").
 * sssim:     root simulator context (read-only for most handlers).
 *
 * Returns 0 on success, negative errno on failure.
 */
int oob_init(struct oob_ctx *ctx, const char *sock_path, struct sssim_ctx *sssim);

/*
 * oob_shutdown – gracefully stop the OOB thread and close all client fds.
 * Blocks until the thread has exited.
 */
void oob_shutdown(struct oob_ctx *ctx);

/* ------------------------------------------------------------------ */
/* SMART                                                                */
/* ------------------------------------------------------------------ */

/*
 * smart_update – recompute all SMART fields from current simulator state.
 * Called periodically (once per second) by the monitoring thread.
 */
void smart_update(struct nvme_smart_log *log, struct sssim_ctx *sssim);

/*
 * smart_get_json – serialise the SMART log to a JSON string.
 * Caller must free() the returned buffer.
 */
char *smart_get_json(const struct nvme_smart_log *log);

/* ------------------------------------------------------------------ */
/* Performance counters                                                 */
/* ------------------------------------------------------------------ */

/*
 * perf_record_io – record a single I/O completion on the hot path.
 * Designed for minimal lock contention (uses spinlock on the histogram
 * bucket, not the entire struct).
 *
 * is_read:  true = read, false = write.
 * bytes:    transfer size in bytes.
 * lat_ns:   end-to-end latency in nanoseconds.
 */
void perf_record_io(struct perf_counters *pc, bool is_read,
                    uint32_t bytes, uint64_t lat_ns);

/*
 * perf_snapshot – compute derived metrics (IOPS, BW, latency percentiles,
 * WAF) into the caller-supplied snapshot.  Thread-safe; takes a read lock.
 */
void perf_snapshot(const struct perf_counters *pc, struct perf_snapshot *out);

/*
 * perf_reset_window – reset the rolling window counters.
 */
void perf_reset_window(struct perf_counters *pc);

/* ------------------------------------------------------------------ */
/* Temperature model                                                    */
/* ------------------------------------------------------------------ */

/*
 * temp_update – recalculate temperature based on current IOPS and BW.
 * Updates the SMART log critical_warning and temperature fields in place.
 * Returns true if throttling state changed (caller should log the event).
 */
bool temp_update(struct temp_model *tm, struct nvme_smart_log *log,
                 double iops, double bw_mbps);

/* ------------------------------------------------------------------ */
/* Command trace                                                        */
/* ------------------------------------------------------------------ */

/*
 * trace_record – append one entry to the ring.  No-op when disabled.
 * Lock-free for concurrent writers via compare-and-swap on head.
 */
void trace_record(struct trace_ring *tr, const struct trace_entry *entry);

/*
 * trace_enable / trace_disable – toggle tracing at runtime.
 */
void trace_enable(struct trace_ring *tr);
void trace_disable(struct trace_ring *tr);

/*
 * trace_dump_jsonl – write the ring's current contents to fd in
 * JSON Lines format (one JSON object per line).
 * Returns the number of records written, or negative errno on I/O error.
 */
int trace_dump_jsonl(const struct trace_ring *tr, int fd);

/* ------------------------------------------------------------------ */
/* /proc interface                                                      */
/* ------------------------------------------------------------------ */

/*
 * proc_hfsss_init – register the /proc/hfsss/ virtual filesystem subtree.
 * No-op on non-Linux platforms; returns 0 without error.
 */
int proc_hfsss_init(struct sssim_ctx *sssim);

/*
 * proc_hfsss_cleanup – deregister /proc/hfsss/.
 */
void proc_hfsss_cleanup(void);

/* ------------------------------------------------------------------ */
/* YAML configuration                                                   */
/* ------------------------------------------------------------------ */

/*
 * config_load_yaml – parse the YAML configuration file and populate
 * the simulator config structure.  Returns 0 on success, negative errno
 * on parse or validation error.  Error details are written to errbuf
 * (at most errbuf_len bytes).
 */
int config_load_yaml(const char *path, struct sssim_config *cfg,
                     char *errbuf, size_t errbuf_len);

#endif /* __HFSSS_OOB_FULL_H */
```

---

## 5. 函数接口详细设计

### 5.1 OOB服务线程主循环

```
oob_server_thread(ctx)
├── bind Unix Domain Socket → ctx->server_fd
├── listen(ctx->server_fd, OOB_MAX_CLIENTS)
└── loop until ctx->state == OOB_STATE_STOPPING:
    ├── pselect/epoll_wait on [server_fd, client_fds[]]
    ├── if server_fd readable → accept new client
    │   └── add to ctx->clients[], set nonblocking
    └── for each readable client_fd:
        ├── recv into client->recv_buf
        ├── attempt JSON parse (may be partial – buffer and retry)
        ├── oob_dispatch_request(ctx, client, json_obj)
        └── send JSON-RPC response back on same fd
```

### 5.2 JSON-RPC请求分发

`oob_dispatch_request` 根据 `method` 字段路由到对应处理函数：

| method | 处理函数 | HTTP类比 |
|--------|----------|----------|
| `status.get` | `handle_status_get` | GET /status |
| `smart.get` | `handle_smart_get` | GET /smart |
| `perf.get` | `handle_perf_get` | GET /perf |
| `perf.reset` | `handle_perf_reset` | POST /perf/reset |
| `channel.get` | `handle_channel_get` | GET /nand/channel/{id} |
| `die.get` | `handle_die_get` | GET /die/{ch}/{chip}/{die} |
| `config.get` | `handle_config_get` | GET /config |
| `config.set` | `handle_config_set` | POST /config |
| `gc.config` | `handle_gc_config` | POST /config/gc |
| `gc.trigger` | `handle_gc_trigger` | POST /gc/trigger |
| `fault.inject` | `handle_fault_inject` | POST /fault/inject |
| `snapshot.save` | `handle_snapshot_save` | POST /snapshot |
| `log.get` | `handle_log_get` | GET /log |
| `trace.enable` | `handle_trace_enable` | POST /debug/trace/enable |
| `trace.disable` | `handle_trace_disable` | POST /debug/trace/disable |
| `trace.dump` | `handle_trace_dump` | GET /debug/trace/dump |

所有处理函数签名：

```c
typedef int (*oob_handler_fn)(struct oob_ctx *ctx,
                              const struct json_object *params,
                              struct json_object *result_out,
                              char *errmsg, size_t errmsg_len);
```

返回0表示成功（result已填充），负数表示错误（errmsg已填充）。

### 5.3 JSON-RPC消息格式

**请求**：

```json
{
  "jsonrpc": "2.0",
  "method": "status.get",
  "params": {},
  "id": 42
}
```

**成功响应**：

```json
{
  "jsonrpc": "2.0",
  "result": { ... },
  "id": 42
}
```

**错误响应**（使用标准JSON-RPC错误代码）：

```json
{
  "jsonrpc": "2.0",
  "error": {
    "code": -32602,
    "message": "Invalid params: channel id out of range"
  },
  "id": 42
}
```

| 错误代码 | 含义 |
|----------|------|
| -32700 | Parse error（JSON格式错误） |
| -32600 | Invalid request |
| -32601 | Method not found |
| -32602 | Invalid params |
| -32000 | Server error（模拟器内部错误） |

### 5.4 关键接口响应格式

#### status.get

```json
{
  "state": "running",
  "uptime_seconds": 3600,
  "nand_capacity_gb": 4096,
  "free_blocks_percent": 18.5,
  "current_iops": 450123,
  "current_bw_mbps": 3456,
  "gc_state": "background",
  "waf": 1.87,
  "temperature_celsius": 52.3,
  "throttle_active": false
}
```

#### smart.get

返回NVMe SMART/Health Information Log Page中所有字段，以可读键名表示（而非字节偏移），例：

```json
{
  "critical_warning": 0,
  "temperature_kelvin": 325,
  "available_spare_pct": 92,
  "available_spare_threshold_pct": 10,
  "percent_used": 3,
  "data_units_read": 1048576,
  "data_units_written": 2097152,
  "host_read_commands": 5000000,
  "host_write_commands": 2000000,
  "power_cycles": 12,
  "power_on_hours": 150,
  "unsafe_shutdowns": 1,
  "media_errors": 0,
  "warn_temp_time_minutes": 0,
  "crit_comp_temp_time_minutes": 0
}
```

#### perf.get

```json
{
  "read_iops": 423500.0,
  "write_iops": 187200.0,
  "read_bw_mbps": 1649.4,
  "write_bw_mbps": 730.1,
  "waf": 1.87,
  "latency_read": {
    "p50_us": 85,
    "p99_us": 210,
    "p999_us": 890
  },
  "latency_write": {
    "p50_us": 120,
    "p99_us": 350,
    "p999_us": 1400
  }
}
```

#### channel.get（params: {"channel_id": 3}）

```json
{
  "channel_id": 3,
  "chips": 4,
  "utilization_pct": 72.3,
  "cmd_queue_depth": 45,
  "current_op": "program",
  "eat_us": 1709127234567
}
```

#### fault.inject

请求参数：

```json
{
  "type": "bad_block",
  "channel": 3,
  "chip": 1,
  "die": 0,
  "plane": 0,
  "block": 2048,
  "immediate": true
}
```

`type` 的合法值：

| 值 | 说明 |
|----|------|
| `bad_block` | 将指定Block标记为永久坏块 |
| `read_error` | 注入页级读错误（需额外字段 `page`） |
| `program_error` | 注入页级写错误 |
| `erase_error` | 注入块级擦除错误 |
| `bit_flip` | 在指定位翻转（需额外字段 `page`, `bit_pos`） |
| `read_disturb_storm` | 模拟集中读扰动 |
| `data_retention` | 加速数据保持时间（需额外字段 `aging_factor`） |

#### config.set（gc配置示例）

```json
{
  "gc.high_watermark_pct": 25,
  "gc.low_watermark_pct": 8,
  "gc.max_bw_pct": 35,
  "gc.algorithm": "cost_benefit"
}
```

### 5.5 SMART更新逻辑

`smart_update` 由监控线程每秒调用一次，执行以下计算：

```
1. temperature
   T_celsius = TEMP_AMBIENT + iops * COEFF_IOPS + bw_mbps * COEFF_BW
   log->temperature = (uint16_t)(T_celsius + 273)

2. available_spare
   free_block_ratio = free_blocks / total_blocks (from FTL block pool)
   log->available_spare = (uint8_t)(free_block_ratio * 100)
   if available_spare < available_spare_thresh:
       log->critical_warning |= SMART_CRIT_SPARE

3. percent_used
   avg_pe = mean erase count across all blocks (from FTL wear level stats)
   log->percent_used = (uint8_t)(avg_pe * 100 / PE_CYCLE_LIMIT)

4. power_on_hours
   log->power_on_hours[0] = (current_time_ns - boot_time_ns) / 3600e9

5. critical_warning: temperature bits
   if T_celsius >= TEMP_WARN_CELSIUS:
       log->critical_warning |= SMART_CRIT_TEMPERATURE
       log->warn_temp_time++
   if T_celsius >= TEMP_CRIT_CELSIUS:
       set throttle_active = true
       (controller reads throttle_active to cap IOPS at 50% of max)
```

### 5.6 延迟分位数计算

延迟直方图采用指数区间（Exponential buckets）：

- Bucket 0：[0, 1µs)
- Bucket 1：[1µs, 2µs)
- Bucket 2：[2µs, 4µs)
- …
- Bucket k：[2^(k-1) µs, 2^k µs)

`perf_record_io` 内联计算 `bucket = floor(log2(lat_us + 1))`（使用 `__builtin_clz` 实现O(1)）并对对应桶做原子加。

`perf_snapshot` 遍历直方图，通过前缀和找到P50/P99/P99.9所在桶，返回桶下边界作为保守估计。

### 5.7 Trace环形缓冲区写入

```
trace_record(tr, entry):
  if !tr->enabled: return
  loop:
    old_head = atomic_load(&tr->head)
    new_head = (old_head + 1) % TRACE_RING_CAPACITY
    if CAS(&tr->head, old_head, new_head): break  // claim slot
  tr->entries[old_head] = *entry
  atomic_fetch_add(&tr->total_count, 1)
```

多个线程并发写入时，每个线程通过CAS（Compare-And-Swap）抢占独立槽位，无需全局锁。读取（dump）时对整个环加读锁，阻塞新的写入直至dump完成。

---

## 6. 流程图

### 6.1 OOB服务器启动与请求处理

```
oob_init()
    │
    ▼
创建 SOCK_STREAM Unix Domain Socket
    │
    ▼
bind(sock_path) → listen()
    │
    ▼
pthread_create(oob_server_thread)
    │
    ▼
ctx->state = OOB_STATE_RUNNING
    │
    └──── return 0 to caller

oob_server_thread():
    ┌─────────────────────────────────────────────────────────┐
    │  epoll_wait([server_fd, client_fd[]])                   │
    │       │                                                  │
    │       ├─ server_fd readable ──► accept() ──► add client │
    │       │                                                  │
    │       └─ client_fd[i] readable                          │
    │               │                                          │
    │               ▼                                          │
    │         recv() → accumulate recv_buf                     │
    │               │                                          │
    │               ▼                                          │
    │         json_parse(recv_buf)                             │
    │               │ success                                  │
    │               ▼                                          │
    │         oob_dispatch_request()                           │
    │               │                                          │
    │               ▼                                          │
    │         build JSON-RPC response                          │
    │               │                                          │
    │               ▼                                          │
    │         send() back to client_fd[i]                      │
    └─────────────────────────────────────────────────────────┘
```

### 6.2 SMART更新与温度告警

```
monitoring_thread() [1 Hz]:
    │
    ▼
perf_snapshot(pc, &snap)
    │
    ▼
temp_update(tm, smart_log, snap.read_iops + snap.write_iops, snap.read_bw + snap.write_bw)
    │
    ├─ T < 70°C ──► clear SMART_CRIT_TEMPERATURE, throttle_active=false
    │
    ├─ 70°C ≤ T < 75°C ──► set SMART_CRIT_TEMPERATURE, warn_temp_time++
    │                        log WARN "temperature warning"
    │
    └─ T ≥ 75°C ──► set SMART_CRIT_TEMPERATURE, throttle_active=true
                     crit_comp_temp_time++
                     log ERROR "temperature critical – throttling IOPS to 50%"
    │
    ▼
smart_update(smart_log, sssim)
    │
    ▼
check P99.9 latency > lat_p999_alert_us?
    ├─ yes ──► log WARN "latency anomaly: P99.9 = %llu µs"
    └─ no  ──► (no action)
```

### 6.3 故障注入流程

```
handle_fault_inject(ctx, params, result):
    │
    ▼
验证参数（channel/chip/die/plane/block/page范围）
    │
    ├─ 参数非法 ──► 返回 JSON-RPC error -32602
    │
    ▼
根据 type 分发:
    │
    ├─ bad_block  ──► bbt_mark_bad(ch, chip, die, block)
    │                  ftl_retire_block(ppn_block)
    │
    ├─ read_error ──► media_inject_read_error(ch, chip, die, plane, block, page)
    │
    ├─ bit_flip   ──► media_inject_bit_flip(ch, chip, die, plane, block, page, bit_pos)
    │
    └─ (其他) ──► 对应 media_inject_* 函数
    │
    ▼
返回 {"result": "injected", "pba": "<ch>.<chip>.<die>.<plane>.<block>"}
```

---

## 7. /proc文件系统接口设计

Linux `/proc` 虚拟文件系统通过 `seq_file` 机制实现（仅Linux）。各文件均为只读，提供人类可读的文本格式。

```
/proc/hfsss/
├── version          # HFSSS version string + build timestamp
├── status           # overall simulator state (mirrors status.get JSON)
├── config           # active config dump (YAML-like key=value)
├── perf_counters    # snapshot of perf_counters (plain text table)
├── channel_stats    # one line per channel: id utilization queue_depth eat_us
├── ftl_stats        # mapping table hit%, GC count, WAF, block pool usage
├── latency_hist     # ASCII histogram of read/write latency buckets
└── version          # HFSSS version string
```

### 7.1 /proc/hfsss/status 示例输出

```
state:           running
uptime_seconds:  3601
nand_capacity_gb: 4096
free_blocks_pct: 18.5
current_read_iops: 423500
current_write_iops: 187200
gc_state:        background
waf:             1.87
temperature_c:   52.3
throttle:        off
```

### 7.2 /proc/hfsss/latency_hist 示例输出

```
Read latency histogram (µs):
  [  0,   1): 0
  [  1,   2): 12345
  [  2,   4): 98765
  [  4,   8): 234001    ████████████
  [  8,  16): 412883    █████████████████████
  [ 16,  32): 505001    █████████████████████████  ← P50
  [ 32,  64): 309821    ████████████████
  [ 64, 128): 187002    █████████
  [128, 256): 42001     ██                         ← P99
  [256, 512): 8101      ▌
  [512,1024): 1201                                 ← P99.9
  ...
```

### 7.3 实现方式

非Linux平台（macOS开发环境）下，`proc_hfsss_init` 直接返回0；相应的内容通过OOB JSON-RPC接口访问，`hfsss-ctrl` CLI会自动降级到Socket接口。

---

## 8. CLI工具设计

`hfsss-ctrl` 是 `hfsss.sock` 的命令行封装，每次调用发送一个JSON-RPC请求并打印结果。

### 8.1 命令列表

| 命令 | JSON-RPC method | 说明 |
|------|----------------|------|
| `hfsss-ctrl status` | `status.get` | 打印仿真器整体状态 |
| `hfsss-ctrl smart` | `smart.get` | 打印SMART信息 |
| `hfsss-ctrl perf [--watch N]` | `perf.get` | 性能统计；`--watch N` 每N秒刷新 |
| `hfsss-ctrl perf reset` | `perf.reset` | 重置窗口计数器 |
| `hfsss-ctrl channel <id>` | `channel.get` | 指定Channel详细信息 |
| `hfsss-ctrl die <ch> <chip> <die>` | `die.get` | 指定Die的P/E计数和坏块情况 |
| `hfsss-ctrl config get` | `config.get` | 打印当前配置 |
| `hfsss-ctrl config set <key>=<val>` | `config.set` | 修改运行时参数 |
| `hfsss-ctrl gc trigger` | `gc.trigger` | 手动触发一轮GC |
| `hfsss-ctrl gc config ...` | `gc.config` | 修改GC参数 |
| `hfsss-ctrl fault inject ...` | `fault.inject` | 注入指定故障 |
| `hfsss-ctrl snapshot save` | `snapshot.save` | 立即触发全量持久化 |
| `hfsss-ctrl log dump [--level=LEVEL] [--last=N]` | `log.get` | 导出事件日志 |
| `hfsss-ctrl trace start` | `trace.enable` | 启动命令Trace |
| `hfsss-ctrl trace stop` | `trace.disable` | 停止命令Trace |
| `hfsss-ctrl trace dump [--output=PATH]` | `trace.dump` | 导出Trace为JSON Lines |

### 8.2 故障注入命令格式

```bash
hfsss-ctrl fault inject \
    --type=bad_block \
    --ch=3 --chip=1 --die=0 --plane=0 --block=2048

hfsss-ctrl fault inject \
    --type=bit_flip \
    --ch=0 --chip=0 --die=0 --plane=0 --block=100 --page=5 --bit=42

hfsss-ctrl fault inject \
    --type=data_retention \
    --ch=2 --chip=0 --die=1 --block=500 --aging_factor=10.0
```

### 8.3 Socket路径约定

默认路径：`/var/run/hfsss/hfsss.sock`。可通过环境变量 `HFSSS_SOCK` 覆盖，或通过 `--socket=PATH` 命令行参数指定。

---

## 9. YAML配置文件设计

配置文件默认路径：`/etc/hfsss/hfsss.yaml`，可通过 `--config=PATH` 命令行参数覆盖。

```yaml
# HFSSS Configuration File – V2.0

version: "2.0"

system:
  log_level: info                        # error|warn|info|debug|trace
  daemon_pid_file: /var/run/hfsss/hfsss.pid

oob:
  socket_path: /var/run/hfsss/hfsss.sock
  max_clients: 16

memory:
  nand_base_addr: "0x1000000000"         # NAND DRAM region start (hex)
  nand_size_gb: 96
  firmware_heap_mb: 512
  write_buffer_mb: 1024
  read_cache_mb: 256

nand:
  channels: 16
  chips_per_channel: 4
  dies_per_chip: 2
  planes_per_die: 2
  blocks_per_plane: 2048
  pages_per_block: 768
  page_size_bytes: 16384
  oob_size_bytes: 384
  cell_type: tlc                         # slc|mlc|tlc|qlc
  pe_cycle_limit: 3000

ftl:
  gc_high_watermark_pct: 20
  gc_low_watermark_pct: 8
  gc_algorithm: greedy                   # greedy|cost_benefit|fifo
  gc_max_bw_pct: 30
  over_provision_pct: 7

persistence:
  enabled: true
  checkpoint_dir: /var/lib/hfsss/data
  checkpoint_interval_writes: 1073741824  # checkpoint every 1GB of host writes
  wal_dir: /var/lib/hfsss/wal

debug:
  trace_enabled: false
  trace_ring_capacity: 100000
  lat_p999_alert_us: 2000                # alert when P99.9 read latency exceeds this
```

### 9.1 配置加载流程

```
config_load_yaml(path, cfg, errbuf, errbuf_len):
  1. fopen(path, "r") → YAML parser init
  2. parse top-level keys, validate each section
  3. for unknown keys: log WARN (not fatal)
  4. for missing required keys: populate with default values, log INFO
  5. range-check all numeric parameters:
       channels: 1-32, chips_per_channel: 1-8, …
  6. on any out-of-range value: set errbuf, return -EINVAL
  7. populate cfg struct fields
  8. return 0
```

---

## 10. 测试要点

| 测试ID | 测试描述 | 验证点 |
|--------|----------|--------|
| OOB-001 | Socket服务器启动/停止 | `hfsss.sock` 创建与删除，端口无残留 |
| OOB-002 | `status.get` 基本查询 | 返回合法JSON，字段齐全 |
| OOB-003 | `smart.get` 字段正确性 | `available_spare` 随GC消耗下降，`power_on_hours` 单调递增 |
| OOB-004 | `perf.get` 延迟直方图 | 单测中注入固定延迟IO，验证P99分位值落入正确桶 |
| OOB-005 | `fault.inject` bad_block | 注入后BBT记录坏块，后续写操作跳过该Block |
| OOB-006 | 温度告警触发 | 模拟高IOPS，验证`critical_warning`置位，throttle_active=true |
| OOB-007 | 温度恢复 | IOPS降低后温度<70°C，验证`critical_warning`清位 |
| OOB-008 | Trace ring无锁并发写 | 32线程并发写入，验证`total_count`准确，无数据覆盖 |
| OOB-009 | `trace.dump` JSON Lines格式 | 输出每行均为合法JSON对象，字段完整 |
| OOB-010 | `config.set` 动态修改 | `gc.high_watermark_pct` 修改后GC行为变化可观测 |
| OOB-011 | 并发客户端 | 16个客户端同时查询，无死锁，响应时间<10ms |
| OOB-012 | 非法JSON请求 | 返回JSON-RPC error -32700，服务器继续运行 |
| OOB-013 | 未知method | 返回JSON-RPC error -32601 |
| OOB-014 | /proc/hfsss/status可读 | `cat /proc/hfsss/status`不报错，含state字段（Linux only） |
| OOB-015 | YAML加载范围校验 | `channels=999`时返回-EINVAL，errbuf含可读错误信息 |
| OOB-016 | hfsss-ctrl perf --watch | 每秒刷新IOPS，值随负载变化 |

---

**文档统计**：
- 覆盖需求：9个（REQ-077至REQ-079，REQ-083，REQ-086，REQ-123至REQ-126）
- 新增头文件：`include/common/oob.h`
- 新增源文件：`src/common/oob.c`、`src/common/smart.c`、`src/common/perf.c`、`src/common/trace.c`、`src/common/temp_model.c`、`src/common/config_yaml.c`、`src/tools/hfsss-ctrl.c`
- 函数接口数：25+
- 测试用例：16个
