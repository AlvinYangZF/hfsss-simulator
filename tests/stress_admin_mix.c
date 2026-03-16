/*
 * stress_admin_mix.c — 30-minute stress test mixing IO with NVMe admin commands.
 *
 * Exercises Format NVM, Sanitize, Firmware Download/Commit, Get Log Page,
 * Get/Set Features, and Identify interleaved with random reads, writes, and
 * trims against the full NVMe → FTL → NAND stack.
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
#define SEED               0xBEEF1234u
#define MAX_TRIM_RANGE     8
#define FW_BUF_SIZE        4096

/* Probability thresholds (cumulative, out of 100) */
#define PCT_WRITE         30
#define PCT_READ          25
#define PCT_TRIM          10
#define PCT_FORMAT         5
#define PCT_SANITIZE       5
#define PCT_FW_UPDATE      5
#define PCT_LOG_PAGE      10
#define PCT_FEATURES       5
#define PCT_IDENTIFY       5
/* remainder falls through to extra reads */

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

/* Warm-up: write every LBA once and populate lba_gen[]. */
static void do_warmup(struct nvme_uspace_dev *dev, uint64_t *lba_gen,
                      uint64_t total_lbas, uint8_t *wbuf)
{
    uint64_t wu_writes = 0, wu_nospc = 0, wu_err = 0;
    for (uint64_t lba = 0; lba < total_lbas && !g_stop; lba++) {
        fill_pattern(wbuf, (uint32_t)lba, 1);
        int rc = nvme_uspace_write(dev, 1, lba, 1, wbuf);
        if (rc == HFSSS_OK)             { lba_gen[lba] = 1; wu_writes++; }
        else if (rc == HFSSS_ERR_NOSPC) wu_nospc++;
        else                            wu_err++;
    }
    nvme_uspace_flush(dev, 1);
    printf("[INFO] Warm-up done: %llu written, %llu NOSPC, %llu errors\n\n",
           (unsigned long long)wu_writes,
           (unsigned long long)wu_nospc,
           (unsigned long long)wu_err);
}

/* ---------------------------------------------------------------
 * main
 * ------------------------------------------------------------- */
int main(void) {
    signal(SIGINT, handle_sigint);

    printf("========================================\n");
    printf("HFSSS 30-Minute Admin + IO Mix Stress Test\n");
    printf("========================================\n");
    printf("Started  : %s\n", now_ts());
    printf("Duration : %d minutes\n", RUN_SECONDS / 60);
    printf("LBA size : %d bytes\n", LBA_SIZE);
    printf("Batch    : %d ops\n", IOS_PER_BATCH);
    printf("----------------------------------------\n\n");

    /* ---- device init (same geometry as stress_mixed_trim) ---- */
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
    cfg.sssim_cfg.total_lbas =
        raw_pages * (100 - cfg.sssim_cfg.op_ratio) / 100;

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

    uint64_t total_errors = 1; /* default to failure until test completes */

    /* ---- tracking arrays ---- */
    uint64_t *lba_gen = calloc(total_lbas, sizeof(uint64_t));
    uint8_t  *wbuf    = malloc(LBA_SIZE);
    uint8_t  *rbuf    = malloc(LBA_SIZE);
    uint8_t  *fw_buf  = malloc(FW_BUF_SIZE);
    uint8_t  *log_buf = malloc(512);

    if (!lba_gen || !wbuf || !rbuf || !fw_buf || !log_buf) {
        fprintf(stderr, "[FATAL] OOM\n");
        goto cleanup;
    }

    /* ---- initial warm-up ---- */
    printf("[INFO] Warm-up: writing all %llu LBAs...\n",
           (unsigned long long)total_lbas);
    do_warmup(&dev, lba_gen, total_lbas, wbuf);

    /* ---- mixed phase ---- */
    uint64_t start_s   = now_s();
    uint64_t deadline  = start_s + RUN_SECONDS;
    uint64_t last_prog = start_s;

    uint64_t total_writes      = 0;
    uint64_t total_reads       = 0;
    uint64_t total_trims       = 0;
    uint64_t total_formats     = 0;
    uint64_t total_sanitizes   = 0;
    uint64_t total_fw_updates  = 0;
    uint64_t total_log_reads   = 0;
    uint64_t total_feat_ops    = 0;
    uint64_t total_identify    = 0;
    uint64_t nospc_events      = 0;
    uint64_t io_errors         = 0;
    uint64_t corrupt_count     = 0;
    uint64_t stale_reads       = 0;
    uint64_t format_stale      = 0;
    uint64_t fw_rev_errors     = 0;
    uint64_t feature_errors    = 0;
    uint64_t batch_num         = 0;

    uint32_t rng = SEED;
    uint8_t  fw_slot_byte = 0;

    struct nvme_dsm_range trim_range;

    /* Cumulative probability thresholds */
    const int thr_write    = PCT_WRITE;
    const int thr_read     = thr_write    + PCT_READ;
    const int thr_trim     = thr_read     + PCT_TRIM;
    const int thr_format   = thr_trim     + PCT_FORMAT;
    const int thr_sanitize = thr_format   + PCT_SANITIZE;
    const int thr_fw       = thr_sanitize + PCT_FW_UPDATE;
    const int thr_log      = thr_fw       + PCT_LOG_PAGE;
    const int thr_feat     = thr_log      + PCT_FEATURES;
    const int thr_ident    = thr_feat     + PCT_IDENTIFY;

    while (!g_stop && now_s() < deadline) {
        for (int i = 0; i < IOS_PER_BATCH && now_s() < deadline; i++) {
            rng = lcg(rng);
            int dice = (int)(rng % 100);

            if (dice < thr_write) {
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
                    nospc_events++;
                } else {
                    fprintf(stderr, "[ERROR] write LBA=%llu rc=%d\n",
                            (unsigned long long)lba, rc);
                    io_errors++;
                }

            } else if (dice < thr_read) {
                /* ---- READ ---- */
                rng = lcg(rng);
                uint64_t lba = rng % total_lbas;
                int rc = nvme_uspace_read(&dev, 1, lba, 1, rbuf);
                if (lba_gen[lba] == 0) {
                    /* Trimmed/formatted/unwritten: must be NOENT */
                    if (rc == HFSSS_OK) {
                        stale_reads++;
                        fprintf(stderr,
                            "[STALE] trimmed/formatted LBA=%llu returned data\n",
                            (unsigned long long)lba);
                    } else if (rc != HFSSS_ERR_NOENT) {
                        io_errors++;
                    }
                } else {
                    if (rc != HFSSS_OK) {
                        fprintf(stderr, "[ERROR] read LBA=%llu rc=%d\n",
                                (unsigned long long)lba, rc);
                        io_errors++;
                    } else {
                        total_reads++;
                        if (!verify_pattern(rbuf, (uint32_t)lba, lba_gen[lba])) {
                            corrupt_count++;
                            fprintf(stderr,
                                "[CORRUPT] LBA=%llu gen=%llu\n",
                                (unsigned long long)lba,
                                (unsigned long long)lba_gen[lba]);
                        }
                    }
                }

            } else if (dice < thr_trim) {
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
                    fprintf(stderr, "[ERROR] trim LBA=%llu nlb=%u rc=%d\n",
                            (unsigned long long)start_lba, nlb, rc);
                    io_errors++;
                } else {
                    total_trims++;
                    for (uint32_t t = 0; t < nlb; t++) {
                        lba_gen[start_lba + t] = 0;
                    }
                }

            } else if (dice < thr_format) {
                /* ---- FORMAT NVM ---- */
                int rc = nvme_uspace_format_nvm(&dev, 1);
                if (rc != HFSSS_OK) {
                    fprintf(stderr, "[ERROR] format_nvm rc=%d\n", rc);
                    io_errors++;
                } else {
                    total_formats++;

                    /* Verify 10 previously-written LBAs now return NOENT */
                    uint32_t checks = 0;
                    uint32_t rng2 = rng;
                    for (uint32_t k = 0; k < 100 && checks < 10; k++) {
                        rng2 = lcg(rng2);
                        uint64_t tlba = rng2 % total_lbas;
                        int rrc = nvme_uspace_read(&dev, 1, tlba, 1, rbuf);
                        if (rrc == HFSSS_OK) {
                            format_stale++;
                            fprintf(stderr,
                                "[FORMAT_BUG] LBA=%llu readable after format\n",
                                (unsigned long long)tlba);
                        }
                        checks++;
                    }

                    /* Reset all tracking and re-warm-up */
                    memset(lba_gen, 0, total_lbas * sizeof(uint64_t));
                    printf("[INFO] Format complete (total=%llu). Re-warming up...\n",
                           (unsigned long long)total_formats);
                    do_warmup(&dev, lba_gen, total_lbas, wbuf);
                }

            } else if (dice < thr_sanitize) {
                /* ---- SANITIZE ---- */
                rng = lcg(rng);
                uint32_t sanact = (rng % 3) + 1;  /* 1, 2, or 3 */
                int rc = nvme_uspace_sanitize(&dev, sanact);
                if (rc != HFSSS_OK) {
                    fprintf(stderr, "[ERROR] sanitize sanact=%u rc=%d\n", sanact, rc);
                    io_errors++;
                } else {
                    total_sanitizes++;

                    /* Verify 10 random LBAs return NOENT */
                    uint32_t checks = 0;
                    uint32_t rng2 = rng;
                    for (uint32_t k = 0; k < 100 && checks < 10; k++) {
                        rng2 = lcg(rng2);
                        uint64_t tlba = rng2 % total_lbas;
                        int rrc = nvme_uspace_read(&dev, 1, tlba, 1, rbuf);
                        if (rrc == HFSSS_OK) {
                            format_stale++;
                            fprintf(stderr,
                                "[SANITIZE_BUG] LBA=%llu readable after sanitize\n",
                                (unsigned long long)tlba);
                        }
                        checks++;
                    }

                    memset(lba_gen, 0, total_lbas * sizeof(uint64_t));
                    printf("[INFO] Sanitize complete (total=%llu). Re-warming up...\n",
                           (unsigned long long)total_sanitizes);
                    do_warmup(&dev, lba_gen, total_lbas, wbuf);
                }

            } else if (dice < thr_fw) {
                /* ---- FIRMWARE DOWNLOAD + COMMIT ---- */
                fw_slot_byte++;
                uint8_t pattern = (uint8_t)(0xA5u ^ fw_slot_byte);
                memset(fw_buf, pattern, FW_BUF_SIZE);

                int rc = nvme_uspace_fw_download(&dev, 0, fw_buf, FW_BUF_SIZE);
                if (rc != HFSSS_OK) {
                    fprintf(stderr, "[ERROR] fw_download rc=%d\n", rc);
                    io_errors++;
                } else {
                    rc = nvme_uspace_fw_commit(&dev, 1, 1);
                    if (rc != HFSSS_OK) {
                        fprintf(stderr, "[ERROR] fw_commit rc=%d\n", rc);
                        io_errors++;
                    } else {
                        total_fw_updates++;

                        /* Verify identify_ctrl reflects the new revision */
                        struct nvme_identify_ctrl ctrl_id;
                        if (nvme_uspace_identify_ctrl(&dev, &ctrl_id) == HFSSS_OK) {
                            /* First byte of fw_buf is 'pattern'; fr[0] must match */
                            if ((uint8_t)ctrl_id.fr[0] != pattern) {
                                fw_rev_errors++;
                                fprintf(stderr,
                                    "[FW_ERR] fr[0]=0x%02X expected=0x%02X\n",
                                    (uint8_t)ctrl_id.fr[0], pattern);
                            }
                        } else {
                            io_errors++;
                        }
                    }
                }

            } else if (dice < thr_log) {
                /* ---- GET LOG PAGE (SMART) ---- */
                int rc = nvme_uspace_get_log_page(&dev, 1, 2, log_buf, 512);
                if (rc != HFSSS_OK) {
                    fprintf(stderr, "[ERROR] get_log_page rc=%d\n", rc);
                    io_errors++;
                } else {
                    total_log_reads++;
                }

            } else if (dice < thr_feat) {
                /* ---- GET/SET FEATURES ---- */
                uint32_t set_val;
                rng = lcg(rng);
                /* Alternate between FID 0x02 (power management) tests */
                set_val = rng & 0x07;  /* PS0..PS7 range */
                int rc = nvme_uspace_set_features(&dev, 0x02, set_val);
                if (rc != HFSSS_OK) {
                    fprintf(stderr, "[ERROR] set_features FID=0x02 rc=%d\n", rc);
                    io_errors++;
                } else {
                    uint32_t got = 0xDEAD;
                    rc = nvme_uspace_get_features(&dev, 0x02, &got);
                    if (rc != HFSSS_OK || got != set_val) {
                        feature_errors++;
                        fprintf(stderr,
                            "[FEAT_ERR] FID=0x02 set=%u got=%u rc=%d\n",
                            set_val, got, rc);
                    } else {
                        total_feat_ops++;
                    }
                }

            } else if (dice < thr_ident) {
                /* ---- IDENTIFY (ctrl + ns) ---- */
                struct nvme_identify_ctrl ctrl_id2;
                struct nvme_identify_ns   ns_id2;
                int rc1 = nvme_uspace_identify_ctrl(&dev, &ctrl_id2);
                int rc2 = nvme_uspace_identify_ns(&dev, 1, &ns_id2);
                if (rc1 != HFSSS_OK || rc2 != HFSSS_OK) {
                    fprintf(stderr, "[ERROR] identify rc1=%d rc2=%d\n", rc1, rc2);
                    io_errors++;
                } else {
                    if (ns_id2.nsze != total_lbas) {
                        fprintf(stderr,
                            "[ERROR] nsze mismatch: got=%llu expected=%llu\n",
                            (unsigned long long)ns_id2.nsze,
                            (unsigned long long)total_lbas);
                        io_errors++;
                    } else {
                        total_identify++;
                    }
                }

            } else {
                /* ---- Extra READ (remainder probability) ---- */
                rng = lcg(rng);
                uint64_t lba = rng % total_lbas;
                int rc = nvme_uspace_read(&dev, 1, lba, 1, rbuf);
                if (lba_gen[lba] == 0) {
                    if (rc == HFSSS_OK) {
                        stale_reads++;
                    } else if (rc != HFSSS_ERR_NOENT) {
                        io_errors++;
                    }
                } else {
                    if (rc != HFSSS_OK) {
                        io_errors++;
                    } else {
                        total_reads++;
                        if (!verify_pattern(rbuf, (uint32_t)lba, lba_gen[lba])) {
                            corrupt_count++;
                        }
                    }
                }
            }
        }
        batch_num++;

        if (batch_num % 128 == 0) {
            nvme_uspace_flush(&dev, 1);
        }

        /* Progress report every PROGRESS_INTERVAL seconds */
        uint64_t t = now_s();
        if (t - last_prog >= PROGRESS_INTERVAL) {
            last_prog = t;
            uint64_t elapsed = t - start_s;
            uint64_t remain  = (deadline > t) ? deadline - t : 0;

            struct ftl_stats stats;
            sssim_get_stats(&dev.sssim, &stats);

            printf("[%s | %4llus / %llus remaining] "
                   "wr=%llu rd=%llu tr=%llu fmt=%llu san=%llu fw=%llu "
                   "gc=%llu waf=%.2f corrupt=%llu stale=%llu ioerr=%llu\n",
                   now_ts(),
                   (unsigned long long)elapsed,
                   (unsigned long long)remain,
                   (unsigned long long)total_writes,
                   (unsigned long long)total_reads,
                   (unsigned long long)total_trims,
                   (unsigned long long)total_formats,
                   (unsigned long long)total_sanitizes,
                   (unsigned long long)total_fw_updates,
                   (unsigned long long)stats.gc_count,
                   stats.waf,
                   (unsigned long long)corrupt_count,
                   (unsigned long long)(stale_reads + format_stale),
                   (unsigned long long)io_errors);
        }
    }

    /* ---- final flush + full verify sweep ---- */
    printf("\n[INFO] Run complete. Final flush + verify sweep...\n");
    nvme_uspace_flush(&dev, 1);

    uint64_t verified     = 0;
    uint64_t final_corrupt = 0;
    uint64_t final_noent_ok = 0;
    uint64_t final_stale   = 0;
    uint64_t dbg_printed   = 0;

    for (uint64_t lba = 0; lba < total_lbas; lba++) {
        int rc = nvme_uspace_read(&dev, 1, lba, 1, rbuf);

        if (lba_gen[lba] == 0) {
            /* Trimmed/formatted: must return NOENT */
            if (rc == HFSSS_ERR_NOENT) {
                final_noent_ok++;
            } else if (rc == HFSSS_OK) {
                final_stale++;
                if (dbg_printed < 5) {
                    fprintf(stderr,
                        "[STALE] final sweep: LBA=%llu trimmed/formatted but readable\n",
                        (unsigned long long)lba);
                    dbg_printed++;
                }
            }
        } else {
            if (rc != HFSSS_OK) {
                if (dbg_printed < 5) {
                    fprintf(stderr, "[READ_FAIL] LBA=%llu gen=%llu rc=%d\n",
                            (unsigned long long)lba,
                            (unsigned long long)lba_gen[lba], rc);
                    dbg_printed++;
                }
                io_errors++;
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
    printf("Admin + IO Mix Stress Test Results\n");
    printf("========================================\n");
    printf("  Duration             : %llus (%llum %llus)\n",
           (unsigned long long)total_s,
           (unsigned long long)(total_s / 60),
           (unsigned long long)(total_s % 60));
    printf("  Total writes         : %llu\n",  (unsigned long long)total_writes);
    printf("  Total reads          : %llu\n",  (unsigned long long)total_reads);
    printf("  Total trims          : %llu\n",  (unsigned long long)total_trims);
    printf("  Format NVM ops       : %llu\n",  (unsigned long long)total_formats);
    printf("  Sanitize ops         : %llu\n",  (unsigned long long)total_sanitizes);
    printf("  FW updates           : %llu\n",  (unsigned long long)total_fw_updates);
    printf("  Log page reads       : %llu\n",  (unsigned long long)total_log_reads);
    printf("  Feature ops          : %llu\n",  (unsigned long long)total_feat_ops);
    printf("  Identify ops         : %llu\n",  (unsigned long long)total_identify);
    printf("  NOSPC events         : %llu\n",  (unsigned long long)nospc_events);
    printf("  IO errors            : %llu\n",  (unsigned long long)io_errors);
    printf("  Corrupt reads        : %llu\n",  (unsigned long long)corrupt_count);
    printf("  Stale reads          : %llu\n",
           (unsigned long long)(stale_reads + format_stale));
    printf("  FW revision errors   : %llu\n",  (unsigned long long)fw_rev_errors);
    printf("  Feature errors       : %llu\n",  (unsigned long long)feature_errors);
    printf("  Final corrupt        : %llu / %llu verified\n",
           (unsigned long long)final_corrupt,
           (unsigned long long)verified);
    printf("  Final NOENT ok       : %llu\n",  (unsigned long long)final_noent_ok);
    printf("  Final stale          : %llu\n",  (unsigned long long)final_stale);
    printf("  GC cycles            : %llu\n",  (unsigned long long)final_stats.gc_count);
    printf("  GC moved pages       : %llu\n",  (unsigned long long)final_stats.moved_pages);
    printf("  WAF                  : %.3f\n",  final_stats.waf);
    printf("  Write IOPS           : %.0f\n",
           total_s > 0 ? (double)total_writes / total_s : 0.0);
    printf("  Read  IOPS           : %.0f\n",
           total_s > 0 ? (double)total_reads  / total_s : 0.0);
    printf("========================================\n");

    total_errors = io_errors + corrupt_count + stale_reads + format_stale
                 + fw_rev_errors + feature_errors + final_corrupt + final_stale;

    if (total_errors == 0) {
        printf("\n  [PASS] No errors detected.\n\n");
    } else {
        printf("\n  [FAIL] %llu error(s) detected!\n\n",
               (unsigned long long)total_errors);
    }

cleanup:
    free(lba_gen);
    free(wbuf);
    free(rbuf);
    free(fw_buf);
    free(log_buf);
    nvme_uspace_delete_io_sq(&dev, 1);
    nvme_uspace_delete_io_cq(&dev, 1);
    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return (total_errors == 0) ? 0 : 1;
}
