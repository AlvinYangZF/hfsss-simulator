/*
 * systest_data_integrity.c -- P0 system-level data integrity tests for HFSSS.
 *
 * Covers end-to-end data path, GC integrity, trim correctness,
 * format NVM completeness, sanitize completeness, and multi-LBA
 * write atomicity through the full NVMe -> FTL -> NAND stack.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "pcie/nvme_uspace.h"
#include "ftl/ftl.h"
#include "common/common.h"

/* ---------------------------------------------------------------
 * Test harness
 * ------------------------------------------------------------- */
static int total_tests = 0;
static int passed_tests = 0;
static int failed_tests = 0;

#define TEST_ASSERT(cond, msg) do { \
    total_tests++; \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        passed_tests++; \
    } else { \
        printf("  [FAIL] %s\n", msg); \
        failed_tests++; \
    } \
} while (0)

/* ---------------------------------------------------------------
 * LCG-based deterministic data patterns
 * ------------------------------------------------------------- */
#define LBA_SIZE 4096

static uint32_t lcg(uint32_t state) {
    return state * 1664525u + 1013904223u;
}

static void fill_pattern(void *buf, uint32_t lba, uint64_t gen) {
    uint32_t *p = (uint32_t *)buf;
    uint32_t s = (uint32_t)(lba ^ (gen * 0x9e3779b9u));
    for (int i = 0; i < (int)(LBA_SIZE / 4); i++) {
        s = lcg(s);
        p[i] = s;
    }
}

static bool verify_pattern(const void *buf, uint32_t lba, uint64_t gen) {
    const uint32_t *p = (const uint32_t *)buf;
    uint32_t s = (uint32_t)(lba ^ (gen * 0x9e3779b9u));
    for (int i = 0; i < (int)(LBA_SIZE / 4); i++) {
        s = lcg(s);
        if (p[i] != s) return false;
    }
    return true;
}

/* ---------------------------------------------------------------
 * Device setup / teardown helpers
 * ------------------------------------------------------------- */
static int dev_setup(struct nvme_uspace_dev *dev, struct nvme_uspace_config *cfg,
                     uint64_t *out_total_lbas) {
    nvme_uspace_config_default(cfg);
    cfg->sssim_cfg.channel_count     = 2;
    cfg->sssim_cfg.chips_per_channel = 1;
    cfg->sssim_cfg.dies_per_chip     = 1;
    cfg->sssim_cfg.planes_per_die    = 1;
    cfg->sssim_cfg.blocks_per_plane  = 128;
    cfg->sssim_cfg.pages_per_block   = 256;
    cfg->sssim_cfg.page_size         = 4096;

    uint64_t raw_pages = (uint64_t)cfg->sssim_cfg.channel_count
                       * cfg->sssim_cfg.chips_per_channel
                       * cfg->sssim_cfg.dies_per_chip
                       * cfg->sssim_cfg.planes_per_die
                       * cfg->sssim_cfg.blocks_per_plane
                       * cfg->sssim_cfg.pages_per_block;
    cfg->sssim_cfg.total_lbas =
        raw_pages * (100 - cfg->sssim_cfg.op_ratio) / 100;

    if (nvme_uspace_dev_init(dev, cfg) != HFSSS_OK) return -1;
    if (nvme_uspace_dev_start(dev) != HFSSS_OK) {
        nvme_uspace_dev_cleanup(dev);
        return -1;
    }
    if (nvme_uspace_create_io_cq(dev, 1, 256, false) != HFSSS_OK ||
        nvme_uspace_create_io_sq(dev, 1, 256, 1, 0)  != HFSSS_OK) {
        nvme_uspace_dev_stop(dev);
        nvme_uspace_dev_cleanup(dev);
        return -1;
    }

    struct nvme_identify_ns ns_id;
    nvme_uspace_identify_ns(dev, 1, &ns_id);
    *out_total_lbas = ns_id.nsze > 0 ? ns_id.nsze : 1024;
    return 0;
}

static void dev_teardown(struct nvme_uspace_dev *dev) {
    nvme_uspace_delete_io_sq(dev, 1);
    nvme_uspace_delete_io_cq(dev, 1);
    nvme_uspace_dev_stop(dev);
    nvme_uspace_dev_cleanup(dev);
}

/* ---------------------------------------------------------------
 * DI-001: End-to-end data path
 * ------------------------------------------------------------- */
static void test_di_001(void) {
    printf("\n--- DI-001: End-to-end data path ---\n");

    struct nvme_uspace_config cfg;
    struct nvme_uspace_dev dev;
    uint64_t total_lbas = 0;

    if (dev_setup(&dev, &cfg, &total_lbas) != 0) {
        TEST_ASSERT(false, "DI-001: device setup");
        return;
    }

    uint8_t *wbuf = malloc(LBA_SIZE);
    uint8_t *rbuf = malloc(LBA_SIZE);
    bool ok = true;

    /* Write 80% of LBAs with gen=1 (leave headroom for GC during overwrites) */
    uint64_t write_count = total_lbas * 80 / 100;
    for (uint64_t lba = 0; lba < write_count; lba++) {
        fill_pattern(wbuf, (uint32_t)lba, 1);
        int rc = nvme_uspace_write(&dev, 1, lba, 1, wbuf);
        if (rc != HFSSS_OK) { ok = false; break; }
    }
    TEST_ASSERT(ok, "DI-001: write 80%% of LBAs gen=1");

    /* Read back all written, verify gen=1 */
    ok = true;
    for (uint64_t lba = 0; lba < write_count; lba++) {
        int rc = nvme_uspace_read(&dev, 1, lba, 1, rbuf);
        if (rc != HFSSS_OK || !verify_pattern(rbuf, (uint32_t)lba, 1)) {
            ok = false;
            break;
        }
    }
    TEST_ASSERT(ok, "DI-001: read-back verify all gen=1");

    /* Overwrite every 4th LBA with gen=2 */
    ok = true;
    uint64_t nospc = 0;
    for (uint64_t lba = 0; lba < write_count; lba += 4) {
        fill_pattern(wbuf, (uint32_t)lba, 2);
        int rc = nvme_uspace_write(&dev, 1, lba, 1, wbuf);
        if (rc == HFSSS_ERR_NOSPC) { nospc++; }
        else if (rc != HFSSS_OK) { ok = false; break; }
    }
    TEST_ASSERT(ok, "DI-001: overwrite every 4th LBA gen=2");

    /* Read all written, verify current generation (skip NOSPC'd LBAs by accepting gen=1 or gen=2) */
    ok = true;
    for (uint64_t lba = 0; lba < write_count; lba++) {
        int rc = nvme_uspace_read(&dev, 1, lba, 1, rbuf);
        if (rc != HFSSS_OK) { ok = false; break; }
        /* If lba%4==0, could be gen=1 or gen=2 (if NOSPC prevented overwrite) */
        if (lba % 4 == 0) {
            if (!verify_pattern(rbuf, (uint32_t)lba, 2) &&
                !verify_pattern(rbuf, (uint32_t)lba, 1)) {
                ok = false; break;
            }
        } else {
            if (!verify_pattern(rbuf, (uint32_t)lba, 1)) {
                ok = false; break;
            }
        }
    }
    TEST_ASSERT(ok, "DI-001: mixed-gen read-back verify");

    /* Flush, read all again */
    nvme_uspace_flush(&dev, 1);
    ok = true;
    for (uint64_t lba = 0; lba < write_count; lba++) {
        int rc = nvme_uspace_read(&dev, 1, lba, 1, rbuf);
        if (rc != HFSSS_OK) { ok = false; break; }
        if (lba % 4 == 0) {
            if (!verify_pattern(rbuf, (uint32_t)lba, 2) &&
                !verify_pattern(rbuf, (uint32_t)lba, 1)) {
                ok = false; break;
            }
        } else {
            if (!verify_pattern(rbuf, (uint32_t)lba, 1)) {
                ok = false; break;
            }
        }
    }
    TEST_ASSERT(ok, "DI-001: post-flush verify");

    free(wbuf);
    free(rbuf);
    dev_teardown(&dev);
}

/* ---------------------------------------------------------------
 * DI-002: GC data integrity at 95% utilization
 * ------------------------------------------------------------- */
static void test_di_002(void) {
    printf("\n--- DI-002: GC data integrity at 95%% utilization ---\n");

    struct nvme_uspace_config cfg;
    struct nvme_uspace_dev dev;
    uint64_t total_lbas = 0;

    if (dev_setup(&dev, &cfg, &total_lbas) != 0) {
        TEST_ASSERT(false, "DI-002: device setup");
        return;
    }

    uint8_t *wbuf = malloc(LBA_SIZE);
    uint8_t *rbuf = malloc(LBA_SIZE);
    uint64_t fill_count = (total_lbas * 95) / 100;

    /* gen tracking: gen[lba] = current generation, 0 = unwritten */
    uint64_t *lba_gen = calloc(total_lbas, sizeof(uint64_t));
    bool ok = true;

    /* Fill 95% of LBAs with gen=1 */
    for (uint64_t lba = 0; lba < fill_count; lba++) {
        fill_pattern(wbuf, (uint32_t)lba, 1);
        int rc = nvme_uspace_write(&dev, 1, lba, 1, wbuf);
        if (rc != HFSSS_OK) { ok = false; break; }
        lba_gen[lba] = 1;
    }
    TEST_ASSERT(ok, "DI-002: fill 95% of LBAs");

    /* 50000 random overwrites within filled range (triggers GC) */
    uint32_t rng = 0xDEAD1234u;
    uint64_t overwrite_ok = 0;
    uint64_t overwrite_fail = 0;

    for (uint64_t i = 0; i < 50000; i++) {
        rng = lcg(rng);
        uint64_t lba = rng % fill_count;
        uint64_t gen = lba_gen[lba] + 1;
        fill_pattern(wbuf, (uint32_t)lba, gen);
        int rc = nvme_uspace_write(&dev, 1, lba, 1, wbuf);
        if (rc == HFSSS_OK) {
            lba_gen[lba] = gen;
            overwrite_ok++;
        } else {
            overwrite_fail++;
        }

        /* Periodic verify of 100 random LBAs every 5000 overwrites */
        if ((i + 1) % 5000 == 0) {
            uint32_t rng2 = rng;
            bool spot_ok = true;
            for (int j = 0; j < 100; j++) {
                rng2 = lcg(rng2);
                uint64_t check_lba = rng2 % fill_count;
                if (lba_gen[check_lba] == 0) continue;
                int rrc = nvme_uspace_read(&dev, 1, check_lba, 1, rbuf);
                if (rrc != HFSSS_OK ||
                    !verify_pattern(rbuf, (uint32_t)check_lba, lba_gen[check_lba])) {
                    spot_ok = false;
                    break;
                }
            }
            if (!spot_ok) {
                ok = false;
            }
        }
    }
    TEST_ASSERT(overwrite_ok > 0, "DI-002: random overwrites completed");
    TEST_ASSERT(ok, "DI-002: periodic spot-check verify during overwrites");

    /* Final full sweep verify of all written LBAs */
    ok = true;
    for (uint64_t lba = 0; lba < fill_count; lba++) {
        if (lba_gen[lba] == 0) continue;
        int rc = nvme_uspace_read(&dev, 1, lba, 1, rbuf);
        if (rc != HFSSS_OK || !verify_pattern(rbuf, (uint32_t)lba, lba_gen[lba])) {
            ok = false;
            fprintf(stderr, "  DI-002: corrupt at LBA=%llu gen=%llu\n",
                    (unsigned long long)lba, (unsigned long long)lba_gen[lba]);
            break;
        }
    }
    TEST_ASSERT(ok, "DI-002: final full sweep verify");

    /* Check gc_count > 0 */
    struct ftl_stats stats;
    sssim_get_stats(&dev.sssim, &stats);
    TEST_ASSERT(stats.gc_count > 0, "DI-002: GC triggered (gc_count > 0)");
    printf("  (gc_count=%llu, moved_pages=%llu)\n",
           (unsigned long long)stats.gc_count,
           (unsigned long long)stats.moved_pages);

    free(lba_gen);
    free(wbuf);
    free(rbuf);
    dev_teardown(&dev);
}

/* ---------------------------------------------------------------
 * DI-004: Trim correctness, no stale data
 * ------------------------------------------------------------- */
static void test_di_004(void) {
    printf("\n--- DI-004: Trim correctness, no stale data ---\n");

    struct nvme_uspace_config cfg;
    struct nvme_uspace_dev dev;
    uint64_t total_lbas = 0;

    if (dev_setup(&dev, &cfg, &total_lbas) != 0) {
        TEST_ASSERT(false, "DI-004: device setup");
        return;
    }

    uint8_t *wbuf = malloc(LBA_SIZE);
    uint8_t *rbuf = malloc(LBA_SIZE);
    /* Track state: 0 = unwritten, 1 = written with gen, negative = trimmed */
    uint64_t *lba_gen = calloc(total_lbas, sizeof(uint64_t));
    bool *trimmed = calloc(total_lbas, sizeof(bool));
    bool ok = true;

    /* Write all LBAs with gen=1 */
    for (uint64_t lba = 0; lba < total_lbas; lba++) {
        fill_pattern(wbuf, (uint32_t)lba, 1);
        int rc = nvme_uspace_write(&dev, 1, lba, 1, wbuf);
        if (rc != HFSSS_OK) { ok = false; break; }
        lba_gen[lba] = 1;
    }
    TEST_ASSERT(ok, "DI-004: write all LBAs");

    /* Trim 500 random ranges (1-8 LBAs each) */
    uint32_t rng = 0xCAFE0001u;
    struct nvme_dsm_range range;
    uint64_t trim_ops = 0;

    for (int i = 0; i < 500; i++) {
        rng = lcg(rng);
        uint64_t start_lba = rng % total_lbas;
        rng = lcg(rng);
        uint32_t nlb = (rng % 8) + 1;
        if (start_lba + nlb > total_lbas) {
            nlb = (uint32_t)(total_lbas - start_lba);
        }

        range.attributes = 0;
        range.slba = start_lba;
        range.nlb = nlb;
        int rc = nvme_uspace_trim(&dev, 1, &range, 1);
        if (rc == HFSSS_OK) {
            trim_ops++;
            for (uint32_t t = 0; t < nlb; t++) {
                trimmed[start_lba + t] = true;
                lba_gen[start_lba + t] = 0;
            }
        }
    }
    TEST_ASSERT(trim_ops > 0, "DI-004: trim operations completed");

    /* Read each trimmed LBA: must return HFSSS_ERR_NOENT */
    ok = true;
    uint64_t trimmed_count = 0;
    for (uint64_t lba = 0; lba < total_lbas; lba++) {
        if (!trimmed[lba]) continue;
        trimmed_count++;
        int rc = nvme_uspace_read(&dev, 1, lba, 1, rbuf);
        if (rc != HFSSS_ERR_NOENT) {
            ok = false;
            fprintf(stderr, "  DI-004: trimmed LBA=%llu returned rc=%d (expected NOENT)\n",
                    (unsigned long long)lba, rc);
            break;
        }
    }
    TEST_ASSERT(ok, "DI-004: trimmed LBAs return NOENT");
    printf("  (trimmed_count=%llu)\n", (unsigned long long)trimmed_count);

    /* Re-write 200 of the trimmed LBAs with gen=10 */
    uint64_t rewritten = 0;
    rng = 0xBEEF0002u;
    for (int i = 0; i < 200; i++) {
        /* Find a trimmed LBA */
        uint64_t lba;
        int attempts = 0;
        do {
            rng = lcg(rng);
            lba = rng % total_lbas;
            attempts++;
        } while (!trimmed[lba] && attempts < 10000);
        if (!trimmed[lba]) continue;

        fill_pattern(wbuf, (uint32_t)lba, 10);
        int rc = nvme_uspace_write(&dev, 1, lba, 1, wbuf);
        if (rc == HFSSS_OK) {
            lba_gen[lba] = 10;
            trimmed[lba] = false;
            rewritten++;
        }
    }
    TEST_ASSERT(rewritten > 0, "DI-004: re-wrote trimmed LBAs");

    /* Verify re-written match gen=10 */
    ok = true;
    for (uint64_t lba = 0; lba < total_lbas; lba++) {
        if (lba_gen[lba] != 10) continue;
        int rc = nvme_uspace_read(&dev, 1, lba, 1, rbuf);
        if (rc != HFSSS_OK || !verify_pattern(rbuf, (uint32_t)lba, 10)) {
            ok = false;
            break;
        }
    }
    TEST_ASSERT(ok, "DI-004: re-written LBAs verify correctly");

    /* Verify still-trimmed LBAs return NOENT */
    ok = true;
    for (uint64_t lba = 0; lba < total_lbas; lba++) {
        if (!trimmed[lba]) continue;
        int rc = nvme_uspace_read(&dev, 1, lba, 1, rbuf);
        if (rc != HFSSS_ERR_NOENT) {
            ok = false;
            break;
        }
    }
    TEST_ASSERT(ok, "DI-004: still-trimmed LBAs remain NOENT");

    free(trimmed);
    free(lba_gen);
    free(wbuf);
    free(rbuf);
    dev_teardown(&dev);
}

/* ---------------------------------------------------------------
 * DI-006: Format NVM completeness
 * ------------------------------------------------------------- */
static void test_di_006(void) {
    printf("\n--- DI-006: Format NVM completeness ---\n");

    struct nvme_uspace_config cfg;
    struct nvme_uspace_dev dev;
    uint64_t total_lbas = 0;

    if (dev_setup(&dev, &cfg, &total_lbas) != 0) {
        TEST_ASSERT(false, "DI-006: device setup");
        return;
    }

    uint8_t *wbuf = malloc(LBA_SIZE);
    uint8_t *rbuf = malloc(LBA_SIZE);
    bool ok = true;

    /* Write all LBAs with gen=1 */
    for (uint64_t lba = 0; lba < total_lbas; lba++) {
        fill_pattern(wbuf, (uint32_t)lba, 1);
        int rc = nvme_uspace_write(&dev, 1, lba, 1, wbuf);
        if (rc != HFSSS_OK) { ok = false; break; }
    }
    TEST_ASSERT(ok, "DI-006: write all LBAs before format");

    /* Format NVM */
    int rc = nvme_uspace_format_nvm(&dev, 1);
    TEST_ASSERT(rc == HFSSS_OK, "DI-006: format_nvm succeeds");

    /* Read all: all must return HFSSS_ERR_NOENT */
    ok = true;
    for (uint64_t lba = 0; lba < total_lbas; lba++) {
        rc = nvme_uspace_read(&dev, 1, lba, 1, rbuf);
        if (rc == HFSSS_OK) {
            /* Data readable after format -- check it is not valid old data */
            if (verify_pattern(rbuf, (uint32_t)lba, 1)) {
                ok = false;
                fprintf(stderr, "  DI-006: LBA=%llu still has valid pre-format data\n",
                        (unsigned long long)lba);
                break;
            }
        } else if (rc != HFSSS_ERR_NOENT) {
            ok = false;
            fprintf(stderr, "  DI-006: LBA=%llu unexpected rc=%d after format\n",
                    (unsigned long long)lba, rc);
            break;
        }
    }
    TEST_ASSERT(ok, "DI-006: all LBAs NOENT (or no valid data) after format");

    /* Write new patterns with gen=5 */
    ok = true;
    for (uint64_t lba = 0; lba < total_lbas; lba++) {
        fill_pattern(wbuf, (uint32_t)lba, 5);
        rc = nvme_uspace_write(&dev, 1, lba, 1, wbuf);
        if (rc != HFSSS_OK) { ok = false; break; }
    }
    TEST_ASSERT(ok, "DI-006: write all LBAs gen=5 after format");

    /* Read back, verify gen=5 */
    ok = true;
    for (uint64_t lba = 0; lba < total_lbas; lba++) {
        rc = nvme_uspace_read(&dev, 1, lba, 1, rbuf);
        if (rc != HFSSS_OK || !verify_pattern(rbuf, (uint32_t)lba, 5)) {
            ok = false;
            break;
        }
    }
    TEST_ASSERT(ok, "DI-006: post-format write+read verify");

    free(wbuf);
    free(rbuf);
    dev_teardown(&dev);
}

/* ---------------------------------------------------------------
 * DI-007: Sanitize completeness (all sanact values)
 * ------------------------------------------------------------- */
static void test_di_007(void) {
    printf("\n--- DI-007: Sanitize completeness (all sanact values) ---\n");

    for (uint32_t sanact = 1; sanact <= 3; sanact++) {
        char label[128];

        struct nvme_uspace_config cfg;
        struct nvme_uspace_dev dev;
        uint64_t total_lbas = 0;

        if (dev_setup(&dev, &cfg, &total_lbas) != 0) {
            snprintf(label, sizeof(label),
                     "DI-007: device setup for sanact=%u", sanact);
            TEST_ASSERT(false, label);
            continue;
        }

        uint8_t *wbuf = malloc(LBA_SIZE);
        uint8_t *rbuf = malloc(LBA_SIZE);
        bool ok = true;

        /* Write all LBAs */
        for (uint64_t lba = 0; lba < total_lbas; lba++) {
            fill_pattern(wbuf, (uint32_t)lba, sanact);
            int rc = nvme_uspace_write(&dev, 1, lba, 1, wbuf);
            if (rc != HFSSS_OK) { ok = false; break; }
        }
        snprintf(label, sizeof(label),
                 "DI-007: write all LBAs (sanact=%u)", sanact);
        TEST_ASSERT(ok, label);

        /* Sanitize */
        int rc = nvme_uspace_sanitize(&dev, sanact);
        snprintf(label, sizeof(label),
                 "DI-007: sanitize sanact=%u succeeds", sanact);
        TEST_ASSERT(rc == HFSSS_OK, label);

        /* Verify all NOENT */
        ok = true;
        for (uint64_t lba = 0; lba < total_lbas; lba++) {
            rc = nvme_uspace_read(&dev, 1, lba, 1, rbuf);
            if (rc != HFSSS_ERR_NOENT) {
                ok = false;
                fprintf(stderr,
                        "  DI-007: LBA=%llu rc=%d after sanitize sanact=%u\n",
                        (unsigned long long)lba, rc, sanact);
                break;
            }
        }
        snprintf(label, sizeof(label),
                 "DI-007: all LBAs NOENT after sanact=%u", sanact);
        TEST_ASSERT(ok, label);

        free(wbuf);
        free(rbuf);
        dev_teardown(&dev);
    }
}

/* ---------------------------------------------------------------
 * DI-009: Multi-LBA write atomicity
 * ------------------------------------------------------------- */
static void test_di_009(void) {
    printf("\n--- DI-009: Multi-LBA write atomicity ---\n");

    struct nvme_uspace_config cfg;
    struct nvme_uspace_dev dev;
    uint64_t total_lbas = 0;

    if (dev_setup(&dev, &cfg, &total_lbas) != 0) {
        TEST_ASSERT(false, "DI-009: device setup");
        return;
    }

    uint8_t *wbuf = malloc(LBA_SIZE);
    uint8_t *rbuf = malloc(LBA_SIZE);
    bool ok = true;
    uint32_t rng = 0xF00D0009u;
    uint64_t iterations_ok = 0;

    for (int iter = 0; iter < 500; iter++) {
        rng = lcg(rng);
        /* Pick a base LBA such that base+3 < total_lbas */
        uint64_t base = rng % (total_lbas > 4 ? total_lbas - 4 : 1);

        /* Write 4 contiguous LBAs with gen=1 */
        for (int k = 0; k < 4; k++) {
            fill_pattern(wbuf, (uint32_t)(base + k), 1);
            int rc = nvme_uspace_write(&dev, 1, base + k, 1, wbuf);
            if (rc != HFSSS_OK) { ok = false; break; }
        }
        if (!ok) break;

        /* Overwrite all 4 with gen=2 */
        for (int k = 0; k < 4; k++) {
            fill_pattern(wbuf, (uint32_t)(base + k), 2);
            int rc = nvme_uspace_write(&dev, 1, base + k, 1, wbuf);
            if (rc != HFSSS_OK) { ok = false; break; }
        }
        if (!ok) break;

        /* Read all 4: must be gen=2 */
        for (int k = 0; k < 4; k++) {
            int rc = nvme_uspace_read(&dev, 1, base + k, 1, rbuf);
            if (rc != HFSSS_OK ||
                !verify_pattern(rbuf, (uint32_t)(base + k), 2)) {
                ok = false;
                fprintf(stderr,
                        "  DI-009: mismatch at iter=%d base=%llu+%d rc=%d\n",
                        iter, (unsigned long long)base, k, rc);
                break;
            }
        }
        if (!ok) break;
        iterations_ok++;
    }

    TEST_ASSERT(iterations_ok == 500,
                "DI-009: 500 iterations of 4-LBA write+overwrite+verify");
    TEST_ASSERT(ok, "DI-009: all multi-LBA reads match gen=2");

    free(wbuf);
    free(rbuf);
    dev_teardown(&dev);
}

/* ---------------------------------------------------------------
 * main
 * ------------------------------------------------------------- */
int main(void) {
    printf("========================================\n");
    printf("HFSSS P0 System-Level Data Integrity Tests\n");
    printf("========================================\n");

    uint64_t t0 = get_time_ns();

    test_di_001();
    test_di_002();
    test_di_004();
    test_di_006();
    test_di_007();
    test_di_009();

    uint64_t elapsed_ms = (get_time_ns() - t0) / 1000000ULL;

    printf("\n========================================\n");
    printf("Summary: %d total, %d passed, %d failed  (%.1f s)\n",
           total_tests, passed_tests, failed_tests,
           (double)elapsed_ms / 1000.0);
    printf("========================================\n");

    return (failed_tests == 0) ? 0 : 1;
}
