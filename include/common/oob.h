#ifndef HFSSS_COMMON_OOB_H
#define HFSSS_COMMON_OOB_H

/*
 * OOB Management module (REQ-082, REQ-083, REQ-084, REQ-088)
 *
 * Provides:
 *  - Unix Domain Socket JSON-RPC 2.0 server
 *  - NVMe SMART/Health Log Page (16 fields)
 *  - Performance counters with latency histogram
 *  - Temperature model
 *  - Command trace ring (OOB variant)
 *  - YAML configuration
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * Socket / server constants (REQ-082)
 * ------------------------------------------------------------------ */
#define OOB_SOCK_PATH_MAX    108
#define OOB_RECV_BUF_SIZE    4096
#define OOB_SEND_BUF_SIZE    8192
#define OOB_MAX_CLIENTS      16
#define OOB_JSONRPC_VERSION  "2.0"
#define OOB_DEFAULT_SOCK     "/tmp/hfsss.sock"

/* ------------------------------------------------------------------
 * SMART / Health Log Page (REQ-084)
 * NVMe Base Specification 2.0, Section 5.14.1.2, Log Page ID 0x02
 * All multi-byte integers are little-endian; 128-bit counters
 * are stored as {lo, hi} uint64_t pairs.
 * ------------------------------------------------------------------ */
struct nvme_smart_log {
    uint8_t  critical_warning;        /* byte 0   – bitfield, SMART_CRIT_* */
    uint16_t temperature;             /* byte 1-2 – Kelvin (273 + Celsius) */
    uint8_t  available_spare;         /* byte 3   – 0-100 % */
    uint8_t  available_spare_thresh;  /* byte 4   – warn threshold */
    uint8_t  percent_used;            /* byte 5   – endurance used (0-255) */
    uint8_t  endurance_crit_warn;     /* byte 6   */
    uint8_t  reserved0[25];           /* byte 7-31 */
    uint64_t data_units_read[2];      /* byte 32-47  – units of 512 kB */
    uint64_t data_units_written[2];   /* byte 48-63  – units of 512 kB */
    uint64_t host_read_commands[2];   /* byte 64-79  */
    uint64_t host_write_commands[2];  /* byte 80-95  */
    uint64_t ctrl_busy_time[2];       /* byte 96-111 – minutes */
    uint64_t power_cycles[2];         /* byte 112-127 */
    uint64_t power_on_hours[2];       /* byte 128-143 */
    uint64_t unsafe_shutdowns[2];     /* byte 144-159 */
    uint64_t media_errors[2];         /* byte 160-175 */
    uint64_t num_err_log_entries[2];  /* byte 176-191 */
    uint32_t warn_temp_time;          /* byte 192-195 – minutes above warn temp */
    uint32_t crit_comp_temp_time;     /* byte 196-199 – minutes above crit temp */
    uint16_t temp_sensor[8];          /* byte 200-215 – per-sensor temperatures */
    uint32_t thm_temp1_trans_count;   /* byte 216-219 */
    uint32_t thm_temp2_trans_count;   /* byte 220-223 */
    uint32_t thm_temp1_total_time;    /* byte 224-227 */
    uint32_t thm_temp2_total_time;    /* byte 228-231 */
    uint8_t  reserved1[280];          /* byte 232-511 */
} __attribute__((packed));           /* 512 bytes total */

/* critical_warning bit definitions */
#define SMART_CRIT_SPARE        (1u << 0)  /* available_spare < threshold */
#define SMART_CRIT_TEMPERATURE  (1u << 1)  /* temperature >= warning */
#define SMART_CRIT_DEGRADED     (1u << 2)  /* NVM reliability degraded */
#define SMART_CRIT_READONLY     (1u << 3)  /* media in read-only mode */
#define SMART_CRIT_VOLATILE_MEM (1u << 4)  /* volatile memory backup failed */

/* Default spare threshold: warn when spare < 10% */
#define SMART_DEFAULT_SPARE_THRESH  10u
#define SMART_KELVIN_OFFSET         273u

/* ------------------------------------------------------------------
 * Latency histogram (REQ-088)
 * 64 exponential buckets: bucket[i] covers [2^i µs, 2^(i+1) µs)
 * ------------------------------------------------------------------ */
#define LATENCY_HIST_BUCKETS  64

/* ------------------------------------------------------------------
 * Performance counters (REQ-088)
 * ------------------------------------------------------------------ */
struct perf_counters {
    uint64_t total_read_ios;
    uint64_t total_write_ios;
    uint64_t total_read_bytes;
    uint64_t total_write_bytes;

    /* Latency histograms (exponential µs buckets) */
    uint64_t read_lat_hist[LATENCY_HIST_BUCKETS];
    uint64_t write_lat_hist[LATENCY_HIST_BUCKETS];

    /* Rolling window (resettable via perf.reset) */
    uint64_t window_read_ios;
    uint64_t window_write_ios;
    uint64_t window_start_ns;

    /* Write amplification */
    uint64_t nand_write_pages;
    uint64_t host_write_pages;

    /* GC */
    uint64_t gc_runs;
    uint64_t gc_pages_moved;
    uint64_t gc_blocks_erased;

    pthread_rwlock_t lock;
    bool             initialized;
};

struct perf_snapshot {
    double   read_iops;
    double   write_iops;
    double   read_bw_mbps;
    double   write_bw_mbps;
    double   waf;
    uint64_t lat_read_p50_us;
    uint64_t lat_read_p99_us;
    uint64_t lat_read_p999_us;
    uint64_t lat_write_p50_us;
    uint64_t lat_write_p99_us;
    uint64_t lat_write_p999_us;
};

/* ------------------------------------------------------------------
 * Temperature model (REQ-083, REQ-084)
 * T = T_ambient + IOPS * COEFF_IOPS + BW_MBps * COEFF_BW
 * ------------------------------------------------------------------ */
#define TEMP_WARN_CELSIUS     70
#define TEMP_CRIT_CELSIUS     75
#define TEMP_AMBIENT_CELSIUS  30
#define TEMP_COEFF_IOPS       0.00004   /* °C per IOPS */
#define TEMP_COEFF_BW         0.002     /* °C per MB/s */

struct temp_model {
    double   current_celsius;
    double   ambient_celsius;
    bool     throttle_active;
    uint32_t warn_minutes;
    uint32_t crit_minutes;
};

/* ------------------------------------------------------------------
 * OOB server state
 * ------------------------------------------------------------------ */
enum oob_state {
    OOB_STATE_STOPPED  = 0,
    OOB_STATE_STARTING = 1,
    OOB_STATE_RUNNING  = 2,
    OOB_STATE_STOPPING = 3,
};

struct oob_client {
    int      fd;
    bool     active;
    char     recv_buf[OOB_RECV_BUF_SIZE];
    uint32_t recv_len;
};

/* Simulator statistics snapshot — populated by the caller before
 * passing to oob_init, or updated via oob_update_stats().          */
struct oob_sim_stats {
    uint64_t uptime_ns;
    uint64_t nand_capacity_bytes;
    uint32_t free_blocks_pct;       /* 0-100 */
    uint32_t gc_queue_depth;
    bool     gc_running;

    struct nvme_smart_log smart;
    struct perf_counters  perf;
    struct temp_model     temp;
};

struct oob_ctx {
    int               server_fd;
    char              sock_path[OOB_SOCK_PATH_MAX];
    enum oob_state    state;
    pthread_t         thread;
    pthread_mutex_t   lock;

    struct oob_client clients[OOB_MAX_CLIENTS];
    uint32_t          client_count;

    /* Simulator stats — updated externally, read by handlers */
    struct oob_sim_stats stats;
    uint64_t             start_ns;

    bool initialized;
};

/* ------------------------------------------------------------------
 * JSON-RPC helpers (internal, exposed for testing)
 * ------------------------------------------------------------------ */

/* Build a JSON-RPC 2.0 success response into buf (returns bytes written) */
int oob_jsonrpc_ok(char *buf, size_t bufsz,
                   int64_t id, const char *result_json);

/* Build a JSON-RPC 2.0 error response into buf */
int oob_jsonrpc_err(char *buf, size_t bufsz,
                    int64_t id, int code, const char *message);

/* Parse a JSON-RPC request string — extract method, id, params_json.
 * method and params_json point into buf (NOT null-terminated copies).
 * Returns 0 on success, -1 on parse error.                           */
int oob_jsonrpc_parse(const char *buf,
                      char *method_out, size_t method_len,
                      int64_t *id_out,
                      char *params_out, size_t params_len);

/* ------------------------------------------------------------------
 * SMART API
 * ------------------------------------------------------------------ */
void smart_init(struct nvme_smart_log *log);
void smart_set_spare(struct nvme_smart_log *log, uint8_t spare_pct);
void smart_set_temperature(struct nvme_smart_log *log, int celsius);
void smart_increment_power_cycles(struct nvme_smart_log *log);
void smart_increment_unsafe_shutdowns(struct nvme_smart_log *log);
void smart_add_media_error(struct nvme_smart_log *log);

/* Serialize to JSON string — caller must free() the result */
char *smart_to_json(const struct nvme_smart_log *log);

/* ------------------------------------------------------------------
 * Performance counter API
 * ------------------------------------------------------------------ */
int  perf_init(struct perf_counters *pc);
void perf_cleanup(struct perf_counters *pc);
void perf_record_io(struct perf_counters *pc, bool is_read,
                    uint32_t bytes, uint64_t lat_ns);
void perf_snapshot(const struct perf_counters *pc, struct perf_snapshot *out);
void perf_reset_window(struct perf_counters *pc);

/* ------------------------------------------------------------------
 * Temperature model API
 * ------------------------------------------------------------------ */
void temp_init(struct temp_model *tm, double ambient_celsius);
bool temp_update(struct temp_model *tm, struct nvme_smart_log *log,
                 double iops, double bw_mbps);

/* ------------------------------------------------------------------
 * OOB server lifecycle (REQ-082)
 * ------------------------------------------------------------------ */
int  oob_init(struct oob_ctx *ctx, const char *sock_path);
void oob_shutdown(struct oob_ctx *ctx);
void oob_update_stats(struct oob_ctx *ctx, const struct oob_sim_stats *stats);

/* Dispatch a single JSON-RPC request string; result written to resp_buf.
 * Used by tests to exercise handlers without a real socket.           */
int  oob_dispatch(struct oob_ctx *ctx, const char *req,
                  char *resp_buf, size_t resp_bufsz);

#ifdef __cplusplus
}
#endif

#endif /* HFSSS_COMMON_OOB_H */
