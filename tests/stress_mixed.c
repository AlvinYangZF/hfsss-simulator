/*
 * stress_mixed.c — 10-minute mixed read/write stress test with data integrity checks.
 *
 * Issues a random mix of reads and writes (configurable ratio) against the full
 * NVMe → FTL → NAND stack.  Reads target only LBAs that have already been
 * written at least once.  Every read is verified against the expected pattern.
 * Reports GC activity, WAF, IOPS, and any corruption or error mid-run.
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
#define RUN_SECONDS        (10 * 60)   /* 10 minutes */
#define LBA_SIZE           4096
#define READ_WRITE_RATIO   50          /* % of IOs that are reads (0-100) */
#define IOS_PER_BATCH      64
#define PROGRESS_INTERVAL  30          /* print progress every N seconds */
#define SEED               0xCAFEBABEu

/* ---------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------- */
static uint64_t now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec;
}

static const char *now_ts(void) {
    static char buf[16];
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    strftime(buf, sizeof(buf), "%H:%M:%S", tm_info);
    return buf;
}

static uint32_t lcg(uint32_t state) {
    return state * 1664525u + 1013904223u;
}

static void fill_pattern(void *buf, uint32_t lba_idx, uint64_t gen) {
    uint32_t *p = (uint32_t *)buf;
    uint32_t s = (uint32_t)(lba_idx ^ (gen * 0x9e3779b9u));
    for (int i = 0; i < (int)(LBA_SIZE / 4); i++) {
        s = lcg(s);
        p[i] = s;
    }
}

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
    printf("HFSSS 10-Minute Mixed Read/Write Stress Test\n");
    printf("========================================\n");
    printf("Started  : %s\n", now_ts());
    printf("Duration : %d minutes\n", RUN_SECONDS / 60);
    printf("Mix      : %d%% reads / %d%% writes\n",
           READ_WRITE_RATIO, 100 - READ_WRITE_RATIO);
    printf("LBA size : %d bytes\n", LBA_SIZE);
    printf("Batch    : %d IOs\n", IOS_PER_BATCH);
    printf("----------------------------------------\n\n");

    /* ---- device init: same geometry as stress_rw ---- */
    struct nvme_uspace_config cfg;
    nvme_uspace_config_default(&cfg);
    cfg.sssim_cfg.channel_count      = 2;
    cfg.sssim_cfg.chips_per_channel  = 1;
    cfg.sssim_cfg.dies_per_chip      = 1;
    cfg.sssim_cfg.planes_per_die     = 1;
    cfg.sssim_cfg.blocks_per_plane   = 128;
    cfg.sssim_cfg.pages_per_block    = 256;
    cfg.sssim_cfg.page_size          = 4096;
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

    struct nvme_identify_ns ns_id;
    nvme_uspace_identify_ns(&dev, 1, &ns_id);
    uint64_t total_lbas = ns_id.nsze > 0 ? ns_id.nsze : 1024;
    printf("[INFO] Device capacity: %llu LBAs (%llu MB)\n",
           (unsigned long long)total_lbas,
           (unsigned long long)(total_lbas * LBA_SIZE / (1024*1024)));

    uint64_t total_errors = 1;

    /* ---- tracking arrays ---- */
    uint64_t *lba_gen = calloc(total_lbas, sizeof(uint64_t));
    uint8_t  *wbuf    = malloc(LBA_SIZE);
    uint8_t  *rbuf    = malloc(LBA_SIZE);

    if (!lba_gen || !wbuf || !rbuf) {
        fprintf(stderr, "[FATAL] OOM allocating buffers\n");
        goto cleanup;
    }

    /* ---- stress loop ---- */
    uint64_t start_s   = now_s();
    uint64_t deadline  = start_s + RUN_SECONDS;
    uint64_t last_prog = start_s;

    uint64_t total_writes  = 0;
    uint64_t total_reads   = 0;
    uint64_t write_errors  = 0;   /* IO errors only */
    uint64_t nospc_errors  = 0;   /* expected throttle events */
    uint64_t read_errors   = 0;
    uint64_t corrupt_count = 0;
    uint64_t written_lbas  = 0;   /* number of LBAs written at least once */
    uint64_t batch_num     = 0;

    uint32_t rng = SEED;

    /*
     * Warm-up: write every LBA once so reads always have valid data.
     * Use a sequential pass to keep WAF low during the fill phase.
     */
    printf("[INFO] Warm-up: writing all %llu LBAs...\n",
           (unsigned long long)total_lbas);
    for (uint64_t lba = 0; lba < total_lbas && !g_stop; lba++) {
        uint64_t gen = 1;
        fill_pattern(wbuf, (uint32_t)lba, gen);
        int rc = nvme_uspace_write(&dev, 1, lba, 1, wbuf);
        if (rc == HFSSS_OK) {
            lba_gen[lba] = gen;
            total_writes++;
            written_lbas++;
        } else if (rc == HFSSS_ERR_NOSPC) {
            nospc_errors++;
        } else {
            write_errors++;
        }
    }
    nvme_uspace_flush(&dev, 1);
    printf("[INFO] Warm-up complete: %llu LBAs written, %llu NOSPC, %llu errors\n",
           (unsigned long long)total_writes,
           (unsigned long long)nospc_errors,
           (unsigned long long)write_errors);

    /* Reset start/deadline to measure the active mixed phase only. */
    start_s   = now_s();
    deadline  = start_s + RUN_SECONDS;
    last_prog = start_s;

    while (!g_stop && now_s() < deadline) {
        for (int i = 0; i < IOS_PER_BATCH && now_s() < deadline; i++) {
            rng = lcg(rng);
            uint64_t lba = rng % total_lbas;

            /* Decide read or write based on ratio and whether LBA is written. */
            rng = lcg(rng);
            bool do_read = (written_lbas > 0) &&
                           ((rng % 100) < (uint32_t)READ_WRITE_RATIO) &&
                           (lba_gen[lba] > 0);

            if (do_read) {
                int rc = nvme_uspace_read(&dev, 1, lba, 1, rbuf);
                if (rc != HFSSS_OK) {
                    fprintf(stderr, "[ERROR] read failed: LBA=%llu rc=%d\n",
                            (unsigned long long)lba, rc);
                    read_errors++;
                } else {
                    total_reads++;
                    if (!verify_pattern(rbuf, (uint32_t)lba, lba_gen[lba])) {
                        corrupt_count++;
                        fprintf(stderr,
                            "[CORRUPT] LBA=%llu gen=%llu "
                            "expected[0]=0x%08X got[0]=0x%08X\n",
                            (unsigned long long)lba,
                            (unsigned long long)lba_gen[lba],
                            ((uint32_t *)wbuf)[0],
                            ((uint32_t *)rbuf)[0]);
                    }
                }
            } else {
                uint64_t gen = lba_gen[lba] + 1;
                fill_pattern(wbuf, (uint32_t)lba, gen);
                int rc = nvme_uspace_write(&dev, 1, lba, 1, wbuf);
                if (rc == HFSSS_OK) {
                    lba_gen[lba] = gen;
                    total_writes++;
                    if (gen == 1) written_lbas++;
                } else if (rc == HFSSS_ERR_NOSPC) {
                    nospc_errors++;
                } else {
                    fprintf(stderr, "[ERROR] write failed: LBA=%llu rc=%d\n",
                            (unsigned long long)lba, rc);
                    write_errors++;
                }
            }
        }
        batch_num++;

        /* Periodic flush */
        if (batch_num % 128 == 0) {
            nvme_uspace_flush(&dev, 1);
        }

        /* Progress report */
        uint64_t t = now_s();
        if (t - last_prog >= PROGRESS_INTERVAL) {
            last_prog = t;
            uint64_t elapsed = t - start_s;
            uint64_t remain  = (deadline > t) ? deadline - t : 0;

            struct ftl_stats stats;
            sssim_get_stats(&dev.sssim, &stats);

            printf("[%s | %3llus / %ds remaining] "
                   "wr=%llu rd=%llu gc=%llu "
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

    /* ---- final flush + full verify sweep ---- */
    printf("\n[INFO] Run complete. Final flush + verify sweep...\n");
    nvme_uspace_flush(&dev, 1);

    uint64_t verified = 0, final_corrupt = 0;
    uint64_t dbg_rerr_printed = 0;
    for (uint64_t lba = 0; lba < total_lbas; lba++) {
        if (lba_gen[lba] == 0) continue;
        int rc = nvme_uspace_read(&dev, 1, lba, 1, rbuf);
        if (rc != HFSSS_OK) {
            if (dbg_rerr_printed < 5) {
                fprintf(stderr, "[READ_FAIL] lba=%llu gen=%llu rc=%d\n",
                        (unsigned long long)lba,
                        (unsigned long long)lba_gen[lba], rc);
                dbg_rerr_printed++;
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

    /* ---- final stats ---- */
    struct ftl_stats final_stats;
    sssim_get_stats(&dev.sssim, &final_stats);

    uint64_t total_s = now_s() - start_s;
    uint64_t total_ios = total_writes + total_reads;

    printf("\n========================================\n");
    printf("Mixed Stress Test Results\n");
    printf("========================================\n");
    printf("  Duration        : %llus (%llum %llus)\n",
           (unsigned long long)total_s,
           (unsigned long long)(total_s / 60),
           (unsigned long long)(total_s % 60));
    printf("  Total writes    : %llu\n",   (unsigned long long)total_writes);
    printf("  Total reads     : %llu\n",   (unsigned long long)total_reads);
    printf("  Effective mix   : %llu%% reads\n",
           total_ios > 0 ? (unsigned long long)(total_reads * 100 / total_ios) : 0ULL);
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

    /* NOSPC excluded: throttle behavior at capacity is correct. */
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
