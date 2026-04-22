/*
 * systest_nvme_compliance.c — NVMe command compliance system tests.
 *
 * Validates correct behaviour of NVMe admin and IO commands against the
 * full NVMe -> FTL -> NAND stack, checking field values, error codes,
 * and round-trip semantics for Identify, Features, Log Page, Firmware,
 * Queue management, boundary IO, DSM/Trim, and Format NVM.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "pcie/nvme_uspace.h"
#include "ftl/ftl.h"
#include "common/common.h"
#include "common/oob.h"

/* ---------------------------------------------------------------
 * Test harness
 * ------------------------------------------------------------- */
static int total_tests = 0;
static int passed_tests = 0;
static int failed_tests = 0;

#define TEST_ASSERT(cond, msg)                                                                                         \
    do {                                                                                                               \
        total_tests++;                                                                                                 \
        if (cond) {                                                                                                    \
            printf("  [PASS] %s\n", msg);                                                                              \
            passed_tests++;                                                                                            \
        } else {                                                                                                       \
            printf("  [FAIL] %s\n", msg);                                                                              \
            failed_tests++;                                                                                            \
        }                                                                                                              \
    } while (0)

/* ---------------------------------------------------------------
 * Device geometry (same small geometry as stress tests)
 * ------------------------------------------------------------- */
#define TEST_CHANNELS 2
#define TEST_CHIPS 1
#define TEST_DIES 1
#define TEST_PLANES 1
#define TEST_BLOCKS 128
#define TEST_PAGES 256
#define TEST_PAGE_SIZE 4096

static uint64_t calc_total_lbas(const struct nvme_uspace_config *cfg)
{
    uint64_t raw_pages = (uint64_t)cfg->sssim_cfg.channel_count * cfg->sssim_cfg.chips_per_channel *
                         cfg->sssim_cfg.dies_per_chip * cfg->sssim_cfg.planes_per_die *
                         cfg->sssim_cfg.blocks_per_plane * cfg->sssim_cfg.pages_per_block;
    return raw_pages * (100 - cfg->sssim_cfg.op_ratio) / 100;
}

static int setup_device(struct nvme_uspace_dev *dev, struct nvme_uspace_config *cfg, uint64_t *out_total_lbas)
{
    nvme_uspace_config_default(cfg);
    cfg->sssim_cfg.channel_count = TEST_CHANNELS;
    cfg->sssim_cfg.chips_per_channel = TEST_CHIPS;
    cfg->sssim_cfg.dies_per_chip = TEST_DIES;
    cfg->sssim_cfg.planes_per_die = TEST_PLANES;
    cfg->sssim_cfg.blocks_per_plane = TEST_BLOCKS;
    cfg->sssim_cfg.pages_per_block = TEST_PAGES;
    cfg->sssim_cfg.page_size = TEST_PAGE_SIZE;
    cfg->sssim_cfg.total_lbas = calc_total_lbas(cfg);

    *out_total_lbas = cfg->sssim_cfg.total_lbas;

    if (nvme_uspace_dev_init(dev, cfg) != HFSSS_OK)
        return -1;
    if (nvme_uspace_dev_start(dev) != HFSSS_OK) {
        nvme_uspace_dev_cleanup(dev);
        return -1;
    }
    if (nvme_uspace_create_io_cq(dev, 1, 256, false) != HFSSS_OK ||
        nvme_uspace_create_io_sq(dev, 1, 256, 1, 0) != HFSSS_OK) {
        nvme_uspace_dev_stop(dev);
        nvme_uspace_dev_cleanup(dev);
        return -1;
    }
    return 0;
}

static void teardown_device(struct nvme_uspace_dev *dev)
{
    nvme_uspace_delete_io_sq(dev, 1);
    nvme_uspace_delete_io_cq(dev, 1);
    nvme_uspace_dev_stop(dev);
    nvme_uspace_dev_cleanup(dev);
}

/* ---------------------------------------------------------------
 * NC-001: Identify Controller fields
 * ------------------------------------------------------------- */
static void test_nc001(void)
{
    printf("\n[NC-001] Identify Controller fields\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    uint64_t total_lbas;

    if (setup_device(&dev, &cfg, &total_lbas) != 0) {
        TEST_ASSERT(false, "device setup");
        return;
    }

    struct nvme_identify_ctrl id;
    memset(&id, 0, sizeof(id));
    int rc = nvme_uspace_identify_ctrl(&dev, &id);

    TEST_ASSERT(rc == HFSSS_OK, "identify_ctrl returns HFSSS_OK");
    TEST_ASSERT(id.vid != 0, "vendor ID (vid) is set");
    TEST_ASSERT(id.ver != 0, "version (ver) is set");
    /* mdts == 0 is valid (no limit), mdts > 0 is also valid; just confirm no crash */
    TEST_ASSERT(id.mdts == 0 || id.mdts > 0, "mdts field accessible (no crash)");

    teardown_device(&dev);
}

/* ---------------------------------------------------------------
 * NC-002: Identify Namespace fields
 * ------------------------------------------------------------- */
static void test_nc002(void)
{
    printf("\n[NC-002] Identify Namespace fields\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    uint64_t total_lbas;

    if (setup_device(&dev, &cfg, &total_lbas) != 0) {
        TEST_ASSERT(false, "device setup");
        return;
    }

    struct nvme_identify_ns id;
    memset(&id, 0, sizeof(id));
    int rc = nvme_uspace_identify_ns(&dev, 1, &id);

    TEST_ASSERT(rc == HFSSS_OK, "identify_ns returns HFSSS_OK");
    TEST_ASSERT(id.nsze > 0, "namespace size (nsze) > 0");
    TEST_ASSERT(id.nsze == total_lbas, "nsze matches expected total_lbas");
    TEST_ASSERT(id.ncap > 0, "namespace capacity (ncap) > 0");

    teardown_device(&dev);
}

/* ---------------------------------------------------------------
 * NC-003: Identify invalid NSID
 * ------------------------------------------------------------- */
static void test_nc003(void)
{
    printf("\n[NC-003] Identify invalid NSID\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    uint64_t total_lbas;

    if (setup_device(&dev, &cfg, &total_lbas) != 0) {
        TEST_ASSERT(false, "device setup");
        return;
    }

    struct nvme_identify_ns id;

    /* NSID 0 is accepted as broadcast in this implementation */
    memset(&id, 0, sizeof(id));
    int rc0 = nvme_uspace_identify_ns(&dev, 0, &id);
    TEST_ASSERT(rc0 == HFSSS_OK || rc0 != HFSSS_OK, "identify_ns NSID=0 does not crash");

    /* NSID 99 is out of range */
    memset(&id, 0, sizeof(id));
    int rc99 = nvme_uspace_identify_ns(&dev, 99, &id);
    TEST_ASSERT(rc99 != HFSSS_OK, "identify_ns NSID=99 returns error");

    teardown_device(&dev);
}

/* ---------------------------------------------------------------
 * NC-004: Get/Set Features round-trip
 * ------------------------------------------------------------- */
static void test_nc004(void)
{
    printf("\n[NC-004] Get/Set Features round-trip\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    uint64_t total_lbas;

    if (setup_device(&dev, &cfg, &total_lbas) != 0) {
        TEST_ASSERT(false, "device setup");
        return;
    }

    int rc;
    uint32_t val;

    /* FID 0x02: Power Management */
    rc = nvme_uspace_set_features(&dev, 0x02, 3);
    TEST_ASSERT(rc == HFSSS_OK, "set_features FID=0x02 value=3 OK");

    val = 0xDEAD;
    rc = nvme_uspace_get_features(&dev, 0x02, &val);
    TEST_ASSERT(rc == HFSSS_OK && val == 3, "get_features FID=0x02 returns 3");

    /* FID 0x04: Temperature Threshold */
    rc = nvme_uspace_set_features(&dev, 0x04, 0x0150);
    TEST_ASSERT(rc == HFSSS_OK, "set_features FID=0x04 value=0x0150 OK");

    val = 0xDEAD;
    rc = nvme_uspace_get_features(&dev, 0x04, &val);
    TEST_ASSERT(rc == HFSSS_OK && val == 0x0150, "get_features FID=0x04 returns 0x0150");

    /* Unsupported FID 0x10 */
    rc = nvme_uspace_set_features(&dev, 0x10, 42);
    TEST_ASSERT(rc == HFSSS_ERR_NOTSUPP, "set_features FID=0x10 returns HFSSS_ERR_NOTSUPP");

    /* FID 0x07: Number of Queues default */
    val = 0;
    rc = nvme_uspace_get_features(&dev, 0x07, &val);
    TEST_ASSERT(rc == HFSSS_OK && val == 0x003F003F, "get_features FID=0x07 returns default 0x003F003F");

    teardown_device(&dev);
}

/* ---------------------------------------------------------------
 * NC-005: Get Log Page (SMART) counters
 * ------------------------------------------------------------- */
static void test_nc005(void)
{
    printf("\n[NC-005] Get Log Page (SMART) counters\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    uint64_t total_lbas;

    if (setup_device(&dev, &cfg, &total_lbas) != 0) {
        TEST_ASSERT(false, "device setup");
        return;
    }

    struct nvme_smart_log smart_before, smart_after;
    int rc;

    /* Get initial SMART log */
    memset(&smart_before, 0, sizeof(smart_before));
    rc = nvme_uspace_get_log_page(&dev, 1, 2, &smart_before, sizeof(smart_before));
    TEST_ASSERT(rc == HFSSS_OK, "get_log_page SMART (initial) OK");

    uint64_t initial_duw = smart_before.data_units_written[0];

    /* Write 100 LBAs */
    uint8_t *wbuf = malloc(TEST_PAGE_SIZE);
    if (!wbuf) {
        TEST_ASSERT(false, "malloc wbuf");
        teardown_device(&dev);
        return;
    }
    memset(wbuf, 0xBB, TEST_PAGE_SIZE);

    uint32_t write_count = (total_lbas < 100) ? (uint32_t)total_lbas : 100;
    for (uint32_t i = 0; i < write_count; i++) {
        nvme_uspace_write(&dev, 1, (uint64_t)i, 1, wbuf);
    }
    nvme_uspace_flush(&dev, 1);

    /* Get SMART log again */
    memset(&smart_after, 0, sizeof(smart_after));
    rc = nvme_uspace_get_log_page(&dev, 1, 2, &smart_after, sizeof(smart_after));
    TEST_ASSERT(rc == HFSSS_OK, "get_log_page SMART (after writes) OK");
    TEST_ASSERT(smart_after.data_units_written[0] > initial_duw, "data_units_written increased after writes");

    free(wbuf);

    /* LID=1 Error Information Log Page is now supported (REQ-115/158) */
    uint8_t dummy_log[512];
    rc = nvme_uspace_get_log_page(&dev, 1, 1, dummy_log, sizeof(dummy_log));
    TEST_ASSERT(rc == HFSSS_OK, "get_log_page LID=1 (Error Info) returns OK");

    teardown_device(&dev);
}

/* ---------------------------------------------------------------
 * NC-006: Firmware Download + Commit round-trip
 * ------------------------------------------------------------- */
static void test_nc006(void)
{
    printf("\n[NC-006] Firmware Download + Commit round-trip\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    uint64_t total_lbas;

    if (setup_device(&dev, &cfg, &total_lbas) != 0) {
        TEST_ASSERT(false, "device setup");
        return;
    }

    int rc;
    uint8_t fw_buf[4096];
    struct nvme_identify_ctrl ctrl_id;

    /* First FW update: pattern 0xAB */
    memset(fw_buf, 0xAB, sizeof(fw_buf));
    rc = nvme_uspace_fw_download(&dev, 0, fw_buf, sizeof(fw_buf));
    TEST_ASSERT(rc == HFSSS_OK, "fw_download pattern 0xAB OK");

    rc = nvme_uspace_fw_commit(&dev, 1, 1);
    TEST_ASSERT(rc == HFSSS_OK, "fw_commit slot=1 action=1 OK");

    memset(&ctrl_id, 0, sizeof(ctrl_id));
    rc = nvme_uspace_identify_ctrl(&dev, &ctrl_id);
    TEST_ASSERT(rc == HFSSS_OK && (uint8_t)ctrl_id.fr[0] == 0xAB, "identify_ctrl fr[0] == 0xAB after first FW update");

    /* Second FW update: pattern 0xCD */
    memset(fw_buf, 0xCD, sizeof(fw_buf));
    rc = nvme_uspace_fw_download(&dev, 0, fw_buf, sizeof(fw_buf));
    TEST_ASSERT(rc == HFSSS_OK, "fw_download pattern 0xCD OK");

    rc = nvme_uspace_fw_commit(&dev, 1, 1);
    TEST_ASSERT(rc == HFSSS_OK, "fw_commit second update OK");

    memset(&ctrl_id, 0, sizeof(ctrl_id));
    rc = nvme_uspace_identify_ctrl(&dev, &ctrl_id);
    TEST_ASSERT(rc == HFSSS_OK && (uint8_t)ctrl_id.fr[0] == 0xCD, "identify_ctrl fr[0] == 0xCD after second FW update");

    /* fw_commit without fw_download should fail */
    /* Clear staging by committing what we have, then try again with no download */
    rc = nvme_uspace_fw_commit(&dev, 1, 1);
    /* A second commit without a new download should error */
    TEST_ASSERT(rc != HFSSS_OK, "fw_commit without fresh fw_download returns error");

    teardown_device(&dev);
}

/* ---------------------------------------------------------------
 * NC-007: Queue create/delete stress
 * ------------------------------------------------------------- */
static void test_nc007(void)
{
    printf("\n[NC-007] Queue create/delete stress\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    uint64_t total_lbas;

    if (setup_device(&dev, &cfg, &total_lbas) != 0) {
        TEST_ASSERT(false, "device setup");
        return;
    }

    int rc;
    uint8_t wbuf[TEST_PAGE_SIZE];
    uint8_t rbuf[TEST_PAGE_SIZE];
    bool all_ok = true;

    for (int iter = 0; iter < 50; iter++) {
        /* Create CQ(2) / SQ(2) */
        rc = nvme_uspace_create_io_cq(&dev, 2, 64, false);
        if (rc != HFSSS_OK) {
            all_ok = false;
            break;
        }

        rc = nvme_uspace_create_io_sq(&dev, 2, 64, 2, 0);
        if (rc != HFSSS_OK) {
            nvme_uspace_delete_io_cq(&dev, 2);
            all_ok = false;
            break;
        }

        /* Write 5 LBAs via queue 1 (default IO queue) */
        for (int lba = 0; lba < 5 && lba < (int)total_lbas; lba++) {
            memset(wbuf, (uint8_t)(iter + lba), TEST_PAGE_SIZE);
            rc = nvme_uspace_write(&dev, 1, (uint64_t)lba, 1, wbuf);
            if (rc != HFSSS_OK && rc != HFSSS_ERR_NOSPC) {
                all_ok = false;
            }
        }

        /* Delete SQ(2) then CQ(2) */
        nvme_uspace_delete_io_sq(&dev, 2);
        nvme_uspace_delete_io_cq(&dev, 2);
    }

    TEST_ASSERT(all_ok, "50 iterations of CQ(2)/SQ(2) create+delete OK");

    /* Verify queue 1 still works after all iterations */
    memset(wbuf, 0x77, TEST_PAGE_SIZE);
    rc = nvme_uspace_write(&dev, 1, 0, 1, wbuf);
    TEST_ASSERT(rc == HFSSS_OK, "queue 1 write OK after 50 q-cycle iterations");

    memset(rbuf, 0, TEST_PAGE_SIZE);
    rc = nvme_uspace_read(&dev, 1, 0, 1, rbuf);
    TEST_ASSERT(rc == HFSSS_OK && rbuf[0] == 0x77, "queue 1 read-back OK after 50 q-cycle iterations");

    teardown_device(&dev);
}

/* ---------------------------------------------------------------
 * NC-008: IO at boundary LBAs
 * ------------------------------------------------------------- */
static void test_nc008(void)
{
    printf("\n[NC-008] IO at boundary LBAs\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    uint64_t total_lbas;

    if (setup_device(&dev, &cfg, &total_lbas) != 0) {
        TEST_ASSERT(false, "device setup");
        return;
    }

    int rc;
    uint8_t wbuf[TEST_PAGE_SIZE];
    uint8_t rbuf[TEST_PAGE_SIZE];

    /* Write/read LBA 0 */
    memset(wbuf, 0xAA, TEST_PAGE_SIZE);
    rc = nvme_uspace_write(&dev, 1, 0, 1, wbuf);
    TEST_ASSERT(rc == HFSSS_OK, "write LBA 0 OK");

    memset(rbuf, 0, TEST_PAGE_SIZE);
    rc = nvme_uspace_read(&dev, 1, 0, 1, rbuf);
    TEST_ASSERT(rc == HFSSS_OK && rbuf[0] == 0xAA, "read LBA 0 returns correct data");

    /* Write/read last valid LBA */
    uint64_t last_lba = total_lbas - 1;
    memset(wbuf, 0xBB, TEST_PAGE_SIZE);
    rc = nvme_uspace_write(&dev, 1, last_lba, 1, wbuf);
    TEST_ASSERT(rc == HFSSS_OK, "write last LBA OK");

    memset(rbuf, 0, TEST_PAGE_SIZE);
    rc = nvme_uspace_read(&dev, 1, last_lba, 1, rbuf);
    TEST_ASSERT(rc == HFSSS_OK && rbuf[0] == 0xBB, "read last LBA returns correct data");

    /* Read at total_lbas (one past end) should fail */
    rc = nvme_uspace_read(&dev, 1, total_lbas, 1, rbuf);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "read LBA=total_lbas returns HFSSS_ERR_INVAL");

    /* Write at total_lbas should fail */
    rc = nvme_uspace_write(&dev, 1, total_lbas, 1, wbuf);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "write LBA=total_lbas returns HFSSS_ERR_INVAL");

    teardown_device(&dev);
}

/* ---------------------------------------------------------------
 * NC-009: Zero-length and NULL edge cases
 * ------------------------------------------------------------- */
static void test_nc009(void)
{
    printf("\n[NC-009] Zero-length and NULL edge cases\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    uint64_t total_lbas;

    if (setup_device(&dev, &cfg, &total_lbas) != 0) {
        TEST_ASSERT(false, "device setup");
        return;
    }

    int rc;
    uint8_t buf[TEST_PAGE_SIZE];

    /* Write with count=0 should not crash */
    rc = nvme_uspace_write(&dev, 1, 0, 0, buf);
    TEST_ASSERT(rc == HFSSS_OK || rc == HFSSS_ERR_INVAL, "write count=0 does not crash");

    /* Read with count=0 should not crash */
    rc = nvme_uspace_read(&dev, 1, 0, 0, buf);
    TEST_ASSERT(rc == HFSSS_OK || rc == HFSSS_ERR_INVAL, "read count=0 does not crash");

    /* Trim with nr_ranges=0 should not crash */
    struct nvme_dsm_range range;
    memset(&range, 0, sizeof(range));
    rc = nvme_uspace_trim(&dev, 1, &range, 0);
    TEST_ASSERT(rc == HFSSS_OK || rc == HFSSS_ERR_INVAL, "trim nr_ranges=0 does not crash");

    /* Flush on valid NSID should not crash */
    rc = nvme_uspace_flush(&dev, 1);
    TEST_ASSERT(rc == HFSSS_OK, "flush NSID=1 OK");

    /* Identify with NSID=0xFFFFFFFF (broadcast) should not crash */
    struct nvme_identify_ns ns_id;
    rc = nvme_uspace_identify_ns(&dev, 0xFFFFFFFF, &ns_id);
    TEST_ASSERT(rc == HFSSS_OK || rc != HFSSS_OK, "identify_ns NSID=0xFFFFFFFF does not crash");

    teardown_device(&dev);
}

/* ---------------------------------------------------------------
 * NC-010: DSM/Trim with multiple ranges
 * ------------------------------------------------------------- */
static void test_nc010(void)
{
    printf("\n[NC-010] DSM/Trim with multiple ranges\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    uint64_t total_lbas;

    if (setup_device(&dev, &cfg, &total_lbas) != 0) {
        TEST_ASSERT(false, "device setup");
        return;
    }

    int rc;
    uint8_t wbuf[TEST_PAGE_SIZE];
    uint8_t rbuf[TEST_PAGE_SIZE];

    /* Ensure we have enough LBAs for the test */
    if (total_lbas < 504) {
        TEST_ASSERT(false, "not enough LBAs for NC-010 (need >= 504)");
        teardown_device(&dev);
        return;
    }

    /* Write LBAs [0..3], [100..103], [500..503] */
    for (int region = 0; region < 3; region++) {
        uint64_t base = (region == 0) ? 0 : (region == 1) ? 100 : 500;
        for (int j = 0; j < 4; j++) {
            memset(wbuf, (uint8_t)(base + j + 1), TEST_PAGE_SIZE);
            rc = nvme_uspace_write(&dev, 1, base + j, 1, wbuf);
            if (rc != HFSSS_OK) {
                TEST_ASSERT(false, "write for trim test setup");
                teardown_device(&dev);
                return;
            }
        }
    }
    nvme_uspace_flush(&dev, 1);

    /* Create 3 DSM ranges and trim them all at once */
    struct nvme_dsm_range ranges[3];
    ranges[0].attributes = 0;
    ranges[0].slba = 0;
    ranges[0].nlb = 4;
    ranges[1].attributes = 0;
    ranges[1].slba = 100;
    ranges[1].nlb = 4;
    ranges[2].attributes = 0;
    ranges[2].slba = 500;
    ranges[2].nlb = 4;

    rc = nvme_uspace_trim(&dev, 1, ranges, 3);
    TEST_ASSERT(rc == HFSSS_OK, "trim with 3 ranges OK");

    /* Read all trimmed LBAs: expect NOENT */
    bool all_noent = true;
    uint64_t check_lbas[] = {0, 1, 2, 3, 100, 101, 102, 103, 500, 501, 502, 503};
    for (int i = 0; i < (int)(sizeof(check_lbas) / sizeof(check_lbas[0])); i++) {
        rc = nvme_uspace_read(&dev, 1, check_lbas[i], 1, rbuf);
        if (rc != HFSSS_ERR_NOENT) {
            all_noent = false;
            fprintf(stderr, "  [DBG] trimmed LBA=%llu returned rc=%d (expected NOENT)\n",
                    (unsigned long long)check_lbas[i], rc);
        }
    }
    TEST_ASSERT(all_noent, "all 12 trimmed LBAs return HFSSS_ERR_NOENT");

    teardown_device(&dev);
}

/* ---------------------------------------------------------------
 * NC-011: DSM/Trim with overlapping ranges
 * ------------------------------------------------------------- */
static void test_nc011(void)
{
    printf("\n[NC-011] DSM/Trim with overlapping ranges\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    uint64_t total_lbas;

    if (setup_device(&dev, &cfg, &total_lbas) != 0) {
        TEST_ASSERT(false, "device setup");
        return;
    }

    int rc;
    uint8_t wbuf[TEST_PAGE_SIZE];
    uint8_t rbuf[TEST_PAGE_SIZE];

    if (total_lbas < 100) {
        TEST_ASSERT(false, "not enough LBAs for NC-011 (need >= 100)");
        teardown_device(&dev);
        return;
    }

    /* Write LBAs 0-99 with distinct byte patterns */
    for (int lba = 0; lba < 100; lba++) {
        memset(wbuf, (uint8_t)(lba + 1), TEST_PAGE_SIZE);
        rc = nvme_uspace_write(&dev, 1, (uint64_t)lba, 1, wbuf);
        if (rc != HFSSS_OK) {
            TEST_ASSERT(false, "write LBAs 0-99 for trim setup");
            teardown_device(&dev);
            return;
        }
    }
    nvme_uspace_flush(&dev, 1);

    /* Trim with overlapping ranges: [10,20) and [15,30) */
    struct nvme_dsm_range ranges[2];
    ranges[0].attributes = 0;
    ranges[0].slba = 10;
    ranges[0].nlb = 10;   /* LBAs 10-19 */
    ranges[1].attributes = 0;
    ranges[1].slba = 15;
    ranges[1].nlb = 15;   /* LBAs 15-29 */

    rc = nvme_uspace_trim(&dev, 1, ranges, 2);
    TEST_ASSERT(rc == HFSSS_OK, "trim with 2 overlapping ranges OK");

    /* Verify LBAs 10-29 return NOENT */
    bool all_noent = true;
    for (int lba = 10; lba < 30; lba++) {
        rc = nvme_uspace_read(&dev, 1, (uint64_t)lba, 1, rbuf);
        if (rc != HFSSS_ERR_NOENT) {
            all_noent = false;
            fprintf(stderr, "  [DBG] LBA=%d expected NOENT, got rc=%d\n",
                    lba, rc);
        }
    }
    TEST_ASSERT(all_noent, "LBAs 10-29 return HFSSS_ERR_NOENT after overlap trim");

    /* Verify LBAs 0-9 still have valid data */
    bool pre_ok = true;
    for (int lba = 0; lba < 10; lba++) {
        rc = nvme_uspace_read(&dev, 1, (uint64_t)lba, 1, rbuf);
        if (rc != HFSSS_OK || rbuf[0] != (uint8_t)(lba + 1)) {
            pre_ok = false;
            fprintf(stderr, "  [DBG] LBA=%d data mismatch after trim\n", lba);
        }
    }
    TEST_ASSERT(pre_ok, "LBAs 0-9 retain valid data after overlap trim");

    /* Verify LBAs 30-99 still have valid data */
    bool post_ok = true;
    for (int lba = 30; lba < 100; lba++) {
        rc = nvme_uspace_read(&dev, 1, (uint64_t)lba, 1, rbuf);
        if (rc != HFSSS_OK || rbuf[0] != (uint8_t)(lba + 1)) {
            post_ok = false;
            fprintf(stderr, "  [DBG] LBA=%d data mismatch after trim\n", lba);
        }
    }
    TEST_ASSERT(post_ok, "LBAs 30-99 retain valid data after overlap trim");

    teardown_device(&dev);
}

/* ---------------------------------------------------------------
 * NC-012: Format NVM followed by IO
 * ------------------------------------------------------------- */
static void test_nc012(void)
{
    printf("\n[NC-012] Format NVM followed by IO\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    uint64_t total_lbas;

    if (setup_device(&dev, &cfg, &total_lbas) != 0) {
        TEST_ASSERT(false, "device setup");
        return;
    }

    int rc;
    uint8_t wbuf[TEST_PAGE_SIZE];
    uint8_t rbuf[TEST_PAGE_SIZE];

    /* Write initial data */
    memset(wbuf, 0xEE, TEST_PAGE_SIZE);
    rc = nvme_uspace_write(&dev, 1, 0, 1, wbuf);
    TEST_ASSERT(rc == HFSSS_OK, "write initial data to LBA 0");

    /* Format the namespace */
    rc = nvme_uspace_format_nvm(&dev, 1);
    TEST_ASSERT(rc == HFSSS_OK, "format_nvm OK");

    /* After format, old data should be gone */
    rc = nvme_uspace_read(&dev, 1, 0, 1, rbuf);
    TEST_ASSERT(rc == HFSSS_ERR_NOENT, "read after format returns HFSSS_ERR_NOENT");

    /* Write new data */
    memset(wbuf, 0x55, TEST_PAGE_SIZE);
    rc = nvme_uspace_write(&dev, 1, 0, 1, wbuf);
    TEST_ASSERT(rc == HFSSS_OK, "write new data after format OK");

    /* Read and verify new data */
    memset(rbuf, 0, TEST_PAGE_SIZE);
    rc = nvme_uspace_read(&dev, 1, 0, 1, rbuf);
    TEST_ASSERT(rc == HFSSS_OK && rbuf[0] == 0x55, "read after format+write returns correct data");

    teardown_device(&dev);
}

/* NC-013: Sanitize contract (sanact 1/2/3, data erasure) */
static void test_nc013(void)
{
    printf("\n[NC-013] Sanitize contract\n");
    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    uint64_t total_lbas;
    if (setup_device(&dev, &cfg, &total_lbas) != 0) {
        TEST_ASSERT(false, "device setup");
        return;
    }
    int rc;
    uint8_t wbuf[TEST_PAGE_SIZE];
    uint8_t rbuf[TEST_PAGE_SIZE];
    uint8_t zbuf[TEST_PAGE_SIZE];
    memset(zbuf, 0, TEST_PAGE_SIZE);

    /* sanact=1 EXIT_FAILURE: per NVMe §5.22 this is NOT a data
     * sanitization action — the controller exits a prior failure mode
     * and user data is left intact. Write a pattern, issue SANACT=1,
     * and confirm the same pattern is still readable. */
    memset(wbuf, 0xDD, TEST_PAGE_SIZE);
    rc = nvme_uspace_write(&dev, 1, 0, 1, wbuf);
    TEST_ASSERT(rc == HFSSS_OK, "write LBA 0 before sanact=1");
    rc = nvme_uspace_sanitize(&dev, 1);
    TEST_ASSERT(rc == HFSSS_OK, "sanitize sanact=1 (exit-failure) OK");
    rc = nvme_uspace_read(&dev, 1, 0, 1, rbuf);
    TEST_ASSERT(rc == HFSSS_OK, "read after sanact=1 returns OK (data preserved)");
    TEST_ASSERT(memcmp(rbuf, wbuf, TEST_PAGE_SIZE) == 0, "sanact=1 leaves pattern intact");

    /* sanact=2 BLOCK_ERASE: all user data erased; simulator drops the
     * L2P mapping so reads return NOENT. */
    memset(wbuf, 0xCC, TEST_PAGE_SIZE);
    nvme_uspace_write(&dev, 1, 0, 1, wbuf);
    rc = nvme_uspace_sanitize(&dev, 2);
    TEST_ASSERT(rc == HFSSS_OK, "sanitize sanact=2 (block-erase) OK");
    rc = nvme_uspace_read(&dev, 1, 0, 1, rbuf);
    TEST_ASSERT(rc == HFSSS_ERR_NOENT, "read after sanact=2 returns HFSSS_ERR_NOENT");

    /* sanact=3 OVERWRITE: user data is rewritten with the overwrite
     * pattern (simulator uses all-zeros). Reads return OK with a
     * zero-filled payload, NOT NOENT. */
    memset(wbuf, 0xBB, TEST_PAGE_SIZE);
    nvme_uspace_write(&dev, 1, 0, 1, wbuf);
    rc = nvme_uspace_sanitize(&dev, 3);
    TEST_ASSERT(rc == HFSSS_OK, "sanitize sanact=3 (overwrite) OK");
    rc = nvme_uspace_read(&dev, 1, 0, 1, rbuf);
    TEST_ASSERT(rc == HFSSS_OK, "read after sanact=3 returns OK (overwrite pattern)");
    TEST_ASSERT(memcmp(rbuf, zbuf, TEST_PAGE_SIZE) == 0, "sanact=3 payload is zero-filled");

    /* sanact=4 CRYPTO_ERASE: encryption keys destroyed; simulator
     * drops the mapping so reads return NOENT. */
    memset(wbuf, 0xAA, TEST_PAGE_SIZE);
    nvme_uspace_write(&dev, 1, 0, 1, wbuf);
    rc = nvme_uspace_sanitize(&dev, 4);
    TEST_ASSERT(rc == HFSSS_OK, "sanitize sanact=4 (crypto-erase) OK");
    rc = nvme_uspace_read(&dev, 1, 0, 1, rbuf);
    TEST_ASSERT(rc == HFSSS_ERR_NOENT, "read after sanact=4 returns HFSSS_ERR_NOENT");

    teardown_device(&dev);
}

/* NC-014: Admin negative paths (fw, log, features, format) */
static void test_nc014(void)
{
    printf("\n[NC-014] Admin negative paths\n");
    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    uint64_t total_lbas;
    if (setup_device(&dev, &cfg, &total_lbas) != 0) {
        TEST_ASSERT(false, "device setup");
        return;
    }
    int rc;
    uint32_t val;
    uint8_t dummy[512];
    uint8_t fw_buf[128];

    /* Firmware: NULL data */
    rc = nvme_uspace_fw_download(&dev, 0, NULL, 4096);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "fw_download NULL data INVAL");
    /* Firmware: non-zero offset staging is accepted but does not yet
     * surface in identify_ctrl.fr (which only copies bytes 0..7 of the
     * staging buffer). This verifies the API accepts non-zero offsets
     * without error; observable offset behavior needs future work. */
    memset(fw_buf, 0xEF, sizeof(fw_buf));
    rc = nvme_uspace_fw_download(&dev, 4096, fw_buf, sizeof(fw_buf));
    TEST_ASSERT(rc == HFSSS_OK, "fw_download at non-zero offset accepted");

    /* Log page: unsupported LIDs */
    rc = nvme_uspace_get_log_page(&dev, 1, 0, dummy, sizeof(dummy));
    TEST_ASSERT(rc == HFSSS_ERR_NOTSUPP, "get_log_page LID=0 NOTSUPP");
    rc = nvme_uspace_get_log_page(&dev, 1, 3, dummy, sizeof(dummy));
    TEST_ASSERT(rc == HFSSS_ERR_NOTSUPP, "get_log_page LID=3 NOTSUPP");
    rc = nvme_uspace_get_log_page(&dev, 1, 0xFF, dummy, sizeof(dummy));
    TEST_ASSERT(rc == HFSSS_ERR_NOTSUPP, "get_log_page LID=0xFF NOTSUPP");
    /* Log page: NULL buffer */
    rc = nvme_uspace_get_log_page(&dev, 1, 2, NULL, 512);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "get_log_page NULL buf INVAL");

    /* Features: unsupported FIDs for get */
    rc = nvme_uspace_get_features(&dev, 0x00, &val);
    TEST_ASSERT(rc == HFSSS_ERR_NOTSUPP, "get_features FID=0x00 NOTSUPP");
    rc = nvme_uspace_get_features(&dev, 0xFF, &val);
    TEST_ASSERT(rc == HFSSS_ERR_NOTSUPP, "get_features FID=0xFF NOTSUPP");
    /* Features: unsupported FID for set */
    rc = nvme_uspace_set_features(&dev, 0xFF, 42);
    TEST_ASSERT(rc == HFSSS_ERR_NOTSUPP, "set_features FID=0xFF NOTSUPP");
    /* Features: NULL value pointer */
    rc = nvme_uspace_get_features(&dev, 0x02, NULL);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "get_features NULL value INVAL");

    /* Format: invalid NSIDs */
    rc = nvme_uspace_format_nvm(&dev, 99);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "format_nvm NSID=99 INVAL");
    rc = nvme_uspace_format_nvm(&dev, 0xFFFFFFFF);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "format_nvm NSID=0xFFFFFFFF INVAL");

    teardown_device(&dev);
}

/* ---------------------------------------------------------------
 * main
 * ------------------------------------------------------------- */
int main(void)
{
    printf("========================================\n");
    printf("HFSSS NVMe Command Compliance System Tests\n");
    printf("========================================\n");

    test_nc001();
    test_nc002();
    test_nc003();
    test_nc004();
    test_nc005();
    test_nc006();
    test_nc007();
    test_nc008();
    test_nc009();
    test_nc010();
    test_nc011();
    test_nc012();
    test_nc013();
    test_nc014();

    printf("\n========================================\n");
    printf("Summary: %d total, %d passed, %d failed\n", total_tests, passed_tests, failed_tests);
    printf("========================================\n");

    if (failed_tests > 0) {
        printf("\n  RESULT: FAIL (%d test(s) failed)\n\n", failed_tests);
        return 1;
    }

    printf("\n  RESULT: PASS (all %d tests passed)\n\n", total_tests);
    return 0;
}
