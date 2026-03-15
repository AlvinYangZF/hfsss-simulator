#include "common/oob.h"
#include "common/log.h"
#include "common/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <fcntl.h>
#include <inttypes.h>

/* ------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------ */
static uint64_t oob_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Minimal JSON string-escape: copies src to dst, escaping " and \ */
static void json_escape(const char *src, char *dst, size_t dstlen) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dstlen; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            dst[j++] = '\\';
        }
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

/* ------------------------------------------------------------------
 * JSON-RPC helpers
 * ------------------------------------------------------------------ */
int oob_jsonrpc_ok(char *buf, size_t bufsz, int64_t id,
                   const char *result_json) {
    return snprintf(buf, bufsz,
        "{\"jsonrpc\":\"2.0\",\"result\":%s,\"id\":%" PRId64 "}\n",
        result_json ? result_json : "null", id);
}

int oob_jsonrpc_err(char *buf, size_t bufsz, int64_t id,
                    int code, const char *message) {
    char esc[256];
    json_escape(message ? message : "", esc, sizeof(esc));
    return snprintf(buf, bufsz,
        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"%s\"},\"id\":%" PRId64 "}\n",
        code, esc, id);
}

/* Minimal JSON field extractor: find "key":"value" or "key":number */
static int json_get_string(const char *json, const char *key,
                            char *out, size_t outlen) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < outlen) {
            if (*p == '\\') p++;  /* skip escape */
            if (*p) out[i++] = *p++;
        }
        out[i] = '\0';
        return (*p == '"') ? 0 : -1;
    }
    /* numeric */
    size_t i = 0;
    while (*p && *p != ',' && *p != '}' && *p != ']' && i + 1 < outlen) {
        out[i++] = *p++;
    }
    while (i > 0 && (out[i-1] == ' ' || out[i-1] == '\t')) i--;
    out[i] = '\0';
    return 0;
}

int oob_jsonrpc_parse(const char *buf,
                      char *method_out, size_t method_len,
                      int64_t *id_out,
                      char *params_out, size_t params_len) {
    if (!buf || !method_out || !id_out) return -1;

    /* Extract method */
    if (json_get_string(buf, "method", method_out, method_len) != 0) {
        return -1;
    }

    /* Extract id (may be absent → use 0) */
    char id_str[32] = "0";
    json_get_string(buf, "id", id_str, sizeof(id_str));
    *id_out = (int64_t)atoll(id_str);

    /* Extract params as raw substring */
    if (params_out && params_len > 0) {
        const char *p = strstr(buf, "\"params\"");
        if (p) {
            p += 8;
            while (*p == ' ' || *p == '\t' || *p == ':') p++;
            /* Copy until balanced brace/bracket closes */
            int depth = 0;
            size_t i = 0;
            while (*p && i + 1 < params_len) {
                if (*p == '{' || *p == '[') depth++;
                else if (*p == '}' || *p == ']') {
                    depth--;
                    if (depth < 0) break;
                }
                params_out[i++] = *p++;
                if (depth == 0) break;
            }
            if (i < params_len) params_out[i] = '\0';
            else params_out[params_len - 1] = '\0';
        } else {
            strncpy(params_out, "{}", params_len);
            params_out[params_len - 1] = '\0';
        }
    }
    return 0;
}

/* ------------------------------------------------------------------
 * SMART implementation
 * ------------------------------------------------------------------ */
void smart_init(struct nvme_smart_log *log) {
    if (!log) return;
    memset(log, 0, sizeof(*log));
    log->available_spare        = 100;
    log->available_spare_thresh = SMART_DEFAULT_SPARE_THRESH;
    /* Room temperature: 30°C + 273 = 303 K */
    log->temperature = (uint16_t)(TEMP_AMBIENT_CELSIUS + SMART_KELVIN_OFFSET);
    for (int i = 0; i < 8; i++)
        log->temp_sensor[i] = log->temperature;
}

void smart_set_spare(struct nvme_smart_log *log, uint8_t spare_pct) {
    if (!log) return;
    log->available_spare = spare_pct;
    if (spare_pct < log->available_spare_thresh)
        log->critical_warning |= SMART_CRIT_SPARE;
    else
        log->critical_warning &= (uint8_t)~SMART_CRIT_SPARE;
}

void smart_set_temperature(struct nvme_smart_log *log, int celsius) {
    if (!log) return;
    uint16_t kelvin = (uint16_t)(celsius + SMART_KELVIN_OFFSET);
    log->temperature = kelvin;
    for (int i = 0; i < 8; i++)
        log->temp_sensor[i] = kelvin;
    if (celsius >= TEMP_WARN_CELSIUS)
        log->critical_warning |= SMART_CRIT_TEMPERATURE;
    else
        log->critical_warning &= (uint8_t)~SMART_CRIT_TEMPERATURE;
}

void smart_increment_power_cycles(struct nvme_smart_log *log) {
    if (!log) return;
    log->power_cycles[0]++;
}

void smart_increment_unsafe_shutdowns(struct nvme_smart_log *log) {
    if (!log) return;
    log->unsafe_shutdowns[0]++;
}

void smart_add_media_error(struct nvme_smart_log *log) {
    if (!log) return;
    log->media_errors[0]++;
    log->num_err_log_entries[0]++;
    log->critical_warning |= SMART_CRIT_DEGRADED;
}

char *smart_to_json(const struct nvme_smart_log *log) {
    if (!log) return NULL;
    int temp_c = (int)log->temperature - (int)SMART_KELVIN_OFFSET;

    char *buf = (char *)malloc(1024);
    if (!buf) return NULL;

    snprintf(buf, 1024,
        "{"
        "\"critical_warning\":%u,"
        "\"temperature_celsius\":%d,"
        "\"available_spare\":%u,"
        "\"available_spare_thresh\":%u,"
        "\"percent_used\":%u,"
        "\"data_units_read\":%" PRIu64 ","
        "\"data_units_written\":%" PRIu64 ","
        "\"host_read_commands\":%" PRIu64 ","
        "\"host_write_commands\":%" PRIu64 ","
        "\"power_cycles\":%" PRIu64 ","
        "\"power_on_hours\":%" PRIu64 ","
        "\"unsafe_shutdowns\":%" PRIu64 ","
        "\"media_errors\":%" PRIu64 ","
        "\"num_err_log_entries\":%" PRIu64 ","
        "\"warn_temp_time\":%u,"
        "\"crit_comp_temp_time\":%u"
        "}",
        (unsigned)log->critical_warning,
        temp_c,
        (unsigned)log->available_spare,
        (unsigned)log->available_spare_thresh,
        (unsigned)log->percent_used,
        log->data_units_read[0],
        log->data_units_written[0],
        log->host_read_commands[0],
        log->host_write_commands[0],
        log->power_cycles[0],
        log->power_on_hours[0],
        log->unsafe_shutdowns[0],
        log->media_errors[0],
        log->num_err_log_entries[0],
        log->warn_temp_time,
        log->crit_comp_temp_time);
    return buf;
}

/* ------------------------------------------------------------------
 * Performance counters
 * ------------------------------------------------------------------ */
int perf_init(struct perf_counters *pc) {
    if (!pc) return HFSSS_ERR_INVAL;
    memset(pc, 0, sizeof(*pc));
    pthread_rwlock_init(&pc->lock, NULL);
    pc->window_start_ns = oob_now_ns();
    pc->initialized = true;
    return HFSSS_OK;
}

void perf_cleanup(struct perf_counters *pc) {
    if (!pc || !pc->initialized) return;
    pthread_rwlock_destroy(&pc->lock);
    pc->initialized = false;
}

/* Map latency in ns to histogram bucket index (exponential µs buckets) */
static int lat_bucket(uint64_t lat_ns) {
    uint64_t lat_us = lat_ns / 1000;
    if (lat_us == 0) return 0;
    int b = 0;
    while (lat_us >>= 1) b++;
    return (b < LATENCY_HIST_BUCKETS) ? b : (LATENCY_HIST_BUCKETS - 1);
}

void perf_record_io(struct perf_counters *pc, bool is_read,
                    uint32_t bytes, uint64_t lat_ns) {
    if (!pc || !pc->initialized) return;

    pthread_rwlock_wrlock(&pc->lock);
    if (is_read) {
        pc->total_read_ios++;
        pc->total_read_bytes += bytes;
        pc->window_read_ios++;
        pc->read_lat_hist[lat_bucket(lat_ns)]++;
    } else {
        pc->total_write_ios++;
        pc->total_write_bytes += bytes;
        pc->window_write_ios++;
        pc->write_lat_hist[lat_bucket(lat_ns)]++;
        pc->host_write_pages++;
    }
    pthread_rwlock_unlock(&pc->lock);
}

/* Compute percentile from histogram: find bucket where cumulative >= pct */
static uint64_t hist_percentile(const uint64_t *hist, uint64_t total,
                                 double pct) {
    if (total == 0) return 0;
    uint64_t target = (uint64_t)(total * pct / 100.0);
    uint64_t cum = 0;
    for (int i = 0; i < LATENCY_HIST_BUCKETS; i++) {
        cum += hist[i];
        if (cum >= target) {
            /* Bucket i represents [2^i µs, 2^(i+1) µs) */
            return (uint64_t)1 << i;
        }
    }
    return (uint64_t)1 << (LATENCY_HIST_BUCKETS - 1);
}

void perf_snapshot(const struct perf_counters *pc, struct perf_snapshot *out) {
    if (!pc || !out) return;

    pthread_rwlock_rdlock((pthread_rwlock_t *)&pc->lock);

    uint64_t elapsed_ns = oob_now_ns() - pc->window_start_ns;
    double elapsed_s = (elapsed_ns > 0) ? (elapsed_ns / 1.0e9) : 1.0;

    out->read_iops    = pc->window_read_ios  / elapsed_s;
    out->write_iops   = pc->window_write_ios / elapsed_s;
    out->read_bw_mbps = pc->total_read_bytes  / elapsed_s / 1e6;
    out->write_bw_mbps= pc->total_write_bytes / elapsed_s / 1e6;
    out->waf = (pc->host_write_pages > 0)
        ? (double)(pc->nand_write_pages + pc->host_write_pages) /
          (double)pc->host_write_pages
        : 1.0;

    uint64_t tot_r = pc->total_read_ios;
    uint64_t tot_w = pc->total_write_ios;
    out->lat_read_p50_us   = hist_percentile(pc->read_lat_hist,  tot_r, 50.0);
    out->lat_read_p99_us   = hist_percentile(pc->read_lat_hist,  tot_r, 99.0);
    out->lat_read_p999_us  = hist_percentile(pc->read_lat_hist,  tot_r, 99.9);
    out->lat_write_p50_us  = hist_percentile(pc->write_lat_hist, tot_w, 50.0);
    out->lat_write_p99_us  = hist_percentile(pc->write_lat_hist, tot_w, 99.0);
    out->lat_write_p999_us = hist_percentile(pc->write_lat_hist, tot_w, 99.9);

    pthread_rwlock_unlock((pthread_rwlock_t *)&pc->lock);
}

void perf_reset_window(struct perf_counters *pc) {
    if (!pc || !pc->initialized) return;
    pthread_rwlock_wrlock(&pc->lock);
    pc->window_read_ios  = 0;
    pc->window_write_ios = 0;
    pc->window_start_ns  = oob_now_ns();
    pthread_rwlock_unlock(&pc->lock);
}

/* ------------------------------------------------------------------
 * Temperature model
 * ------------------------------------------------------------------ */
void temp_init(struct temp_model *tm, double ambient_celsius) {
    if (!tm) return;
    memset(tm, 0, sizeof(*tm));
    tm->ambient_celsius  = ambient_celsius;
    tm->current_celsius  = ambient_celsius;
    tm->throttle_active  = false;
}

bool temp_update(struct temp_model *tm, struct nvme_smart_log *log,
                 double iops, double bw_mbps) {
    if (!tm) return false;

    double t = tm->ambient_celsius
             + iops    * TEMP_COEFF_IOPS
             + bw_mbps * TEMP_COEFF_BW;
    tm->current_celsius = t;

    bool was_throttle = tm->throttle_active;
    tm->throttle_active = (t >= TEMP_CRIT_CELSIUS);

    if (log) {
        smart_set_temperature(log, (int)t);
        if (t >= TEMP_WARN_CELSIUS) {
            tm->warn_minutes++;
            log->warn_temp_time = tm->warn_minutes;
        }
        if (t >= TEMP_CRIT_CELSIUS) {
            tm->crit_minutes++;
            log->crit_comp_temp_time = tm->crit_minutes;
        }
    }
    return (tm->throttle_active != was_throttle);
}

/* ------------------------------------------------------------------
 * JSON-RPC method handlers
 * ------------------------------------------------------------------ */
static int handle_status_get(struct oob_ctx *ctx,
                              char *result, size_t rlen) {
    pthread_mutex_lock(&ctx->lock);
    uint64_t uptime_s = (oob_now_ns() - ctx->start_ns) / 1000000000ULL;
    double cap_gb = ctx->stats.nand_capacity_bytes / 1e9;
    snprintf(result, rlen,
        "{"
        "\"state\":\"running\","
        "\"uptime_seconds\":%" PRIu64 ","
        "\"nand_capacity_gb\":%.1f,"
        "\"free_blocks_percent\":%u,"
        "\"gc_running\":%s,"
        "\"gc_queue_depth\":%u"
        "}",
        uptime_s, cap_gb,
        ctx->stats.free_blocks_pct,
        ctx->stats.gc_running ? "true" : "false",
        ctx->stats.gc_queue_depth);
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

static int handle_smart_get(struct oob_ctx *ctx,
                             char *result, size_t rlen) {
    pthread_mutex_lock(&ctx->lock);
    char *js = smart_to_json(&ctx->stats.smart);
    pthread_mutex_unlock(&ctx->lock);
    if (!js) { strncpy(result, "null", rlen); return -1; }
    strncpy(result, js, rlen - 1);
    result[rlen - 1] = '\0';
    free(js);
    return 0;
}

static int handle_perf_get(struct oob_ctx *ctx,
                            char *result, size_t rlen) {
    struct perf_snapshot snap;
    pthread_mutex_lock(&ctx->lock);
    perf_snapshot(&ctx->stats.perf, &snap);
    pthread_mutex_unlock(&ctx->lock);
    snprintf(result, rlen,
        "{"
        "\"read_iops\":%.1f,"
        "\"write_iops\":%.1f,"
        "\"read_bw_mbps\":%.2f,"
        "\"write_bw_mbps\":%.2f,"
        "\"waf\":%.3f,"
        "\"lat_read_p50_us\":%" PRIu64 ","
        "\"lat_read_p99_us\":%" PRIu64 ","
        "\"lat_read_p999_us\":%" PRIu64 ","
        "\"lat_write_p50_us\":%" PRIu64 ","
        "\"lat_write_p99_us\":%" PRIu64 ","
        "\"lat_write_p999_us\":%" PRIu64
        "}",
        snap.read_iops, snap.write_iops,
        snap.read_bw_mbps, snap.write_bw_mbps,
        snap.waf,
        snap.lat_read_p50_us, snap.lat_read_p99_us, snap.lat_read_p999_us,
        snap.lat_write_p50_us, snap.lat_write_p99_us, snap.lat_write_p999_us);
    return 0;
}

static int handle_perf_reset(struct oob_ctx *ctx,
                              char *result, size_t rlen) {
    perf_reset_window(&ctx->stats.perf);
    strncpy(result, "\"ok\"", rlen);
    return 0;
}

static int handle_gc_trigger(struct oob_ctx *ctx,
                              char *result, size_t rlen) {
    (void)ctx;
    /* In the standalone OOB module, GC trigger is advisory */
    snprintf(result, rlen, "\"gc_trigger_requested\"");
    return 0;
}

static int handle_trace_enable(struct oob_ctx *ctx,
                                char *result, size_t rlen) {
    (void)ctx;
    snprintf(result, rlen, "\"trace_enabled\"");
    return 0;
}

static int handle_trace_disable(struct oob_ctx *ctx,
                                 char *result, size_t rlen) {
    (void)ctx;
    snprintf(result, rlen, "\"trace_disabled\"");
    return 0;
}

static int handle_log_get(struct oob_ctx *ctx,
                           char *result, size_t rlen) {
    (void)ctx;
    snprintf(result, rlen, "{\"entries\":[]}");
    return 0;
}

static int handle_config_get(struct oob_ctx *ctx,
                               char *result, size_t rlen) {
    (void)ctx;
    snprintf(result, rlen,
        "{\"spare_thresh\":%u,\"temp_warn\":%d,\"temp_crit\":%d}",
        SMART_DEFAULT_SPARE_THRESH, TEMP_WARN_CELSIUS, TEMP_CRIT_CELSIUS);
    return 0;
}

/* ------------------------------------------------------------------
 * Dispatch table
 * ------------------------------------------------------------------ */
typedef int (*handler_fn)(struct oob_ctx *, char *, size_t);

static struct {
    const char *method;
    handler_fn  fn;
} dispatch_table[] = {
    { "status.get",     handle_status_get  },
    { "smart.get",      handle_smart_get   },
    { "perf.get",       handle_perf_get    },
    { "perf.reset",     handle_perf_reset  },
    { "gc.trigger",     handle_gc_trigger  },
    { "trace.enable",   handle_trace_enable  },
    { "trace.disable",  handle_trace_disable },
    { "log.get",        handle_log_get     },
    { "config.get",     handle_config_get  },
};

#define DISPATCH_COUNT ((int)(sizeof(dispatch_table)/sizeof(dispatch_table[0])))

/* ------------------------------------------------------------------
 * oob_dispatch — used directly by tests and the server loop
 * ------------------------------------------------------------------ */
int oob_dispatch(struct oob_ctx *ctx, const char *req,
                 char *resp_buf, size_t resp_bufsz) {
    if (!ctx || !req || !resp_buf) return HFSSS_ERR_INVAL;

    char method[64]    = {0};
    char params[512]   = {0};
    int64_t id = 0;

    if (oob_jsonrpc_parse(req, method, sizeof(method),
                          &id, params, sizeof(params)) != 0) {
        oob_jsonrpc_err(resp_buf, resp_bufsz, 0, -32700, "Parse error");
        return HFSSS_ERR_INVAL;
    }

    /* Find handler */
    for (int i = 0; i < DISPATCH_COUNT; i++) {
        if (strcmp(dispatch_table[i].method, method) == 0) {
            char result[OOB_SEND_BUF_SIZE - 128];
            int ret = dispatch_table[i].fn(ctx, result, sizeof(result));
            if (ret == 0) {
                oob_jsonrpc_ok(resp_buf, resp_bufsz, id, result);
            } else {
                oob_jsonrpc_err(resp_buf, resp_bufsz, id, -32000, "Internal error");
            }
            return HFSSS_OK;
        }
    }

    oob_jsonrpc_err(resp_buf, resp_bufsz, id, -32601, "Method not found");
    return HFSSS_ERR_NOENT;
}

/* ------------------------------------------------------------------
 * Server thread
 * ------------------------------------------------------------------ */
static void *oob_server_thread(void *arg) {
    struct oob_ctx *ctx = (struct oob_ctx *)arg;

    ctx->state = OOB_STATE_RUNNING;
    HFSSS_LOG_INFO("OOB", "server started on %s", ctx->sock_path);

    fd_set rfds;
    struct timeval tv;
    char resp[OOB_SEND_BUF_SIZE];

    while (ctx->state == OOB_STATE_RUNNING) {
        FD_ZERO(&rfds);
        FD_SET(ctx->server_fd, &rfds);
        int maxfd = ctx->server_fd;

        pthread_mutex_lock(&ctx->lock);
        for (uint32_t i = 0; i < OOB_MAX_CLIENTS; i++) {
            if (ctx->clients[i].active) {
                FD_SET(ctx->clients[i].fd, &rfds);
                if (ctx->clients[i].fd > maxfd) maxfd = ctx->clients[i].fd;
            }
        }
        pthread_mutex_unlock(&ctx->lock);

        tv.tv_sec  = 0;
        tv.tv_usec = 200000;  /* 200ms poll */
        int nready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (nready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (nready == 0) continue;

        /* New client connection */
        if (FD_ISSET(ctx->server_fd, &rfds)) {
            int cfd = accept(ctx->server_fd, NULL, NULL);
            if (cfd >= 0) {
                pthread_mutex_lock(&ctx->lock);
                bool added = false;
                for (uint32_t i = 0; i < OOB_MAX_CLIENTS; i++) {
                    if (!ctx->clients[i].active) {
                        ctx->clients[i].fd      = cfd;
                        ctx->clients[i].active  = true;
                        ctx->clients[i].recv_len = 0;
                        ctx->client_count++;
                        added = true;
                        break;
                    }
                }
                if (!added) { close(cfd); }
                pthread_mutex_unlock(&ctx->lock);
            }
        }

        /* Client data */
        pthread_mutex_lock(&ctx->lock);
        for (uint32_t i = 0; i < OOB_MAX_CLIENTS; i++) {
            if (!ctx->clients[i].active) continue;
            int cfd = ctx->clients[i].fd;
            if (!FD_ISSET(cfd, &rfds)) continue;

            uint32_t space = OOB_RECV_BUF_SIZE - ctx->clients[i].recv_len - 1;
            ssize_t n = recv(cfd, ctx->clients[i].recv_buf + ctx->clients[i].recv_len,
                             space, 0);
            if (n <= 0) {
                close(cfd);
                ctx->clients[i].active  = false;
                ctx->clients[i].recv_len = 0;
                ctx->client_count--;
                continue;
            }
            ctx->clients[i].recv_len += (uint32_t)n;
            ctx->clients[i].recv_buf[ctx->clients[i].recv_len] = '\0';

            /* Process complete JSON objects */
            char *p = ctx->clients[i].recv_buf;
            while (*p) {
                /* Find matching closing brace for JSON object */
                if (*p != '{') { p++; continue; }
                int depth = 0;
                char *start = p;
                while (*p) {
                    if (*p == '{') depth++;
                    else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
                    p++;
                }
                if (depth != 0) break;  /* incomplete */
                char saved = *p;
                *p = '\0';
                pthread_mutex_unlock(&ctx->lock);
                oob_dispatch(ctx, start, resp, sizeof(resp));
                pthread_mutex_lock(&ctx->lock);
                *p = saved;
                size_t rlen = strlen(resp);
                send(cfd, resp, rlen, MSG_NOSIGNAL);
            }
            /* Compact buffer */
            size_t remaining = strlen(p);
            memmove(ctx->clients[i].recv_buf, p, remaining + 1);
            ctx->clients[i].recv_len = (uint32_t)remaining;
        }
        pthread_mutex_unlock(&ctx->lock);
    }

    /* Close all clients */
    pthread_mutex_lock(&ctx->lock);
    for (uint32_t i = 0; i < OOB_MAX_CLIENTS; i++) {
        if (ctx->clients[i].active) {
            close(ctx->clients[i].fd);
            ctx->clients[i].active = false;
        }
    }
    pthread_mutex_unlock(&ctx->lock);

    HFSSS_LOG_INFO("OOB", "server thread exiting");
    return NULL;
}

/* ------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */
int oob_init(struct oob_ctx *ctx, const char *sock_path) {
    if (!ctx) return HFSSS_ERR_INVAL;

    memset(ctx, 0, sizeof(*ctx));
    const char *path = sock_path ? sock_path : OOB_DEFAULT_SOCK;
    strncpy(ctx->sock_path, path, OOB_SOCK_PATH_MAX - 1);

    pthread_mutex_init(&ctx->lock, NULL);
    smart_init(&ctx->stats.smart);
    perf_init(&ctx->stats.perf);
    temp_init(&ctx->stats.temp, TEMP_AMBIENT_CELSIUS);
    ctx->start_ns = oob_now_ns();

    /* Create Unix domain socket */
    ctx->server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx->server_fd < 0) {
        HFSSS_LOG_WARN("OOB", "socket() failed: %d", errno);
        return HFSSS_ERR_IO;
    }

    /* Allow reuse */
    unlink(ctx->sock_path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ctx->sock_path, sizeof(addr.sun_path) - 1);

    if (bind(ctx->server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        HFSSS_LOG_WARN("OOB", "bind() failed: %d", errno);
        close(ctx->server_fd);
        ctx->server_fd = -1;
        return HFSSS_ERR_IO;
    }

    if (listen(ctx->server_fd, OOB_MAX_CLIENTS) != 0) {
        close(ctx->server_fd);
        ctx->server_fd = -1;
        return HFSSS_ERR_IO;
    }

    ctx->state = OOB_STATE_STARTING;
    ctx->initialized = true;

    if (pthread_create(&ctx->thread, NULL, oob_server_thread, ctx) != 0) {
        close(ctx->server_fd);
        ctx->server_fd = -1;
        ctx->state = OOB_STATE_STOPPED;
        return HFSSS_ERR_IO;
    }

    /* Wait briefly for thread to start */
    struct timespec ts = {0, 10000000L};  /* 10ms */
    nanosleep(&ts, NULL);

    return HFSSS_OK;
}

void oob_shutdown(struct oob_ctx *ctx) {
    if (!ctx || !ctx->initialized) return;

    ctx->state = OOB_STATE_STOPPING;
    pthread_join(ctx->thread, NULL);

    if (ctx->server_fd >= 0) {
        close(ctx->server_fd);
        ctx->server_fd = -1;
    }
    unlink(ctx->sock_path);

    perf_cleanup(&ctx->stats.perf);
    pthread_mutex_destroy(&ctx->lock);
    ctx->initialized = false;
    ctx->state = OOB_STATE_STOPPED;
    HFSSS_LOG_INFO("OOB", "server stopped");
}

void oob_update_stats(struct oob_ctx *ctx, const struct oob_sim_stats *stats) {
    if (!ctx || !stats) return;
    pthread_mutex_lock(&ctx->lock);
    ctx->stats.uptime_ns          = stats->uptime_ns;
    ctx->stats.nand_capacity_bytes = stats->nand_capacity_bytes;
    ctx->stats.free_blocks_pct    = stats->free_blocks_pct;
    ctx->stats.gc_running         = stats->gc_running;
    ctx->stats.gc_queue_depth     = stats->gc_queue_depth;
    ctx->stats.smart              = stats->smart;
    ctx->stats.temp               = stats->temp;
    pthread_mutex_unlock(&ctx->lock);
}
