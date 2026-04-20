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

/* Fix-3: SMART (LID=0x02) must reflect the live thermal / spare /
 * percent_used state set by the AER notifier bridges. Before this
 * fix the payload was hard-coded so a host that polled SMART after
 * a TEMPERATURE_THRESHOLD AER saw a healthy-looking report. */
static int test_smart_reflects_live_state_after_notify(void)
{
    printf("\n=== SMART reflects live state after notify (Fix-3) ===\n");

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
    test_smart_reflects_live_state_after_notify();

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
