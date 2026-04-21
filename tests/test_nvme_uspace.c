#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include "pcie/nvme_uspace.h"
#include "pcie/nvme.h"
#include "pcie/smart_monitor.h"
#include "common/thermal.h"

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

/* Shared small-SSD config for the fast tests; the nvme_uspace_config
 * default allocates a 1 TB geometry that blows out per-test memory. */
static void nvme_uspace_config_small(struct nvme_uspace_config *config)
{
    nvme_uspace_config_default(config);
    config->sssim_cfg.page_size         = 4096;
    config->sssim_cfg.spare_size        = 64;
    config->sssim_cfg.channel_count     = 2;
    config->sssim_cfg.chips_per_channel = 2;
    config->sssim_cfg.dies_per_chip     = 1;
    config->sssim_cfg.planes_per_die    = 1;
    config->sssim_cfg.blocks_per_plane  = 64;
    config->sssim_cfg.pages_per_block   = 64;
    config->sssim_cfg.total_lbas        = 512;
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

/* Host-initiated Telemetry Log Page (LID=0x07, REQ-174).
 * White-box: inject events directly into dev->telemetry so we know the
 * exact state, then assert the log page header + Data Area 1 reflect it. */
static int test_log_page_telemetry_host(void)
{
    printf("\n=== Telemetry Log Page LID=0x07 (REQ-174) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;
    nvme_uspace_config_small(&config);
    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "tel-host: dev_init");
    ret = nvme_uspace_dev_start(&dev);
    TEST_ASSERT(ret == HFSSS_OK, "tel-host: dev_start");

    /* Inject 3 telemetry events with distinct types + payloads. */
    uint8_t p1[TEL_PAYLOAD_LEN]; memset(p1, 0x11, sizeof(p1));
    uint8_t p2[TEL_PAYLOAD_LEN]; memset(p2, 0x22, sizeof(p2));
    uint8_t p3[TEL_PAYLOAD_LEN]; memset(p3, 0x33, sizeof(p3));
    telemetry_record(&dev.telemetry, TEL_EVENT_THERMAL, 1, p1, sizeof(p1));
    telemetry_record(&dev.telemetry, TEL_EVENT_GC,      2, p2, sizeof(p2));
    telemetry_record(&dev.telemetry, TEL_EVENT_WEAR,    0, p3, sizeof(p3));

    uint8_t buf[1024];
    memset(buf, 0xAB, sizeof(buf));
    ret = nvme_uspace_get_log_page(&dev, 0, NVME_LID_TELEMETRY_HOST,
                                   buf, sizeof(buf));
    TEST_ASSERT(ret == HFSSS_OK, "tel-host: LID=0x07 dispatch OK");

    struct nvme_telemetry_log_header *hdr =
        (struct nvme_telemetry_log_header *)buf;
    TEST_ASSERT(hdr->log_identifier == NVME_LID_TELEMETRY_HOST,
                "tel-host: header.log_identifier == 0x07");
    TEST_ASSERT(hdr->host_gen_number == 1,
                "tel-host: host_gen_number advanced to 1 on first read");
    TEST_ASSERT(hdr->data_area_1_last_block >= 1,
                "tel-host: data_area_1_last_block set (events present)");

    /* Data Area 1 starts at byte 512, events in newest-first order. */
    struct tel_event *events = (struct tel_event *)(buf + 512);
    TEST_ASSERT(events[0].type == TEL_EVENT_WEAR,
                "tel-host: newest event first (WEAR)");
    TEST_ASSERT(events[1].type == TEL_EVENT_GC,
                "tel-host: second-newest event is GC");
    TEST_ASSERT(events[2].type == TEL_EVENT_THERMAL,
                "tel-host: oldest event is THERMAL");
    TEST_ASSERT(events[0].payload[0] == 0x33,
                "tel-host: newest event payload preserved");

    /* Second read: gen_number must advance again per NVMe spec. */
    memset(buf, 0, sizeof(buf));
    ret = nvme_uspace_get_log_page(&dev, 0, NVME_LID_TELEMETRY_HOST,
                                   buf, sizeof(buf));
    TEST_ASSERT(ret == HFSSS_OK, "tel-host: second read OK");
    hdr = (struct nvme_telemetry_log_header *)buf;
    TEST_ASSERT(hdr->host_gen_number == 2,
                "tel-host: host_gen_number advances on every read");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Controller-initiated Telemetry Log Page (LID=0x08, REQ-175).
 * ctrl_data_available must be 0 with an empty ring and 1 after events
 * appear. The ctrl_gen_number only advances when NEW events were added
 * since the previous read. */
static int test_log_page_telemetry_ctrl(void)
{
    printf("\n=== Telemetry Log Page LID=0x08 (REQ-175) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;
    nvme_uspace_config_small(&config);
    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "tel-ctrl: dev_init");
    ret = nvme_uspace_dev_start(&dev);
    TEST_ASSERT(ret == HFSSS_OK, "tel-ctrl: dev_start");

    uint8_t buf[1024];

    /* Empty ring: ctrl_data_available must be 0 and gen stays at 0. */
    memset(buf, 0xAB, sizeof(buf));
    ret = nvme_uspace_get_log_page(&dev, 0, NVME_LID_TELEMETRY_CTRL,
                                   buf, sizeof(buf));
    TEST_ASSERT(ret == HFSSS_OK, "tel-ctrl: empty-ring dispatch OK");
    struct nvme_telemetry_log_header *hdr =
        (struct nvme_telemetry_log_header *)buf;
    TEST_ASSERT(hdr->log_identifier == NVME_LID_TELEMETRY_CTRL,
                "tel-ctrl: header.log_identifier == 0x08");
    TEST_ASSERT(hdr->ctrl_data_available == 0,
                "tel-ctrl: ctrl_data_available=0 when ring is empty");
    TEST_ASSERT(hdr->ctrl_gen_number == 0,
                "tel-ctrl: ctrl_gen_number=0 when never advanced");

    /* After 2 events: data_available=1, gen advances. */
    telemetry_record(&dev.telemetry, TEL_EVENT_ERROR, 2, NULL, 0);
    telemetry_record(&dev.telemetry, TEL_EVENT_SLA_VIOL, 3, NULL, 0);
    memset(buf, 0, sizeof(buf));
    ret = nvme_uspace_get_log_page(&dev, 0, NVME_LID_TELEMETRY_CTRL,
                                   buf, sizeof(buf));
    TEST_ASSERT(ret == HFSSS_OK, "tel-ctrl: after-events dispatch OK");
    hdr = (struct nvme_telemetry_log_header *)buf;
    TEST_ASSERT(hdr->ctrl_data_available == 1,
                "tel-ctrl: ctrl_data_available=1 once events present");
    uint8_t gen_after_first_events = hdr->ctrl_gen_number;
    TEST_ASSERT(gen_after_first_events == 1,
                "tel-ctrl: ctrl_gen_number advanced to 1");

    /* Re-read with NO new events: gen must NOT advance. */
    memset(buf, 0, sizeof(buf));
    ret = nvme_uspace_get_log_page(&dev, 0, NVME_LID_TELEMETRY_CTRL,
                                   buf, sizeof(buf));
    TEST_ASSERT(ret == HFSSS_OK, "tel-ctrl: idempotent re-read OK");
    hdr = (struct nvme_telemetry_log_header *)buf;
    TEST_ASSERT(hdr->ctrl_gen_number == gen_after_first_events,
                "tel-ctrl: ctrl_gen_number unchanged with no new events");

    /* Add 1 more event, re-read: gen advances. */
    telemetry_record(&dev.telemetry, TEL_EVENT_POWER, 0, NULL, 0);
    memset(buf, 0, sizeof(buf));
    ret = nvme_uspace_get_log_page(&dev, 0, NVME_LID_TELEMETRY_CTRL,
                                   buf, sizeof(buf));
    TEST_ASSERT(ret == HFSSS_OK, "tel-ctrl: after-new-event dispatch OK");
    hdr = (struct nvme_telemetry_log_header *)buf;
    TEST_ASSERT(hdr->ctrl_gen_number == (uint8_t)(gen_after_first_events + 1),
                "tel-ctrl: ctrl_gen_number advances on new events");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Vendor-specific Log Page (LID=0xC0, REQ-176): internal counter snapshot. */
static int test_log_page_vendor_counters(void)
{
    printf("\n=== Vendor Log Page LID=0xC0 (REQ-176) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;
    nvme_uspace_config_small(&config);
    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "tel-vendor: dev_init");
    ret = nvme_uspace_dev_start(&dev);
    TEST_ASSERT(ret == HFSSS_OK, "tel-vendor: dev_start");

    /* Inject events across different types so events_by_type can be
     * asserted with known ground truth. */
    telemetry_record(&dev.telemetry, TEL_EVENT_THERMAL, 0, NULL, 0);
    telemetry_record(&dev.telemetry, TEL_EVENT_THERMAL, 0, NULL, 0);
    telemetry_record(&dev.telemetry, TEL_EVENT_GC, 1, NULL, 0);
    telemetry_record(&dev.telemetry, TEL_EVENT_ERROR, 2, NULL, 0);
    telemetry_record(&dev.telemetry, TEL_EVENT_ERROR, 2, NULL, 0);
    telemetry_record(&dev.telemetry, TEL_EVENT_ERROR, 2, NULL, 0);

    struct nvme_vendor_log_counters counters;
    memset(&counters, 0xAB, sizeof(counters));
    ret = nvme_uspace_get_log_page(&dev, 0, NVME_LID_VENDOR_COUNTERS,
                                   &counters, sizeof(counters));
    TEST_ASSERT(ret == HFSSS_OK, "tel-vendor: LID=0xC0 dispatch OK");
    TEST_ASSERT(counters.magic == NVME_VENDOR_LOG_MAGIC,
                "tel-vendor: magic == NVME_VENDOR_LOG_MAGIC");
    TEST_ASSERT(counters.total_events == 6,
                "tel-vendor: total_events matches injected count");
    TEST_ASSERT(counters.events_in_ring == 6,
                "tel-vendor: events_in_ring matches live ring count");
    TEST_ASSERT(counters.events_by_type[TEL_EVENT_THERMAL] == 2,
                "tel-vendor: per-type counter: THERMAL=2");
    TEST_ASSERT(counters.events_by_type[TEL_EVENT_GC] == 1,
                "tel-vendor: per-type counter: GC=1");
    TEST_ASSERT(counters.events_by_type[TEL_EVENT_ERROR] == 3,
                "tel-vendor: per-type counter: ERROR=3");
    TEST_ASSERT(counters.events_by_type[TEL_EVENT_POWER] == 0,
                "tel-vendor: per-type counter: POWER stays zero");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Unknown LIDs still get NOTSUPP (spec-correct for unsupported pages). */
static int test_log_page_unknown_lid_notsupp(void)
{
    printf("\n=== Unknown Log Page LIDs -> NOTSUPP ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;
    nvme_uspace_config_small(&config);
    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "tel-unknown: dev_init");
    ret = nvme_uspace_dev_start(&dev);
    TEST_ASSERT(ret == HFSSS_OK, "tel-unknown: dev_start");

    uint8_t buf[512];
    ret = nvme_uspace_get_log_page(&dev, 0, 0x04, buf, sizeof(buf));
    TEST_ASSERT(ret == HFSSS_ERR_NOTSUPP,
                "tel-unknown: LID=0x04 returns NOTSUPP");
    ret = nvme_uspace_get_log_page(&dev, 0, 0x05, buf, sizeof(buf));
    TEST_ASSERT(ret == HFSSS_ERR_NOTSUPP,
                "tel-unknown: LID=0x05 returns NOTSUPP");
    ret = nvme_uspace_get_log_page(&dev, 0, 0xC1, buf, sizeof(buf));
    TEST_ASSERT(ret == HFSSS_ERR_NOTSUPP,
                "tel-unknown: LID=0xC1 returns NOTSUPP (vendor range but unwired)");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Scenario A: dispatch_admin_cmd on AER parks the CID (cpl->status
 * stays 0, cdw0 stays 0). A subsequent nvme_uspace_aer_post_event
 * delivers the event through the outstanding CID, returning the
 * completion the admin CQ layer should post. */
static int test_aer_dispatch_queues_then_post_event(void)
{
    printf("\n=== AER through admin dispatch: queue then event (REQ-063) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;
    nvme_uspace_config_small(&config);
    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "aer-disp: dev_init");
    ret = nvme_uspace_dev_start(&dev);
    TEST_ASSERT(ret == HFSSS_OK, "aer-disp: dev_start");

    struct nvme_sq_entry sq_cmd;
    struct nvme_cq_entry cpl;
    memset(&sq_cmd, 0, sizeof(sq_cmd));
    memset(&cpl, 0, sizeof(cpl));
    sq_cmd.opcode     = NVME_ADMIN_ASYNC_EVENT;
    sq_cmd.command_id = 0x1234;

    ret = nvme_uspace_dispatch_admin_cmd(&dev, &sq_cmd, &cpl, NULL, 0);
    TEST_ASSERT(ret == HFSSS_OK, "aer-disp: dispatch returns OK");
    TEST_ASSERT(cpl.status == 0,
                "aer-disp: cpl.status stays 0 (queued, no completion yet)");
    TEST_ASSERT(cpl.cdw0 == 0,
                "aer-disp: cpl.cdw0 stays 0 until event arrives");
    TEST_ASSERT(hal_aer_outstanding_count(&dev.aer) == 1,
                "aer-disp: AER parked in outstanding queue");

    /* Now controller reports an event — it should be delivered to
     * the pending AER, and post_event returns the completion details. */
    bool delivered = false;
    u16  cid_out   = 0;
    struct nvme_cq_entry event_cqe;
    memset(&event_cqe, 0, sizeof(event_cqe));
    ret = nvme_uspace_aer_post_event(&dev,
                                     NVME_AER_TYPE_NOTICE,
                                     NVME_AEI_NOTICE_FW_ACTIVATION_START,
                                     NVME_LID_FW_SLOT,
                                     &delivered, &cid_out, &event_cqe);
    TEST_ASSERT(ret == HFSSS_OK, "aer-disp: post_event OK");
    TEST_ASSERT(delivered == true,
                "aer-disp: event delivered to waiting AER");
    TEST_ASSERT(cid_out == 0x1234,
                "aer-disp: completion carries the originally submitted CID");
    u32 expected_dw0 = ((u32)NVME_AER_TYPE_NOTICE & 0x7) |
                       ((u32)NVME_AEI_NOTICE_FW_ACTIVATION_START << 8) |
                       ((u32)NVME_LID_FW_SLOT << 16);
    TEST_ASSERT(event_cqe.cdw0 == expected_dw0,
                "aer-disp: event CQE DW0 packs type/info/log");
    TEST_ASSERT(hal_aer_outstanding_count(&dev.aer) == 0,
                "aer-disp: outstanding drained after event delivery");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Scenario B: event already buffered -> the AER admin command
 * completes immediately, cpl carries the event DW0 and SC=SUCCESS. */
static int test_aer_dispatch_completes_immediately_when_event_pending(void)
{
    printf("\n=== AER through admin dispatch: event then submit (REQ-063) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;
    nvme_uspace_config_small(&config);
    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "aer-disp2: dev_init");
    ret = nvme_uspace_dev_start(&dev);
    TEST_ASSERT(ret == HFSSS_OK, "aer-disp2: dev_start");

    /* Buffer an event before any AER is submitted. */
    bool delivered = true;
    u16  cid_out   = 0;
    struct nvme_cq_entry ignore;
    memset(&ignore, 0, sizeof(ignore));
    ret = nvme_uspace_aer_post_event(&dev,
                                     NVME_AER_TYPE_SMART_HEALTH,
                                     NVME_AEI_SMART_SPARE_BELOW_THRESHOLD,
                                     NVME_LID_SMART,
                                     &delivered, &cid_out, &ignore);
    TEST_ASSERT(ret == HFSSS_OK, "aer-disp2: pre-event post_event OK");
    TEST_ASSERT(delivered == false,
                "aer-disp2: pre-event buffered (no AER waiting)");
    TEST_ASSERT(hal_aer_pending_count(&dev.aer) == 1,
                "aer-disp2: pending_count == 1");

    /* Now dispatch the AER — should complete with the buffered event. */
    struct nvme_sq_entry sq_cmd;
    struct nvme_cq_entry cpl;
    memset(&sq_cmd, 0, sizeof(sq_cmd));
    memset(&cpl, 0, sizeof(cpl));
    sq_cmd.opcode     = NVME_ADMIN_ASYNC_EVENT;
    sq_cmd.command_id = 0x7777;

    ret = nvme_uspace_dispatch_admin_cmd(&dev, &sq_cmd, &cpl, NULL, 0);
    TEST_ASSERT(ret == HFSSS_OK, "aer-disp2: dispatch OK");
    u16 expected_status = NVME_BUILD_STATUS(NVME_SC_SUCCESS,
                                            NVME_STATUS_TYPE_GENERIC);
    TEST_ASSERT(cpl.status == expected_status,
                "aer-disp2: cpl.status = SC=SUCCESS on immediate completion");
    u32 expected_dw0 = ((u32)NVME_AER_TYPE_SMART_HEALTH & 0x7) |
                       ((u32)NVME_AEI_SMART_SPARE_BELOW_THRESHOLD << 8) |
                       ((u32)NVME_LID_SMART << 16);
    TEST_ASSERT(cpl.cdw0 == expected_dw0,
                "aer-disp2: cpl.cdw0 packs type/info/log from buffered event");
    TEST_ASSERT(hal_aer_pending_count(&dev.aer) == 0,
                "aer-disp2: pending drained by immediate submit");
    TEST_ASSERT(hal_aer_outstanding_count(&dev.aer) == 0,
                "aer-disp2: outstanding stays empty (immediate completion)");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* REQ-178: thermal-threshold crossing records a telemetry event AND
 * consumes a waiting host AER with the temperature-threshold info. */
static int test_aer_notify_thermal_delivers_temperature_event(void)
{
    printf("\n=== AER notify_thermal wiring (REQ-178) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;
    nvme_uspace_config_small(&config);
    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "notify-therm: dev_init");
    nvme_uspace_dev_start(&dev);

    /* Host submits an AER so there's something waiting. */
    struct nvme_sq_entry sq_cmd;
    struct nvme_cq_entry cpl;
    memset(&sq_cmd, 0, sizeof(sq_cmd));
    memset(&cpl, 0, sizeof(cpl));
    sq_cmd.opcode     = NVME_ADMIN_ASYNC_EVENT;
    sq_cmd.command_id = 0xCAFE;
    nvme_uspace_dispatch_admin_cmd(&dev, &sq_cmd, &cpl, NULL, 0);
    TEST_ASSERT(hal_aer_outstanding_count(&dev.aer) == 1,
                "notify-therm: AER parked before notify");

    /* Fire a HEAVY thermal transition. */
    bool delivered = false;
    u16  cid_out   = 0;
    struct nvme_cq_entry event_cqe;
    memset(&event_cqe, 0, sizeof(event_cqe));
    ret = nvme_uspace_aer_notify_thermal(&dev, 3 /* HEAVY */,
                                         &delivered, &cid_out, &event_cqe);
    TEST_ASSERT(ret == HFSSS_OK, "notify-therm: notify OK");
    TEST_ASSERT(delivered == true,
                "notify-therm: thermal AER consumed the waiting AER");
    TEST_ASSERT(cid_out == 0xCAFE,
                "notify-therm: completion carries the submitted CID");
    u32 expected_dw0 = ((u32)NVME_AER_TYPE_SMART_HEALTH & 0x7) |
                       ((u32)NVME_AEI_SMART_TEMPERATURE_THRESHOLD << 8) |
                       ((u32)NVME_LID_SMART << 16);
    TEST_ASSERT(event_cqe.cdw0 == expected_dw0,
                "notify-therm: CQE DW0 encodes TEMPERATURE_THRESHOLD + SMART");
    TEST_ASSERT(hal_aer_outstanding_count(&dev.aer) == 0,
                "notify-therm: outstanding drained");

    /* Telemetry recorded even when no host is waiting. Reset dev. */
    nvme_uspace_dev_cleanup(&dev);
    TEST_ASSERT(nvme_uspace_dev_init(&dev, &config) == HFSSS_OK,
                "notify-therm: second dev_init");
    nvme_uspace_dev_start(&dev);
    ret = nvme_uspace_aer_notify_thermal(&dev, 4 /* SHUTDOWN */,
                                         &delivered, NULL, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "notify-therm: notify (no host) OK");
    TEST_ASSERT(delivered == false,
                "notify-therm: no host AER -> event buffered");
    TEST_ASSERT(dev.telemetry.count == 1,
                "notify-therm: telemetry event recorded even without host");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* REQ-178: remaining-life drop triggers NVM_SUBSYS_RELIABILITY AER. */
static int test_aer_notify_wear_delivers_reliability_event(void)
{
    printf("\n=== AER notify_wear wiring (REQ-178) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;
    nvme_uspace_config_small(&config);
    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "notify-wear: dev_init");
    nvme_uspace_dev_start(&dev);

    struct nvme_sq_entry sq_cmd;
    struct nvme_cq_entry cpl;
    memset(&sq_cmd, 0, sizeof(sq_cmd));
    memset(&cpl, 0, sizeof(cpl));
    sq_cmd.opcode     = NVME_ADMIN_ASYNC_EVENT;
    sq_cmd.command_id = 0xBEEF;
    nvme_uspace_dispatch_admin_cmd(&dev, &sq_cmd, &cpl, NULL, 0);

    bool delivered = false;
    u16  cid_out   = 0;
    struct nvme_cq_entry event_cqe;
    memset(&event_cqe, 0, sizeof(event_cqe));
    /* 4% remaining = critical. */
    ret = nvme_uspace_aer_notify_wear(&dev, 4,
                                      &delivered, &cid_out, &event_cqe);
    TEST_ASSERT(ret == HFSSS_OK, "notify-wear: notify OK");
    TEST_ASSERT(delivered == true, "notify-wear: waiting AER consumed");
    TEST_ASSERT(cid_out == 0xBEEF, "notify-wear: CID matches submit");
    u32 expected_dw0 = ((u32)NVME_AER_TYPE_SMART_HEALTH & 0x7) |
                       ((u32)NVME_AEI_SMART_NVM_SUBSYS_RELIABILITY << 8) |
                       ((u32)NVME_LID_SMART << 16);
    TEST_ASSERT(event_cqe.cdw0 == expected_dw0,
                "notify-wear: CQE DW0 encodes NVM_SUBSYS_RELIABILITY");
    TEST_ASSERT(dev.telemetry.count == 1,
                "notify-wear: wear event recorded to telemetry ring");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* REQ-178: spare below threshold posts SPARE_BELOW_THRESHOLD AER. */
static int test_aer_notify_spare_delivers_spare_event(void)
{
    printf("\n=== AER notify_spare wiring (REQ-178) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;
    nvme_uspace_config_small(&config);
    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "notify-spare: dev_init");
    nvme_uspace_dev_start(&dev);

    struct nvme_sq_entry sq_cmd;
    struct nvme_cq_entry cpl;
    memset(&sq_cmd, 0, sizeof(sq_cmd));
    memset(&cpl, 0, sizeof(cpl));
    sq_cmd.opcode     = NVME_ADMIN_ASYNC_EVENT;
    sq_cmd.command_id = 0xFEED;
    nvme_uspace_dispatch_admin_cmd(&dev, &sq_cmd, &cpl, NULL, 0);

    bool delivered = false;
    u16  cid_out   = 0;
    struct nvme_cq_entry event_cqe;
    memset(&event_cqe, 0, sizeof(event_cqe));
    ret = nvme_uspace_aer_notify_spare(&dev, 5 /* 5% spare */,
                                       &delivered, &cid_out, &event_cqe);
    TEST_ASSERT(ret == HFSSS_OK, "notify-spare: notify OK");
    TEST_ASSERT(delivered == true, "notify-spare: waiting AER consumed");
    TEST_ASSERT(cid_out == 0xFEED, "notify-spare: CID matches submit");
    u32 expected_dw0 = ((u32)NVME_AER_TYPE_SMART_HEALTH & 0x7) |
                       ((u32)NVME_AEI_SMART_SPARE_BELOW_THRESHOLD << 8) |
                       ((u32)NVME_LID_SMART << 16);
    TEST_ASSERT(event_cqe.cdw0 == expected_dw0,
                "notify-spare: CQE DW0 encodes SPARE_BELOW_THRESHOLD");
    TEST_ASSERT(dev.telemetry.count == 1,
                "notify-spare: spare event recorded to telemetry ring");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Helper: seed nsid=1 as ACTIVE in dev->keys so Opal lock has
 * something to transition from. Tests operate against nsid=1 since
 * that's the only NSID the I/O path currently accepts. */
static void seed_opal_ns_active(struct nvme_uspace_dev *dev, u32 nsid)
{
    dev->keys.entries[0].nsid  = nsid;
    dev->keys.entries[0].state = KEY_ACTIVE;
    dev->keys.crc32 = hfsss_crc32(&dev->keys,
                                  offsetof(struct key_table, crc32));
}

/* Build a Security Send / Receive frame: [opcode|nsid(LE)|auth]. */
static void build_opal_frame(u8 *out, u8 opcode, u32 nsid, const u8 *auth)
{
    memset(out, 0, OPAL_CMD_FRAME_LEN);
    out[0] = opcode;
    out[1] = (u8)(nsid & 0xFF);
    out[2] = (u8)((nsid >> 8) & 0xFF);
    out[3] = (u8)((nsid >> 16) & 0xFF);
    out[4] = (u8)((nsid >> 24) & 0xFF);
    if (auth) {
        memcpy(out + 5, auth, SEC_KEY_LEN);
    }
}

/* REQ-161: SECURITY_SEND LOCK flips the NS to locked and the I/O
 * path refuses subsequent reads/writes with SC=OP_DENIED. */
static int test_opal_security_send_lock_blocks_io(void)
{
    printf("\n=== Opal: SECURITY_SEND lock blocks I/O (REQ-161) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;
    nvme_uspace_config_small(&config);
    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "opal-io: dev_init");
    nvme_uspace_dev_start(&dev);
    seed_opal_ns_active(&dev, 1);

    /* Baseline: unlocked write succeeds. */
    u8 wbuf[4096];
    memset(wbuf, 0xC5, sizeof(wbuf));
    ret = nvme_uspace_write(&dev, 1, 0, 1, wbuf);
    TEST_ASSERT(ret == HFSSS_OK, "opal-io: baseline write OK while unlocked");

    /* SECURITY_SEND LOCK. */
    u8 frame[OPAL_CMD_FRAME_LEN];
    build_opal_frame(frame, OPAL_CMD_LOCK, 1, NULL);
    struct nvme_sq_entry sq; struct nvme_cq_entry cpl;
    memset(&sq, 0, sizeof(sq)); memset(&cpl, 0, sizeof(cpl));
    sq.opcode     = NVME_ADMIN_SECURITY_SEND;
    sq.command_id = 0x0001;
    ret = nvme_uspace_dispatch_admin_cmd(&dev, &sq, &cpl,
                                         frame, sizeof(frame));
    TEST_ASSERT(ret == HFSSS_OK, "opal-io: SECURITY_SEND dispatch OK");
    TEST_ASSERT(cpl.status == 0, "opal-io: LOCK completes with SC=SUCCESS");
    TEST_ASSERT(opal_is_locked(&dev.keys, 1) == true,
                "opal-io: namespace now locked");

    /* Both read and write must fail with HFSSS_ERR_AUTH at the API
     * layer (maps to SC=0x16 at the CQE layer). */
    u8 rbuf[4096]; memset(rbuf, 0, sizeof(rbuf));
    ret = nvme_uspace_read(&dev, 1, 0, 1, rbuf);
    TEST_ASSERT(ret == HFSSS_ERR_AUTH,
                "opal-io: read returns ERR_AUTH on locked NS");
    ret = nvme_uspace_write(&dev, 1, 0, 1, wbuf);
    TEST_ASSERT(ret == HFSSS_ERR_AUTH,
                "opal-io: write returns ERR_AUTH on locked NS");

    /* Dispatch a READ and check the CQE carries SC=0x16 (OP_DENIED). */
    struct nvme_sq_entry rdsq; struct nvme_cq_entry rdcpl;
    memset(&rdsq, 0, sizeof(rdsq)); memset(&rdcpl, 0, sizeof(rdcpl));
    rdsq.opcode     = NVME_NVM_READ;
    rdsq.nsid       = 1;
    rdsq.command_id = 0xABCD;
    rdsq.cdw10      = 0; rdsq.cdw11 = 0; rdsq.cdw12 = 0;
    ret = nvme_uspace_dispatch_io_cmd(&dev, &rdsq, &rdcpl,
                                      rbuf, sizeof(rbuf));
    TEST_ASSERT(ret == HFSSS_OK, "opal-io: dispatch_io returns OK envelope");
    u16 expected_sc = NVME_BUILD_STATUS(NVME_SC_OP_DENIED,
                                        NVME_STATUS_TYPE_GENERIC);
    TEST_ASSERT(rdcpl.status == expected_sc,
                "opal-io: CQE carries SC=0x16 (OP_DENIED) on locked read");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* REQ-161: SECURITY_SEND UNLOCK with correct auth restores I/O. */
static int test_opal_security_send_unlock_restores_io(void)
{
    printf("\n=== Opal: SECURITY_SEND unlock restores I/O (REQ-161) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;
    nvme_uspace_config_small(&config);
    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "opal-unlock: dev_init");
    nvme_uspace_dev_start(&dev);
    seed_opal_ns_active(&dev, 1);

    /* Lock first. */
    u8 frame[OPAL_CMD_FRAME_LEN];
    build_opal_frame(frame, OPAL_CMD_LOCK, 1, NULL);
    struct nvme_sq_entry sq; struct nvme_cq_entry cpl;
    memset(&sq, 0, sizeof(sq)); memset(&cpl, 0, sizeof(cpl));
    sq.opcode = NVME_ADMIN_SECURITY_SEND;
    nvme_uspace_dispatch_admin_cmd(&dev, &sq, &cpl, frame, sizeof(frame));
    TEST_ASSERT(opal_is_locked(&dev.keys, 1) == true,
                "opal-unlock: baseline locked");

    /* Derive the correct auth from the device MK (all zeros). */
    u8 correct_auth[SEC_KEY_LEN];
    opal_derive_auth(dev.opal_mk, 1, correct_auth);

    build_opal_frame(frame, OPAL_CMD_UNLOCK, 1, correct_auth);
    memset(&cpl, 0, sizeof(cpl));
    ret = nvme_uspace_dispatch_admin_cmd(&dev, &sq, &cpl,
                                         frame, sizeof(frame));
    TEST_ASSERT(ret == HFSSS_OK, "opal-unlock: dispatch OK");
    TEST_ASSERT(cpl.status == 0,
                "opal-unlock: UNLOCK with correct auth returns SUCCESS");
    TEST_ASSERT(opal_is_locked(&dev.keys, 1) == false,
                "opal-unlock: namespace now unlocked");

    /* Read / write recover. */
    u8 wbuf[4096]; memset(wbuf, 0x66, sizeof(wbuf));
    u8 rbuf[4096]; memset(rbuf, 0, sizeof(rbuf));
    ret = nvme_uspace_write(&dev, 1, 0, 1, wbuf);
    TEST_ASSERT(ret == HFSSS_OK, "opal-unlock: write recovers after unlock");
    ret = nvme_uspace_read(&dev, 1, 0, 1, rbuf);
    TEST_ASSERT(ret == HFSSS_OK, "opal-unlock: read recovers after unlock");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* REQ-161: UNLOCK with the wrong auth returns SC=OP_DENIED and the
 * namespace stays locked. */
static int test_opal_security_send_wrong_auth_keeps_locked(void)
{
    printf("\n=== Opal: wrong-auth unlock rejected (REQ-161) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;
    nvme_uspace_config_small(&config);
    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "opal-wrong: dev_init");
    nvme_uspace_dev_start(&dev);
    seed_opal_ns_active(&dev, 1);

    u8 frame[OPAL_CMD_FRAME_LEN];
    build_opal_frame(frame, OPAL_CMD_LOCK, 1, NULL);
    struct nvme_sq_entry sq; struct nvme_cq_entry cpl;
    memset(&sq, 0, sizeof(sq)); memset(&cpl, 0, sizeof(cpl));
    sq.opcode = NVME_ADMIN_SECURITY_SEND;
    nvme_uspace_dispatch_admin_cmd(&dev, &sq, &cpl, frame, sizeof(frame));

    u8 wrong_auth[SEC_KEY_LEN];
    memset(wrong_auth, 0xFF, sizeof(wrong_auth));
    build_opal_frame(frame, OPAL_CMD_UNLOCK, 1, wrong_auth);
    memset(&cpl, 0, sizeof(cpl));
    ret = nvme_uspace_dispatch_admin_cmd(&dev, &sq, &cpl,
                                         frame, sizeof(frame));
    TEST_ASSERT(ret == HFSSS_OK, "opal-wrong: dispatch OK");
    u16 expected_sc = NVME_BUILD_STATUS(NVME_SC_OP_DENIED,
                                        NVME_STATUS_TYPE_GENERIC);
    TEST_ASSERT(cpl.status == expected_sc,
                "opal-wrong: wrong-auth returns SC=OP_DENIED");
    TEST_ASSERT(opal_is_locked(&dev.keys, 1) == true,
                "opal-wrong: NS still locked after rejection");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* REQ-161: SECURITY_RECV STATUS reflects current lock state. */
static int test_opal_security_recv_reports_lock_state(void)
{
    printf("\n=== Opal: SECURITY_RECV STATUS reports lock (REQ-161) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;
    nvme_uspace_config_small(&config);
    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "opal-recv: dev_init");
    nvme_uspace_dev_start(&dev);
    seed_opal_ns_active(&dev, 1);

    u8 frame[OPAL_CMD_FRAME_LEN];
    struct nvme_sq_entry sq; struct nvme_cq_entry cpl;

    /* Initial: unlocked. */
    build_opal_frame(frame, OPAL_CMD_STATUS, 1, NULL);
    memset(&sq, 0, sizeof(sq)); memset(&cpl, 0, sizeof(cpl));
    sq.opcode = NVME_ADMIN_SECURITY_RECV;
    ret = nvme_uspace_dispatch_admin_cmd(&dev, &sq, &cpl,
                                         frame, sizeof(frame));
    TEST_ASSERT(ret == HFSSS_OK, "opal-recv: STATUS dispatch OK");
    TEST_ASSERT(cpl.status == 0, "opal-recv: STATUS SUCCESS");
    TEST_ASSERT(frame[5] == 0, "opal-recv: STATUS byte=0 when unlocked");

    /* Lock then re-query. */
    u8 lock_frame[OPAL_CMD_FRAME_LEN];
    build_opal_frame(lock_frame, OPAL_CMD_LOCK, 1, NULL);
    struct nvme_sq_entry lsq; struct nvme_cq_entry lcpl;
    memset(&lsq, 0, sizeof(lsq)); memset(&lcpl, 0, sizeof(lcpl));
    lsq.opcode = NVME_ADMIN_SECURITY_SEND;
    nvme_uspace_dispatch_admin_cmd(&dev, &lsq, &lcpl,
                                   lock_frame, sizeof(lock_frame));

    build_opal_frame(frame, OPAL_CMD_STATUS, 1, NULL);
    memset(&cpl, 0, sizeof(cpl));
    ret = nvme_uspace_dispatch_admin_cmd(&dev, &sq, &cpl,
                                         frame, sizeof(frame));
    TEST_ASSERT(ret == HFSSS_OK, "opal-recv: second STATUS dispatch OK");
    TEST_ASSERT(frame[5] == 1, "opal-recv: STATUS byte=1 when locked");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* The default namespace (nsid=1, matching Identify Controller's
 * NN=1) must be auto-registered into dev->keys at init. Otherwise
 * SECURITY_SEND/LOCK on nsid=1 returns SC=INVALID_FIELD because
 * key_table_init leaves every slot KEY_EMPTY, making Opal
 * unreachable without a test helper that patches dev->keys. */
static int test_opal_default_ns_registered_at_init(void)
{
    printf("\n=== Opal: default ns auto-registered at init ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;
    nvme_uspace_config_small(&config);
    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "ns-seed: dev_init");
    nvme_uspace_dev_start(&dev);

    /* Without any manual seed, nsid=1 must already be present in the
     * key table as KEY_ACTIVE so Opal LOCK can transition it. */
    TEST_ASSERT(dev.keys.entries[0].nsid == 1,
                "ns-seed: entries[0].nsid == 1");
    TEST_ASSERT(dev.keys.entries[0].state == KEY_ACTIVE,
                "ns-seed: entries[0].state == KEY_ACTIVE");
    TEST_ASSERT(opal_is_locked(&dev.keys, 1) == false,
                "ns-seed: nsid=1 reports unlocked");

    /* SECURITY_SEND LOCK on nsid=1 now completes with SC=SUCCESS. */
    u8 frame[OPAL_CMD_FRAME_LEN];
    build_opal_frame(frame, OPAL_CMD_LOCK, 1, NULL);
    struct nvme_sq_entry sq; struct nvme_cq_entry cpl;
    memset(&sq, 0, sizeof(sq)); memset(&cpl, 0, sizeof(cpl));
    sq.opcode     = NVME_ADMIN_SECURITY_SEND;
    sq.command_id = 0x0101;
    ret = nvme_uspace_dispatch_admin_cmd(&dev, &sq, &cpl,
                                         frame, sizeof(frame));
    TEST_ASSERT(ret == HFSSS_OK, "ns-seed: dispatch envelope OK");
    TEST_ASSERT(cpl.status == 0,
                "ns-seed: LOCK completes with SC=SUCCESS (not INVALID_FIELD)");
    TEST_ASSERT(opal_is_locked(&dev.keys, 1) == true,
                "ns-seed: nsid=1 now locked after LOCK");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* SMART (LID=0x02) must reflect the live thermal / spare /
 * percent_used state set by the AER notifier bridges. A hard-coded
 * payload would contradict the async event that told the host to
 * poll — the host would see a healthy-looking SMART right after a
 * TEMPERATURE_THRESHOLD AER. */
static int test_smart_reflects_live_state_after_notify(void)
{
    printf("\n=== SMART reflects live state after notify ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;
    nvme_uspace_config_small(&config);
    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "smart-live: dev_init");
    nvme_uspace_dev_start(&dev);

    /* Baseline SMART. */
    struct smart_log_mirror smart;
    memset(&smart, 0, sizeof(smart));
    ret = nvme_uspace_get_log_page(&dev, 1, NVME_LID_SMART,
                                   &smart, sizeof(smart));
    TEST_ASSERT(ret == HFSSS_OK, "smart-live: baseline get_log_page OK");
    TEST_ASSERT(smart.temperature == 300,
                "smart-live: baseline temperature=300 K (nominal)");
    TEST_ASSERT(smart.avail_spare == 100,
                "smart-live: baseline avail_spare=100%");
    TEST_ASSERT(smart.percent_used == 0,
                "smart-live: baseline percent_used=0%");
    TEST_ASSERT(smart.critical_warning == 0,
                "smart-live: baseline critical_warning clear");

    /* Drive a HEAVY thermal notify -> SMART temperature + critical bit
     * flip to the elevated mapping. */
    bool delivered;
    ret = nvme_uspace_aer_notify_thermal(&dev, 3 /* HEAVY */,
                                         &delivered, NULL, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "smart-live: notify_thermal(HEAVY) OK");
    memset(&smart, 0, sizeof(smart));
    nvme_uspace_get_log_page(&dev, 1, NVME_LID_SMART,
                             &smart, sizeof(smart));
    TEST_ASSERT(smart.temperature == 358,
                "smart-live: temperature raised to HEAVY (85C = 358 K)");
    TEST_ASSERT((smart.critical_warning & 0x02) != 0,
                "smart-live: critical_warning bit1 (temp threshold) set");

    /* Drive a wear notify at 4% remaining -> percent_used=96 and
     * reliability-degraded bit set. */
    ret = nvme_uspace_aer_notify_wear(&dev, 4,
                                      &delivered, NULL, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "smart-live: notify_wear(4%) OK");
    memset(&smart, 0, sizeof(smart));
    nvme_uspace_get_log_page(&dev, 1, NVME_LID_SMART,
                             &smart, sizeof(smart));
    TEST_ASSERT(smart.percent_used == 96,
                "smart-live: percent_used = 100 - remaining_life_pct");
    TEST_ASSERT((smart.critical_warning & 0x04) != 0,
                "smart-live: critical_warning bit2 (reliability) set");

    /* Drive a spare notify at 5% -> avail_spare=5 and spare-below bit. */
    ret = nvme_uspace_aer_notify_spare(&dev, 5,
                                       &delivered, NULL, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "smart-live: notify_spare(5%) OK");
    memset(&smart, 0, sizeof(smart));
    nvme_uspace_get_log_page(&dev, 1, NVME_LID_SMART,
                             &smart, sizeof(smart));
    TEST_ASSERT(smart.avail_spare == 5,
                "smart-live: avail_spare matches notified value");
    TEST_ASSERT((smart.critical_warning & 0x01) != 0,
                "smart-live: critical_warning bit0 (spare) set");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Keys lock stress: spawn a reader thread hammering nvme_uspace_read
 * AND a STATUS-poller thread hammering SECURITY_RECV STATUS while the
 * main thread flips LOCK / UNLOCK via SECURITY_SEND. The dev->keys_lock
 * must serialize all three paths. Without it the reader could see
 * stale "unlocked" and still complete I/O after the host believes
 * the namespace is locked; or the STATUS byte could tear between the
 * opal_is_locked probe and memory store. */
struct keys_race_args {
    struct nvme_uspace_dev *dev;
    atomic_bool stop;
    u64 reads_total;
    u64 reads_auth;
    u64 status_polls;
    u64 status_locked;
};

static void *keys_race_reader(void *arg)
{
    struct keys_race_args *a = (struct keys_race_args *)arg;
    u8 buf[4096];
    while (!atomic_load(&a->stop)) {
        int rc = nvme_uspace_read(a->dev, 1, 0, 1, buf);
        a->reads_total++;
        if (rc == HFSSS_ERR_AUTH) a->reads_auth++;
    }
    return NULL;
}

static void *keys_race_status_poller(void *arg)
{
    struct keys_race_args *a = (struct keys_race_args *)arg;
    u8 frame[OPAL_CMD_FRAME_LEN];
    struct nvme_sq_entry sq;
    struct nvme_cq_entry cpl;
    while (!atomic_load(&a->stop)) {
        memset(&sq, 0, sizeof(sq));
        memset(&cpl, 0, sizeof(cpl));
        sq.opcode = NVME_ADMIN_SECURITY_RECV;
        build_opal_frame(frame, OPAL_CMD_STATUS, 1, NULL);
        int dr = nvme_uspace_dispatch_admin_cmd(a->dev, &sq, &cpl,
                                                frame, sizeof(frame));
        /* Any dispatch glitch here would indicate keys_lock didn't
         * cover the STATUS path. Bail out without mutating counters
         * so the main test surfaces the stuck counter. */
        if (dr != HFSSS_OK || cpl.status != 0) {
            continue;
        }
        a->status_polls++;
        if (frame[5] == 1) a->status_locked++;
    }
    return NULL;
}

static int test_keys_lock_concurrent_lock_and_read(void)
{
    printf("\n=== Keys lock: concurrent LOCK/UNLOCK + read + STATUS stay coherent ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    nvme_uspace_config_small(&cfg);
    int ret = nvme_uspace_dev_init(&dev, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "keys-race: dev_init");
    if (ret != HFSSS_OK) {
        /* Hard guard — without a device the threads would segfault. */
        return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
    }
    nvme_uspace_dev_start(&dev);

    struct keys_race_args args = { .dev = &dev };
    atomic_store(&args.stop, false);

    pthread_t reader_t;
    int rc_r = pthread_create(&reader_t, NULL, keys_race_reader, &args);
    TEST_ASSERT(rc_r == 0, "keys-race: reader thread spawned");
    pthread_t status_t;
    int rc_s = pthread_create(&status_t, NULL, keys_race_status_poller, &args);
    TEST_ASSERT(rc_s == 0, "keys-race: status poller thread spawned");
    if (rc_r != 0 || rc_s != 0) {
        atomic_store(&args.stop, true);
        if (rc_r == 0) pthread_join(reader_t, NULL);
        if (rc_s == 0) pthread_join(status_t, NULL);
        nvme_uspace_dev_stop(&dev);
        nvme_uspace_dev_cleanup(&dev);
        return TEST_FAIL;
    }

    u8 frame[OPAL_CMD_FRAME_LEN];
    u8 auth[SEC_KEY_LEN];
    opal_derive_auth(dev.opal_mk, 1, auth);
    struct nvme_sq_entry sq;
    struct nvme_cq_entry cpl;

    /* 1000 iterations of LOCK -> short sleep -> UNLOCK. Every
     * dispatch is status-checked — a non-zero cpl.status would
     * indicate the admin path hit an error under contention. */
    u32 send_ok = 0;
    struct timespec short_sleep = { .tv_sec = 0, .tv_nsec = 1000 }; /* 1us */
    for (int i = 0; i < 1000; i++) {
        memset(&sq, 0, sizeof(sq));
        memset(&cpl, 0, sizeof(cpl));
        sq.opcode = NVME_ADMIN_SECURITY_SEND;
        build_opal_frame(frame, OPAL_CMD_LOCK, 1, NULL);
        int dr1 = nvme_uspace_dispatch_admin_cmd(&dev, &sq, &cpl,
                                                 frame, sizeof(frame));
        if (dr1 == HFSSS_OK && cpl.status == 0) send_ok++;

        nanosleep(&short_sleep, NULL);

        memset(&cpl, 0, sizeof(cpl));
        build_opal_frame(frame, OPAL_CMD_UNLOCK, 1, auth);
        int dr2 = nvme_uspace_dispatch_admin_cmd(&dev, &sq, &cpl,
                                                 frame, sizeof(frame));
        if (dr2 == HFSSS_OK && cpl.status == 0) send_ok++;
    }

    atomic_store(&args.stop, true);
    pthread_join(reader_t, NULL);
    pthread_join(status_t, NULL);

    /* Every LOCK + every UNLOCK must have completed cleanly under
     * the lock. send_ok counts 2 completions per iteration. */
    TEST_ASSERT(send_ok == 2000,
                "keys-race: every SECURITY_SEND dispatch succeeded");
    TEST_ASSERT(args.reads_total > 0,
                "keys-race: reader made progress");
    TEST_ASSERT(args.reads_auth > 0,
                "keys-race: reader observed at least one locked state");
    TEST_ASSERT(args.status_polls > 0,
                "keys-race: status poller made progress");
    TEST_ASSERT(args.status_locked > 0,
                "keys-race: status poller observed at least one locked state");
    TEST_ASSERT(opal_is_locked(&dev.keys, 1) == false,
                "keys-race: final state is unlocked (ended with UNLOCK)");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ------------------------------------------------------------------
 * REQ-178: SMART monitor runtime producer. Tests use synchronous
 * smart_monitor_poll_once() to avoid thread-timing flakiness. A
 * separate start/stop test proves the thread path itself works.
 * ------------------------------------------------------------------ */
struct smart_mock_src {
    u8 thermal;
    u8 remaining_life;
    u8 spare;
};

static u8 mock_get_thermal(void *ctx)
{
    return ((struct smart_mock_src *)ctx)->thermal;
}
static u8 mock_get_rlp(void *ctx)
{
    return ((struct smart_mock_src *)ctx)->remaining_life;
}
static u8 mock_get_spare(void *ctx)
{
    return ((struct smart_mock_src *)ctx)->spare;
}

static int test_smart_monitor_poll_fires_aer_on_threshold_cross(void)
{
    printf("\n=== SMART monitor: poll -> AER on threshold cross (REQ-178) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    nvme_uspace_config_small(&cfg);
    int ret = nvme_uspace_dev_init(&dev, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "monitor: dev_init");
    nvme_uspace_dev_start(&dev);

    struct smart_mock_src src = {
        .thermal = 0, .remaining_life = 100, .spare = 100,
    };
    struct smart_monitor mon;
    struct smart_monitor_config mcfg = {
        .dev                = &dev,
        .poll_interval_ms   = 0,   /* manual poll via poll_once */
        .get_thermal        = mock_get_thermal,
        .get_remaining_life = mock_get_rlp,
        .get_spare          = mock_get_spare,
        .cb_ctx             = &src,
    };
    ret = smart_monitor_init(&mon, &mcfg);
    TEST_ASSERT(ret == HFSSS_OK, "monitor: init");

    /* Baseline poll at nominal values: no AER. */
    smart_monitor_poll_once(&mon);
    TEST_ASSERT(mon.notify_count_thermal == 0,
                "monitor: no thermal AER at nominal");
    TEST_ASSERT(mon.notify_count_wear == 0,
                "monitor: no wear AER at 100%% life");
    TEST_ASSERT(mon.notify_count_spare == 0,
                "monitor: no spare AER at 100%% spare");

    /* Temperature jumps to HEAVY: one thermal AER, dev state stamped. */
    src.thermal = THERMAL_LEVEL_HEAVY;
    smart_monitor_poll_once(&mon);
    TEST_ASSERT(mon.notify_count_thermal == 1,
                "monitor: 1 thermal AER after rise to HEAVY");
    TEST_ASSERT(dev.thermal_level == THERMAL_LEVEL_HEAVY,
                "monitor: dev.thermal_level updated");

    /* Same level on next poll: no duplicate AER. */
    smart_monitor_poll_once(&mon);
    TEST_ASSERT(mon.notify_count_thermal == 1,
                "monitor: no duplicate thermal AER at same level");

    /* Cooling back to NONE should also emit (level changed). */
    src.thermal = THERMAL_LEVEL_NONE;
    smart_monitor_poll_once(&mon);
    TEST_ASSERT(mon.notify_count_thermal == 2,
                "monitor: thermal AER on cool-down edge too");

    /* Wear drops to 80%% life (20%% used -> bucket 2): emit. */
    src.remaining_life = 80;
    smart_monitor_poll_once(&mon);
    TEST_ASSERT(mon.notify_count_wear == 1,
                "monitor: 1 wear AER after 20%% used");

    /* Further drop to 75%% (still bucket 2 used): no new AER. */
    src.remaining_life = 75;
    smart_monitor_poll_once(&mon);
    TEST_ASSERT(mon.notify_count_wear == 1,
                "monitor: no new wear AER within same bucket");

    /* Drop to 70%% (bucket 3 used): new AER. */
    src.remaining_life = 70;
    smart_monitor_poll_once(&mon);
    TEST_ASSERT(mon.notify_count_wear == 2,
                "monitor: 2nd wear AER after crossing 30%% used");

    /* Spare drops to 5%% (bucket 0): emit spare AER. */
    src.spare = 5;
    smart_monitor_poll_once(&mon);
    TEST_ASSERT(mon.notify_count_spare == 1,
                "monitor: spare AER after crossing 10%% watermark");

    /* Spare stays at 5%%: no new AER. */
    smart_monitor_poll_once(&mon);
    TEST_ASSERT(mon.notify_count_spare == 1,
                "monitor: no duplicate spare AER when unchanged");

    smart_monitor_cleanup(&mon);
    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* A device that enumerates already-degraded must fire AERs on the
 * very first poll, not stay silent until a later change. A
 * have_baseline latch that skipped the first event would mask a
 * critical boot-time state from the host. */
static int test_smart_monitor_degraded_at_start(void)
{
    printf("\n=== SMART monitor: degraded-at-start fires AER on first poll ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    nvme_uspace_config_small(&cfg);
    int ret = nvme_uspace_dev_init(&dev, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "degraded: dev_init");
    nvme_uspace_dev_start(&dev);

    /* Device boots up already hot and worn with low spare. Each axis
     * is non-nominal vs the init seed (thermal 0 / wear_bucket 0 /
     * spare_bucket 10), so all three notifiers should fire on the
     * very first poll. */
    struct smart_mock_src src = {
        .thermal = THERMAL_LEVEL_MODERATE,
        .remaining_life = 50,  /* wear_bucket 5 used */
        .spare = 5,            /* spare_bucket 0 */
    };
    struct smart_monitor mon;
    struct smart_monitor_config mcfg = {
        .dev                = &dev,
        .poll_interval_ms   = 0,
        .get_thermal        = mock_get_thermal,
        .get_remaining_life = mock_get_rlp,
        .get_spare          = mock_get_spare,
        .cb_ctx             = &src,
    };
    ret = smart_monitor_init(&mon, &mcfg);
    TEST_ASSERT(ret == HFSSS_OK, "degraded: init");

    smart_monitor_poll_once(&mon);
    TEST_ASSERT(mon.notify_count_thermal == 1,
                "degraded: thermal AER fired on first poll");
    TEST_ASSERT(mon.notify_count_wear == 1,
                "degraded: wear AER fired on first poll");
    TEST_ASSERT(mon.notify_count_spare == 1,
                "degraded: spare AER fired on first poll");

    /* Second poll at same values: no duplicate. */
    smart_monitor_poll_once(&mon);
    TEST_ASSERT(mon.notify_count_thermal == 1,
                "degraded: no duplicate thermal on steady state");
    TEST_ASSERT(mon.notify_count_wear == 1,
                "degraded: no duplicate wear on steady state");
    TEST_ASSERT(mon.notify_count_spare == 1,
                "degraded: no duplicate spare on steady state");

    smart_monitor_cleanup(&mon);
    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* The embedded smart_monitor must be auto-wired into the
 * nvme_uspace_dev lifecycle — started by nvme_uspace_dev_start,
 * stopped by nvme_uspace_dev_stop — so callers can plug in real
 * data sources via nvme_uspace_dev_set_smart_source without
 * restarting the device. */
static int test_smart_monitor_autowired_into_dev(void)
{
    printf("\n=== SMART monitor: auto-wired into nvme_uspace_dev ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    nvme_uspace_config_small(&cfg);
    int ret = nvme_uspace_dev_init(&dev, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "autowire: dev_init");

    /* Before start, the embedded monitor is initialized but not
     * running yet. */
    TEST_ASSERT(dev.smart_mon.initialized == true,
                "autowire: embedded monitor initialized by dev_init");
    TEST_ASSERT(dev.smart_mon.running == false,
                "autowire: embedded monitor not yet running");

    nvme_uspace_dev_start(&dev);
    TEST_ASSERT(dev.smart_mon.running == true,
                "autowire: dev_start spins the monitor thread");

    /* Inject a real data source AFTER start — the running thread
     * picks up the new callbacks on the next poll. */
    struct smart_mock_src src = {
        .thermal = THERMAL_LEVEL_LIGHT,
        .remaining_life = 90,
        .spare = 15,
    };
    ret = nvme_uspace_dev_set_smart_source(&dev, mock_get_thermal,
                                           mock_get_rlp, mock_get_spare,
                                           &src);
    TEST_ASSERT(ret == HFSSS_OK, "autowire: set_smart_source OK");

    /* Let the thread run at least two poll intervals (default 1s is
     * too slow for a test — we use the defaults here precisely to
     * exercise the production path, so accept up to ~2.2s wait). */
    struct timespec ts = { .tv_sec = 2, .tv_nsec = 200 * 1000 * 1000 };
    nanosleep(&ts, NULL);

    nvme_uspace_dev_stop(&dev);
    TEST_ASSERT(dev.smart_mon.running == false,
                "autowire: dev_stop joins the monitor thread");

    /* Thermal went 0 -> LIGHT at least once. */
    TEST_ASSERT(dev.smart_mon.notify_count_thermal >= 1,
                "autowire: thermal AER fired via production path");

    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

static int test_smart_monitor_thread_lifecycle(void)
{
    printf("\n=== SMART monitor: start/stop thread lifecycle (REQ-178) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    nvme_uspace_config_small(&cfg);
    int ret = nvme_uspace_dev_init(&dev, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "monitor-thread: dev_init");
    nvme_uspace_dev_start(&dev);

    /* Start from nominal — the first poll seeds baseline without
     * firing; a subsequent poll after we mutate the mock sees real
     * change events and emits AERs. This separates the baseline
     * behavior from the event-detection behavior in one test. */
    struct smart_mock_src src = {
        .thermal = 0, .remaining_life = 100, .spare = 100,
    };
    struct smart_monitor mon;
    struct smart_monitor_config mcfg = {
        .dev                = &dev,
        .poll_interval_ms   = 5,
        .get_thermal        = mock_get_thermal,
        .get_remaining_life = mock_get_rlp,
        .get_spare          = mock_get_spare,
        .cb_ctx             = &src,
    };
    ret = smart_monitor_init(&mon, &mcfg);
    TEST_ASSERT(ret == HFSSS_OK, "monitor-thread: init");

    ret = smart_monitor_start(&mon);
    TEST_ASSERT(ret == HFSSS_OK, "monitor-thread: start OK");
    TEST_ASSERT(mon.running == true, "monitor-thread: running flag set");

    /* Let the thread seed baseline (a few poll cycles). */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 30 * 1000 * 1000 };
    nanosleep(&ts, NULL);

    /* Now mutate the mock values; the running thread should pick up
     * the new readings within one poll interval and emit AERs. */
    src.thermal        = THERMAL_LEVEL_HEAVY;
    src.remaining_life = 50;
    /* Give the thread at least a couple of polls to observe + react. */
    ts.tv_nsec = 80 * 1000 * 1000;
    nanosleep(&ts, NULL);

    smart_monitor_stop(&mon);
    TEST_ASSERT(mon.running == false, "monitor-thread: stopped cleanly");

    /* Thermal change NONE -> HEAVY and wear bucket 0 -> 5 must both
     * have surfaced as at least one AER each during the second sleep. */
    TEST_ASSERT(mon.notify_count_thermal >= 1,
                "monitor-thread: thermal AER fired after mutation");
    TEST_ASSERT(mon.notify_count_wear >= 1,
                "monitor-thread: wear AER fired after mutation");

    smart_monitor_cleanup(&mon);
    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* REQ-148: per-NS IOPS cap engages on the real nvme_uspace I/O
 * path. Policy arms via nvme_uspace_dev_set_qos_policy; throttled
 * reads return HFSSS_ERR_BUSY (SC=NAMESPACE_NOT_READY at the CQE
 * layer). Boundary checks confirm the full 1K..2M range is accepted
 * by the setter without clamping or spurious INVAL. */
static int test_qos_per_ns_iops_cap_engages(void)
{
    printf("\n=== QoS: per-NS IOPS cap engages on I/O path (REQ-148) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    nvme_uspace_config_small(&cfg);
    int ret = nvme_uspace_dev_init(&dev, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "iops: dev_init");
    nvme_uspace_dev_start(&dev);

    /* Pre-fill LBA 0 with an unenforced policy so the later reads
     * don't trip the NOENT path on unprogrammed pages. */
    u8 wbuf[4096]; memset(wbuf, 0x5A, sizeof(wbuf));
    TEST_ASSERT(nvme_uspace_write(&dev, 1, 0, 1, wbuf) == HFSSS_OK,
                "iops: baseline write with policy unenforced");

    /* Arm a 1K IOPS cap with no burst; first wave of reads drains
     * the bucket, subsequent reads surface HFSSS_ERR_BUSY. */
    struct ns_qos_policy pol = {
        .nsid = 1, .iops_limit = 1000, .bw_limit_mbps = 0,
        .latency_target_us = 0, .burst_allowance = 0, .enforced = true,
    };
    TEST_ASSERT(nvme_uspace_dev_set_qos_policy(&dev, 1, &pol) == HFSSS_OK,
                "iops: arm 1K IOPS policy");

    u8 rbuf[4096];
    u32 ok = 0, busy = 0, other = 0;
    for (int i = 0; i < 3000; i++) {
        int rc = nvme_uspace_read(&dev, 1, 0, 1, rbuf);
        if      (rc == HFSSS_OK)       ok++;
        else if (rc == HFSSS_ERR_BUSY) busy++;
        else                           other++;
    }
    TEST_ASSERT(other == 0, "iops: no unexpected rc on capped path");
    TEST_ASSERT(ok > 0 && ok <= 1500,
                "iops: some reads passed within the bucket budget");
    TEST_ASSERT(busy > 0, "iops: at least one read throttled by cap");

    /* Dispatch path maps HFSSS_ERR_BUSY to SC=NAMESPACE_NOT_READY
     * on the CQE so an NVMe host sees a retryable status rather
     * than a generic device error. */
    struct ns_qos_policy pol_tight = pol;
    pol_tight.iops_limit = 1000;  /* spec lower bound — drains fast on a tight loop */
    TEST_ASSERT(nvme_uspace_dev_set_qos_policy(&dev, 1, &pol_tight) == HFSSS_OK,
                "iops: tighten cap for dispatch CQE check");

    /* Bucket starts at max = iops_limit = 1000. Dispatch enough
     * commands to drain it plus some headroom for refill during
     * the loop. */
    u16 throttled_sc = 0;
    for (int i = 0; i < 3000; i++) {
        struct nvme_sq_entry sq; struct nvme_cq_entry cpl;
        memset(&sq, 0, sizeof(sq)); memset(&cpl, 0, sizeof(cpl));
        sq.opcode = NVME_NVM_READ; sq.nsid = 1;
        sq.command_id = (u16)(i & 0xFFFF);
        (void)nvme_uspace_dispatch_io_cmd(&dev, &sq, &cpl, rbuf, sizeof(rbuf));
        if (cpl.status != 0) { throttled_sc = cpl.status; break; }
    }
    TEST_ASSERT(throttled_sc != 0, "iops: dispatcher observed a throttle");
    u16 expected = NVME_BUILD_STATUS(NVME_SC_NAMESPACE_NOT_READY,
                                     NVME_STATUS_TYPE_GENERIC);
    TEST_ASSERT(throttled_sc == expected,
                "iops: throttled CQE carries SC=NAMESPACE_NOT_READY");

    /* REQ-148 range sanity: both the 1K lower bound and the 2M
     * upper bound configure cleanly. */
    struct ns_qos_policy lo = pol; lo.iops_limit = 1000;
    struct ns_qos_policy hi = pol; hi.iops_limit = 2000000;
    TEST_ASSERT(nvme_uspace_dev_set_qos_policy(&dev, 1, &lo) == HFSSS_OK,
                "iops: 1K IOPS (spec lower bound) accepted");
    TEST_ASSERT(nvme_uspace_dev_set_qos_policy(&dev, 1, &hi) == HFSSS_OK,
                "iops: 2M IOPS (spec upper bound) accepted");

    /* nsid validation: only the advertised namespace (NN=1) is
     * accepted. Phantom namespaces (2..QOS_MAX_NAMESPACES) are
     * rejected so the setter mirrors what the I/O path honors. */
    TEST_ASSERT(nvme_uspace_dev_set_qos_policy(&dev, 0, &pol)
                == HFSSS_ERR_INVAL,
                "iops: nsid=0 rejected");
    TEST_ASSERT(nvme_uspace_dev_set_qos_policy(&dev, 2, &pol)
                == HFSSS_ERR_INVAL,
                "iops: phantom nsid=2 rejected (above advertised NN)");
    TEST_ASSERT(nvme_uspace_dev_set_qos_policy(&dev, QOS_MAX_NAMESPACES + 1,
                                               &pol) == HFSSS_ERR_INVAL,
                "iops: nsid past max rejected");

    /* Range validation: reject sub-1K and super-2M IOPS limits. */
    struct ns_qos_policy under = pol; under.iops_limit = 999;
    struct ns_qos_policy over  = pol; over.iops_limit  = 2000001;
    TEST_ASSERT(nvme_uspace_dev_set_qos_policy(&dev, 1, &under)
                == HFSSS_ERR_INVAL,
                "iops: 999 IOPS below spec lower bound rejected");
    TEST_ASSERT(nvme_uspace_dev_set_qos_policy(&dev, 1, &over)
                == HFSSS_ERR_INVAL,
                "iops: 2000001 IOPS above spec upper bound rejected");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* REQ-149: per-NS bandwidth cap engages on the real I/O path. The
 * bucket is sized in bytes; large reads drain it faster than small
 * ones, and the throttle is tied to byte rate not request rate. */
static int test_qos_per_ns_bw_cap_engages(void)
{
    printf("\n=== QoS: per-NS bandwidth cap engages on I/O path (REQ-149) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    nvme_uspace_config_small(&cfg);
    int ret = nvme_uspace_dev_init(&dev, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "bw: dev_init");
    nvme_uspace_dev_start(&dev);

    /* Prefill a handful of LBAs so the reads don't trip NOENT. */
    u8 wbuf[4096]; memset(wbuf, 0xA5, sizeof(wbuf));
    for (u64 lba = 0; lba < 8; lba++) {
        TEST_ASSERT(nvme_uspace_write(&dev, 1, lba, 1, wbuf) == HFSSS_OK,
                    "bw: baseline write");
    }

    /* Arm a 50 MB/s BW cap (spec lower bound) with IOPS unlimited.
     * Bucket starts at 50 MiB; reading 8KB * many drains it fast. */
    struct ns_qos_policy pol = {
        .nsid = 1, .iops_limit = 0, .bw_limit_mbps = 50,
        .latency_target_us = 0, .burst_allowance = 0, .enforced = true,
    };
    TEST_ASSERT(nvme_uspace_dev_set_qos_policy(&dev, 1, &pol) == HFSSS_OK,
                "bw: arm 50 MB/s cap");

    u8 rbuf[4096];
    u32 ok = 0, busy = 0, other = 0;
    /* ~400 MB of reads against a 50 MB/s cap so consumption rate
     * clearly exceeds the refill rate. A short test loop where
     * refill keeps up would never exhaust the bucket, which is
     * what caught a flaky earlier iteration — pick 100_000 so the
     * consumption-to-refill ratio is ~8x and the throttle is
     * deterministic even on a fast host. */
    for (int i = 0; i < 100000; i++) {
        int rc = nvme_uspace_read(&dev, 1, (u64)(i % 8), 1, rbuf);
        if      (rc == HFSSS_OK)       ok++;
        else if (rc == HFSSS_ERR_BUSY) busy++;
        else                           other++;
    }
    TEST_ASSERT(other == 0, "bw: no unexpected rc on capped path");
    TEST_ASSERT(busy > 0, "bw: BW cap throttled some reads");
    TEST_ASSERT(ok > 0, "bw: initial bucket burst let some reads through");

    /* REQ-149 range sanity: 50 MB/s (spec lower) + 14 GB/s
     * (spec upper = 14000 MB/s) both configure cleanly. */
    struct ns_qos_policy lo = pol; lo.bw_limit_mbps = 50;
    struct ns_qos_policy hi = pol; hi.bw_limit_mbps = 14000;
    TEST_ASSERT(nvme_uspace_dev_set_qos_policy(&dev, 1, &lo) == HFSSS_OK,
                "bw: 50 MB/s (spec lower) accepted");
    TEST_ASSERT(nvme_uspace_dev_set_qos_policy(&dev, 1, &hi) == HFSSS_OK,
                "bw: 14 GB/s (spec upper) accepted");

    /* Out-of-range BW rejected. */
    struct ns_qos_policy under = pol; under.bw_limit_mbps = 49;
    struct ns_qos_policy over  = pol; over.bw_limit_mbps  = 14001;
    TEST_ASSERT(nvme_uspace_dev_set_qos_policy(&dev, 1, &under)
                == HFSSS_ERR_INVAL,
                "bw: 49 MB/s below spec lower bound rejected");
    TEST_ASSERT(nvme_uspace_dev_set_qos_policy(&dev, 1, &over)
                == HFSSS_ERR_INVAL,
                "bw: 14001 MB/s above spec upper bound rejected");

    /* Combined IOPS + BW policy: both buckets must engage. With a
     * tight IOPS cap and an unlimited BW cap, IOPS throttling
     * dominates. Flip the ratio and BW dominates. Prove both
     * paths through qos_acquire_tokens fire under combined
     * enforcement. */
    struct ns_qos_policy combined = {
        .nsid = 1, .iops_limit = 1000, .bw_limit_mbps = 14000,
        .latency_target_us = 0, .burst_allowance = 0, .enforced = true,
    };
    TEST_ASSERT(nvme_uspace_dev_set_qos_policy(&dev, 1, &combined) == HFSSS_OK,
                "bw+iops: combined policy accepted");
    u32 busy_iops_dom = 0;
    for (int i = 0; i < 5000; i++) {
        if (nvme_uspace_read(&dev, 1, 0, 1, rbuf) == HFSSS_ERR_BUSY) {
            busy_iops_dom++;
        }
    }
    TEST_ASSERT(busy_iops_dom > 0,
                "bw+iops: IOPS-dominated combined policy throttles");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* REQ-151: hot-reconfigure. Commands flow under tight cap, caller
 * replaces the policy without stopping the device, subsequent
 * commands see the new cap without restart. */
static int test_qos_hot_reconfigure_live(void)
{
    printf("\n=== QoS: hot-reconfigure takes effect on next command (REQ-151) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    nvme_uspace_config_small(&cfg);
    int ret = nvme_uspace_dev_init(&dev, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "hotrc: dev_init");
    nvme_uspace_dev_start(&dev);

    u8 wbuf[4096]; memset(wbuf, 0xCC, sizeof(wbuf));
    TEST_ASSERT(nvme_uspace_write(&dev, 1, 0, 1, wbuf) == HFSSS_OK,
                "hotrc: baseline write");

    /* Tight initial cap — most reads throttle. */
    struct ns_qos_policy tight = {
        .nsid = 1, .iops_limit = 1000, .bw_limit_mbps = 0,
        .latency_target_us = 0, .burst_allowance = 0, .enforced = true,
    };
    TEST_ASSERT(nvme_uspace_dev_set_qos_policy(&dev, 1, &tight) == HFSSS_OK,
                "hotrc: arm tight 1K IOPS (spec lower) policy");

    /* Bucket starts at 1000 tokens. Drive enough traffic to drain
     * the bucket + outrun the wall-clock refill (~1us per token
     * at 1M/s equivalent; but the bucket refills only at the cap
     * rate so it still throttles across 3000 reads). */
    u8 rbuf[4096];
    u32 busy_before = 0;
    for (int i = 0; i < 3000; i++) {
        int rc = nvme_uspace_read(&dev, 1, 0, 1, rbuf);
        if (rc == HFSSS_ERR_BUSY) busy_before++;
    }
    TEST_ASSERT(busy_before > 0, "hotrc: tight policy throttles");

    /* Hot-reconfigure to unenforced — next reads all pass. */
    struct ns_qos_policy relaxed = tight;
    relaxed.enforced = false;
    TEST_ASSERT(nvme_uspace_dev_set_qos_policy(&dev, 1, &relaxed) == HFSSS_OK,
                "hotrc: relax policy without stopping traffic");

    u32 busy_after = 0;
    for (int i = 0; i < 500; i++) {
        int rc = nvme_uspace_read(&dev, 1, 0, 1, rbuf);
        if (rc == HFSSS_ERR_BUSY) busy_after++;
    }
    TEST_ASSERT(busy_after == 0,
                "hotrc: relaxed policy stops throttling on next command");

    /* Re-tighten to a fresh 2M IOPS cap — huge bucket means all
     * 500 reads pass; this exercises the high end of REQ-148's
     * advertised 1K..2M range under hot-reconfig. */
    struct ns_qos_policy big = tight;
    big.iops_limit = 2000000;
    TEST_ASSERT(nvme_uspace_dev_set_qos_policy(&dev, 1, &big) == HFSSS_OK,
                "hotrc: swap to 2M IOPS cap");
    u32 busy_big = 0;
    for (int i = 0; i < 500; i++) {
        if (nvme_uspace_read(&dev, 1, 0, 1, rbuf) == HFSSS_ERR_BUSY) busy_big++;
    }
    TEST_ASSERT(busy_big == 0, "hotrc: 2M cap lets all 500 reads through");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* REQ-150: P99 SLA enforcement with rollback callback. On N
 * consecutive breaches the caller-supplied rollback fires and the
 * consecutive-violations counter resets so the next window is
 * evaluated independently. */
static u32 g_sla_rollback_calls = 0;
static u32 g_sla_last_nsid = 0;
static u64 g_sla_last_p99_us = 0;
static void sla_rollback_cb(u32 nsid, u64 p99_us, void *ctx)
{
    (void)ctx;
    g_sla_rollback_calls++;
    g_sla_last_nsid = nsid;
    g_sla_last_p99_us = p99_us;
}

static int test_qos_sla_rollback(void)
{
    printf("\n=== QoS: P99 SLA rollback fires on sustained breach (REQ-150) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    nvme_uspace_config_small(&cfg);
    int ret = nvme_uspace_dev_init(&dev, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "sla: dev_init");
    nvme_uspace_dev_start(&dev);

    g_sla_rollback_calls = 0;
    ret = nvme_uspace_dev_set_sla_rollback(&dev, 1, /*target_us=*/1000,
                                           /*trigger_count=*/3,
                                           sla_rollback_cb, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "sla: rollback installed");

    /* Seed the histogram with low-latency samples so P99 is below
     * the 1ms target. check_sla must report no breach. */
    for (int i = 0; i < 1000; i++) {
        nvme_uspace_dev_record_latency(&dev, 1, 100000);   /* 100us */
    }
    TEST_ASSERT(nvme_uspace_dev_check_sla(&dev, 1) == false,
                "sla: healthy P99 doesn't trigger rollback");
    TEST_ASSERT(g_sla_rollback_calls == 0,
                "sla: callback silent on healthy P99");

    /* Inject outliers that pull P99 above 1ms, then check 3 times.
     * Each check sees a breach, so consecutive_violations climbs
     * 1 -> 2 -> 3 and the rollback fires on the third call. */
    for (int i = 0; i < 50; i++) {
        nvme_uspace_dev_record_latency(&dev, 1, 64ULL * 1000 * 1000); /* 64ms */
    }
    TEST_ASSERT(nvme_uspace_dev_check_sla(&dev, 1) == false,
                "sla: 1st breach — consecutive=1, below trigger");
    TEST_ASSERT(nvme_uspace_dev_check_sla(&dev, 1) == false,
                "sla: 2nd breach — consecutive=2, below trigger");
    TEST_ASSERT(nvme_uspace_dev_check_sla(&dev, 1) == true,
                "sla: 3rd breach — trigger reached, rollback fires");
    TEST_ASSERT(g_sla_rollback_calls == 1,
                "sla: callback fired exactly once");
    TEST_ASSERT(g_sla_last_nsid == 1,
                "sla: callback received correct nsid");
    TEST_ASSERT(g_sla_last_p99_us > 1000,
                "sla: callback received a P99 above target");
    TEST_ASSERT(dev.sla_by_ns[0].fire_count == 1,
                "sla: slot fire_count tracks rollback invocations");

    /* Consecutive counter must reset after fire so another 3
     * breaches are required to fire again. */
    TEST_ASSERT(dev.lat_by_ns[0].consecutive_violations == 0,
                "sla: consecutive resets after rollback");
    TEST_ASSERT(nvme_uspace_dev_check_sla(&dev, 1) == false,
                "sla: fresh window needs another 3 breaches");
    TEST_ASSERT(nvme_uspace_dev_check_sla(&dev, 1) == false,
                "sla: 2nd breach of new window");
    TEST_ASSERT(nvme_uspace_dev_check_sla(&dev, 1) == true,
                "sla: 3rd breach of new window fires again");
    TEST_ASSERT(g_sla_rollback_calls == 2,
                "sla: second rollback fired");

    /* trigger_count=0 disables the rollback without touching the
     * monitor. Subsequent checks still evaluate SLA but never fire
     * the callback. */
    ret = nvme_uspace_dev_set_sla_rollback(&dev, 1, 1000, 0, NULL, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "sla: rollback disabled");
    for (int i = 0; i < 5; i++) (void)nvme_uspace_dev_check_sla(&dev, 1);
    TEST_ASSERT(g_sla_rollback_calls == 2,
                "sla: disabled rollback stays silent");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Callback that re-arms the rollback from inside itself. Without
 * the snapshot-before-fire fix this would race qos_lock against
 * the trailing slot update in check_sla and could clobber the new
 * registration. */
static u32 g_reentrant_calls = 0;
static struct nvme_uspace_dev *g_reentrant_dev = NULL;
static void reentrant_cb(u32 nsid, u64 p99_us, void *ctx)
{
    (void)p99_us; (void)ctx;
    g_reentrant_calls++;
    /* Re-install with a bigger trigger_count so the second window
     * has fresh semantics — this validates the header's
     * "callback may reconfigure" guarantee. */
    (void)nvme_uspace_dev_set_sla_rollback(g_reentrant_dev, nsid,
                                           1000, 5, reentrant_cb, NULL);
}

static int test_qos_sla_rollback_callback_reentrancy(void)
{
    printf("\n=== QoS: SLA rollback callback can reinstall itself (REQ-150) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    nvme_uspace_config_small(&cfg);
    int ret = nvme_uspace_dev_init(&dev, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "reent: dev_init");
    nvme_uspace_dev_start(&dev);

    g_reentrant_calls = 0;
    g_reentrant_dev = &dev;
    ret = nvme_uspace_dev_set_sla_rollback(&dev, 1, 1000, 2,
                                           reentrant_cb, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "reent: initial rollback installed");

    for (int i = 0; i < 1000; i++) {
        nvme_uspace_dev_record_latency(&dev, 1, 100000);
    }
    for (int i = 0; i < 50; i++) {
        nvme_uspace_dev_record_latency(&dev, 1, 64ULL * 1000 * 1000);
    }
    /* Two consecutive breaches -> first callback fires. Inside the
     * callback we re-install with trigger=5, so the next fire
     * needs 5 more breaches, not 2. */
    (void)nvme_uspace_dev_check_sla(&dev, 1);
    (void)nvme_uspace_dev_check_sla(&dev, 1);
    TEST_ASSERT(g_reentrant_calls == 1,
                "reent: first callback fired exactly once");
    TEST_ASSERT(dev.sla_by_ns[0].trigger_count == 5,
                "reent: callback's re-install survived post-fire updates");

    /* 4 more breaches still below the new trigger=5. */
    for (int i = 0; i < 4; i++) (void)nvme_uspace_dev_check_sla(&dev, 1);
    TEST_ASSERT(g_reentrant_calls == 1,
                "reent: 4 breaches below new trigger stay silent");
    (void)nvme_uspace_dev_check_sla(&dev, 1);  /* 5th breach */
    TEST_ASSERT(g_reentrant_calls == 2,
                "reent: 5th breach fires under the re-installed policy");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Full-path wire-in: nvme_uspace_dispatch_io_cmd must stamp the
 * completion into the per-NS latency monitor. Without this REQ-150
 * would stay helper-only and check_sla would never see real data. */
static int test_qos_dispatch_records_latency(void)
{
    printf("\n=== QoS: dispatch_io_cmd records completion latency (REQ-150) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    nvme_uspace_config_small(&cfg);
    int ret = nvme_uspace_dev_init(&dev, &cfg);
    TEST_ASSERT(ret == HFSSS_OK, "wire: dev_init");
    nvme_uspace_dev_start(&dev);

    u64 before = dev.lat_by_ns[0].total_samples;
    u8 buf[4096];

    /* Run a write + read through the full dispatch path so the
     * latency hook in nvme_uspace_dispatch_io_cmd fires. */
    struct nvme_sq_entry sq; struct nvme_cq_entry cpl;
    memset(&sq, 0, sizeof(sq)); memset(&cpl, 0, sizeof(cpl));
    sq.opcode     = NVME_NVM_WRITE;
    sq.nsid       = 1;
    sq.command_id = 0x01;
    (void)nvme_uspace_dispatch_io_cmd(&dev, &sq, &cpl, buf, sizeof(buf));

    memset(&sq, 0, sizeof(sq)); memset(&cpl, 0, sizeof(cpl));
    sq.opcode     = NVME_NVM_READ;
    sq.nsid       = 1;
    sq.command_id = 0x02;
    (void)nvme_uspace_dispatch_io_cmd(&dev, &sq, &cpl, buf, sizeof(buf));

    u64 after = dev.lat_by_ns[0].total_samples;
    TEST_ASSERT(after >= before + 2,
                "wire: dispatch path recorded >= 2 latency samples");

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
    test_log_page_telemetry_host();
    test_log_page_telemetry_ctrl();
    test_log_page_vendor_counters();
    test_log_page_unknown_lid_notsupp();
    test_aer_dispatch_queues_then_post_event();
    test_aer_dispatch_completes_immediately_when_event_pending();
    test_aer_notify_thermal_delivers_temperature_event();
    test_aer_notify_wear_delivers_reliability_event();
    test_aer_notify_spare_delivers_spare_event();
    test_opal_security_send_lock_blocks_io();
    test_opal_security_send_unlock_restores_io();
    test_opal_security_send_wrong_auth_keeps_locked();
    test_opal_security_recv_reports_lock_state();
    test_opal_default_ns_registered_at_init();
    test_smart_reflects_live_state_after_notify();
    test_smart_monitor_poll_fires_aer_on_threshold_cross();
    test_smart_monitor_degraded_at_start();
    test_smart_monitor_autowired_into_dev();
    test_smart_monitor_thread_lifecycle();
    test_keys_lock_concurrent_lock_and_read();
    test_qos_per_ns_iops_cap_engages();
    test_qos_per_ns_bw_cap_engages();
    test_qos_hot_reconfigure_live();
    test_qos_sla_rollback();
    test_qos_sla_rollback_callback_reentrancy();
    test_qos_dispatch_records_latency();

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
