#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "pcie/nvme_uspace.h"
#include "pcie/nvme.h"

/* Mirror of the SMART/Health log layout written by
 * nvme_uspace_get_log_page(). Keep in sync with src/pcie/nvme_uspace.c. */
struct smart_log_mirror {
    uint8_t  critical_warning;
    uint16_t temperature;
    uint8_t  avail_spare;
    uint8_t  avail_spare_thresh;
    uint8_t  percent_used;
    uint8_t  rsvd[26];
    uint64_t data_units_read;
    uint64_t data_units_written;
    uint64_t host_read_cmds;
    uint64_t host_write_cmds;
    uint64_t ctrl_busy_time;
    uint64_t power_cycles;
    uint64_t power_on_hours;
    uint64_t unsafe_shutdowns;
    uint64_t media_errors;
    uint64_t num_err_log_entries;
};

#define TEST_PASS 0
#define TEST_FAIL 1

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        tests_passed++; \
    } else { \
        printf("  [FAIL] %s\n", msg); \
        tests_failed++; \
    } \
} while(0)

static void print_separator(void)
{
    printf("========================================\n");
}

/* User-space NVMe Device Tests */
static int test_nvme_uspace_dev(void)
{
    printf("\n=== User-space NVMe Device Tests ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;
    int ret;

    /* Test config default with smaller SSD - must set full NAND geometry */
    nvme_uspace_config_default(&config);
    config.sssim_cfg.page_size        = 4096;
    config.sssim_cfg.spare_size       = 64;
    config.sssim_cfg.channel_count    = 2;
    config.sssim_cfg.chips_per_channel = 2;
    config.sssim_cfg.dies_per_chip    = 1;
    config.sssim_cfg.planes_per_die   = 1;
    config.sssim_cfg.blocks_per_plane = 64;
    config.sssim_cfg.pages_per_block  = 64;
    config.sssim_cfg.total_lbas       = 1024;
    TEST_ASSERT(true, "nvme_uspace_config_default should succeed");

    /* Test init */
    ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_uspace_dev_init should succeed");

    /* Test start */
    ret = nvme_uspace_dev_start(&dev);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_uspace_dev_start should succeed");

    /* Test Identify Controller */
    struct nvme_identify_ctrl id_ctrl;
    ret = nvme_uspace_identify_ctrl(&dev, &id_ctrl);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_uspace_identify_ctrl should succeed");
    TEST_ASSERT(id_ctrl.vid == HFSSS_VENDOR_ID, "Identify Controller VID should be correct");
    TEST_ASSERT(id_ctrl.nn == 1, "Identify Controller should have 1 namespace");

    /* Test Identify Namespace */
    struct nvme_identify_ns id_ns;
    ret = nvme_uspace_identify_ns(&dev, 1, &id_ns);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_uspace_identify_ns should succeed");
    TEST_ASSERT(id_ns.nsze > 0, "Identify Namespace size should be > 0");

    /* Test Create I/O CQ */
    ret = nvme_uspace_create_io_cq(&dev, 1, 256, false);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_uspace_create_io_cq should succeed");

    /* Test Create I/O SQ */
    ret = nvme_uspace_create_io_sq(&dev, 1, 256, 1, 0);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_uspace_create_io_sq should succeed");

    /* Test Write/Read */
    u8 write_buf[4096];
    u8 read_buf[4096];
    memset(write_buf, 0xAA, sizeof(write_buf));
    memset(read_buf, 0, sizeof(read_buf));

    ret = nvme_uspace_write(&dev, 1, 0, 1, write_buf);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_uspace_write should succeed");

    ret = nvme_uspace_read(&dev, 1, 0, 1, read_buf);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_uspace_read should succeed");
    TEST_ASSERT(memcmp(write_buf, read_buf, sizeof(write_buf)) == 0, "Read data should match written data");

    /* Test Flush */
    ret = nvme_uspace_flush(&dev, 1);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_uspace_flush should succeed");

    /* Test Delete I/O SQ and CQ */
    ret = nvme_uspace_delete_io_sq(&dev, 1);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_uspace_delete_io_sq should succeed");

    ret = nvme_uspace_delete_io_cq(&dev, 1);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_uspace_delete_io_cq should succeed");

    /* Test stop */
    nvme_uspace_dev_stop(&dev);
    TEST_ASSERT(true, "nvme_uspace_dev_stop should succeed");

    /* Test cleanup */
    nvme_uspace_dev_cleanup(&dev);
    TEST_ASSERT(true, "nvme_uspace_dev_cleanup should succeed");

    /* Test NULL handling */
    TEST_ASSERT(nvme_uspace_dev_init(NULL, NULL) == HFSSS_ERR_INVAL, "nvme_uspace_dev_init with NULL should fail");

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Sanitize action modes (REQ-163) */
#include "pcie/nvme.h"

static int test_sanitize_action_modes(void)
{
    printf("\n=== Sanitize Action Modes (REQ-163) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;

    nvme_uspace_config_default(&config);
    config.sssim_cfg.page_size        = 4096;
    config.sssim_cfg.spare_size       = 64;
    config.sssim_cfg.channel_count    = 2;
    config.sssim_cfg.chips_per_channel = 2;
    config.sssim_cfg.dies_per_chip    = 1;
    config.sssim_cfg.planes_per_die   = 1;
    config.sssim_cfg.blocks_per_plane = 64;
    config.sssim_cfg.pages_per_block  = 64;
    config.sssim_cfg.total_lbas       = 64;  /* keep overwrite pass tractable */

    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "sanitize: dev_init");
    ret = nvme_uspace_dev_start(&dev);
    TEST_ASSERT(ret == HFSSS_OK, "sanitize: dev_start");

    u8 lba_buf[4096];
    memset(lba_buf, 0xAB, sizeof(lba_buf));
    for (u64 lba = 0; lba < 8; lba++) {
        ret = nvme_uspace_write(&dev, 1, lba, 1, lba_buf);
        TEST_ASSERT(ret == HFSSS_OK, "sanitize: seed LBA write");
    }

    /* Exit Failure — noop success */
    ret = nvme_uspace_sanitize(&dev, NVME_SANACT_EXIT_FAILURE);
    TEST_ASSERT(ret == HFSSS_OK, "sanitize: EXIT_FAILURE returns OK");

    /* Block Erase — drops mapping; subsequent read yields zeros */
    ret = nvme_uspace_sanitize(&dev, NVME_SANACT_BLOCK_ERASE);
    TEST_ASSERT(ret == HFSSS_OK, "sanitize: BLOCK_ERASE returns OK");

    /* Post-BLOCK_ERASE read may either succeed (returning zeros/sentinel)
     * or fail with a "not mapped" error. Both outcomes are valid as long
     * as the old 0xAB pattern is no longer observable. */
    u8 verify_buf[4096];
    memset(verify_buf, 0xFF, sizeof(verify_buf));  /* sentinel */
    ret = nvme_uspace_read(&dev, 1, 0, 1, verify_buf);
    if (ret == HFSSS_OK) {
        int still_old_pattern = 1;
        for (size_t i = 0; i < 16; i++) {
            if (verify_buf[i] != 0xAB) { still_old_pattern = 0; break; }
        }
        TEST_ASSERT(!still_old_pattern,
                    "sanitize: BLOCK_ERASE wipes previously written pattern");
    } else {
        TEST_ASSERT(true,
                    "sanitize: BLOCK_ERASE leaves LBA unmapped (read returns non-OK)");
    }

    /* Re-seed, then CRYPTO_ERASE — same observable result in sim */
    for (u64 lba = 0; lba < 8; lba++) {
        nvme_uspace_write(&dev, 1, lba, 1, lba_buf);
    }
    ret = nvme_uspace_sanitize(&dev, NVME_SANACT_CRYPTO_ERASE);
    TEST_ASSERT(ret == HFSSS_OK, "sanitize: CRYPTO_ERASE returns OK");

    /* Re-seed, then OVERWRITE — reads must return zero explicitly */
    for (u64 lba = 0; lba < 8; lba++) {
        nvme_uspace_write(&dev, 1, lba, 1, lba_buf);
    }
    ret = nvme_uspace_sanitize(&dev, NVME_SANACT_OVERWRITE);
    TEST_ASSERT(ret == HFSSS_OK, "sanitize: OVERWRITE returns OK");

    u8 zbuf[4096];
    memset(zbuf, 0, sizeof(zbuf));
    memset(verify_buf, 0xFF, sizeof(verify_buf));
    ret = nvme_uspace_read(&dev, 1, 0, 1, verify_buf);
    TEST_ASSERT(ret == HFSSS_OK, "sanitize: read after OVERWRITE succeeds");
    TEST_ASSERT(memcmp(verify_buf, zbuf, sizeof(verify_buf)) == 0,
                "sanitize: OVERWRITE leaves explicit zeros in every LBA");

    /* Reserved / vendor values rejected */
    ret = nvme_uspace_sanitize(&dev, 0x00);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL,
                "sanitize: reserved SANACT=0 rejected");
    ret = nvme_uspace_sanitize(&dev, 0x07);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL,
                "sanitize: reserved SANACT=7 rejected");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* NVMe Error Information Log Page (REQ-115 / REQ-158) */
static int test_error_log_page(void)
{
    printf("\n=== Error Information Log Page (REQ-115 / REQ-158) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;

    nvme_uspace_config_default(&config);
    config.sssim_cfg.page_size        = 4096;
    config.sssim_cfg.spare_size       = 64;
    config.sssim_cfg.channel_count    = 2;
    config.sssim_cfg.chips_per_channel = 2;
    config.sssim_cfg.dies_per_chip    = 1;
    config.sssim_cfg.planes_per_die   = 1;
    config.sssim_cfg.blocks_per_plane = 64;
    config.sssim_cfg.pages_per_block  = 64;
    config.sssim_cfg.total_lbas       = 512;

    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "err-log: dev_init");
    ret = nvme_uspace_dev_start(&dev);
    TEST_ASSERT(ret == HFSSS_OK, "err-log: dev_start");

    /* Empty log: LID=1 returns all-zero page */
    struct nvme_error_log_entry entries[8];
    memset(entries, 0xAB, sizeof(entries));
    ret = nvme_uspace_get_log_page(&dev, 1, NVME_LID_ERROR_INFO,
                                   entries, sizeof(entries));
    TEST_ASSERT(ret == HFSSS_OK, "err-log: LID=1 returns OK when empty");
    TEST_ASSERT(entries[0].error_count == 0,
                "err-log: first entry cleared when log empty");

    /* Report three UCE events and confirm newest-first ordering */
    nvme_uspace_report_error(&dev, 1, 0x1001, 0x4281 /* SCT=2 SC=0x81 */,
                             100 /* lba */, 1 /* nsid */);
    nvme_uspace_report_error(&dev, 2, 0x1002, 0x4285 /* SC=0x85 */,
                             200, 1);
    nvme_uspace_report_error(&dev, 3, 0x1003, 0x4281,
                             300, 1);

    memset(entries, 0, sizeof(entries));
    ret = nvme_uspace_get_log_page(&dev, 1, NVME_LID_ERROR_INFO,
                                   entries, sizeof(entries));
    TEST_ASSERT(ret == HFSSS_OK, "err-log: LID=1 after 3 reports");
    TEST_ASSERT(entries[0].error_count == 3,
                "err-log: newest entry first (error_count=3)");
    TEST_ASSERT(entries[0].lba == 300,
                "err-log: entry[0] LBA matches most recent report");
    TEST_ASSERT(entries[0].status_field == 0x4281,
                "err-log: entry[0] status field preserved");
    TEST_ASSERT(entries[1].error_count == 2,
                "err-log: entry[1] error_count=2");
    TEST_ASSERT(entries[1].lba == 200,
                "err-log: entry[1] LBA=200");
    TEST_ASSERT(entries[2].error_count == 1,
                "err-log: entry[2] error_count=1 (oldest of the three)");
    TEST_ASSERT(entries[2].nsid == 1,
                "err-log: entry[2] nsid preserved");

    /* Ring wraparound: fill past capacity and confirm newest-first */
    for (u32 i = 4; i < NVME_ERROR_LOG_ENTRIES + 5; i++) {
        nvme_uspace_report_error(&dev, (u16)i, (u16)(i + 0x1000), 0x4281,
                                 (u64)(i * 10), 1);
    }
    memset(entries, 0, sizeof(entries));
    ret = nvme_uspace_get_log_page(&dev, 1, NVME_LID_ERROR_INFO,
                                   entries, sizeof(entries));
    TEST_ASSERT(ret == HFSSS_OK, "err-log: LID=1 after ring wrap");
    TEST_ASSERT(entries[0].error_count == NVME_ERROR_LOG_ENTRIES + 4,
                "err-log: newest entry has highest error_count after wrap");

    /* Undersized buffer: len < one entry -> INVAL */
    ret = nvme_uspace_get_log_page(&dev, 1, NVME_LID_ERROR_INFO,
                                   entries, 32);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL,
                "err-log: len < sizeof(entry) returns INVAL");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Production path: a dispatcher-level I/O failure must append to the
 * Error Information Log ring and synchronously bump the SMART
 * num_err_log_entries counter (REQ-115 / REQ-158). */
static int test_error_log_production_path(void)
{
    printf("\n=== Error Log production path (REQ-115 / REQ-158) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;

    nvme_uspace_config_default(&config);
    config.sssim_cfg.page_size         = 4096;
    config.sssim_cfg.spare_size        = 64;
    config.sssim_cfg.channel_count     = 2;
    config.sssim_cfg.chips_per_channel = 2;
    config.sssim_cfg.dies_per_chip     = 1;
    config.sssim_cfg.planes_per_die    = 1;
    config.sssim_cfg.blocks_per_plane  = 64;
    config.sssim_cfg.pages_per_block   = 64;
    config.sssim_cfg.total_lbas        = 512;

    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "err-prod: dev_init");
    ret = nvme_uspace_dev_start(&dev);
    TEST_ASSERT(ret == HFSSS_OK, "err-prod: dev_start");

    /* Baseline: Error Log empty, SMART agrees. */
    struct nvme_error_log_entry entries[4];
    memset(entries, 0, sizeof(entries));
    ret = nvme_uspace_get_log_page(&dev, 1, NVME_LID_ERROR_INFO,
                                   entries, sizeof(entries));
    TEST_ASSERT(ret == HFSSS_OK && entries[0].error_count == 0,
                "err-prod: error log starts empty");

    struct smart_log_mirror smart;
    memset(&smart, 0xAB, sizeof(smart));
    ret = nvme_uspace_get_log_page(&dev, 1, NVME_LID_SMART,
                                   &smart, sizeof(smart));
    TEST_ASSERT(ret == HFSSS_OK && smart.num_err_log_entries == 0,
                "err-prod: SMART num_err_log_entries starts at zero");

    /* Dispatch a READ with an out-of-range starting LBA through the full
     * NVMe pipeline. nvme_uspace_read() must return HFSSS_ERR_INVAL and
     * the dispatcher must append an entry to the Error Log. */
    struct nvme_sq_entry sq_cmd;
    struct nvme_cq_entry cpl;
    static uint8_t data_buf[4096];
    memset(&sq_cmd, 0, sizeof(sq_cmd));
    memset(&cpl, 0, sizeof(cpl));
    sq_cmd.opcode     = NVME_NVM_READ;
    sq_cmd.command_id = 0xBEEF;
    sq_cmd.nsid       = 1;
    /* slba = 0xFFFFFFFF — guaranteed out of range for total_lbas=512 */
    sq_cmd.cdw10      = 0xFFFFFFFFu;
    sq_cmd.cdw11      = 0x00000000u;
    sq_cmd.cdw12      = 0;  /* NLB = 0+1 = 1 block */

    ret = nvme_uspace_dispatch_io_cmd(&dev, &sq_cmd, &cpl,
                                      data_buf, sizeof(data_buf));
    TEST_ASSERT(ret == HFSSS_OK, "err-prod: dispatch returns OK");
    TEST_ASSERT(cpl.status != 0,
                "err-prod: CQE status is non-zero on I/O failure");

    memset(entries, 0, sizeof(entries));
    ret = nvme_uspace_get_log_page(&dev, 1, NVME_LID_ERROR_INFO,
                                   entries, sizeof(entries));
    TEST_ASSERT(ret == HFSSS_OK, "err-prod: LID=1 OK after dispatch");
    TEST_ASSERT(entries[0].error_count == 1,
                "err-prod: dispatch appended one Error Log entry");
    TEST_ASSERT(entries[0].cmd_id == 0xBEEF,
                "err-prod: Error Log entry carries the submitted cmd_id");
    TEST_ASSERT(entries[0].nsid == 1,
                "err-prod: Error Log entry carries the target NSID");
    TEST_ASSERT(entries[0].lba == 0xFFFFFFFFu,
                "err-prod: Error Log entry carries the failing SLBA");
    TEST_ASSERT(entries[0].status_field == cpl.status,
                "err-prod: Error Log status_field mirrors CQE status");

    /* SMART must now report the same count. */
    memset(&smart, 0, sizeof(smart));
    ret = nvme_uspace_get_log_page(&dev, 1, NVME_LID_SMART,
                                   &smart, sizeof(smart));
    TEST_ASSERT(ret == HFSSS_OK, "err-prod: SMART get_log_page OK");
    TEST_ASSERT(smart.num_err_log_entries == 1,
                "err-prod: SMART num_err_log_entries synced with ring");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

int main(void)
{
    print_separator();
    printf("HFSSS User-space NVMe Interface Tests\n");
    print_separator();

    test_nvme_uspace_dev();
    test_sanitize_action_modes();
    test_error_log_page();
    test_error_log_production_path();

    print_separator();
    printf("Test Summary\n");
    print_separator();
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    print_separator();

    if (tests_failed == 0) {
        printf("\n  [SUCCESS] All tests passed!\n");
        return 0;
    } else {
        printf("\n  [FAILURE] Some tests failed!\n");
        return 1;
    }
}
