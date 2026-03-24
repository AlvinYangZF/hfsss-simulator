# High-Fidelity Full-Stack SSD Simulator (HFSSS) Low-Level Design

**Document Name**: Out-of-Band Management Low-Level Design
**Document Version**: V1.0
**Date**: 2026-03-15
**Design Phase**: V2.0 (Beta)
**Classification**: Internal

---

## Revision History

| Version | Date       | Author | Description        |
|---------|------------|--------|--------------------|
| V1.0    | 2026-03-15 | HFSSS  | Initial release    |

---

## Table of Contents

1. [Module Overview](#1-module-overview)
2. [Requirements Traceability](#2-requirements-traceability)
3. [Data Structure Design](#3-data-structure-design)
4. [Header File Design](#4-header-file-design)
5. [Function Interface Design](#5-function-interface-design)
6. [Flow Diagrams](#6-flow-diagrams)
7. [/proc Filesystem Interface Design](#7-proc-filesystem-interface-design)
8. [CLI Tool Design](#8-cli-tool-design)
9. [YAML Configuration File Design](#9-yaml-configuration-file-design)
10. [Test Plan](#10-test-plan)

---

## 1. Module Overview

The Out-of-Band Management module (OOB) provides external observation and control capabilities for the simulator at runtime, completely isolated from the host I/O data path. Users and operations tools can interact with the simulator during operation through three interfaces:

1. **Unix Domain Socket (JSON-RPC 2.0)**: The primary read/write control interface, supporting queries, configuration, fault injection, snapshots, and other operations;
2. **/proc Filesystem Interface**: Read-only statistical information, suitable for polling by monitoring scripts;
3. **hfsss-ctrl CLI Tool**: A command-line wrapper for the above Socket interface, targeting developers and debugging scenarios.

The OOB module runs as an independent daemon thread and does not block any main I/O path. It uses a reader-writer lock (`pthread_rwlock_t`) internally to access shared statistical data, with impact on the main path controlled to the nanosecond level.

**Requirements Coverage**: REQ-077, REQ-078, REQ-079, REQ-083 (temperature and performance anomaly alerts), REQ-086 (Debug Trace export), REQ-123, REQ-124, REQ-125, REQ-126.

---

## 2. Requirements Traceability

| Req ID  | Description | Priority | Target Version |
|---------|-------------|----------|----------------|
| REQ-077 | Unix Domain Socket JSON-RPC + /proc interface | P0 | V2.0 |
| REQ-078 | Status/SMART/Performance/Config/Fault/GC/Snapshot/Log/Channel/Die interfaces | P0 | V2.0 |
| REQ-079 | NVMe SMART/Health Log Page (0x02) field simulation | P0 | V2.0 |
| REQ-083 | Latency anomaly alerts + temperature simulation model | P0 | V2.0 |
| REQ-086 | Command Trace/NAND Trace/FTL Trace + performance counters | P0 | V2.0 |
| REQ-123 | JSON-RPC interface detailed specification | P0 | V2.0 |
| REQ-124 | /proc/hfsss/ directory structure | P0 | V2.0 |
| REQ-125 | hfsss-ctrl CLI tool | P0 | V2.0 |
| REQ-126 | YAML configuration file loading | P0 | V1.0 |

---

## 3. Data Structure Design

### 3.1 OOB Context

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
    uint8_t  critical_warning;        /* byte 0   - bitfield, see SMART_CRIT_* */
    uint16_t temperature;             /* byte 1-2 - Kelvin (273 + Celsius) */
    uint8_t  available_spare;         /* byte 3   - 0-100 % */
    uint8_t  available_spare_thresh;  /* byte 4   - threshold for warning */
    uint8_t  percent_used;            /* byte 5   - 0-255, >100 means worn out */
    uint8_t  endurance_crit_warn;     /* byte 6   */
    uint8_t  reserved0[25];           /* byte 7-31 */
    uint64_t data_units_read[2];      /* byte 32-47  - units of 512kB */
    uint64_t data_units_written[2];   /* byte 48-63  - units of 512kB */
    uint64_t host_read_commands[2];   /* byte 64-79  */
    uint64_t host_write_commands[2];  /* byte 80-95  */
    uint64_t ctrl_busy_time[2];       /* byte 96-111 - minutes */
    uint64_t power_cycles[2];         /* byte 112-127 */
    uint64_t power_on_hours[2];       /* byte 128-143 */
    uint64_t unsafe_shutdowns[2];     /* byte 144-159 */
    uint64_t media_errors[2];         /* byte 160-175 */
    uint64_t num_err_log_entries[2];  /* byte 176-191 */
    uint32_t warn_temp_time;          /* byte 192-195 - minutes above warning temp */
    uint32_t crit_comp_temp_time;     /* byte 196-199 - minutes above critical temp */
    uint16_t temp_sensor[8];          /* byte 200-215 - per-sensor temperatures */
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

### 3.3 Performance Statistics

```c
#define LATENCY_HIST_BUCKETS  64   /* exponential buckets: 1us, 2us, 4us ... */

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

### 3.4 Temperature Model

```c
#define TEMP_WARN_CELSIUS    70
#define TEMP_CRIT_CELSIUS    75
#define TEMP_AMBIENT_CELSIUS 30

/* Linear model: T = T_ambient + IOPS * COEFF_I + BW_MBps * COEFF_B */
#define TEMP_COEFF_IOPS     0.00004   /* deg C per IOPS */
#define TEMP_COEFF_BW       0.002     /* deg C per MB/s */

struct temp_model {
    double   current_celsius;
    double   ambient_celsius;
    bool     throttle_active;  /* true when T >= TEMP_CRIT_CELSIUS */
    uint32_t warn_minutes;     /* cumulative minutes above warn threshold */
    uint32_t crit_minutes;     /* cumulative minutes above crit threshold */
};
```

### 3.5 Command Trace

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

/* Single trace record - 64 bytes */
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

## 4. Header File Design

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
 * oob_init - initialise and launch the OOB listener thread.
 *
 * sock_path: Unix domain socket path (e.g. "/var/run/hfsss/hfsss.sock").
 * sssim:     root simulator context (read-only for most handlers).
 *
 * Returns 0 on success, negative errno on failure.
 */
int oob_init(struct oob_ctx *ctx, const char *sock_path, struct sssim_ctx *sssim);

/*
 * oob_shutdown - gracefully stop the OOB thread and close all client fds.
 * Blocks until the thread has exited.
 */
void oob_shutdown(struct oob_ctx *ctx);

/* ------------------------------------------------------------------ */
/* SMART                                                                */
/* ------------------------------------------------------------------ */

/*
 * smart_update - recompute all SMART fields from current simulator state.
 * Called periodically (once per second) by the monitoring thread.
 */
void smart_update(struct nvme_smart_log *log, struct sssim_ctx *sssim);

/*
 * smart_get_json - serialise the SMART log to a JSON string.
 * Caller must free() the returned buffer.
 */
char *smart_get_json(const struct nvme_smart_log *log);

/* ------------------------------------------------------------------ */
/* Performance counters                                                 */
/* ------------------------------------------------------------------ */

/*
 * perf_record_io - record a single I/O completion on the hot path.
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
 * perf_snapshot - compute derived metrics (IOPS, BW, latency percentiles,
 * WAF) into the caller-supplied snapshot.  Thread-safe; takes a read lock.
 */
void perf_snapshot(const struct perf_counters *pc, struct perf_snapshot *out);

/*
 * perf_reset_window - reset the rolling window counters.
 */
void perf_reset_window(struct perf_counters *pc);

/* ------------------------------------------------------------------ */
/* Temperature model                                                    */
/* ------------------------------------------------------------------ */

/*
 * temp_update - recalculate temperature based on current IOPS and BW.
 * Updates the SMART log critical_warning and temperature fields in place.
 * Returns true if throttling state changed (caller should log the event).
 */
bool temp_update(struct temp_model *tm, struct nvme_smart_log *log,
                 double iops, double bw_mbps);

/* ------------------------------------------------------------------ */
/* Command trace                                                        */
/* ------------------------------------------------------------------ */

/*
 * trace_record - append one entry to the ring.  No-op when disabled.
 * Lock-free for concurrent writers via compare-and-swap on head.
 */
void trace_record(struct trace_ring *tr, const struct trace_entry *entry);

/*
 * trace_enable / trace_disable - toggle tracing at runtime.
 */
void trace_enable(struct trace_ring *tr);
void trace_disable(struct trace_ring *tr);

/*
 * trace_dump_jsonl - write the ring's current contents to fd in
 * JSON Lines format (one JSON object per line).
 * Returns the number of records written, or negative errno on I/O error.
 */
int trace_dump_jsonl(const struct trace_ring *tr, int fd);

/* ------------------------------------------------------------------ */
/* /proc interface                                                      */
/* ------------------------------------------------------------------ */

/*
 * proc_hfsss_init - register the /proc/hfsss/ virtual filesystem subtree.
 * No-op on non-Linux platforms; returns 0 without error.
 */
int proc_hfsss_init(struct sssim_ctx *sssim);

/*
 * proc_hfsss_cleanup - deregister /proc/hfsss/.
 */
void proc_hfsss_cleanup(void);

/* ------------------------------------------------------------------ */
/* YAML configuration                                                   */
/* ------------------------------------------------------------------ */

/*
 * config_load_yaml - parse the YAML configuration file and populate
 * the simulator config structure.  Returns 0 on success, negative errno
 * on parse or validation error.  Error details are written to errbuf
 * (at most errbuf_len bytes).
 */
int config_load_yaml(const char *path, struct sssim_config *cfg,
                     char *errbuf, size_t errbuf_len);

#endif /* __HFSSS_OOB_FULL_H */
```

---

## 5. Function Interface Design

### 5.1 OOB Server Thread Main Loop

```
oob_server_thread(ctx)
+-- bind Unix Domain Socket -> ctx->server_fd
+-- listen(ctx->server_fd, OOB_MAX_CLIENTS)
+-- loop until ctx->state == OOB_STATE_STOPPING:
    +-- pselect/epoll_wait on [server_fd, client_fds[]]
    +-- if server_fd readable -> accept new client
    |   +-- add to ctx->clients[], set nonblocking
    +-- for each readable client_fd:
        +-- recv into client->recv_buf
        +-- attempt JSON parse (may be partial - buffer and retry)
        +-- oob_dispatch_request(ctx, client, json_obj)
        +-- send JSON-RPC response back on same fd
```

### 5.2 JSON-RPC Request Dispatch

`oob_dispatch_request` routes based on the `method` field to the corresponding handler function:

| method | Handler Function | HTTP Analogy |
|--------|-----------------|--------------|
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

All handler function signatures:

```c
typedef int (*oob_handler_fn)(struct oob_ctx *ctx,
                              const struct json_object *params,
                              struct json_object *result_out,
                              char *errmsg, size_t errmsg_len);
```

Returns 0 on success (result populated), negative on error (errmsg populated).

### 5.3 JSON-RPC Message Format

**Request**:

```json
{
  "jsonrpc": "2.0",
  "method": "status.get",
  "params": {},
  "id": 42
}
```

**Success Response**:

```json
{
  "jsonrpc": "2.0",
  "result": { ... },
  "id": 42
}
```

**Error Response** (uses standard JSON-RPC error codes):

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

| Error Code | Meaning |
|------------|---------|
| -32700 | Parse error (malformed JSON) |
| -32600 | Invalid request |
| -32601 | Method not found |
| -32602 | Invalid params |
| -32000 | Server error (simulator internal error) |

### 5.4 Key Interface Response Formats

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

Returns all fields from the NVMe SMART/Health Information Log Page with human-readable key names (rather than byte offsets), e.g.:

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

#### channel.get (params: {"channel_id": 3})

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

Request parameters:

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

Valid values for `type`:

| Value | Description |
|-------|-------------|
| `bad_block` | Mark the specified block as permanently bad |
| `read_error` | Inject page-level read error (requires additional `page` field) |
| `program_error` | Inject page-level write error |
| `erase_error` | Inject block-level erase error |
| `bit_flip` | Flip a specific bit (requires additional `page`, `bit_pos` fields) |
| `read_disturb_storm` | Simulate concentrated read disturb |
| `data_retention` | Accelerate data retention degradation (requires additional `aging_factor` field) |

#### config.set (GC configuration example)

```json
{
  "gc.high_watermark_pct": 25,
  "gc.low_watermark_pct": 8,
  "gc.max_bw_pct": 35,
  "gc.algorithm": "cost_benefit"
}
```

### 5.5 SMART Update Logic

`smart_update` is called once per second by the monitoring thread and performs the following calculations:

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

### 5.6 Latency Percentile Calculation

The latency histogram uses exponential buckets:

- Bucket 0: [0, 1us)
- Bucket 1: [1us, 2us)
- Bucket 2: [2us, 4us)
- ...
- Bucket k: [2^(k-1) us, 2^k us)

`perf_record_io` computes the bucket index inline as `bucket = floor(log2(lat_us + 1))` (implemented using `__builtin_clz` for O(1) complexity) and atomically increments the corresponding bucket.

`perf_snapshot` iterates through the histogram, finding the bucket containing P50/P99/P99.9 via prefix sum, and returns the bucket lower boundary as a conservative estimate.

### 5.7 Trace Ring Buffer Write

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

When multiple threads write concurrently, each thread claims an independent slot via CAS (Compare-And-Swap) without requiring a global lock. During reads (dump), a read lock is taken on the entire ring, blocking new writes until the dump is complete.

---

## 6. Flow Diagrams

### 6.1 OOB Server Startup and Request Processing

```
oob_init()
    |
    v
Create SOCK_STREAM Unix Domain Socket
    |
    v
bind(sock_path) -> listen()
    |
    v
pthread_create(oob_server_thread)
    |
    v
ctx->state = OOB_STATE_RUNNING
    |
    +---- return 0 to caller

oob_server_thread():
    +-------------------------------------------------------------+
    |  epoll_wait([server_fd, client_fd[]])                       |
    |       |                                                      |
    |       +- server_fd readable --> accept() --> add client      |
    |       |                                                      |
    |       +- client_fd[i] readable                               |
    |               |                                              |
    |               v                                              |
    |         recv() -> accumulate recv_buf                         |
    |               |                                              |
    |               v                                              |
    |         json_parse(recv_buf)                                  |
    |               | success                                      |
    |               v                                              |
    |         oob_dispatch_request()                                |
    |               |                                              |
    |               v                                              |
    |         build JSON-RPC response                              |
    |               |                                              |
    |               v                                              |
    |         send() back to client_fd[i]                          |
    +-------------------------------------------------------------+
```

### 6.2 SMART Update and Temperature Alert

```
monitoring_thread() [1 Hz]:
    |
    v
perf_snapshot(pc, &snap)
    |
    v
temp_update(tm, smart_log, snap.read_iops + snap.write_iops, snap.read_bw + snap.write_bw)
    |
    +- T < 70 deg C --> clear SMART_CRIT_TEMPERATURE, throttle_active=false
    |
    +- 70 deg C <= T < 75 deg C --> set SMART_CRIT_TEMPERATURE, warn_temp_time++
    |                               log WARN "temperature warning"
    |
    +- T >= 75 deg C --> set SMART_CRIT_TEMPERATURE, throttle_active=true
                         crit_comp_temp_time++
                         log ERROR "temperature critical - throttling IOPS to 50%"
    |
    v
smart_update(smart_log, sssim)
    |
    v
check P99.9 latency > lat_p999_alert_us?
    +- yes --> log WARN "latency anomaly: P99.9 = %llu us"
    +- no  --> (no action)
```

### 6.3 Fault Injection Flow

```
handle_fault_inject(ctx, params, result):
    |
    v
Validate parameters (channel/chip/die/plane/block/page range)
    |
    +- Invalid params --> return JSON-RPC error -32602
    |
    v
Dispatch by type:
    |
    +- bad_block  --> bbt_mark_bad(ch, chip, die, block)
    |                  ftl_retire_block(ppn_block)
    |
    +- read_error --> media_inject_read_error(ch, chip, die, plane, block, page)
    |
    +- bit_flip   --> media_inject_bit_flip(ch, chip, die, plane, block, page, bit_pos)
    |
    +- (others)   --> corresponding media_inject_* function
    |
    v
Return {"result": "injected", "pba": "<ch>.<chip>.<die>.<plane>.<block>"}
```

---

## 7. /proc Filesystem Interface Design

The Linux `/proc` virtual filesystem is implemented via the `seq_file` mechanism (Linux only). All files are read-only and provide human-readable text format.

```
/proc/hfsss/
+-- version          # HFSSS version string + build timestamp
+-- status           # overall simulator state (mirrors status.get JSON)
+-- config           # active config dump (YAML-like key=value)
+-- perf_counters    # snapshot of perf_counters (plain text table)
+-- channel_stats    # one line per channel: id utilization queue_depth eat_us
+-- ftl_stats        # mapping table hit%, GC count, WAF, block pool usage
+-- latency_hist     # ASCII histogram of read/write latency buckets
+-- version          # HFSSS version string
```

### 7.1 /proc/hfsss/status Sample Output

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

### 7.2 /proc/hfsss/latency_hist Sample Output

```
Read latency histogram (us):
  [  0,   1): 0
  [  1,   2): 12345
  [  2,   4): 98765
  [  4,   8): 234001    ############
  [  8,  16): 412883    #####################
  [ 16,  32): 505001    #########################  <- P50
  [ 32,  64): 309821    ################
  [ 64, 128): 187002    #########
  [128, 256): 42001     ##                         <- P99
  [256, 512): 8101      |
  [512,1024): 1201                                 <- P99.9
  ...
```

### 7.3 Implementation Notes

On non-Linux platforms (macOS development environment), `proc_hfsss_init` returns 0 directly without error; the corresponding content is accessed through the OOB JSON-RPC interface, and `hfsss-ctrl` CLI automatically falls back to the Socket interface.

---

## 8. CLI Tool Design

`hfsss-ctrl` is a command-line wrapper for `hfsss.sock`, sending one JSON-RPC request per invocation and printing the result.

### 8.1 Command List

| Command | JSON-RPC method | Description |
|---------|----------------|-------------|
| `hfsss-ctrl status` | `status.get` | Print overall simulator status |
| `hfsss-ctrl smart` | `smart.get` | Print SMART information |
| `hfsss-ctrl perf [--watch N]` | `perf.get` | Performance statistics; `--watch N` refreshes every N seconds |
| `hfsss-ctrl perf reset` | `perf.reset` | Reset window counters |
| `hfsss-ctrl channel <id>` | `channel.get` | Detailed information for specified channel |
| `hfsss-ctrl die <ch> <chip> <die>` | `die.get` | P/E count and bad block status for specified die |
| `hfsss-ctrl config get` | `config.get` | Print current configuration |
| `hfsss-ctrl config set <key>=<val>` | `config.set` | Modify runtime parameters |
| `hfsss-ctrl gc trigger` | `gc.trigger` | Manually trigger one round of GC |
| `hfsss-ctrl gc config ...` | `gc.config` | Modify GC parameters |
| `hfsss-ctrl fault inject ...` | `fault.inject` | Inject specified fault |
| `hfsss-ctrl snapshot save` | `snapshot.save` | Immediately trigger full persistence |
| `hfsss-ctrl log dump [--level=LEVEL] [--last=N]` | `log.get` | Export event log |
| `hfsss-ctrl trace start` | `trace.enable` | Start command trace |
| `hfsss-ctrl trace stop` | `trace.disable` | Stop command trace |
| `hfsss-ctrl trace dump [--output=PATH]` | `trace.dump` | Export trace as JSON Lines |

### 8.2 Fault Injection Command Format

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

### 8.3 Socket Path Convention

Default path: `/var/run/hfsss/hfsss.sock`. Can be overridden via the `HFSSS_SOCK` environment variable or the `--socket=PATH` command-line argument.

---

## 9. YAML Configuration File Design

Configuration file default path: `/etc/hfsss/hfsss.yaml`, overridable via the `--config=PATH` command-line argument.

```yaml
# HFSSS Configuration File - V2.0

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

### 9.1 Configuration Loading Flow

```
config_load_yaml(path, cfg, errbuf, errbuf_len):
  1. fopen(path, "r") -> YAML parser init
  2. parse top-level keys, validate each section
  3. for unknown keys: log WARN (not fatal)
  4. for missing required keys: populate with default values, log INFO
  5. range-check all numeric parameters:
       channels: 1-32, chips_per_channel: 1-8, ...
  6. on any out-of-range value: set errbuf, return -EINVAL
  7. populate cfg struct fields
  8. return 0
```

---

## 10. Test Plan

| Test ID | Description | Verification Point |
|---------|-------------|-------------------|
| OOB-001 | Socket server start/stop | `hfsss.sock` created and deleted, no residual ports |
| OOB-002 | `status.get` basic query | Returns valid JSON with all fields present |
| OOB-003 | `smart.get` field correctness | `available_spare` decreases with GC consumption, `power_on_hours` monotonically increases |
| OOB-004 | `perf.get` latency histogram | Inject fixed-latency I/O in unit test, verify P99 percentile falls into correct bucket |
| OOB-005 | `fault.inject` bad_block | After injection, BBT records bad block, subsequent write operations skip that block |
| OOB-006 | Temperature alert trigger | Simulate high IOPS, verify `critical_warning` is set, throttle_active=true |
| OOB-007 | Temperature recovery | After IOPS decreases and temperature < 70 deg C, verify `critical_warning` is cleared |
| OOB-008 | Trace ring lock-free concurrent write | 32 threads write concurrently, verify `total_count` is accurate, no data overwrite |
| OOB-009 | `trace.dump` JSON Lines format | Each output line is a valid JSON object with all fields present |
| OOB-010 | `config.set` dynamic modification | After modifying `gc.high_watermark_pct`, observe change in GC behavior |
| OOB-011 | Concurrent clients | 16 clients querying simultaneously, no deadlocks, response time < 10ms |
| OOB-012 | Invalid JSON request | Returns JSON-RPC error -32700, server continues running |
| OOB-013 | Unknown method | Returns JSON-RPC error -32601 |
| OOB-014 | /proc/hfsss/status readable | `cat /proc/hfsss/status` succeeds, contains state field (Linux only) |
| OOB-015 | YAML loading range validation | `channels=999` returns -EINVAL, errbuf contains readable error message |
| OOB-016 | hfsss-ctrl perf --watch | Refreshes IOPS every second, values change with load |

---

**Document Statistics**:
- Requirements covered: 9 (REQ-077 through REQ-079, REQ-083, REQ-086, REQ-123 through REQ-126)
- New header files: `include/common/oob.h`
- New source files: `src/common/oob.c`, `src/common/smart.c`, `src/common/perf.c`, `src/common/trace.c`, `src/common/temp_model.c`, `src/common/config_yaml.c`, `src/tools/hfsss-ctrl.c`
- Function interfaces: 25+
- Test cases: 16

## Appendix: Cross-References

| Reference | Document |
|-----------|----------|
| Fault injection integration | LLD_08_FAULT_INJECTION |
| Bootloader OOB initialization | LLD_09_BOOTLOADER |
| Temperature model coefficients | LLD_12_REALTIME_SERVICES |
| NOR Flash SysInfo partition | LLD_14_NOR_FLASH |
| Persistence format for traces | LLD_15_PERSISTENCE_FORMAT |
