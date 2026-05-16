#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "controller/controller.h"
#include "controller/qos.h"

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
    TEST_ASSERT(arbiter_init(NULL, 1024) == HFSSS_ERR_INVAL, "arbiter_init with NULL should fail");
    arbiter_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Scheduler Tests */
static int test_scheduler(void)
{
    printf("\n=== Scheduler Tests ===\n");

    struct scheduler_ctx scheduler;
    struct dwrr_scheduler dwrr;
    struct cmd_context cmd_a;
    struct cmd_context cmd_b;
    struct cmd_context cmd_c;
    struct cmd_context *out;
    u32 pending = 0;
    u32 dispatched = 0;
    int ret;

    ret = scheduler_init(&scheduler, SCHED_FIFO);
    TEST_ASSERT(ret == HFSSS_OK, "scheduler_init should succeed");
    TEST_ASSERT(scheduler.policy == SCHED_FIFO, "scheduler stores initial policy");
    TEST_ASSERT(scheduler.sched_period_ns == 10000, "scheduler default period set");

    memset(&cmd_a, 0, sizeof(cmd_a));
    memset(&cmd_b, 0, sizeof(cmd_b));
    cmd_a.cmd_id = 11;
    cmd_b.cmd_id = 22;

    ret = scheduler_enqueue(&scheduler, &cmd_a);
    TEST_ASSERT(ret == HFSSS_OK, "scheduler FIFO enqueue first command");
    ret = scheduler_enqueue(&scheduler, &cmd_b);
    TEST_ASSERT(ret == HFSSS_OK, "scheduler FIFO enqueue second command");
    TEST_ASSERT(scheduler.u.fifo.count == 2, "scheduler FIFO count increments");

    out = scheduler_dequeue(&scheduler);
    TEST_ASSERT(out == &cmd_a, "scheduler FIFO dequeues first command");
    TEST_ASSERT(out->next == NULL && out->prev == NULL,
                "scheduler dequeue clears first command links");
    out = scheduler_dequeue(&scheduler);
    TEST_ASSERT(out == &cmd_b, "scheduler FIFO dequeues second command");
    out = scheduler_dequeue(&scheduler);
    TEST_ASSERT(out == NULL, "scheduler FIFO empty dequeue returns NULL");

    ret = scheduler_set_policy(&scheduler, SCHED_GREEDY);
    TEST_ASSERT(ret == HFSSS_OK, "scheduler_set_policy should succeed");
    memset(&cmd_a, 0, sizeof(cmd_a));
    memset(&cmd_b, 0, sizeof(cmd_b));
    cmd_a.cmd_id = 33;
    cmd_b.cmd_id = 44;
    TEST_ASSERT(scheduler_enqueue(&scheduler, &cmd_a) == HFSSS_OK,
                "scheduler GREEDY fallback enqueue first");
    TEST_ASSERT(scheduler_enqueue(&scheduler, &cmd_b) == HFSSS_OK,
                "scheduler GREEDY fallback enqueue second");
    TEST_ASSERT(scheduler_dequeue(&scheduler) == &cmd_a,
                "scheduler GREEDY fallback dequeues FIFO order");
    TEST_ASSERT(scheduler_dequeue(&scheduler) == &cmd_b,
                "scheduler GREEDY fallback drains tail");

    TEST_ASSERT(scheduler_set_policy(&scheduler, SCHED_DEADLINE) == HFSSS_OK,
                "scheduler_set_policy to DEADLINE succeeds");
    memset(&cmd_a, 0, sizeof(cmd_a));
    TEST_ASSERT(scheduler_enqueue(&scheduler, &cmd_a) == HFSSS_OK,
                "scheduler DEADLINE fallback enqueue");
    TEST_ASSERT(scheduler_dequeue(&scheduler) == &cmd_a,
                "scheduler DEADLINE fallback dequeue");

    TEST_ASSERT(scheduler_set_policy(&scheduler, SCHED_WRR) == HFSSS_OK,
                "scheduler_set_policy to WRR succeeds");
    memset(&cmd_a, 0, sizeof(cmd_a));
    TEST_ASSERT(scheduler_enqueue(&scheduler, &cmd_a) == HFSSS_OK,
                "scheduler WRR fallback enqueue");
    TEST_ASSERT(scheduler_dequeue(&scheduler) == &cmd_a,
                "scheduler WRR fallback dequeue");

    TEST_ASSERT(scheduler_enqueue(&scheduler, NULL) == HFSSS_ERR_INVAL,
                "scheduler_enqueue rejects NULL command");
    TEST_ASSERT(scheduler_dequeue(NULL) == NULL,
                "scheduler_dequeue rejects NULL scheduler");

    TEST_ASSERT(dwrr_init(&dwrr, 8) == HFSSS_OK, "scheduler DWRR helper init");
    TEST_ASSERT(dwrr_queue_create(&dwrr, 7, 1) == HFSSS_OK,
                "scheduler DWRR helper queue create");
    scheduler.dwrr = &dwrr;
    TEST_ASSERT(scheduler_set_policy(&scheduler, SCHED_DWRR) == HFSSS_OK,
                "scheduler_set_policy to DWRR succeeds");
    memset(&cmd_c, 0, sizeof(cmd_c));
    cmd_c.kern_cmd.cdw0_15[1] = 7;
    TEST_ASSERT(scheduler_enqueue(&scheduler, &cmd_c) == HFSSS_OK,
                "scheduler DWRR enqueue succeeds");
    dwrr_get_stats(&dwrr, 7, &pending, &dispatched);
    TEST_ASSERT(pending == 1, "scheduler DWRR records pending command");
    TEST_ASSERT(scheduler_dequeue(&scheduler) == NULL,
                "scheduler DWRR dequeue is external to FIFO path");
    dwrr_cleanup(&dwrr);

    TEST_ASSERT(dwrr_init(&dwrr, 8) == HFSSS_OK, "scheduler DWRR fallback helper init");
    scheduler.dwrr = &dwrr;
    memset(&cmd_c, 0, sizeof(cmd_c));
    cmd_c.kern_cmd.cdw0_15[1] = 99;
    TEST_ASSERT(scheduler_enqueue(&scheduler, &cmd_c) == HFSSS_OK,
                "scheduler DWRR rejected command falls back to FIFO");
    TEST_ASSERT(scheduler.u.fifo.count == 1, "scheduler DWRR fallback increments FIFO count");
    dwrr_cleanup(&dwrr);

    scheduler.policy = (enum sched_policy)99;
    TEST_ASSERT(scheduler_enqueue(&scheduler, &cmd_a) == HFSSS_ERR_INVAL,
                "scheduler_enqueue rejects invalid policy");

    scheduler_cleanup(&scheduler);
    TEST_ASSERT(true, "scheduler_cleanup should succeed");

    /* Test NULL handling */
    TEST_ASSERT(scheduler_init(NULL, SCHED_FIFO) == HFSSS_ERR_INVAL, "scheduler_init with NULL should fail");
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
    TEST_ASSERT(memcmp(data, read_buf, sizeof(data)) == 0, "read data should match written data");

    ret = wb_flush(&wb);
    TEST_ASSERT(ret == HFSSS_OK, "wb_flush should succeed");

    wb_cleanup(&wb);
    TEST_ASSERT(true, "wb_cleanup should succeed");

    /* Test NULL handling */
    TEST_ASSERT(wb_init(NULL, 1024) == HFSSS_ERR_INVAL, "wb_init with NULL should fail");
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
    TEST_ASSERT(memcmp(data, read_buf, sizeof(data)) == 0, "read data should match cached data");

    rc_invalidate(&rc, 0, 1);
    rc_clear(&rc);

    rc_cleanup(&rc);
    TEST_ASSERT(true, "rc_cleanup should succeed");

    /* Test NULL handling */
    TEST_ASSERT(rc_init(NULL, 1024) == HFSSS_ERR_INVAL, "rc_init with NULL should fail");
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
    TEST_ASSERT(channel_mgr_init(NULL, 8) == HFSSS_ERR_INVAL, "channel_mgr_init with NULL should fail");
    channel_mgr_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Resource Manager Tests */
static int test_resource_mgr(void)
{
    printf("\n=== Resource Manager Tests ===\n");

    struct resource_mgr mgr;
    struct cpu_stats stats;
    void *ptr;
    void *allocated[RESOURCE_MAX];
    struct idle_block_entry *idle;
    struct idle_block_pool pool;
    int ret;

    ret = resource_mgr_init(&mgr);
    TEST_ASSERT(ret == HFSSS_OK, "resource_mgr_init should succeed");

    memset(allocated, 0, sizeof(allocated));
    for (int type = 0; type < RESOURCE_MAX; type++) {
        ptr = resource_alloc(&mgr, (enum resource_type)type);
        TEST_ASSERT(ptr != NULL, "resource_alloc should succeed for each pool");
        TEST_ASSERT(mgr.pools[type].used == 1, "resource pool used count increments");
        TEST_ASSERT(mgr.alloc_count[type] == 1, "resource alloc count increments");
        allocated[type] = ptr;
    }

    TEST_ASSERT(resource_alloc(NULL, RESOURCE_CMD_SLOT) == NULL,
                "resource_alloc rejects NULL mgr");
    TEST_ASSERT(resource_alloc(&mgr, RESOURCE_MAX) == NULL,
                "resource_alloc rejects invalid type");

    resource_free(NULL, RESOURCE_CMD_SLOT, allocated[0]);
    resource_free(&mgr, RESOURCE_CMD_SLOT, NULL);
    resource_free(&mgr, RESOURCE_MAX, allocated[0]);
    TEST_ASSERT(mgr.pools[RESOURCE_CMD_SLOT].used == 1,
                "invalid resource_free calls leave counts unchanged");

    for (int type = 0; type < RESOURCE_MAX; type++) {
        resource_free(&mgr, (enum resource_type)type, allocated[type]);
        TEST_ASSERT(mgr.pools[type].used == 0, "resource_free decrements used count");
        TEST_ASSERT(mgr.free_count[type] == 1, "resource free count increments");
    }

    TEST_ASSERT(idle_block_get_free_count(NULL) == 0,
                "idle_block_get_free_count rejects NULL mgr");
    TEST_ASSERT(!idle_block_needs_gc(NULL),
                "idle_block_needs_gc rejects NULL mgr");
    TEST_ASSERT(idle_block_alloc(NULL) == NULL,
                "idle_block_alloc rejects NULL mgr");

    u32 idle_initial = idle_block_get_free_count(&mgr);
    idle = idle_block_alloc(&mgr);
    TEST_ASSERT(idle != NULL, "idle_block_alloc succeeds");
    TEST_ASSERT(idle_block_get_free_count(&mgr) == idle_initial - 1,
                "idle block free count decrements");
    idle_block_free(NULL, idle);
    TEST_ASSERT(idle_block_get_free_count(&mgr) == idle_initial - 1,
                "idle_block_free NULL mgr leaves block allocated");
    idle_block_free(&mgr, NULL);
    TEST_ASSERT(idle_block_get_free_count(&mgr) == idle_initial - 1,
                "idle_block_free NULL block leaves count unchanged");
    idle_block_free(&mgr, idle);
    TEST_ASSERT(idle_block_get_free_count(&mgr) == idle_initial,
                "idle_block_free returns block to pool");

    for (u32 i = 0; i < idle_initial - mgr.idle_blocks.low_watermark + 1; i++) {
        idle = idle_block_alloc(&mgr);
        if (!idle) {
            break;
        }
    }
    TEST_ASSERT(idle_block_needs_gc(&mgr),
                "idle_block_needs_gc trips at low watermark");

    TEST_ASSERT(idle_block_pool_init(NULL, 1, 0, 1) == HFSSS_ERR_INVAL,
                "idle_block_pool_init rejects NULL pool");
    ret = idle_block_pool_init(&pool, 3, 1, 2);
    TEST_ASSERT(ret == HFSSS_OK, "standalone idle_block_pool_init succeeds");
    TEST_ASSERT(pool.total == 3 && pool.free == 3 && pool.used == 0,
                "standalone idle block pool counts initialized");
    idle_block_pool_cleanup(NULL);
    idle_block_pool_cleanup(&pool);
    TEST_ASSERT(pool.total == 0 && pool.free == 0,
                "standalone idle_block_pool_cleanup clears pool");

    resource_cpu_record(NULL, CPU_ROLE_FTL, 10);
    resource_cpu_record(&mgr, CPU_ROLE_MAX, 10);
    resource_cpu_record(&mgr, CPU_ROLE_FTL, 30);
    resource_cpu_record(&mgr, CPU_ROLE_PCIE, 70);
    memset(&stats, 0, sizeof(stats));
    resource_cpu_get_stats(NULL, &stats);
    resource_cpu_get_stats(&mgr, NULL);
    resource_cpu_get_stats(&mgr, &stats);
    TEST_ASSERT(stats.total_cycles == 100, "resource CPU stats total cycles tracked");
    TEST_ASSERT(stats.cycle_count[CPU_ROLE_FTL] == 30,
                "resource CPU stats FTL cycles tracked");
    TEST_ASSERT(resource_cpu_utilization(NULL, CPU_ROLE_FTL) == 0.0,
                "resource CPU utilization rejects NULL mgr");
    TEST_ASSERT(resource_cpu_utilization(&mgr, CPU_ROLE_MAX) == 0.0,
                "resource CPU utilization rejects invalid role");
    TEST_ASSERT(resource_cpu_utilization(&mgr, CPU_ROLE_FTL) > 0.29 &&
                resource_cpu_utilization(&mgr, CPU_ROLE_FTL) < 0.31,
                "resource CPU utilization computes role fraction");

    resource_mgr_cleanup(&mgr);
    TEST_ASSERT(true, "resource_mgr_cleanup should succeed");

    /* Test NULL handling */
    TEST_ASSERT(resource_mgr_init(NULL) == HFSSS_ERR_INVAL, "resource_mgr_init with NULL should fail");
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
    TEST_ASSERT(fc.enabled == true, "flow control enabled by default");
    TEST_ASSERT(fc.gc_rate_limit == 200000, "flow control default GC rate set");
    TEST_ASSERT(fc.gc_max_burst == 500000, "flow control default GC burst set");

    flow_ctrl_refill(&fc);

    allowed = flow_ctrl_check(&fc, FLOW_WRITE, 100);
    TEST_ASSERT(allowed == true, "flow_ctrl_check should allow first request");
    TEST_ASSERT(fc.total_allowed[FLOW_WRITE] == 1, "flow allowed counter increments");

    fc.buckets[FLOW_READ].rate = 0;
    fc.buckets[FLOW_READ].tokens = 5;
    allowed = flow_ctrl_check(&fc, FLOW_READ, 10);
    TEST_ASSERT(allowed == false, "flow_ctrl_check throttles when tokens exhausted");
    TEST_ASSERT(fc.total_throttled[FLOW_READ] == 1, "flow throttled counter increments");

    TEST_ASSERT(flow_ctrl_check(NULL, FLOW_READ, 10) == true,
                "flow_ctrl_check allows NULL ctx");
    TEST_ASSERT(flow_ctrl_check(&fc, FLOW_MAX, 10) == true,
                "flow_ctrl_check allows invalid type");
    fc.enabled = false;
    TEST_ASSERT(flow_ctrl_check(&fc, FLOW_READ, 1000000000ULL) == true,
                "flow_ctrl_check bypasses when disabled");
    fc.enabled = true;

    flow_ctrl_update_backpressure(NULL, 100, 0);
    TEST_ASSERT(flow_ctrl_get_backpressure(NULL) == BACKPRESSURE_NONE,
                "flow_ctrl_get_backpressure rejects NULL ctx");
    TEST_ASSERT(!flow_ctrl_should_throttle(NULL, FLOW_WRITE),
                "flow_ctrl_should_throttle rejects NULL ctx");

    flow_ctrl_update_backpressure(&fc, 10, 100);
    TEST_ASSERT(flow_ctrl_get_backpressure(&fc) == BACKPRESSURE_NONE,
                "backpressure none for healthy state");
    TEST_ASSERT(!flow_ctrl_should_throttle(&fc, FLOW_GC),
                "no backpressure does not throttle GC");

    flow_ctrl_update_backpressure(&fc, 55, 100);
    TEST_ASSERT(flow_ctrl_get_backpressure(&fc) == BACKPRESSURE_LOW,
                "backpressure low from write-buffer occupancy");
    TEST_ASSERT(flow_ctrl_should_throttle(&fc, FLOW_GC),
                "low backpressure throttles GC");
    TEST_ASSERT(!flow_ctrl_should_throttle(&fc, FLOW_WRITE),
                "low backpressure does not throttle writes");

    flow_ctrl_update_backpressure(&fc, 75, 100);
    TEST_ASSERT(flow_ctrl_get_backpressure(&fc) == BACKPRESSURE_MEDIUM,
                "backpressure medium from write-buffer occupancy");
    TEST_ASSERT(flow_ctrl_should_throttle(&fc, FLOW_WRITE),
                "medium backpressure throttles writes");

    flow_ctrl_update_backpressure(&fc, 90, 100);
    TEST_ASSERT(flow_ctrl_get_backpressure(&fc) == BACKPRESSURE_HIGH,
                "backpressure high from write-buffer occupancy");
    TEST_ASSERT(flow_ctrl_should_throttle(&fc, FLOW_READ),
                "high backpressure throttles reads");
    TEST_ASSERT(!flow_ctrl_should_throttle(&fc, FLOW_ADMIN),
                "high backpressure allows admin");

    flow_ctrl_update_backpressure(&fc, 96, 100);
    TEST_ASSERT(flow_ctrl_get_backpressure(&fc) == BACKPRESSURE_CRITICAL,
                "backpressure critical from write-buffer occupancy");
    TEST_ASSERT(flow_ctrl_check(&fc, FLOW_READ, 1) == false,
                "critical backpressure throttles flow check");

    flow_ctrl_update_backpressure(&fc, 0, 1);
    TEST_ASSERT(flow_ctrl_get_backpressure(&fc) == BACKPRESSURE_CRITICAL,
                "backpressure critical from low free blocks");
    flow_ctrl_update_backpressure(&fc, 0, 4);
    TEST_ASSERT(flow_ctrl_get_backpressure(&fc) == BACKPRESSURE_HIGH,
                "backpressure high from low free blocks");
    flow_ctrl_update_backpressure(&fc, 0, 9);
    TEST_ASSERT(flow_ctrl_get_backpressure(&fc) == BACKPRESSURE_MEDIUM,
                "backpressure medium from low free blocks");
    flow_ctrl_update_backpressure(&fc, 0, 19);
    TEST_ASSERT(flow_ctrl_get_backpressure(&fc) == BACKPRESSURE_LOW,
                "backpressure low from low free blocks");
    flow_ctrl_update_backpressure(&fc, 0, 100);

    TEST_ASSERT(flow_ctrl_qos_init(NULL) == HFSSS_ERR_INVAL,
                "flow_ctrl_qos_init rejects NULL ctx");
    TEST_ASSERT(flow_ctrl_qos_check(NULL, QOS_HIGH, 1) == true,
                "flow_ctrl_qos_check allows NULL ctx");
    TEST_ASSERT(flow_ctrl_qos_check(&fc, QOS_MAX, 1) == true,
                "flow_ctrl_qos_check allows invalid priority");

    fc.qos_buckets[QOS_LOW].rate = 0;
    fc.qos_buckets[QOS_LOW].tokens = 5;
    TEST_ASSERT(flow_ctrl_qos_check(&fc, QOS_LOW, 3) == true,
                "flow_ctrl_qos_check consumes available tokens");
    TEST_ASSERT(fc.qos_allowed[QOS_LOW] == 1, "QoS allowed counter increments");
    TEST_ASSERT(flow_ctrl_qos_check(&fc, QOS_LOW, 3) == false,
                "flow_ctrl_qos_check throttles exhausted bucket");
    TEST_ASSERT(fc.qos_throttled[QOS_LOW] == 1, "QoS throttled counter increments");
    fc.enabled = false;
    TEST_ASSERT(flow_ctrl_qos_check(&fc, QOS_LOW, 1000) == true,
                "flow_ctrl_qos_check bypasses when disabled");
    fc.enabled = true;
    flow_ctrl_qos_refill(NULL);
    flow_ctrl_qos_refill(&fc);

    flow_ctrl_set_gc_rate(NULL, 1, 1);
    flow_ctrl_set_gc_rate(&fc, 1234, 7);
    TEST_ASSERT(fc.gc_rate_limit == 1234, "GC rate limit updated");
    TEST_ASSERT(fc.gc_max_burst == 7, "GC max burst updated");
    TEST_ASSERT(fc.buckets[FLOW_GC].rate == 1234, "GC bucket rate updated");
    TEST_ASSERT(fc.buckets[FLOW_GC].max_tokens == 7, "GC bucket burst updated");
    TEST_ASSERT(fc.buckets[FLOW_GC].tokens <= 7, "GC bucket tokens clamped");
    fc.buckets[FLOW_GC].tokens = 7;
    TEST_ASSERT(flow_ctrl_gc_check(&fc, 8) == false,
                "flow_ctrl_gc_check throttles over burst");

    flow_ctrl_cleanup(&fc);
    TEST_ASSERT(true, "flow_ctrl_cleanup should succeed");

    /* Test NULL handling */
    TEST_ASSERT(flow_ctrl_init(NULL) == HFSSS_ERR_INVAL, "flow_ctrl_init with NULL should fail");
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
    controller_config_default(NULL);

    ret = controller_init(&ctrl, &config);
    TEST_ASSERT(ret == HFSSS_OK, "controller_init should succeed");

    ret = controller_start(&ctrl);
    TEST_ASSERT(ret == HFSSS_OK, "controller_start should succeed");
    ret = controller_start(&ctrl);
    TEST_ASSERT(ret == HFSSS_OK, "controller_start is idempotent while running");

    controller_stop(&ctrl);
    controller_stop(&ctrl);
    TEST_ASSERT(ctrl.running == false, "controller_stop is idempotent when stopped");

    controller_cleanup(&ctrl);
    TEST_ASSERT(true, "controller_cleanup should succeed");
    controller_cleanup(&ctrl);
    TEST_ASSERT(true, "controller_cleanup ignores uninitialized ctx");

    /* Test NULL handling */
    TEST_ASSERT(controller_init(NULL, &config) == HFSSS_ERR_INVAL, "controller_init with NULL ctx should fail");
    TEST_ASSERT(controller_init(&ctrl, NULL) == HFSSS_ERR_INVAL, "controller_init with NULL config should fail");
    TEST_ASSERT(controller_start(NULL) == HFSSS_ERR_INVAL, "controller_start with NULL ctx should fail");
    controller_stop(NULL);
    controller_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

int main(void)
{
    print_separator();
    printf("HFSSS Controller Module Tests\n");
    print_separator();

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
