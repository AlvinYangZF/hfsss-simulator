#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include "common/log.h"
#include "common/mempool.h"
#include "common/msgqueue.h"
#include "common/semaphore.h"
#include "common/mutex.h"
#include "common/memory.h"
#include "common/watchdog.h"

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

/* Log Tests */
static int test_log(void)
{
    printf("\n=== Log Module Tests ===\n");

    struct log_ctx ctx;
    int ret;

    ret = log_init(&ctx, 1024, LOG_LEVEL_DEBUG);
    TEST_ASSERT(ret == HFSSS_OK, "log_init should succeed");

    log_set_level(&ctx, LOG_LEVEL_DEBUG);

    log_info(&ctx, "TEST", "This is an info message");
    log_debug(&ctx, "TEST", "This is a debug message");
    log_warn(&ctx, "TEST", "This is a warning message");
    log_error(&ctx, "TEST", "This is an error message");

    /* Test log persistence to file */
    const char *test_log_file = "/tmp/hfsss_test.log";
    remove(test_log_file); /* Remove if exists */
    log_set_output_file(&ctx, test_log_file);
    log_info(&ctx, "TEST", "This is a persisted info message");
    log_error(&ctx, "TEST", "This is a persisted error message");
    log_set_output_file(&ctx, NULL); /* Close file */

    /* Verify file was created and has content */
    FILE *f = fopen(test_log_file, "r");
    TEST_ASSERT(f != NULL, "log file should be created");
    if (f) {
        char buf[512];
        int found = 0;
        while (fgets(buf, sizeof(buf), f)) {
            if (strstr(buf, "persisted info message")) {
                found++;
            }
            if (strstr(buf, "persisted error message")) {
                found++;
            }
        }
        fclose(f);
        TEST_ASSERT(found == 2, "log file should contain both persisted messages");
        remove(test_log_file);
    }

    log_cleanup(&ctx);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Assert/Panic Tests (basic compile check and macro test) */
static int test_assert(void)
{
    printf("\n=== Assert Module Tests ===\n");

    /* Test that HFSSS_ASSERT with true condition doesn't crash */
    HFSSS_ASSERT(1 == 1);
    TEST_ASSERT(1, "HFSSS_ASSERT(true) should not crash");

    HFSSS_ASSERT_MSG(2 == 2, "Two should equal two");
    TEST_ASSERT(1, "HFSSS_ASSERT_MSG(true) should not crash");

    /* Note: Testing HFSSS_ASSERT(false) would crash, so we skip that in tests */

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Memory Pool Tests */
static int test_mempool(void)
{
    printf("\n=== Memory Pool Tests ===\n");

    struct mem_pool pool;
    int ret;

    ret = mem_pool_init(&pool, 64, 1024);
    TEST_ASSERT(ret == HFSSS_OK, "mem_pool_init should succeed");

    void *ptr1 = mem_pool_alloc(&pool);
    TEST_ASSERT(ptr1 != NULL, "first allocation should succeed");

    void *ptr2 = mem_pool_alloc(&pool);
    TEST_ASSERT(ptr2 != NULL, "second allocation should succeed");

    u32 used, free;
    u64 alloc_total, free_total;
    mem_pool_stats(&pool, &used, &free, &alloc_total, &free_total);
    TEST_ASSERT(used == 2, "used count should be 2");

    mem_pool_free(&pool, ptr1);
    mem_pool_stats(&pool, &used, &free, &alloc_total, &free_total);
    TEST_ASSERT(used == 1, "used count should be 1 after free");

    mem_pool_free(&pool, ptr2);
    mem_pool_stats(&pool, &used, &free, &alloc_total, &free_total);
    TEST_ASSERT(used == 0, "used count should be 0 after second free");

    /* Test full allocation */
    void *ptrs[1024];
    int alloc_count = 0;
    for (int i = 0; i < 1024; i++) {
        ptrs[i] = mem_pool_alloc(&pool);
        if (ptrs[i]) {
            alloc_count++;
        }
    }
    TEST_ASSERT(alloc_count == 1024, "should allocate all 1024 blocks");

    void *extra = mem_pool_alloc(&pool);
    TEST_ASSERT(extra == NULL, "allocation should fail when pool is full");

    for (int i = 0; i < 1024; i++) {
        if (ptrs[i]) {
            mem_pool_free(&pool, ptrs[i]);
        }
    }

    mem_pool_cleanup(&pool);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Memory Pool Alignment Regression
 *
 * When allocator metadata was moved out-of-band, an earlier version
 * of this file used raw block_size as the slot stride. That meant
 * mem_pool_alloc() with block_size=1 could return addresses only one
 * byte apart — so only one of every MEMPOOL_MIN_ALIGN allocations was
 * naturally aligned for wider types. Any caller storing a uint64_t or
 * a pointer in such a block would hit UB. The fix rounds the internal
 * stride up to MEMPOOL_MIN_ALIGN and uses it for all address math. */
static int test_mempool_alignment(void)
{
    printf("\n=== Memory Pool Alignment Regression ===\n");

    /* Try several small sizes that are NOT multiples of the minimum
     * alignment. Each must still hand out MEMPOOL_MIN_ALIGN-aligned
     * pointers for every slot. */
    const u32 sizes[] = {1, 3, 7, 9, 15, 17};
    const int n_sizes = (int)(sizeof(sizes) / sizeof(sizes[0]));

    for (int s = 0; s < n_sizes; s++) {
        struct mem_pool pool;
        int ret = mem_pool_init(&pool, sizes[s], 32);
        TEST_ASSERT(ret == HFSSS_OK, "init with small block_size");

        TEST_ASSERT(pool.slot_size >= MEMPOOL_MIN_ALIGN, "slot_size rounded up to MEMPOOL_MIN_ALIGN");
        TEST_ASSERT((pool.slot_size % MEMPOOL_MIN_ALIGN) == 0, "slot_size is a multiple of MEMPOOL_MIN_ALIGN");

        void *ptrs[32];
        int all_aligned = 1;
        for (int i = 0; i < 32; i++) {
            ptrs[i] = mem_pool_alloc(&pool);
            if (!ptrs[i]) {
                all_aligned = 0;
                break;
            }
            if (((uintptr_t)ptrs[i] % MEMPOOL_MIN_ALIGN) != 0) {
                all_aligned = 0;
                break;
            }
        }
        TEST_ASSERT(all_aligned, "every alloc is MEMPOOL_MIN_ALIGN-aligned");

        /* round-trip each one to confirm free() still recognises the
         * address under the new slot_size stride. */
        for (int i = 0; i < 32; i++) {
            if (ptrs[i]) {
                mem_pool_free(&pool, ptrs[i]);
            }
        }
        u32 used, free_c;
        u64 at, ft;
        mem_pool_stats(&pool, &used, &free_c, &at, &ft);
        TEST_ASSERT(used == 0, "all slots freed after round-trip");

        mem_pool_cleanup(&pool);
    }

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Message Queue Tests */
static int test_msgqueue(void)
{
    printf("\n=== Message Queue Tests ===\n");

    struct msg_queue mq;
    int ret;

    ret = msg_queue_init(&mq, 64, 16);
    TEST_ASSERT(ret == HFSSS_OK, "msg_queue_init should succeed");

    /* Test send/recv */
    char send_msg1[64] = "Hello, HFSSS!";
    ret = msg_queue_send(&mq, send_msg1, 0);
    TEST_ASSERT(ret == HFSSS_OK, "first send should succeed");

    TEST_ASSERT(msg_queue_count(&mq) == 1, "queue count should be 1 after send");

    char recv_msg1[64];
    ret = msg_queue_recv(&mq, recv_msg1, 0);
    TEST_ASSERT(ret == HFSSS_OK, "first recv should succeed");
    TEST_ASSERT(strcmp(recv_msg1, send_msg1) == 0, "received message should match");

    TEST_ASSERT(msg_queue_count(&mq) == 0, "queue count should be 0 after recv");

    /* Test tryrecv on empty queue */
    char recv_empty[64];
    ret = msg_queue_tryrecv(&mq, recv_empty);
    TEST_ASSERT(ret == HFSSS_ERR_NOENT, "tryrecv on empty queue should fail");

    /* Test multiple messages */
    for (int i = 0; i < 16; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Message %d", i);
        ret = msg_queue_send(&mq, msg, 0);
        TEST_ASSERT(ret == HFSSS_OK, "send should succeed");
    }

    TEST_ASSERT(msg_queue_count(&mq) == 16, "queue should be full");

    /* Test trysend on full queue */
    char extra_msg[64] = "Extra Message";
    ret = msg_queue_trysend(&mq, extra_msg);
    TEST_ASSERT(ret == HFSSS_ERR_BUSY, "trysend on full queue should fail");

    /* Receive all messages */
    for (int i = 0; i < 16; i++) {
        char msg[64];
        ret = msg_queue_recv(&mq, msg, 0);
        TEST_ASSERT(ret == HFSSS_OK, "recv should succeed");

        char expected[64];
        snprintf(expected, sizeof(expected), "Message %d", i);
        TEST_ASSERT(strcmp(msg, expected) == 0, "message order should be preserved");
    }

    msg_queue_cleanup(&mq);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Semaphore Tests */
static int test_semaphore(void)
{
    printf("\n=== Semaphore Tests ===\n");

    struct semaphore sem;
    int ret;

    /* Test init with initial count 1 */
    ret = semaphore_init(&sem, 1);
    TEST_ASSERT(ret == HFSSS_OK, "semaphore_init should succeed with initial count 1");

    /* Test trytake - should succeed */
    ret = semaphore_trytake(&sem);
    TEST_ASSERT(ret == HFSSS_OK, "semaphore_trytake should succeed when count > 0");

    /* Test get_count - should be 0 */
    ret = semaphore_get_count(&sem);
    TEST_ASSERT(ret == 0, "semaphore_get_count should return 0 after take");

    /* Test trytake - should fail */
    ret = semaphore_trytake(&sem);
    TEST_ASSERT(ret == HFSSS_ERR_BUSY, "semaphore_trytake should fail when count == 0");

    /* Test give */
    ret = semaphore_give(&sem);
    TEST_ASSERT(ret == HFSSS_OK, "semaphore_give should succeed");

    /* Test get_count - should be 1 */
    ret = semaphore_get_count(&sem);
    TEST_ASSERT(ret == 1, "semaphore_get_count should return 1 after give");

    /* Test take with timeout 0 (non-blocking) */
    ret = semaphore_take(&sem, 0);
    TEST_ASSERT(ret == HFSSS_OK, "semaphore_take with timeout 0 should succeed when count > 0");

    /* Test take with timeout 0 - should fail */
    ret = semaphore_take(&sem, 0);
    TEST_ASSERT(ret == HFSSS_ERR_BUSY, "semaphore_take with timeout 0 should fail when count == 0");

    /* Test give multiple times */
    semaphore_give(&sem);
    semaphore_give(&sem);
    semaphore_give(&sem);
    ret = semaphore_get_count(&sem);
    TEST_ASSERT(ret == 3, "semaphore_get_count should return 3 after three gives");

    /* Test stats */
    u64 wait_count, signal_count;
    semaphore_stats(&sem, &wait_count, &signal_count);
    TEST_ASSERT(signal_count == 4, "signal_count should be 4 (four gives)");

    semaphore_cleanup(&sem);

    /* Test init with initial count 0 */
    ret = semaphore_init(&sem, 0);
    TEST_ASSERT(ret == HFSSS_OK, "semaphore_init should succeed with initial count 0");

    ret = semaphore_get_count(&sem);
    TEST_ASSERT(ret == 0, "semaphore_get_count should return 0");

    semaphore_cleanup(&sem);

    /* Test init with initial count 5 */
    ret = semaphore_init(&sem, 5);
    TEST_ASSERT(ret == HFSSS_OK, "semaphore_init should succeed with initial count 5");

    for (int i = 0; i < 5; i++) {
        ret = semaphore_trytake(&sem);
        TEST_ASSERT(ret == HFSSS_OK, "semaphore_trytake should succeed 5 times");
    }

    ret = semaphore_trytake(&sem);
    TEST_ASSERT(ret == HFSSS_ERR_BUSY, "semaphore_trytake should fail after 5 takes");

    semaphore_cleanup(&sem);

    /* Test NULL handling */
    ret = semaphore_init(NULL, 1);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "semaphore_init with NULL should fail");

    ret = semaphore_trytake(NULL);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "semaphore_trytake with NULL should fail");

    ret = semaphore_take(NULL, 0);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "semaphore_take with NULL should fail");

    ret = semaphore_give(NULL);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "semaphore_give with NULL should fail");

    ret = semaphore_get_count(NULL);
    TEST_ASSERT(ret == -1, "semaphore_get_count with NULL should return -1");

    semaphore_stats(NULL, NULL, NULL);
    /* Should not crash */

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Mutex Tests */
static int test_mutex(void)
{
    printf("\n=== Mutex Tests ===\n");

    struct mutex mtx;
    int ret;

    /* Test init */
    ret = mutex_init(&mtx);
    TEST_ASSERT(ret == HFSSS_OK, "mutex_init should succeed");

    /* Test trylock - should succeed */
    ret = mutex_trylock(&mtx);
    TEST_ASSERT(ret == HFSSS_OK, "mutex_trylock should succeed when unlocked");

    /* Test unlock */
    ret = mutex_unlock(&mtx);
    TEST_ASSERT(ret == HFSSS_OK, "mutex_unlock should succeed");

    /* Test lock with timeout 0 (no timeout) */
    ret = mutex_lock(&mtx, 0);
    TEST_ASSERT(ret == HFSSS_OK, "mutex_lock should succeed");

    /* Test recursive locking - should succeed (recursive mutex) */
    ret = mutex_trylock(&mtx);
    TEST_ASSERT(ret == HFSSS_OK, "mutex_trylock should succeed recursively");

    /* Unlock twice */
    mutex_unlock(&mtx);
    mutex_unlock(&mtx);

    /* Test stats */
    u64 lock_count, unlock_count;
    mutex_stats(&mtx, &lock_count, &unlock_count);
    TEST_ASSERT(lock_count == 3, "lock_count should be 3");
    TEST_ASSERT(unlock_count == 3, "unlock_count should be 3");

    mutex_cleanup(&mtx);

    /* Test NULL handling */
    ret = mutex_init(NULL);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "mutex_init with NULL should fail");

    ret = mutex_trylock(NULL);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "mutex_trylock with NULL should fail");

    ret = mutex_lock(NULL, 0);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "mutex_lock with NULL should fail");

    ret = mutex_unlock(NULL);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "mutex_unlock with NULL should fail");

    mutex_stats(NULL, NULL, NULL);
    /* Should not crash */

    /* Test multiple lock/unlock cycles */
    ret = mutex_init(&mtx);
    TEST_ASSERT(ret == HFSSS_OK, "mutex_init should succeed again");

    for (int i = 0; i < 100; i++) {
        mutex_lock(&mtx, 0);
        mutex_unlock(&mtx);
    }

    mutex_stats(&mtx, &lock_count, &unlock_count);
    TEST_ASSERT(lock_count == 100, "lock_count should be 100");
    TEST_ASSERT(unlock_count == 100, "unlock_count should be 100");

    mutex_cleanup(&mtx);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Memory Tests */
static int test_memory(void)
{
    printf("\n=== Memory Module Tests ===\n");

    struct mem_region region;
    int ret;

    /* Test basic allocation */
    ret = mem_region_alloc(&region, 1024 * 1024, MEM_ALLOC_ZERO | MEM_ALLOC_POPULATE);
    TEST_ASSERT(ret == HFSSS_OK, "mem_region_alloc should succeed");
    TEST_ASSERT(region.addr != NULL, "region.addr should not be NULL");
    TEST_ASSERT(region.size >= 1024 * 1024, "region.size should be at least 1MB");

    /* Test bump allocator */
    void *ptr1 = mem_region_bump_alloc(&region, 256);
    TEST_ASSERT(ptr1 != NULL, "first bump alloc should succeed");

    void *ptr2 = mem_region_bump_alloc(&region, 512);
    TEST_ASSERT(ptr2 != NULL, "second bump alloc should succeed");
    TEST_ASSERT(ptr2 > ptr1, "second alloc should be after first");

    /* Test that memory is zero-initialized */
    if (ptr1) {
        int all_zero = 1;
        for (int i = 0; i < 256; i++) {
            if (((char *)ptr1)[i] != 0) {
                all_zero = 0;
                break;
            }
        }
        TEST_ASSERT(all_zero, "memory should be zero-initialized");
    }

    /* Test bump reset */
    mem_region_bump_reset(&region);
    TEST_ASSERT(region.allocated == 0, "allocated should be 0 after reset");

    void *ptr3 = mem_region_bump_alloc(&region, 1024);
    TEST_ASSERT(ptr3 == ptr1, "alloc after reset should reuse memory");

    /* Free the region */
    mem_region_free(&region);
    TEST_ASSERT(region.addr == NULL, "region.addr should be NULL after free");

    /* Test huge page availability check */
    bool hugetlb_available = mem_hugetlb_available();
    /* Just check that the function returns a value without crashing */
    TEST_ASSERT(1, "mem_hugetlb_available should execute");

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Watchdog Tests */
static int timeout_occurred = 0;
static int timeout_task_id = -1;

static void watchdog_test_timeout_cb(int task_id, const char *task_name, void *user_data)
{
    (void)task_name;
    (void)user_data;
    timeout_occurred = 1;
    timeout_task_id = task_id;
}

static int test_watchdog(void)
{
    printf("\n=== Watchdog Module Tests ===\n");

    struct watchdog_ctx ctx;
    int ret;
    int task_id1, task_id2;

    /* Test init */
    ret = watchdog_init(&ctx, 100 * 1000 * 1000ULL, /* 100ms check interval */
                        watchdog_test_timeout_cb, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "watchdog_init should succeed");

    /* Test register tasks */
    task_id1 = watchdog_register_task(&ctx, "task1", 500 * 1000 * 1000ULL, NULL); /* 500ms timeout */
    TEST_ASSERT(task_id1 >= 0, "register task1 should succeed");

    task_id2 = watchdog_register_task(&ctx, "task2", 2000 * 1000 * 1000ULL, NULL); /* 2s timeout */
    TEST_ASSERT(task_id2 >= 0, "register task2 should succeed");
    TEST_ASSERT(task_id2 != task_id1, "task IDs should be different");

    /* Test stats */
    u64 timeout_count;
    int active_tasks;
    watchdog_stats(&ctx, &timeout_count, &active_tasks);
    TEST_ASSERT(active_tasks == 2, "should have 2 active tasks");
    TEST_ASSERT(timeout_count == 0, "timeout_count should be 0");

    /* Test feeding */
    ret = watchdog_feed(&ctx, task_id1);
    TEST_ASSERT(ret == HFSSS_OK, "feed task1 should succeed");

    /* Test disable/enable */
    ret = watchdog_disable_task(&ctx, task_id2);
    TEST_ASSERT(ret == HFSSS_OK, "disable task2 should succeed");

    watchdog_stats(&ctx, &timeout_count, &active_tasks);
    TEST_ASSERT(active_tasks == 1, "should have 1 active task after disable");

    ret = watchdog_enable_task(&ctx, task_id2);
    TEST_ASSERT(ret == HFSSS_OK, "enable task2 should succeed");

    /* Test unregister */
    ret = watchdog_unregister_task(&ctx, task_id2);
    TEST_ASSERT(ret == HFSSS_OK, "unregister task2 should succeed");

    /* Test starting the watchdog (without expecting a timeout) */
    timeout_occurred = 0;
    ret = watchdog_start(&ctx);
    TEST_ASSERT(ret == HFSSS_OK, "watchdog_start should succeed");

    /* Feed task1 a few times */
    for (int i = 0; i < 5; i++) {
        watchdog_feed(&ctx, task_id1);
        sleep_ns(50 * 1000 * 1000ULL); /* 50ms */
    }

    TEST_ASSERT(timeout_occurred == 0, "no timeout should have occurred yet");

    /* Stop the watchdog */
    watchdog_stop(&ctx);

    /* Cleanup */
    watchdog_cleanup(&ctx);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Main */
int main(void)
{
    printf("========================================\n");
    printf("HFSSS Common Service Tests\n");
    printf("========================================\n");

    int result = 0;

    (void)result; /* Suppress unused variable warning */

    test_log();
    test_assert();
    test_mempool();
    test_mempool_alignment();
    test_msgqueue();
    test_semaphore();
    test_mutex();
    test_memory();
    test_watchdog();

    printf("\n========================================\n");
    printf("Test Summary\n");
    printf("========================================\n");
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    if (tests_failed == 0) {
        printf("\n  [SUCCESS] All tests passed!\n");
        return 0;
    } else {
        printf("\n  [FAILURE] Some tests failed!\n");
        return 1;
    }
}
