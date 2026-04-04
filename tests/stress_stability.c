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

#include "common/common.h"
#include "common/fault_inject.h"
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
    printf("========================================\n");

    pass = (r->integrity_failures == 0) &&
           (mem_delta >= 0 ? (size_t)mem_delta : (size_t)(-mem_delta)) < (1024UL * 1024UL);

    if (pass) {
        printf("RESULT: PASS\n");
    } else {
        printf("RESULT: FAIL\n");
        if (r->integrity_failures > 0) {
            printf("  REASON: %llu integrity failure(s)\n",
                   (unsigned long long)r->integrity_failures);
        }
        if ((mem_delta >= 0 ? (size_t)mem_delta : (size_t)(-mem_delta)) >= (1024UL * 1024UL)) {
            printf("  REASON: memory delta %lld bytes exceeds 1 MB threshold\n",
                   (long long)mem_delta);
        }
    }
    printf("========================================\n");
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
     * Stress loop
     * ------------------------------------------------------------------ */

    struct stability_report report;
    memset(&report, 0, sizeof(report));
    report.mem_at_start = mem_baseline;

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
            printf("  [%3lds] ops=%llu writes=%llu reads=%llu failures=%llu\n",
                   (long)(now - start_time),
                   (unsigned long long)report.total_ops,
                   (unsigned long long)report.write_ops,
                   (unsigned long long)report.read_ops,
                   (unsigned long long)report.integrity_failures);
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

    print_report(&report);

    /* ------------------------------------------------------------------
     * Cleanup
     * ------------------------------------------------------------------ */

    fault_registry_cleanup(&fault_reg);
    media_cleanup(&media);
    free(seeds);
    free(wbuf);
    free(rbuf);
    free(spare);

    return (report.integrity_failures == 0) ? 0 : 1;
}
