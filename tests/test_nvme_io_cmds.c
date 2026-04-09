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

/*
 * Single controller context shared by all tests.
 * sizeof(struct nvme_ctrl_ctx) is ~5432 bytes; keep it off the stack.
 */
static struct nvme_ctrl_ctx g_ctrl;

/* Build an SQ entry with the given opcode, nsid, and command_id */
static struct nvme_sq_entry make_io_sqe(u8 opcode, u32 nsid, u16 cid)
{
    struct nvme_sq_entry cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = opcode;
    cmd.nsid = nsid;
    cmd.command_id = cid;
    return cmd;
}

/* Read command (NVME_NVM_READ, opcode 0x02) */
static void test_io_read(void)
{
    printf("\n=== IO Read Command ===\n");

    struct nvme_sq_entry cmd = make_io_sqe(NVME_NVM_READ, 1, 10);
    struct nvme_cq_entry cpl;

    /* Set LBA fields: cdw10/cdw11 = starting LBA, cdw12 = NLB - 1 */
    cmd.cdw10 = 0;
    cmd.cdw11 = 0;
    cmd.cdw12 = 7; /* 8 blocks */

    int ret = nvme_ctrl_process_io_cmd(&g_ctrl, &cmd, &cpl);
    TEST_ASSERT(ret == HFSSS_OK, "read dispatch should return OK");
    TEST_ASSERT(cpl.status == NVME_SC_SUCCESS, "read status should be SUCCESS");
    TEST_ASSERT(cpl.command_id == 10, "read completion cid should match");
}

/* Write command (NVME_NVM_WRITE, opcode 0x01) */
static void test_io_write(void)
{
    printf("\n=== IO Write Command ===\n");

    struct nvme_sq_entry cmd = make_io_sqe(NVME_NVM_WRITE, 1, 20);
    struct nvme_cq_entry cpl;

    cmd.cdw10 = 100;
    cmd.cdw11 = 0;
    cmd.cdw12 = 3; /* 4 blocks */

    int ret = nvme_ctrl_process_io_cmd(&g_ctrl, &cmd, &cpl);
    TEST_ASSERT(ret == HFSSS_OK, "write dispatch should return OK");
    TEST_ASSERT(cpl.status == NVME_SC_SUCCESS, "write status should be SUCCESS");
    TEST_ASSERT(cpl.command_id == 20, "write completion cid should match");
}

/* Flush command (NVME_NVM_FLUSH, opcode 0x00) */
static void test_io_flush(void)
{
    printf("\n=== IO Flush Command ===\n");

    struct nvme_sq_entry cmd = make_io_sqe(NVME_NVM_FLUSH, 1, 30);
    struct nvme_cq_entry cpl;

    int ret = nvme_ctrl_process_io_cmd(&g_ctrl, &cmd, &cpl);
    TEST_ASSERT(ret == HFSSS_OK, "flush dispatch should return OK");
    TEST_ASSERT(cpl.status == NVME_SC_SUCCESS, "flush status should be SUCCESS");
    TEST_ASSERT(cpl.command_id == 30, "flush completion cid should match");
}

/* Dataset Management / Trim (NVME_NVM_DATASET_MANAGEMENT, opcode 0x09) */
static void test_io_dsm(void)
{
    printf("\n=== IO Dataset Management (Trim) ===\n");

    struct nvme_sq_entry cmd = make_io_sqe(NVME_NVM_DATASET_MANAGEMENT, 1, 40);
    struct nvme_cq_entry cpl;

    /* cdw11 bit 2 = deallocate hint */
    cmd.cdw11 = NVME_DSM_ATTR_DEALLOCATE;

    int ret = nvme_ctrl_process_io_cmd(&g_ctrl, &cmd, &cpl);
    TEST_ASSERT(ret == HFSSS_OK, "dsm dispatch should return OK");
    TEST_ASSERT(cpl.status == NVME_SC_SUCCESS, "dsm status should be SUCCESS");
    TEST_ASSERT(cpl.command_id == 40, "dsm completion cid should match");
}

/*
 * Invalid NSID -- the current dispatch stub does not validate NSID,
 * so it returns SUCCESS. This test documents actual behavior.
 */
static void test_io_invalid_nsid(void)
{
    printf("\n=== IO Invalid NSID ===\n");

    struct nvme_sq_entry cmd = make_io_sqe(NVME_NVM_READ, 99, 50);
    struct nvme_cq_entry cpl;

    int ret = nvme_ctrl_process_io_cmd(&g_ctrl, &cmd, &cpl);
    TEST_ASSERT(ret == HFSSS_OK, "invalid nsid dispatch should return OK");
    /* Current stub does not validate NSID -- succeeds unconditionally */
    TEST_ASSERT(cpl.status == NVME_SC_SUCCESS, "invalid nsid status should be SUCCESS (no validation yet)");
    TEST_ASSERT(cpl.command_id == 50, "invalid nsid completion cid should match");
}

/*
 * Out-of-range LBA -- stub does not check LBA bounds.
 * Documents actual behavior.
 */
static void test_io_out_of_range_lba(void)
{
    printf("\n=== IO Out-of-Range LBA ===\n");

    struct nvme_sq_entry cmd = make_io_sqe(NVME_NVM_WRITE, 1, 60);
    struct nvme_cq_entry cpl;

    cmd.cdw10 = 0xFFFFFFFF;
    cmd.cdw11 = 0xFFFFFFFF;
    cmd.cdw12 = 0xFF;

    int ret = nvme_ctrl_process_io_cmd(&g_ctrl, &cmd, &cpl);
    TEST_ASSERT(ret == HFSSS_OK, "out-of-range LBA dispatch should return OK");
    /* Current stub does not check LBA range */
    TEST_ASSERT(cpl.status == NVME_SC_SUCCESS, "out-of-range LBA status should be SUCCESS (no validation yet)");
}

/*
 * Zero-length / malformed request -- NLB=0 means 1 block in NVMe spec,
 * but with a zeroed-out SQE the stub just succeeds. Documents behavior.
 */
static void test_io_zero_length(void)
{
    printf("\n=== IO Zero-Length Request ===\n");

    struct nvme_sq_entry cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_NVM_READ;
    cmd.command_id = 70;
    /* All other fields left at zero: nsid=0, LBA=0, NLB=0 */

    struct nvme_cq_entry cpl;

    int ret = nvme_ctrl_process_io_cmd(&g_ctrl, &cmd, &cpl);
    TEST_ASSERT(ret == HFSSS_OK, "zero-length dispatch should return OK");
    TEST_ASSERT(cpl.status == NVME_SC_SUCCESS, "zero-length status should be SUCCESS (no validation yet)");
    TEST_ASSERT(cpl.command_id == 70, "zero-length completion cid should match");
}

/* Unsupported IO opcode triggers INVALID_OPCODE status */
static void test_io_unsupported_opcode(void)
{
    printf("\n=== IO Unsupported Opcode ===\n");

    struct nvme_sq_entry cmd = make_io_sqe(0xFE, 1, 80);
    struct nvme_cq_entry cpl;

    int ret = nvme_ctrl_process_io_cmd(&g_ctrl, &cmd, &cpl);
    TEST_ASSERT(ret == HFSSS_OK, "unsupported opcode dispatch should still return OK");

    u16 expected = NVME_BUILD_STATUS(NVME_SC_INVALID_OPCODE, NVME_STATUS_TYPE_GENERIC);
    TEST_ASSERT(cpl.status == expected, "unsupported opcode should set INVALID_OPCODE status");
    TEST_ASSERT(cpl.command_id == 80, "unsupported opcode completion cid should match");
}

/* Verify CQE sq_id is derived from cmd->flags (per implementation) */
static void test_io_cqe_sq_id(void)
{
    printf("\n=== IO CQE SQ ID Propagation ===\n");

    struct nvme_sq_entry cmd = make_io_sqe(NVME_NVM_READ, 1, 90);
    struct nvme_cq_entry cpl;

    cmd.flags = 0x03; /* low byte of flags used as sq_id */

    int ret = nvme_ctrl_process_io_cmd(&g_ctrl, &cmd, &cpl);
    TEST_ASSERT(ret == HFSSS_OK, "sq_id test dispatch should return OK");
    TEST_ASSERT(cpl.sq_id == 3, "cqe sq_id should reflect cmd flags low 16 bits");
}

/* Write Zeroes (NVME_NVM_WRITE_ZEROES, opcode 0x08) */
static void test_io_write_zeroes(void)
{
    printf("\n=== IO Write Zeroes Command ===\n");

    struct nvme_sq_entry cmd = make_io_sqe(NVME_NVM_WRITE_ZEROES, 1, 100);
    struct nvme_cq_entry cpl;

    cmd.cdw10 = 0;
    cmd.cdw11 = 0;
    cmd.cdw12 = 15; /* 16 blocks */

    int ret = nvme_ctrl_process_io_cmd(&g_ctrl, &cmd, &cpl);
    TEST_ASSERT(ret == HFSSS_OK, "write zeroes dispatch should return OK");
    TEST_ASSERT(cpl.status == NVME_SC_SUCCESS, "write zeroes status should be SUCCESS");
    TEST_ASSERT(cpl.command_id == 100, "write zeroes completion cid should match");
}

int main(void)
{
    print_separator();
    printf("HFSSS NVMe IO Command Tests\n");
    print_separator();

    int ret = nvme_ctrl_init(&g_ctrl);
    if (ret != HFSSS_OK) {
        printf("FATAL: nvme_ctrl_init failed with %d\n", ret);
        return 1;
    }

    test_io_read();
    test_io_write();
    test_io_flush();
    test_io_dsm();
    test_io_invalid_nsid();
    test_io_out_of_range_lba();
    test_io_zero_length();
    test_io_unsupported_opcode();
    test_io_cqe_sq_id();
    test_io_write_zeroes();

    nvme_ctrl_cleanup(&g_ctrl);

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
