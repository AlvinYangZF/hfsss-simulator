#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "common/log.h"
#include "common/mempool.h"
#include "common/msgqueue.h"
#include "common/semaphore.h"
#include "common/mutex.h"

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
    test_msgqueue();
    test_semaphore();
    test_mutex();

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
