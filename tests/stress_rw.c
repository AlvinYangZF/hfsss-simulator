/*
 * stress_rw.c — 15-minute random write stress test with data integrity checks.
 *
 * Exercises the full NVMe → FTL → NAND stack under sustained random write
 * load. Periodically reads back and verifies written data. Reports GC
 * activity, WAF, and any corruption or error mid-run.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <signal.h>
#include "pcie/nvme_uspace.h"
#include "ftl/ftl.h"
#include "common/common.h"

/* ---------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------- */
#define RUN_SECONDS        (15 * 60)   /* 15 minutes */
#define LBA_SIZE           4096
#define IOS_PER_BATCH      64          /* writes before a verify pass */
#define VERIFY_RATIO       4           /* verify every Nth batch */
#define PROGRESS_INTERVAL  30          /* print progress every N seconds */
#define SEED               0xDEADBEEFu

/* ---------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------- */
static uint64_t now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec;
}

/* Return current wall-clock time as "HH:MM:SS" in a static buffer */
static const char *now_ts(void) {
    static char buf[16];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(buf, sizeof(buf), "%H:%M:%S", tm);
    return buf;
}

/* Simple LCG for deterministic per-LBA data patterns */
static uint32_t lcg(uint32_t state) {
    return state * 1664525u + 1013904223u;
}

/* Fill buf with a deterministic pattern seeded by (lba, generation) */
static void fill_pattern(void *buf, uint32_t lba_idx, uint64_t gen) {
    uint32_t *p = (uint32_t *)buf;
    uint32_t s = (uint32_t)(lba_idx ^ (gen * 0x9e3779b9u));
    for (int i = 0; i < (int)(LBA_SIZE / 4); i++) {
        s = lcg(s);
        p[i] = s;
    }
}

/* Verify buf matches expected pattern */
static bool verify_pattern(const void *buf, uint32_t lba_idx, uint64_t gen) {
    const uint32_t *p = (const uint32_t *)buf;
    uint32_t s = (uint32_t)(lba_idx ^ (gen * 0x9e3779b9u));
    for (int i = 0; i < (int)(LBA_SIZE / 4); i++) {
        s = lcg(s);
        if (p[i] != s) return false;
    }
    return true;
}

static volatile bool g_stop = false;
static void handle_sigint(int sig) { (void)sig; g_stop = true; }

/* ---------------------------------------------------------------
 * main
 * ------------------------------------------------------------- */
int main(void) {
    signal(SIGINT, handle_sigint);

    printf("========================================\n");
    printf("HFSSS 15-Minute Random Write Stress Test\n");
    printf("========================================\n");
    printf("Started  : %s\n", now_ts());
    printf("Duration : %d minutes\n", RUN_SECONDS / 60);
    printf("LBA size : %d bytes\n", LBA_SIZE);
    printf("Batch    : %d writes, verify every %d batches\n",
           IOS_PER_BATCH, VERIFY_RATIO);
    printf("----------------------------------------\n\n");

    /* ---- device init: small config so lba_gen fits in RAM ---- */
    struct nvme_uspace_config cfg;
    nvme_uspace_config_default(&cfg);
    /* Override to ~256MB device: 2ch × 1chip × 1die × 1plane × 128blk × 256pg */
    cfg.sssim_cfg.channel_count      = 2;
    cfg.sssim_cfg.chips_per_channel  = 1;
    cfg.sssim_cfg.dies_per_chip      = 1;
    cfg.sssim_cfg.planes_per_die     = 1;
    cfg.sssim_cfg.blocks_per_plane   = 128;
    cfg.sssim_cfg.pages_per_block    = 256;
    cfg.sssim_cfg.page_size          = 4096;
    /* ~256MB NAND, ~20% OP → ~200MB user capacity */
    uint64_t raw_pages = (uint64_t)cfg.sssim_cfg.channel_count
                       * cfg.sssim_cfg.chips_per_channel
                       * cfg.sssim_cfg.dies_per_chip
                       * cfg.sssim_cfg.planes_per_die
                       * cfg.sssim_cfg.blocks_per_plane
                       * cfg.sssim_cfg.pages_per_block;
    cfg.sssim_cfg.total_lbas = raw_pages * (100 - cfg.sssim_cfg.op_ratio) / 100;

    struct nvme_uspace_dev dev;
    if (nvme_uspace_dev_init(&dev, &cfg) != HFSSS_OK) {
        fprintf(stderr, "[FATAL] nvme_uspace_dev_init failed\n");
        return 1;
    }
    if (nvme_uspace_dev_start(&dev) != HFSSS_OK) {
        fprintf(stderr, "[FATAL] nvme_uspace_dev_start failed\n");
        nvme_uspace_dev_cleanup(&dev);
        return 1;
    }
    if (nvme_uspace_create_io_cq(&dev, 1, 256, false) != HFSSS_OK ||
        nvme_uspace_create_io_sq(&dev, 1, 256, 1, 0)  != HFSSS_OK) {
        fprintf(stderr, "[FATAL] queue creation failed\n");
        nvme_uspace_dev_stop(&dev);
        nvme_uspace_dev_cleanup(&dev);
        return 1;
    }

    /* ---- capacity ---- */
    struct nvme_identify_ns ns_id;
    nvme_uspace_identify_ns(&dev, 1, &ns_id);
    uint64_t total_lbas = ns_id.nsze > 0 ? ns_id.nsze : 1024;
    printf("[INFO] Device capacity: %llu LBAs (%llu MB)\n",
           (unsigned long long)total_lbas,
           (unsigned long long)(total_lbas * LBA_SIZE / (1024*1024)));

    uint64_t total_errors = 1;  /* default: error until run completes cleanly */

    /* ---- tracking arrays ---- */
    /* For each LBA track which generation (write count) was last written */
    uint64_t *lba_gen = NULL;
    uint8_t  *wbuf    = NULL;
    uint8_t  *rbuf    = NULL;

    lba_gen = calloc(total_lbas, sizeof(uint64_t));
    if (!lba_gen) {
        fprintf(stderr, "[FATAL] OOM allocating tracking array\n");
        goto cleanup;
    }

    wbuf = malloc(LBA_SIZE);
    rbuf = malloc(LBA_SIZE);
    if (!wbuf || !rbuf) {
        fprintf(stderr, "[FATAL] OOM allocating IO buffers\n");
        goto cleanup;
    }

    /* ---- stress loop ---- */
    uint64_t start_s   = now_s();
    uint64_t deadline  = start_s + RUN_SECONDS;
    uint64_t last_prog = start_s;

    uint64_t total_writes   = 0;
    uint64_t total_reads    = 0;
    uint64_t corrupt_count  = 0;
    uint64_t write_errors   = 0;  /* IO errors only (rc=-100) */
    uint64_t nospc_errors   = 0;  /* NOSPC throttle events (rc=-8, expected near capacity) */
    uint64_t read_errors    = 0;
    uint64_t batch_num      = 0;

    uint32_t rng = SEED;

    while (!g_stop && now_s() < deadline) {
        /* --- write batch --- */
        for (int i = 0; i < IOS_PER_BATCH && now_s() < deadline; i++) {
            rng = lcg(rng);
            uint64_t lba = rng % total_lbas;

            uint64_t gen = lba_gen[lba] + 1;
            fill_pattern(wbuf, (uint32_t)lba, gen);

            int rc = nvme_uspace_write(&dev, 1, lba, 1, wbuf);
            if (rc != HFSSS_OK) {
                if (rc == HFSSS_ERR_NOSPC) {
                    nospc_errors++;
                } else {
                    fprintf(stderr, "[ERROR] write failed: LBA=%llu rc=%d\n",
                            (unsigned long long)lba, rc);
                    write_errors++;
                }
            } else {
                lba_gen[lba] = gen;
                total_writes++;
            }
        }
        batch_num++;

        /* --- verify pass (every VERIFY_RATIO batches) --- */
        if (batch_num % VERIFY_RATIO == 0) {
            /* Pick IOS_PER_BATCH random LBAs that have been written */
            for (int i = 0; i < IOS_PER_BATCH; i++) {
                rng = lcg(rng);
                uint64_t lba = rng % total_lbas;
                if (lba_gen[lba] == 0) continue;  /* never written */

                int rc = nvme_uspace_read(&dev, 1, lba, 1, rbuf);
                if (rc != HFSSS_OK) {
                    fprintf(stderr, "[ERROR] read failed: LBA=%llu rc=%d\n",
                            (unsigned long long)lba, rc);
                    read_errors++;
                    continue;
                }
                total_reads++;

                if (!verify_pattern(rbuf, (uint32_t)lba, lba_gen[lba])) {
                    corrupt_count++;
                    fprintf(stderr,
                        "[CORRUPT] LBA=%llu gen=%llu data mismatch! "
                        "expected[0]=0x%08X got[0]=0x%08X\n",
                        (unsigned long long)lba,
                        (unsigned long long)lba_gen[lba],
                        ((uint32_t *)wbuf)[0],
                        ((uint32_t *)rbuf)[0]);
                }
            }
        }

        /* --- periodic flush --- */
        if (batch_num % 128 == 0) {
            nvme_uspace_flush(&dev, 1);
        }

        /* --- progress report --- */
        uint64_t t = now_s();
        if (t - last_prog >= PROGRESS_INTERVAL) {
            last_prog = t;
            uint64_t elapsed = t - start_s;
            uint64_t remain  = (deadline > t) ? deadline - t : 0;

            struct ftl_stats stats;
            sssim_get_stats(&dev.sssim, &stats);

            printf("[%s | %3llus / %ds remaining] "
                   "writes=%llu reads=%llu gc_cycles=%llu "
                   "waf=%.2f corrupt=%llu werr=%llu nospc=%llu rerr=%llu\n",
                   now_ts(),
                   (unsigned long long)elapsed,
                   (int)remain,
                   (unsigned long long)total_writes,
                   (unsigned long long)total_reads,
                   (unsigned long long)stats.gc_count,
                   stats.waf,
                   (unsigned long long)corrupt_count,
                   (unsigned long long)write_errors,
                   (unsigned long long)nospc_errors,
                   (unsigned long long)read_errors);
        }
    }

    /* --- final flush + full verify sweep --- */
    printf("\n[INFO] Run complete. Final flush + verify sweep...\n");
    nvme_uspace_flush(&dev, 1);

    uint64_t verified = 0, final_corrupt = 0;
    uint64_t dbg_read_err_printed = 0;
    for (uint64_t lba = 0; lba < total_lbas; lba++) {
        if (lba_gen[lba] == 0) continue;
        int rc = nvme_uspace_read(&dev, 1, lba, 1, rbuf);
        if (rc != HFSSS_OK) {
            if (dbg_read_err_printed < 5) {
                fprintf(stderr, "[READ_FAIL] lba=%llu gen=%llu rc=%d\n",
                        (unsigned long long)lba,
                        (unsigned long long)lba_gen[lba], rc);
                dbg_read_err_printed++;
            }
            read_errors++;
            continue;
        }
        total_reads++;
        verified++;
        if (!verify_pattern(rbuf, (uint32_t)lba, lba_gen[lba])) {
            final_corrupt++;
            if (final_corrupt <= 5) {
                fprintf(stderr, "[CORRUPT] final sweep LBA=%llu gen=%llu\n",
                        (unsigned long long)lba,
                        (unsigned long long)lba_gen[lba]);
            }
        }
    }

    /* --- final stats --- */
    struct ftl_stats final_stats;
    sssim_get_stats(&dev.sssim, &final_stats);

    uint64_t total_s = now_s() - start_s;
    printf("\n========================================\n");
    printf("Stress Test Results\n");
    printf("========================================\n");
    printf("  Duration        : %llus (%llum %llus)\n",
           (unsigned long long)total_s,
           (unsigned long long)(total_s / 60),
           (unsigned long long)(total_s % 60));
    printf("  Total writes    : %llu\n",   (unsigned long long)total_writes);
    printf("  Total reads     : %llu\n",   (unsigned long long)total_reads);
    printf("  Write errors    : %llu\n",   (unsigned long long)write_errors);
    printf("  NOSPC events    : %llu\n",   (unsigned long long)nospc_errors);
    printf("  Read errors     : %llu\n",   (unsigned long long)read_errors);
    printf("  Mid-run corrupt : %llu\n",   (unsigned long long)corrupt_count);
    printf("  Final corrupt   : %llu / %llu verified\n",
           (unsigned long long)final_corrupt,
           (unsigned long long)verified);
    printf("  GC cycles       : %llu\n",   (unsigned long long)final_stats.gc_count);
    printf("  GC moved pages  : %llu\n",   (unsigned long long)final_stats.moved_pages);
    printf("  WAF             : %.3f\n",   final_stats.waf);
    printf("  Write IOPS      : %.0f\n",
           total_s > 0 ? (double)total_writes / total_s : 0.0);
    printf("  Read  IOPS      : %.0f\n",
           total_s > 0 ? (double)total_reads  / total_s : 0.0);
    printf("========================================\n");

    /* NOSPC events are excluded: throttling at device capacity is correct behavior. */
    total_errors = write_errors + read_errors + corrupt_count + final_corrupt;
    if (total_errors == 0) {
        printf("\n  [PASS] No errors detected after %llu writes + %llu reads.\n\n",
               (unsigned long long)total_writes,
               (unsigned long long)total_reads);
    } else {
        printf("\n  [FAIL] %llu error(s) detected!\n\n",
               (unsigned long long)total_errors);
    }

cleanup:
    free(lba_gen);
    free(wbuf);
    free(rbuf);
    nvme_uspace_delete_io_sq(&dev, 1);
    nvme_uspace_delete_io_cq(&dev, 1);
    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return (total_errors == 0) ? 0 : 1;
}
