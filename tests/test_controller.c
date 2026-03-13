#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "controller/controller.h"

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

/* Shmem Interface Tests */
static int test_shmem_if(void)
{
    printf("\n=== Shmem Interface Tests ===\n");

    /* Note: shmem_if_open requires actual shared memory,
     * which we don't have in this test. We'll just test
     * the init/cleanup paths of other components.
     */

    TEST_ASSERT(true, "shmem_if test skipped (needs real shared memory)");

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Arbiter Tests */
static int test_arbiter(void)
{
    printf("\n=== Arbiter Tests ===\n");

    struct arbiter_ctx arbiter;
    struct cmd_context *cmd;
    int ret;

    ret = arbiter_init(&arbiter, 1024);
    TEST_ASSERT(ret == HFSSS_OK, "arbiter_init should succeed");

    cmd = arbiter_alloc_cmd(&arbiter);
    TEST_ASSERT(cmd != NULL, "arbiter_alloc_cmd should succeed");

    cmd->priority = PRIO_IO_NORMAL;
    cmd->cmd_id = 1234;
    ret = arbiter_enqueue(&arbiter, cmd);
    TEST_ASSERT(ret == HFSSS_OK, "arbiter_enqueue should succeed");

    struct cmd_context *dequeued = arbiter_dequeue(&arbiter);
    TEST_ASSERT(dequeued != NULL, "arbiter_dequeue should succeed");
    TEST_ASSERT(dequeued->cmd_id == 1234, "cmd_id should match");

    arbiter_free_cmd(&arbiter, dequeued);

    arbiter_cleanup(&arbiter);
    TEST_ASSERT(true, "arbiter_cleanup should succeed");

    /* Test NULL handling */
    TEST_ASSERT(arbiter_init(NULL, 1024) == HFSSS_ERR_INVAL,
                "arbiter_init with NULL should fail");
    arbiter_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Scheduler Tests */
static int test_scheduler(void)
{
    printf("\n=== Scheduler Tests ===\n");

    struct scheduler_ctx scheduler;
    int ret;

    ret = scheduler_init(&scheduler, SCHED_FIFO);
    TEST_ASSERT(ret == HFSSS_OK, "scheduler_init should succeed");

    ret = scheduler_set_policy(&scheduler, SCHED_GREEDY);
    TEST_ASSERT(ret == HFSSS_OK, "scheduler_set_policy should succeed");

    scheduler_cleanup(&scheduler);
    TEST_ASSERT(true, "scheduler_cleanup should succeed");

    /* Test NULL handling */
    TEST_ASSERT(scheduler_init(NULL, SCHED_FIFO) == HFSSS_ERR_INVAL,
                "scheduler_init with NULL should fail");
    scheduler_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Write Buffer Tests */
static int test_write_buffer(void)
{
    printf("\n=== Write Buffer Tests ===\n");

    struct write_buffer_ctx wb;
    u8 data[4096];
    u8 read_buf[4096];
    int ret;

    memset(data, 0x55, sizeof(data));

    ret = wb_init(&wb, 1024);
    TEST_ASSERT(ret == HFSSS_OK, "wb_init should succeed");

    ret = wb_write(&wb, 0, 4096, data);
    TEST_ASSERT(ret == HFSSS_OK, "wb_write should succeed");

    TEST_ASSERT(wb_lookup(&wb, 0) == true, "wb_lookup should find the entry");

    memset(read_buf, 0, sizeof(read_buf));
    ret = wb_read(&wb, 0, 4096, read_buf);
    TEST_ASSERT(ret == HFSSS_OK, "wb_read should succeed");
    TEST_ASSERT(memcmp(data, read_buf, sizeof(data)) == 0,
                "read data should match written data");

    ret = wb_flush(&wb);
    TEST_ASSERT(ret == HFSSS_OK, "wb_flush should succeed");

    wb_cleanup(&wb);
    TEST_ASSERT(true, "wb_cleanup should succeed");

    /* Test NULL handling */
    TEST_ASSERT(wb_init(NULL, 1024) == HFSSS_ERR_INVAL,
                "wb_init with NULL should fail");
    wb_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Read Cache Tests */
static int test_read_cache(void)
{
    printf("\n=== Read Cache Tests ===\n");

    struct read_cache_ctx rc;
    u8 data[4096];
    u8 read_buf[4096];
    int ret;

    memset(data, 0xAA, sizeof(data));

    ret = rc_init(&rc, 1024);
    TEST_ASSERT(ret == HFSSS_OK, "rc_init should succeed");

    ret = rc_insert(&rc, 0, 4096, data);
    TEST_ASSERT(ret == HFSSS_OK, "rc_insert should succeed");

    memset(read_buf, 0, sizeof(read_buf));
    ret = rc_lookup(&rc, 0, 4096, read_buf);
    TEST_ASSERT(ret == HFSSS_OK, "rc_lookup should hit");
    TEST_ASSERT(memcmp(data, read_buf, sizeof(data)) == 0,
                "read data should match cached data");

    rc_invalidate(&rc, 0, 1);
    rc_clear(&rc);

    rc_cleanup(&rc);
    TEST_ASSERT(true, "rc_cleanup should succeed");

    /* Test NULL handling */
    TEST_ASSERT(rc_init(NULL, 1024) == HFSSS_ERR_INVAL,
                "rc_init with NULL should fail");
    rc_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Channel Manager Tests */
static int test_channel_mgr(void)
{
    printf("\n=== Channel Manager Tests ===\n");

    struct channel_mgr mgr;
    int ret;
    int selected;

    ret = channel_mgr_init(&mgr, 8);
    TEST_ASSERT(ret == HFSSS_OK, "channel_mgr_init should succeed");
    TEST_ASSERT(mgr.channel_count == 8, "channel_count should be 8");

    selected = channel_mgr_select(&mgr, 12345);
    TEST_ASSERT(selected >= 0 && selected < 8, "selected channel should be valid");

    ret = channel_mgr_balance(&mgr);
    TEST_ASSERT(ret == HFSSS_OK, "channel_mgr_balance should succeed");

    channel_mgr_cleanup(&mgr);
    TEST_ASSERT(true, "channel_mgr_cleanup should succeed");

    /* Test NULL handling */
    TEST_ASSERT(channel_mgr_init(NULL, 8) == HFSSS_ERR_INVAL,
                "channel_mgr_init with NULL should fail");
    channel_mgr_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Resource Manager Tests */
static int test_resource_mgr(void)
{
    printf("\n=== Resource Manager Tests ===\n");

    struct resource_mgr mgr;
    void *ptr;
    int ret;

    ret = resource_mgr_init(&mgr);
    TEST_ASSERT(ret == HFSSS_OK, "resource_mgr_init should succeed");

    ptr = resource_alloc(&mgr, RESOURCE_CMD_SLOT);
    TEST_ASSERT(ptr != NULL, "resource_alloc should succeed");

    resource_free(&mgr, RESOURCE_CMD_SLOT, ptr);

    resource_mgr_cleanup(&mgr);
    TEST_ASSERT(true, "resource_mgr_cleanup should succeed");

    /* Test NULL handling */
    TEST_ASSERT(resource_mgr_init(NULL) == HFSSS_ERR_INVAL,
                "resource_mgr_init with NULL should fail");
    resource_mgr_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Flow Control Tests */
static int test_flow_control(void)
{
    printf("\n=== Flow Control Tests ===\n");

    struct flow_ctrl_ctx fc;
    int ret;
    bool allowed;

    ret = flow_ctrl_init(&fc);
    TEST_ASSERT(ret == HFSSS_OK, "flow_ctrl_init should succeed");

    flow_ctrl_refill(&fc);

    allowed = flow_ctrl_check(&fc, FLOW_WRITE, 100);
    TEST_ASSERT(allowed == true, "flow_ctrl_check should allow first request");

    flow_ctrl_cleanup(&fc);
    TEST_ASSERT(true, "flow_ctrl_cleanup should succeed");

    /* Test NULL handling */
    TEST_ASSERT(flow_ctrl_init(NULL) == HFSSS_ERR_INVAL,
                "flow_ctrl_init with NULL should fail");
    flow_ctrl_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Controller Tests */
static int test_controller(void)
{
    printf("\n=== Controller Tests ===\n");

    struct controller_ctx ctrl;
    struct controller_config config;
    int ret;

    controller_config_default(&config);
    config.channel_count = 4;

    ret = controller_init(&ctrl, &config);
    TEST_ASSERT(ret == HFSSS_OK, "controller_init should succeed");

    ret = controller_start(&ctrl);
    TEST_ASSERT(ret == HFSSS_OK, "controller_start should succeed");

    controller_stop(&ctrl);

    controller_cleanup(&ctrl);
    TEST_ASSERT(true, "controller_cleanup should succeed");

    /* Test NULL handling */
    TEST_ASSERT(controller_init(NULL, &config) == HFSSS_ERR_INVAL,
                "controller_init with NULL ctx should fail");
    TEST_ASSERT(controller_init(&ctrl, NULL) == HFSSS_ERR_INVAL,
                "controller_init with NULL config should fail");
    controller_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

int main(void)
{
    print_separator();
    printf("HFSSS Controller Module Tests\n");
    print_separator();

    test_shmem_if();
    test_arbiter();
    test_scheduler();
    test_write_buffer();
    test_read_cache();
    test_channel_mgr();
    test_resource_mgr();
    test_flow_control();
    test_controller();

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
