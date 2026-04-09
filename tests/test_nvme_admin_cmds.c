#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pcie/nvme.h"

#define TEST_PASS 0
#define TEST_FAIL 1

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                                                                                         \
    do {                                                                                                               \
        tests_run++;                                                                                                   \
        if (cond) {                                                                                                    \
            printf("  [PASS] %s\n", msg);                                                                              \
            tests_passed++;                                                                                            \
        } else {                                                                                                       \
            printf("  [FAIL] %s\n", msg);                                                                              \
            tests_failed++;                                                                                            \
        }                                                                                                              \
    } while (0)

static void print_separator(void)
{
    printf("========================================\n");
}

/* Test: Identify Controller via nvme_ctrl_process_identify (CNS=1) */
static void test_identify_controller(void)
{
    printf("\n=== Identify Controller (CNS=1) ===\n");

    struct nvme_sq_entry cmd;
    u8 data[4096];

    memset(&cmd, 0, sizeof(cmd));
    memset(data, 0, sizeof(data));

    cmd.cdw10 = 0x01; /* CNS=1: Identify Controller */

    int rc = nvme_ctrl_process_identify(&cmd, data, sizeof(data));
    TEST_ASSERT(rc == NVME_SC_SUCCESS, "identify controller should return success");

    struct nvme_identify_ctrl *id = (struct nvme_identify_ctrl *)data;
    TEST_ASSERT(id->vid == HFSSS_VENDOR_ID, "vendor id should match HFSSS_VENDOR_ID");
    TEST_ASSERT(id->nn == 1, "number of namespaces should be 1");
    TEST_ASSERT(strncmp(id->mn, "HFSSS NVMe SSD", 14) == 0, "model number should start with HFSSS NVMe SSD");
    TEST_ASSERT(id->ver == NVME_VERSION_2_0, "controller version should be NVMe 2.0");
}

/* Test: Identify Namespace via nvme_ctrl_process_identify (CNS=0) */
static void test_identify_namespace(void)
{
    printf("\n=== Identify Namespace (CNS=0, NSID=1) ===\n");

    struct nvme_sq_entry cmd;
    u8 data[4096];

    memset(&cmd, 0, sizeof(cmd));
    memset(data, 0, sizeof(data));

    cmd.cdw10 = 0x00; /* CNS=0: Identify Namespace */
    cmd.nsid = 1;

    int rc = nvme_ctrl_process_identify(&cmd, data, sizeof(data));
    TEST_ASSERT(rc == NVME_SC_SUCCESS, "identify namespace should return success");

    struct nvme_identify_ns *ns = (struct nvme_identify_ns *)data;
    TEST_ASSERT(ns->nsze > 0, "namespace size should be non-zero");
    TEST_ASSERT(ns->ncap > 0, "namespace capacity should be non-zero");
    TEST_ASSERT(ns->lbaf[0].lbads == 12, "LBA format 0 data size should be 2^12 (4KB)");
}

/* Test: Identify Namespace with invalid NSID */
static void test_identify_ns_invalid_nsid(void)
{
    printf("\n=== Identify Namespace Invalid NSID ===\n");

    struct nvme_sq_entry cmd;
    u8 data[4096];

    memset(&cmd, 0, sizeof(cmd));
    memset(data, 0, sizeof(data));

    cmd.cdw10 = 0x00; /* CNS=0 */
    cmd.nsid = 99;    /* non-existent namespace */

    int rc = nvme_ctrl_process_identify(&cmd, data, sizeof(data));
    TEST_ASSERT(rc == NVME_SC_INVALID_NAMESPACE, "identify with invalid nsid should return INVALID_NAMESPACE");
}

/* Test: Active Namespace List (CNS=2) */
static void test_identify_active_ns_list(void)
{
    printf("\n=== Identify Active Namespace List (CNS=2) ===\n");

    struct nvme_sq_entry cmd;
    u8 data[4096];

    memset(&cmd, 0, sizeof(cmd));
    memset(data, 0, sizeof(data));

    cmd.cdw10 = 0x02; /* CNS=2: Active Namespace List */

    int rc = nvme_ctrl_process_identify(&cmd, data, sizeof(data));
    TEST_ASSERT(rc == NVME_SC_SUCCESS, "active namespace list should return success");

    u32 first_nsid = ((u32 *)data)[0];
    TEST_ASSERT(first_nsid == 1, "first active namespace id should be 1");
}

/* Test: Identify with unsupported CNS value */
static void test_identify_invalid_cns(void)
{
    printf("\n=== Identify Invalid CNS ===\n");

    struct nvme_sq_entry cmd;
    u8 data[4096];

    memset(&cmd, 0, sizeof(cmd));
    memset(data, 0, sizeof(data));

    cmd.cdw10 = 0x10; /* CNS=0x10: I/O Command Set Specific (falls to default) */

    int rc = nvme_ctrl_process_identify(&cmd, data, sizeof(data));
    TEST_ASSERT(rc == NVME_SC_INVALID_FIELD, "unsupported CNS should return INVALID_FIELD");
}

/* Test: Identify with NULL / undersized buffer */
static void test_identify_invalid_args(void)
{
    printf("\n=== Identify Invalid Arguments ===\n");

    struct nvme_sq_entry cmd;
    u8 data[4096];

    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw10 = 0x01;

    int rc = nvme_ctrl_process_identify(NULL, data, sizeof(data));
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "identify with NULL cmd should return INVAL");

    rc = nvme_ctrl_process_identify(&cmd, NULL, sizeof(data));
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "identify with NULL data should return INVAL");

    rc = nvme_ctrl_process_identify(&cmd, data, 512);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "identify with undersized buffer should return INVAL");
}

/* Test: Get Features admin command via nvme_ctrl_process_admin_cmd */
static void test_admin_get_features(void)
{
    printf("\n=== Admin Get Features ===\n");

    struct nvme_ctrl_ctx ctrl;
    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cpl;

    int ret = nvme_ctrl_init(&ctrl);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_ctrl_init should succeed");

    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.opcode = NVME_ADMIN_GET_FEATURES;
    cmd.command_id = 42;
    cmd.cdw10 = 0x07; /* Feature ID: Number of Queues */

    ret = nvme_ctrl_process_admin_cmd(&ctrl, &cmd, &cpl);
    TEST_ASSERT(ret == HFSSS_OK, "process_admin_cmd should return OK");
    TEST_ASSERT(cpl.status == NVME_SC_SUCCESS, "get features status should be SUCCESS");
    TEST_ASSERT(cpl.command_id == 42, "completion command_id should match submission");

    nvme_ctrl_cleanup(&ctrl);
}

/* Test: Set Features admin command */
static void test_admin_set_features(void)
{
    printf("\n=== Admin Set Features ===\n");

    struct nvme_ctrl_ctx ctrl;
    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cpl;

    int ret = nvme_ctrl_init(&ctrl);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_ctrl_init should succeed");

    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.opcode = NVME_ADMIN_SET_FEATURES;
    cmd.command_id = 100;
    cmd.cdw10 = 0x07;       /* Feature ID: Number of Queues */
    cmd.cdw11 = 0x001F001F; /* 32 SQs, 32 CQs */

    ret = nvme_ctrl_process_admin_cmd(&ctrl, &cmd, &cpl);
    TEST_ASSERT(ret == HFSSS_OK, "process_admin_cmd should return OK");
    TEST_ASSERT(cpl.status == NVME_SC_SUCCESS, "set features status should be SUCCESS");
    TEST_ASSERT(cpl.command_id == 100, "completion command_id should match submission");

    nvme_ctrl_cleanup(&ctrl);
}

/* Test: Get Log Page admin command */
static void test_admin_get_log_page(void)
{
    printf("\n=== Admin Get Log Page ===\n");

    struct nvme_ctrl_ctx ctrl;
    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cpl;

    int ret = nvme_ctrl_init(&ctrl);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_ctrl_init should succeed");

    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.opcode = NVME_ADMIN_GET_LOG_PAGE;
    cmd.command_id = 7;
    cmd.cdw10 = 0x02; /* Log Page ID: SMART/Health */

    ret = nvme_ctrl_process_admin_cmd(&ctrl, &cmd, &cpl);
    TEST_ASSERT(ret == HFSSS_OK, "process_admin_cmd should return OK");
    TEST_ASSERT(cpl.status == NVME_SC_SUCCESS, "get log page status should be SUCCESS");
    TEST_ASSERT(cpl.command_id == 7, "completion command_id should match submission");
    TEST_ASSERT(cpl.sq_id == 0, "admin command sq_id should be 0");

    nvme_ctrl_cleanup(&ctrl);
}

/* Test: Unsupported admin opcode */
static void test_admin_unsupported_opcode(void)
{
    printf("\n=== Admin Unsupported Opcode ===\n");

    struct nvme_ctrl_ctx ctrl;
    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cpl;

    int ret = nvme_ctrl_init(&ctrl);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_ctrl_init should succeed");

    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.opcode = 0xFF; /* undefined admin opcode */
    cmd.command_id = 55;

    ret = nvme_ctrl_process_admin_cmd(&ctrl, &cmd, &cpl);
    TEST_ASSERT(ret == HFSSS_OK, "process_admin_cmd should return OK even for invalid opcode");

    u16 expected = NVME_BUILD_STATUS(NVME_SC_INVALID_OPCODE, NVME_STATUS_TYPE_GENERIC);
    TEST_ASSERT(cpl.status == expected, "unsupported opcode should set INVALID_OPCODE status");
    TEST_ASSERT(cpl.command_id == 55, "completion command_id should be preserved");

    nvme_ctrl_cleanup(&ctrl);
}

/* Test: Admin Identify opcode dispatch (status only, no data path) */
static void test_admin_identify_dispatch(void)
{
    printf("\n=== Admin Identify Dispatch ===\n");

    struct nvme_ctrl_ctx ctrl;
    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cpl;

    int ret = nvme_ctrl_init(&ctrl);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_ctrl_init should succeed");

    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.command_id = 1;

    ret = nvme_ctrl_process_admin_cmd(&ctrl, &cmd, &cpl);
    TEST_ASSERT(ret == HFSSS_OK, "process_admin_cmd should return OK");
    TEST_ASSERT(cpl.status == NVME_SC_SUCCESS, "identify admin dispatch should return SUCCESS");

    nvme_ctrl_cleanup(&ctrl);
}

int main(void)
{
    print_separator();
    printf("HFSSS NVMe Admin Command Tests\n");
    print_separator();

    test_identify_controller();
    test_identify_namespace();
    test_identify_ns_invalid_nsid();
    test_identify_active_ns_list();
    test_identify_invalid_cns();
    test_identify_invalid_args();
    test_admin_get_features();
    test_admin_set_features();
    test_admin_get_log_page();
    test_admin_unsupported_opcode();
    test_admin_identify_dispatch();

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
    }

    printf("\n  [FAILURE] Some tests failed!\n");
    return 1;
}
