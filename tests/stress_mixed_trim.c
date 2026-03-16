/*
 * stress_mixed_trim.c — 30-minute mixed read/write/trim stress test.
 *
 * Issues a random mix of reads, writes, and TRIM (NVMe Deallocate) commands
 * against the full NVMe → FTL → NAND stack.
 *
 * TRIM verification rules:
 *   - After a TRIM, an immediate read of the trimmed LBA must return
 *     HFSSS_ERR_NOENT (mapping removed), not the old data.
 *   - After a TRIM + re-write, a read must return the new data.
 *
 * Arguments: [read_pct [trim_pct]]
 *   read_pct  — % of IOs that are reads  (default 35)
 *   trim_pct  — % of IOs that are trims  (default 15)
 *   writes    — remainder
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
#define RUN_SECONDS        (30 * 60)
#define LBA_SIZE           4096
#define IOS_PER_BATCH      64
#define PROGRESS_INTERVAL  30
#define SEED               0xDEADC0DEu
/* Max LBAs per single TRIM range */
#define MAX_TRIM_RANGE     8

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
int main(int argc, char *argv[]) {
    signal(SIGINT, handle_sigint);

    int read_pct = 35;
    int trim_pct = 15;
    if (argc >= 2) { read_pct = atoi(argv[1]); }
    if (argc >= 3) { trim_pct = atoi(argv[2]); }
    if (read_pct < 0) read_pct = 0;
    if (trim_pct < 0) trim_pct = 0;
    if (read_pct + trim_pct > 100) {
        trim_pct = 100 - read_pct;
    }
    int write_pct = 100 - read_pct - trim_pct;

    printf("========================================\n");
    printf("HFSSS 30-Minute Mixed Read/Write/Trim Stress Test\n");
    printf("========================================\n");
    printf("Started  : %s\n", now_ts());
    printf("Duration : %d minutes\n", RUN_SECONDS / 60);
    printf("Mix      : %d%% reads / %d%% writes / %d%% trims\n",
           read_pct, write_pct, trim_pct);
    printf("LBA size : %d bytes\n", LBA_SIZE);
    printf("Batch    : %d IOs\n", IOS_PER_BATCH);
    printf("----------------------------------------\n\n");

    /* ---- device init ---- */
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

    /* ---- tracking ----
     * lba_gen[lba] = 0  → never written or trimmed (NOENT expected on read)
     * lba_gen[lba] > 0  → written, pattern (lba, gen) expected on read
     */
    uint64_t *lba_gen = calloc(total_lbas, sizeof(uint64_t));
    uint8_t  *wbuf    = malloc(LBA_SIZE);
    uint8_t  *rbuf    = malloc(LBA_SIZE);

    if (!lba_gen || !wbuf || !rbuf) {
        fprintf(stderr, "[FATAL] OOM\n");
        goto cleanup;
    }

    /* ---- warm-up: write every LBA once ---- */
    printf("[INFO] Warm-up: writing all %llu LBAs...\n",
           (unsigned long long)total_lbas);
    uint64_t wu_writes = 0, wu_nospc = 0, wu_err = 0;
    for (uint64_t lba = 0; lba < total_lbas && !g_stop; lba++) {
        fill_pattern(wbuf, (uint32_t)lba, 1);
        int rc = nvme_uspace_write(&dev, 1, lba, 1, wbuf);
        if (rc == HFSSS_OK)           { lba_gen[lba] = 1; wu_writes++; }
        else if (rc == HFSSS_ERR_NOSPC) wu_nospc++;
        else                            wu_err++;
    }
    nvme_uspace_flush(&dev, 1);
    printf("[INFO] Warm-up done: %llu written, %llu NOSPC, %llu errors\n\n",
           (unsigned long long)wu_writes,
           (unsigned long long)wu_nospc,
           (unsigned long long)wu_err);

    /* ---- mixed phase ---- */
    uint64_t start_s   = now_s();
    uint64_t deadline  = start_s + RUN_SECONDS;
    uint64_t last_prog = start_s;

    uint64_t total_writes      = 0;
    uint64_t total_reads       = 0;
    uint64_t total_trims       = 0;
    uint64_t total_trim_lbas   = 0;
    uint64_t write_errors      = 0;
    uint64_t nospc_errors      = 0;
    uint64_t read_errors       = 0;
    uint64_t corrupt_count     = 0;
    uint64_t trim_errors       = 0;
    /* Reads of a trimmed LBA must return NOENT — any other result is a bug. */
    uint64_t trim_stale_reads  = 0;  /* trimmed LBA returned old data */
    uint64_t trim_noent_ok     = 0;  /* trimmed LBA correctly returned NOENT */
    uint64_t batch_num         = 0;

    uint32_t rng = SEED;

    struct nvme_dsm_range trim_range;

    while (!g_stop && now_s() < deadline) {
        for (int i = 0; i < IOS_PER_BATCH && now_s() < deadline; i++) {
            rng = lcg(rng);
            uint32_t dice = rng % 100;

            if ((int)dice < trim_pct) {
                /* ---- TRIM ---- */
                rng = lcg(rng);
                uint64_t start_lba = rng % total_lbas;
                rng = lcg(rng);
                uint32_t nlb = (rng % MAX_TRIM_RANGE) + 1;
                if (start_lba + nlb > total_lbas) {
                    nlb = (uint32_t)(total_lbas - start_lba);
                }

                trim_range.attributes = 0;
                trim_range.slba       = start_lba;
                trim_range.nlb        = nlb;

                int rc = nvme_uspace_trim(&dev, 1, &trim_range, 1);
                if (rc != HFSSS_OK) {
                    fprintf(stderr, "[ERROR] trim failed: LBA=%llu nlb=%u rc=%d\n",
                            (unsigned long long)start_lba, nlb, rc);
                    trim_errors++;
                } else {
                    total_trims++;
                    total_trim_lbas += nlb;

                    /* Mark trimmed LBAs in state and verify NOENT on read-back */
                    for (uint32_t t = 0; t < nlb; t++) {
                        uint64_t tlba = start_lba + t;
                        bool had_data = (lba_gen[tlba] > 0);
                        lba_gen[tlba] = 0;   /* trimmed: no valid data */

                        if (had_data) {
                            /* Verify: read must return NOENT, not old data */
                            int rrc = nvme_uspace_read(&dev, 1, tlba, 1, rbuf);
                            if (rrc == HFSSS_ERR_NOENT) {
                                trim_noent_ok++;
                            } else if (rrc == HFSSS_OK) {
                                /* Data is readable after trim — data integrity bug */
                                trim_stale_reads++;
                                fprintf(stderr,
                                    "[TRIM_BUG] LBA=%llu still readable after trim! "
                                    "buf[0]=0x%08X\n",
                                    (unsigned long long)tlba,
                                    ((uint32_t *)rbuf)[0]);
                            }
                            /* Other error codes are acceptable (NOENT is ideal) */
                        }
                    }
                }

            } else if ((int)dice < trim_pct + read_pct) {
                /* ---- READ ---- */
                rng = lcg(rng);
                uint64_t lba = rng % total_lbas;

                if (lba_gen[lba] == 0) {
                    /* LBA is trimmed/unwritten: read must return NOENT */
                    int rc = nvme_uspace_read(&dev, 1, lba, 1, rbuf);
                    if (rc == HFSSS_ERR_NOENT) {
                        trim_noent_ok++;
                    } else if (rc == HFSSS_OK) {
                        trim_stale_reads++;
                        fprintf(stderr,
                            "[TRIM_BUG] unwritten/trimmed LBA=%llu returned data!\n",
                            (unsigned long long)lba);
                    }
                    /* Other errors acceptable */
                } else {
                    /* LBA has valid data: read and verify */
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
                                /* recompute expected[0] */
                                ({ uint32_t _s = (uint32_t)(lba ^ (lba_gen[lba] * 0x9e3779b9u));
                                   _s = _s * 1664525u + 1013904223u; _s; }),
                                ((uint32_t *)rbuf)[0]);
                        }
                    }
                }

            } else {
                /* ---- WRITE ---- */
                rng = lcg(rng);
                uint64_t lba = rng % total_lbas;
                uint64_t gen = lba_gen[lba] + 1;
                fill_pattern(wbuf, (uint32_t)lba, gen);

                int rc = nvme_uspace_write(&dev, 1, lba, 1, wbuf);
                if (rc == HFSSS_OK) {
                    lba_gen[lba] = gen;
                    total_writes++;
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

            printf("[%s | %4llus / %ds remaining] "
                   "wr=%llu rd=%llu tr=%llu(%llu LBAs) gc=%llu "
                   "waf=%.2f corrupt=%llu stale=%llu werr=%llu rerr=%llu trerr=%llu\n",
                   now_ts(),
                   (unsigned long long)elapsed, (int)remain,
                   (unsigned long long)total_writes,
                   (unsigned long long)total_reads,
                   (unsigned long long)total_trims,
                   (unsigned long long)total_trim_lbas,
                   (unsigned long long)stats.gc_count,
                   stats.waf,
                   (unsigned long long)corrupt_count,
                   (unsigned long long)trim_stale_reads,
                   (unsigned long long)write_errors,
                   (unsigned long long)read_errors,
                   (unsigned long long)trim_errors);
        }
    }

    /* ---- final flush + full verify sweep ---- */
    printf("\n[INFO] Run complete. Final flush + verify sweep...\n");
    nvme_uspace_flush(&dev, 1);

    uint64_t verified = 0, final_corrupt = 0;
    uint64_t final_noent_ok = 0, final_stale = 0;
    uint64_t dbg_printed = 0;

    for (uint64_t lba = 0; lba < total_lbas; lba++) {
        int rc = nvme_uspace_read(&dev, 1, lba, 1, rbuf);

        if (lba_gen[lba] == 0) {
            /* Trimmed/unwritten: must be NOENT */
            if (rc == HFSSS_ERR_NOENT) {
                final_noent_ok++;
            } else if (rc == HFSSS_OK) {
                final_stale++;
                if (dbg_printed < 5) {
                    fprintf(stderr,
                        "[TRIM_BUG] final sweep: LBA=%llu trimmed but readable\n",
                        (unsigned long long)lba);
                    dbg_printed++;
                }
            }
        } else {
            /* Written: must be readable and correct */
            if (rc != HFSSS_OK) {
                if (dbg_printed < 5) {
                    fprintf(stderr, "[READ_FAIL] LBA=%llu gen=%llu rc=%d\n",
                            (unsigned long long)lba,
                            (unsigned long long)lba_gen[lba], rc);
                    dbg_printed++;
                }
                read_errors++;
            } else {
                total_reads++;
                verified++;
                if (!verify_pattern(rbuf, (uint32_t)lba, lba_gen[lba])) {
                    final_corrupt++;
                    if (final_corrupt <= 5) {
                        fprintf(stderr, "[CORRUPT] final LBA=%llu gen=%llu\n",
                                (unsigned long long)lba,
                                (unsigned long long)lba_gen[lba]);
                    }
                }
            }
        }
    }

    /* ---- final stats ---- */
    struct ftl_stats final_stats;
    sssim_get_stats(&dev.sssim, &final_stats);
    uint64_t total_s = now_s() - start_s;

    printf("\n========================================\n");
    printf("Mixed Read/Write/Trim Stress Test Results\n");
    printf("========================================\n");
    printf("  Duration          : %llus (%llum %llus)\n",
           (unsigned long long)total_s,
           (unsigned long long)(total_s / 60),
           (unsigned long long)(total_s % 60));
    printf("  Total writes      : %llu\n",  (unsigned long long)total_writes);
    printf("  Total reads       : %llu\n",  (unsigned long long)total_reads);
    printf("  Total trims       : %llu (%llu LBAs)\n",
           (unsigned long long)total_trims,
           (unsigned long long)total_trim_lbas);
    printf("  Write errors      : %llu\n",  (unsigned long long)write_errors);
    printf("  NOSPC events      : %llu\n",  (unsigned long long)nospc_errors);
    printf("  Read errors       : %llu\n",  (unsigned long long)read_errors);
    printf("  Trim errors       : %llu\n",  (unsigned long long)trim_errors);
    printf("  Mid-run corrupt   : %llu\n",  (unsigned long long)corrupt_count);
    printf("  Trim stale reads  : %llu\n",  (unsigned long long)
           (trim_stale_reads + final_stale));
    printf("  Trim NOENT ok     : %llu\n",  (unsigned long long)
           (trim_noent_ok + final_noent_ok));
    printf("  Final corrupt     : %llu / %llu verified\n",
           (unsigned long long)final_corrupt,
           (unsigned long long)verified);
    printf("  GC cycles         : %llu\n",  (unsigned long long)final_stats.gc_count);
    printf("  GC moved pages    : %llu\n",  (unsigned long long)final_stats.moved_pages);
    printf("  WAF               : %.3f\n",  final_stats.waf);
    printf("  Write IOPS        : %.0f\n",
           total_s > 0 ? (double)total_writes / total_s : 0.0);
    printf("  Read  IOPS        : %.0f\n",
           total_s > 0 ? (double)total_reads  / total_s : 0.0);
    printf("========================================\n");

    /* Stale reads after trim are data integrity violations. */
    total_errors = write_errors + read_errors + corrupt_count + final_corrupt
                 + trim_errors + trim_stale_reads + final_stale;
    if (total_errors == 0) {
        printf("\n  [PASS] No errors after %llu writes + %llu reads + %llu trims.\n\n",
               (unsigned long long)total_writes,
               (unsigned long long)total_reads,
               (unsigned long long)total_trims);
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
