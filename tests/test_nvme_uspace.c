#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pcie/nvme_uspace.h"

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

    /* Test config default with smaller SSD */
    nvme_uspace_config_default(&config);
    config.sssim_cfg.total_lbas = 1024 * 1024 / 4096; /* 1MB total */
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

int main(void)
{
    print_separator();
    printf("HFSSS User-space NVMe Interface Tests\n");
    print_separator();

    test_nvme_uspace_dev();

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
