/*
 * stress_stability.c — Configurable-duration stability stress test.
 *
 * REQ-132: Fault injection framework stress testing
 * REQ-134: Stability and long-duration reliability verification
 *
 * Runs a mixed read/write workload (70% reads, 30% writes) directly against
 * the media layer with periodic integrity checks and fault injection.
 * Accepts duration via STRESS_DURATION env var or argv[1] (default 60s).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>

#include "common/common.h"
#include "common/fault_inject.h"
#include "common/system_monitor.h"
#include "media/media.h"

/* -------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------- */

#define DEFAULT_DURATION_SEC    60
#define VERIFY_INTERVAL         1000    /* verify integrity every N ops */
#define FAULT_INJECT_INTERVAL   5000    /* inject a fault every N ops */
#define PROGRESS_INTERVAL_SEC   10      /* print progress every N seconds */

/* NAND geometry: 2ch × 2chips × 2dies × 2planes × 256blocks × 128pages */
#define NAND_CHANNELS           2
#define NAND_CHIPS              2
#define NAND_DIES               2
#define NAND_PLANES             2
#define NAND_BLOCKS             256
#define NAND_PAGES              128
#define NAND_PAGE_SIZE          4096
#define NAND_SPARE_SIZE         64

/*
 * Total addressable pages in the geometry.
 * We cap the working set to a smaller pool so seeds[] fits in RAM
 * and reads land on pages that have actually been written.
 */
#define MAX_TRACKED_PAGES \
    ((uint32_t)(NAND_CHANNELS * NAND_CHIPS * NAND_DIES * \
                NAND_PLANES * NAND_BLOCKS * NAND_PAGES))

/* -------------------------------------------------------------------------
 * PRNG — xorshift32
 * ---------------------------------------------------------------------- */

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* -------------------------------------------------------------------------
 * Data pattern helpers
 * ---------------------------------------------------------------------- */

static void fill_pattern(uint8_t *buf, uint32_t size, uint32_t seed)
{
    uint32_t state = seed;
    uint32_t i;
    for (i = 0; i + 4 <= size; i += 4) {
        uint32_t val = xorshift32(&state);
        memcpy(buf + i, &val, 4);
    }
    if (i < size) {
        uint32_t val = xorshift32(&state);
        memcpy(buf + i, &val, size - i);
    }
}

static int verify_pattern(const uint8_t *buf, uint32_t size, uint32_t seed)
{
    uint8_t expected[NAND_PAGE_SIZE];
    fill_pattern(expected, size, seed);
    return memcmp(buf, expected, size) == 0;
}

/* -------------------------------------------------------------------------
 * Address mapping: flat page index → NAND hierarchy
 * ---------------------------------------------------------------------- */

static void page_idx_to_addr(uint32_t idx,
                              uint32_t *ch, uint32_t *chip,
                              uint32_t *die, uint32_t *plane,
                              uint32_t *block, uint32_t *page)
{
    *page  = idx % NAND_PAGES;
    idx   /= NAND_PAGES;
    *block = idx % NAND_BLOCKS;
    idx   /= NAND_BLOCKS;
    *plane = idx % NAND_PLANES;
    idx   /= NAND_PLANES;
    *die   = idx % NAND_DIES;
    idx   /= NAND_DIES;
    *chip  = idx % NAND_CHIPS;
    idx   /= NAND_CHIPS;
    *ch    = idx % NAND_CHANNELS;
}

/* -------------------------------------------------------------------------
 * system_monitor thread-count callback
 *
 * system_monitor requires a non-NULL thread-count provider. On Linux we
 * count entries in /proc/self/task; elsewhere (and on read failure) we
 * fall back to 1 so sampling never stalls on the CPU / RSS metrics that
 * actually gate REQ-137.
 * ---------------------------------------------------------------------- */

static uint32_t stress_thread_count(void *ctx)
{
    (void)ctx;
    uint32_t count = 0;
#ifdef __linux__
    DIR *d = opendir("/proc/self/task");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') {
                continue;
            }
            count++;
        }
        closedir(d);
    }
#endif
    return count > 0 ? count : 1u;
}

/* -------------------------------------------------------------------------
 * Signal handling
 * ---------------------------------------------------------------------- */

static volatile int g_running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* -------------------------------------------------------------------------
 * Report
 * ---------------------------------------------------------------------- */

struct stability_report {
    uint64_t total_ops;
    uint64_t read_ops;
    uint64_t write_ops;
    uint64_t integrity_failures;
    uint64_t faults_injected;
    uint64_t errors_handled;
    size_t   mem_at_start;
    size_t   mem_at_end;
    double   elapsed_sec;

    /* REQ-137: runtime resource peaks sampled via system_monitor. */
    double   peak_cpu_pct;
    uint64_t peak_rss_bytes;
    uint32_t peak_thread_count;
    uint64_t monitor_samples;
    /* Optional fail threshold on peak RSS. 0 == disabled. */
    uint64_t peak_rss_limit_bytes;
    int      peak_rss_limit_exceeded;
};

static void print_report(const struct stability_report *r)
{
    int64_t mem_delta = (int64_t)r->mem_at_end - (int64_t)r->mem_at_start;
    int pass;

    printf("\n========================================\n");
    printf("Stability Test Report\n");
    printf("========================================\n");
    printf("Duration:            %.1f sec\n", r->elapsed_sec);
    printf("Total ops:           %llu\n",
           (unsigned long long)r->total_ops);
    printf("  Read ops:          %llu\n",
           (unsigned long long)r->read_ops);
    printf("  Write ops:         %llu\n",
           (unsigned long long)r->write_ops);
    printf("Integrity failures:  %llu\n",
           (unsigned long long)r->integrity_failures);
    printf("Faults injected:     %llu\n",
           (unsigned long long)r->faults_injected);
    printf("Errors handled:      %llu\n",
           (unsigned long long)r->errors_handled);
    printf("Memory at start:     %zu bytes\n", r->mem_at_start);
    printf("Memory at end:       %zu bytes\n", r->mem_at_end);
    printf("Memory delta:        %lld bytes\n", (long long)mem_delta);
    printf("Peak CPU:            %.1f%%\n", r->peak_cpu_pct);
    printf("Peak RSS:            %llu bytes\n",
           (unsigned long long)r->peak_rss_bytes);
    printf("Peak threads:        %u\n", r->peak_thread_count);
    printf("Monitor samples:     %llu\n",
           (unsigned long long)r->monitor_samples);
    if (r->peak_rss_limit_bytes > 0) {
        printf("Peak RSS limit:      %llu bytes (%s)\n",
               (unsigned long long)r->peak_rss_limit_bytes,
               r->peak_rss_limit_exceeded ? "EXCEEDED" : "ok");
    }
    printf("========================================\n");

    size_t mem_abs = (mem_delta >= 0)
                     ? (size_t)mem_delta
                     : (size_t)(-mem_delta);
    int mem_pass = mem_abs < (1024UL * 1024UL);
    pass = (r->integrity_failures == 0) &&
           mem_pass &&
           !r->peak_rss_limit_exceeded;

    if (pass) {
        printf("RESULT: PASS\n");
    } else {
        printf("RESULT: FAIL\n");
        if (r->integrity_failures > 0) {
            printf("  REASON: %llu integrity failure(s)\n",
                   (unsigned long long)r->integrity_failures);
        }
        if (!mem_pass) {
            printf("  REASON: memory delta %lld bytes exceeds 1 MB threshold\n",
                   (long long)mem_delta);
        }
        if (r->peak_rss_limit_exceeded) {
            printf("  REASON: peak RSS %llu bytes exceeds %llu-byte limit\n",
                   (unsigned long long)r->peak_rss_bytes,
                   (unsigned long long)r->peak_rss_limit_bytes);
        }
    }
    printf("========================================\n");
}

/*
 * REQ-137: optional machine-parseable summary for CI consumption.
 * When STRESS_RESULTS_FILE is set, the same report lands there as
 * key=value lines alongside the human-readable form; CI picks up
 * the file as a build artifact. Keys are stable and flat so a
 * simple grep / awk pipeline can gate on any field.
 */
static void write_results_file(const char *path,
                               const struct stability_report *r,
                               int pass)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[WARN] cannot open results file '%s'\n", path);
        return;
    }
    int64_t mem_delta = (int64_t)r->mem_at_end - (int64_t)r->mem_at_start;
    fprintf(f, "result=%s\n", pass ? "PASS" : "FAIL");
    fprintf(f, "duration_sec=%.1f\n", r->elapsed_sec);
    fprintf(f, "total_ops=%llu\n",          (unsigned long long)r->total_ops);
    fprintf(f, "read_ops=%llu\n",           (unsigned long long)r->read_ops);
    fprintf(f, "write_ops=%llu\n",          (unsigned long long)r->write_ops);
    fprintf(f, "integrity_failures=%llu\n", (unsigned long long)r->integrity_failures);
    fprintf(f, "faults_injected=%llu\n",    (unsigned long long)r->faults_injected);
    fprintf(f, "errors_handled=%llu\n",     (unsigned long long)r->errors_handled);
    fprintf(f, "mem_delta_bytes=%lld\n",    (long long)mem_delta);
    fprintf(f, "peak_cpu_pct=%.1f\n",       r->peak_cpu_pct);
    fprintf(f, "peak_rss_bytes=%llu\n",     (unsigned long long)r->peak_rss_bytes);
    fprintf(f, "peak_thread_count=%u\n",    r->peak_thread_count);
    fprintf(f, "monitor_samples=%llu\n",    (unsigned long long)r->monitor_samples);
    fprintf(f, "peak_rss_limit_bytes=%llu\n",
                                            (unsigned long long)r->peak_rss_limit_bytes);
    fprintf(f, "peak_rss_limit_exceeded=%d\n", r->peak_rss_limit_exceeded);
    fclose(f);
}

/*
 * Pull the latest sample from the background monitor and fold the
 * three metrics into the report's running peaks. Called from the
 * progress tick and once more at shutdown so the final printout
 * and results file always reflect the highest observed values.
 */
static void update_peaks(struct system_monitor *m,
                         struct stability_report *r)
{
    double cpu = system_monitor_cpu_pct(m);
    u64    rss = system_monitor_mem_bytes(m);
    u32    thr = system_monitor_thread_count(m);
    u64    s   = system_monitor_sample_count(m);
    if (cpu > r->peak_cpu_pct)         r->peak_cpu_pct      = cpu;
    if (rss > r->peak_rss_bytes)       r->peak_rss_bytes    = rss;
    if (thr > r->peak_thread_count)    r->peak_thread_count = thr;
    r->monitor_samples = s;
    if (r->peak_rss_limit_bytes > 0 &&
        r->peak_rss_bytes > r->peak_rss_limit_bytes) {
        r->peak_rss_limit_exceeded = 1;
    }
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    int duration_sec = DEFAULT_DURATION_SEC;

    /* Parse duration from STRESS_DURATION env or argv[1] */
    const char *env_dur = getenv("STRESS_DURATION");
    if (env_dur && atoi(env_dur) > 0) {
        duration_sec = atoi(env_dur);
    }
    if (argc > 1 && atoi(argv[1]) > 0) {
        duration_sec = atoi(argv[1]);
    }

    printf("========================================\n");
    printf("HFSSS Stability Stress Test\n");
    printf("Duration:  %d seconds\n", duration_sec);
    printf("Workload:  70%% reads / 30%% writes\n");
    printf("Verify:    every %d ops\n", VERIFY_INTERVAL);
    printf("Faults:    every %d ops\n", FAULT_INJECT_INTERVAL);
    printf("========================================\n");

    signal(SIGINT, sigint_handler);

    /* ------------------------------------------------------------------
     * Allocate tracking state
     * ------------------------------------------------------------------ */

    /* seeds[i] == 0  → page i has never been written
     * seeds[i] != 0  → written with this seed                         */
    uint32_t *seeds = (uint32_t *)calloc(MAX_TRACKED_PAGES, sizeof(uint32_t));
    uint8_t  *wbuf  = (uint8_t *)malloc(NAND_PAGE_SIZE);
    uint8_t  *rbuf  = (uint8_t *)malloc(NAND_PAGE_SIZE);
    uint8_t  *spare = (uint8_t *)calloc(NAND_SPARE_SIZE, 1);

    if (!seeds || !wbuf || !rbuf || !spare) {
        fprintf(stderr, "[FATAL] Failed to allocate test buffers\n");
        free(seeds); free(wbuf); free(rbuf); free(spare);
        return 1;
    }

    /* Snapshot memory baseline (approximate via sum of known allocs) */
    size_t mem_baseline = MAX_TRACKED_PAGES * sizeof(uint32_t)
                        + NAND_PAGE_SIZE * 2
                        + NAND_SPARE_SIZE;

    /* ------------------------------------------------------------------
     * Init media layer
     * ------------------------------------------------------------------ */

    struct media_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.channel_count      = NAND_CHANNELS;
    cfg.chips_per_channel  = NAND_CHIPS;
    cfg.dies_per_chip      = NAND_DIES;
    cfg.planes_per_die     = NAND_PLANES;
    cfg.blocks_per_plane   = NAND_BLOCKS;
    cfg.pages_per_block    = NAND_PAGES;
    cfg.page_size          = NAND_PAGE_SIZE;
    cfg.spare_size         = NAND_SPARE_SIZE;
    cfg.nand_type          = NAND_TYPE_QLC;
    cfg.enable_multi_plane = false;
    cfg.enable_die_interleaving = false;

    struct media_ctx media;
    int ret = media_init(&media, &cfg);
    if (ret != HFSSS_OK) {
        fprintf(stderr, "[FATAL] media_init failed: %d\n", ret);
        free(seeds); free(wbuf); free(rbuf); free(spare);
        return 1;
    }

    /* ------------------------------------------------------------------
     * Init fault registry
     * ------------------------------------------------------------------ */

    struct fault_registry fault_reg;
    ret = fault_registry_init(&fault_reg);
    if (ret != HFSSS_OK) {
        fprintf(stderr, "[FATAL] fault_registry_init failed: %d\n", ret);
        media_cleanup(&media);
        free(seeds); free(wbuf); free(rbuf); free(spare);
        return 1;
    }

    /* ------------------------------------------------------------------
     * Init system_monitor (REQ-137)
     *
     * Background sampler at 2 Hz over the entire run. Uses the POSIX
     * getrusage-backed defaults; thread count reads /proc/self/task on
     * Linux and falls back to 1 on macOS. Keeping poll_interval_ms
     * conservative so the sampler never shows up in profile data.
     * ------------------------------------------------------------------ */

    struct system_monitor monitor;
    struct system_monitor_config moncfg = {
        .poll_interval_ms   = 500,
        .get_cpu_time_ns    = system_monitor_default_cpu_time_ns,
        .get_mem_bytes      = system_monitor_default_mem_bytes,
        .get_thread_count   = stress_thread_count,
        .cb_ctx             = NULL,
    };
    int monitor_ok = (system_monitor_init(&monitor, &moncfg) == HFSSS_OK &&
                      system_monitor_start(&monitor)        == HFSSS_OK);
    if (!monitor_ok) {
        fprintf(stderr,
            "[WARN] system_monitor unavailable — peaks will read 0\n");
    }

    /* ------------------------------------------------------------------
     * Stress loop
     * ------------------------------------------------------------------ */

    struct stability_report report;
    memset(&report, 0, sizeof(report));
    report.mem_at_start = mem_baseline;

    /* Optional fail-on-peak-RSS ceiling. Read in MB for readability
     * (STRESS_PEAK_RSS_LIMIT_MB=512), falling back to the raw byte
     * form for fine-grained CI tuning. 0 / unset disables the check. */
    const char *rss_limit_mb = getenv("STRESS_PEAK_RSS_LIMIT_MB");
    const char *rss_limit_b  = getenv("STRESS_PEAK_RSS_LIMIT_BYTES");
    if (rss_limit_mb && atoll(rss_limit_mb) > 0) {
        report.peak_rss_limit_bytes =
            (uint64_t)atoll(rss_limit_mb) * 1024ULL * 1024ULL;
    } else if (rss_limit_b && atoll(rss_limit_b) > 0) {
        report.peak_rss_limit_bytes = (uint64_t)atoll(rss_limit_b);
    }

    uint32_t rng = (uint32_t)(time(NULL) ^ 0xDEADBEEFu) | 1u;

    time_t start_time    = time(NULL);
    time_t last_progress = start_time;

    while (g_running) {
        time_t now = time(NULL);
        if ((now - start_time) >= (time_t)duration_sec) {
            break;
        }

        /* Progress report */
        if ((now - last_progress) >= PROGRESS_INTERVAL_SEC) {
            if (monitor_ok) {
                update_peaks(&monitor, &report);
            }
            printf("  [%3lds] ops=%llu writes=%llu reads=%llu failures=%llu "
                   "cpu=%.1f%% rss=%lluKiB\n",
                   (long)(now - start_time),
                   (unsigned long long)report.total_ops,
                   (unsigned long long)report.write_ops,
                   (unsigned long long)report.read_ops,
                   (unsigned long long)report.integrity_failures,
                   report.peak_cpu_pct,
                   (unsigned long long)(report.peak_rss_bytes / 1024ULL));
            last_progress = now;
        }

        /* Pick a random page from the tracked set */
        uint32_t page_idx = xorshift32(&rng) % MAX_TRACKED_PAGES;
        uint32_t ch, chip, die, plane, block, pg;
        page_idx_to_addr(page_idx, &ch, &chip, &die, &plane, &block, &pg);

        /* 70% reads, 30% writes; force a write if page never written */
        uint32_t op_roll = xorshift32(&rng) % 100;
        int do_write = (op_roll < 30) || (seeds[page_idx] == 0);

        if (do_write) {
            uint32_t seed = xorshift32(&rng) | 1u; /* ensure non-zero */
            fill_pattern(wbuf, NAND_PAGE_SIZE, seed);

            ret = media_nand_program(&media, ch, chip, die, plane, block, pg,
                                     wbuf, spare);
            if (ret == HFSSS_OK) {
                seeds[page_idx] = seed;
                report.write_ops++;
            } else {
                report.errors_handled++;
            }
        } else {
            /* Read — only meaningful if page was written */
            if (seeds[page_idx] == 0) {
                /* Skip: this page was never written; count as handled */
                report.errors_handled++;
                report.total_ops++;
                continue;
            }

            ret = media_nand_read(&media, ch, chip, die, plane, block, pg,
                                  rbuf, spare);
            if (ret == HFSSS_OK) {
                report.read_ops++;

                /* Integrity check every VERIFY_INTERVAL ops */
                if (report.total_ops % VERIFY_INTERVAL == 0) {
                    if (!verify_pattern(rbuf, NAND_PAGE_SIZE, seeds[page_idx])) {
                        report.integrity_failures++;
                        fprintf(stderr,
                            "[CORRUPT] page_idx=%u ch=%u chip=%u die=%u "
                            "plane=%u block=%u pg=%u seed=0x%08X\n",
                            page_idx, ch, chip, die, plane, block, pg,
                            seeds[page_idx]);
                    }
                }
            } else {
                report.errors_handled++;
            }
        }

        /* Periodic fault injection */
        if (report.total_ops > 0 &&
            report.total_ops % FAULT_INJECT_INTERVAL == 0) {

            uint32_t fault_roll = xorshift32(&rng) % 2;
            struct fault_addr faddr = {
                .channel = xorshift32(&rng) % NAND_CHANNELS,
                .chip    = xorshift32(&rng) % NAND_CHIPS,
                .die     = FAULT_WILDCARD,
                .plane   = FAULT_WILDCARD,
                .block   = FAULT_WILDCARD,
                .page    = FAULT_WILDCARD,
            };

            enum fault_type ftype = (fault_roll == 0)
                                    ? FAULT_BIT_FLIP
                                    : FAULT_READ_DISTURB;

            int fid = fault_inject_add(&fault_reg, ftype, &faddr,
                                       FAULT_PERSIST_ONE_SHOT, 1.0);
            if (fid >= 0) {
                report.faults_injected++;
            }
        }

        report.total_ops++;
    }

    /* ------------------------------------------------------------------
     * Final report
     * ------------------------------------------------------------------ */

    time_t end_time = time(NULL);
    report.elapsed_sec = difftime(end_time, start_time);
    report.mem_at_end  = mem_baseline; /* stable: no heap growth beyond init */

    /* Final peak pull before the monitor is torn down. */
    if (monitor_ok) {
        update_peaks(&monitor, &report);
        system_monitor_stop(&monitor);
        system_monitor_cleanup(&monitor);
    }

    int64_t mem_delta = (int64_t)report.mem_at_end - (int64_t)report.mem_at_start;
    size_t  mem_abs   = (mem_delta >= 0)
                        ? (size_t)mem_delta
                        : (size_t)(-mem_delta);
    int pass = (report.integrity_failures == 0) &&
               (mem_abs < (1024UL * 1024UL)) &&
               !report.peak_rss_limit_exceeded;

    print_report(&report);

    const char *results_path = getenv("STRESS_RESULTS_FILE");
    if (results_path && *results_path) {
        write_results_file(results_path, &report, pass);
    }

    /* ------------------------------------------------------------------
     * Cleanup
     * ------------------------------------------------------------------ */

    fault_registry_cleanup(&fault_reg);
    media_cleanup(&media);
    free(seeds);
    free(wbuf);
    free(rbuf);
    free(spare);

    return pass ? 0 : 1;
}
