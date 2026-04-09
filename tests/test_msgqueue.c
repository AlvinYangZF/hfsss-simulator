#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/msgqueue.h"

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

/* Queue capacity and message size used across tests */
#define TEST_MSG_SIZE 64
#define TEST_QUEUE_LEN 8

/* ---- invalid argument checks ---- */

static void test_invalid_args(void)
{
    printf("\n=== msg_queue Invalid Argument Checks ===\n");

    struct msg_queue mq;
    char buf[TEST_MSG_SIZE];

    TEST_ASSERT(msg_queue_init(NULL, TEST_MSG_SIZE, TEST_QUEUE_LEN) == HFSSS_ERR_INVAL,
                "init should reject NULL queue pointer");
    TEST_ASSERT(msg_queue_init(&mq, 0, TEST_QUEUE_LEN) == HFSSS_ERR_INVAL, "init should reject zero msg_size");
    TEST_ASSERT(msg_queue_init(&mq, TEST_MSG_SIZE, 0) == HFSSS_ERR_INVAL, "init should reject zero queue_len");

    int ret = msg_queue_init(&mq, TEST_MSG_SIZE, TEST_QUEUE_LEN);
    TEST_ASSERT(ret == HFSSS_OK, "init with valid args should succeed");
    if (ret != HFSSS_OK) {
        return;
    }

    TEST_ASSERT(msg_queue_send(NULL, buf, 0) == HFSSS_ERR_INVAL, "send should reject NULL queue");
    TEST_ASSERT(msg_queue_send(&mq, NULL, 0) == HFSSS_ERR_INVAL, "send should reject NULL msg");
    TEST_ASSERT(msg_queue_recv(NULL, buf, 0) == HFSSS_ERR_INVAL, "recv should reject NULL queue");
    TEST_ASSERT(msg_queue_recv(&mq, NULL, 0) == HFSSS_ERR_INVAL, "recv should reject NULL msg");
    TEST_ASSERT(msg_queue_trysend(NULL, buf) == HFSSS_ERR_INVAL, "trysend should reject NULL queue");
    TEST_ASSERT(msg_queue_trysend(&mq, NULL) == HFSSS_ERR_INVAL, "trysend should reject NULL msg");
    TEST_ASSERT(msg_queue_tryrecv(NULL, buf) == HFSSS_ERR_INVAL, "tryrecv should reject NULL queue");
    TEST_ASSERT(msg_queue_tryrecv(&mq, NULL) == HFSSS_ERR_INVAL, "tryrecv should reject NULL msg");
    TEST_ASSERT(msg_queue_count(NULL) == 0, "count should return 0 for NULL queue");

    msg_queue_cleanup(&mq);
    msg_queue_cleanup(NULL);
    TEST_ASSERT(1, "cleanup should tolerate NULL queue");
}

/* ---- send busy on zero timeout (full queue) ---- */

static void test_send_busy_zero_timeout(void)
{
    printf("\n=== msg_queue_send Busy on Zero Timeout ===\n");

    struct msg_queue mq;
    int ret = msg_queue_init(&mq, TEST_MSG_SIZE, TEST_QUEUE_LEN);
    TEST_ASSERT(ret == HFSSS_OK, "init should succeed");
    if (ret != HFSSS_OK) {
        return;
    }

    char buf[TEST_MSG_SIZE];
    for (u32 i = 0; i < TEST_QUEUE_LEN; i++) {
        snprintf(buf, sizeof(buf), "fill-%u", i);
        ret = msg_queue_send(&mq, buf, 0);
        TEST_ASSERT(ret == HFSSS_OK, "fill send should succeed");
    }

    snprintf(buf, sizeof(buf), "overflow");
    ret = msg_queue_send(&mq, buf, 0);
    TEST_ASSERT(ret == HFSSS_ERR_BUSY, "send on full queue with timeout_ns=0 should return BUSY");

    msg_queue_cleanup(&mq);
}

/* ---- recv no-entry on zero timeout (empty queue) ---- */

static void test_recv_noent_zero_timeout(void)
{
    printf("\n=== msg_queue_recv No-Entry on Zero Timeout ===\n");

    struct msg_queue mq;
    int ret = msg_queue_init(&mq, TEST_MSG_SIZE, TEST_QUEUE_LEN);
    TEST_ASSERT(ret == HFSSS_OK, "init should succeed");
    if (ret != HFSSS_OK) {
        return;
    }

    char buf[TEST_MSG_SIZE];
    ret = msg_queue_recv(&mq, buf, 0);
    TEST_ASSERT(ret == HFSSS_ERR_NOENT, "recv on empty queue with timeout_ns=0 should return NOENT");

    msg_queue_cleanup(&mq);
}

/* ---- send timeout on full queue ---- */

static void test_send_timeout_full(void)
{
    printf("\n=== msg_queue_send Timeout on Full Queue ===\n");

    struct msg_queue mq;
    int ret = msg_queue_init(&mq, TEST_MSG_SIZE, TEST_QUEUE_LEN);
    TEST_ASSERT(ret == HFSSS_OK, "init should succeed");
    if (ret != HFSSS_OK) {
        return;
    }

    char buf[TEST_MSG_SIZE];
    for (u32 i = 0; i < TEST_QUEUE_LEN; i++) {
        snprintf(buf, sizeof(buf), "fill-%u", i);
        msg_queue_send(&mq, buf, 0);
    }

    u64 before = get_time_ns();
    snprintf(buf, sizeof(buf), "blocked");
    ret = msg_queue_send(&mq, buf, 10000000ULL);
    u64 elapsed = get_time_ns() - before;

    TEST_ASSERT(ret == HFSSS_ERR_TIMEOUT || ret == HFSSS_ERR_BUSY,
                "send on full queue with 10ms timeout should timeout or busy");
    TEST_ASSERT(elapsed < 500000000ULL, "send timeout should complete within 500ms");

    msg_queue_cleanup(&mq);
}

/* ---- recv timeout on empty queue ---- */

static void test_recv_timeout_empty(void)
{
    printf("\n=== msg_queue_recv Timeout on Empty Queue ===\n");

    struct msg_queue mq;
    int ret = msg_queue_init(&mq, TEST_MSG_SIZE, TEST_QUEUE_LEN);
    TEST_ASSERT(ret == HFSSS_OK, "init should succeed");
    if (ret != HFSSS_OK) {
        return;
    }

    char buf[TEST_MSG_SIZE];
    u64 before = get_time_ns();
    ret = msg_queue_recv(&mq, buf, 10000000ULL);
    u64 elapsed = get_time_ns() - before;

    TEST_ASSERT(ret == HFSSS_ERR_TIMEOUT || ret == HFSSS_ERR_NOENT,
                "recv on empty queue with 10ms timeout should timeout or noent");
    TEST_ASSERT(elapsed < 500000000ULL, "recv timeout should complete within 500ms");

    msg_queue_cleanup(&mq);
}

/* ---- ring wraparound ---- */

static void test_ring_wraparound(void)
{
    printf("\n=== msg_queue Ring Wraparound ===\n");

    struct msg_queue mq;
    int ret = msg_queue_init(&mq, sizeof(u32), 4);
    TEST_ASSERT(ret == HFSSS_OK, "init should succeed");
    if (ret != HFSSS_OK) {
        return;
    }

    u32 total_rounds = 3;
    u32 queue_cap = 4;
    u32 msg_count = total_rounds * queue_cap;
    int all_ok = 1;

    for (u32 i = 0; i < msg_count; i++) {
        u32 val = i;
        ret = msg_queue_trysend(&mq, &val);
        if (ret != HFSSS_OK) {
            all_ok = 0;
            break;
        }

        u32 out = 0;
        ret = msg_queue_tryrecv(&mq, &out);
        if (ret != HFSSS_OK || out != i) {
            all_ok = 0;
            break;
        }
    }

    TEST_ASSERT(all_ok, "repeated send/recv should preserve data across ring wraps");
    TEST_ASSERT(msg_queue_count(&mq) == 0, "queue should be empty after balanced send/recv");

    msg_queue_cleanup(&mq);
}

/* ---- stats tracking ---- */

static void test_stats(void)
{
    printf("\n=== msg_queue_stats Tracking ===\n");

    struct msg_queue mq;
    int ret = msg_queue_init(&mq, sizeof(u32), 8);
    TEST_ASSERT(ret == HFSSS_OK, "init should succeed");
    if (ret != HFSSS_OK) {
        return;
    }

    u64 sc = 0, rc = 0;
    msg_queue_stats(&mq, &sc, &rc);
    TEST_ASSERT(sc == 0 && rc == 0, "initial stats should be zero");

    u32 val = 42;
    for (int i = 0; i < 5; i++) {
        msg_queue_trysend(&mq, &val);
    }

    for (int i = 0; i < 3; i++) {
        msg_queue_tryrecv(&mq, &val);
    }

    msg_queue_stats(&mq, &sc, &rc);
    TEST_ASSERT(sc == 5, "send_count should be 5");
    TEST_ASSERT(rc == 3, "recv_count should be 3");

    msg_queue_stats(&mq, &sc, NULL);
    TEST_ASSERT(sc == 5, "stats should tolerate NULL recv_count");
    msg_queue_stats(&mq, NULL, &rc);
    TEST_ASSERT(rc == 3, "stats should tolerate NULL send_count");

    msg_queue_cleanup(&mq);
}

/* ---- producer/consumer wakeup with two threads ---- */

struct pc_ctx {
    struct msg_queue *mq;
    u32 count;
    int ok;
};

static void *producer_fn(void *arg)
{
    struct pc_ctx *ctx = (struct pc_ctx *)arg;
    ctx->ok = 1;

    for (u32 i = 0; i < ctx->count; i++) {
        u32 val = i;
        int ret = msg_queue_send(ctx->mq, &val, 500000000ULL);
        if (ret != HFSSS_OK) {
            ctx->ok = 0;
            return NULL;
        }
    }
    return NULL;
}

static void *consumer_fn(void *arg)
{
    struct pc_ctx *ctx = (struct pc_ctx *)arg;
    ctx->ok = 1;

    for (u32 i = 0; i < ctx->count; i++) {
        u32 val = 0;
        int ret = msg_queue_recv(ctx->mq, &val, 500000000ULL);
        if (ret != HFSSS_OK || val != i) {
            ctx->ok = 0;
            return NULL;
        }
    }
    return NULL;
}

static void test_producer_consumer(void)
{
    printf("\n=== msg_queue Producer/Consumer Wakeup ===\n");

    struct msg_queue mq;
    u32 num_msgs = 64;
    int ret = msg_queue_init(&mq, sizeof(u32), 4);
    TEST_ASSERT(ret == HFSSS_OK, "init should succeed");
    if (ret != HFSSS_OK) {
        return;
    }

    struct pc_ctx prod_ctx = {.mq = &mq, .count = num_msgs, .ok = 0};
    struct pc_ctx cons_ctx = {.mq = &mq, .count = num_msgs, .ok = 0};

    pthread_t prod_tid, cons_tid;
    pthread_create(&cons_tid, NULL, consumer_fn, &cons_ctx);
    pthread_create(&prod_tid, NULL, producer_fn, &prod_ctx);

    pthread_join(prod_tid, NULL);
    pthread_join(cons_tid, NULL);

    TEST_ASSERT(prod_ctx.ok, "producer should complete all sends");
    TEST_ASSERT(cons_ctx.ok, "consumer should receive all msgs in order");
    TEST_ASSERT(msg_queue_count(&mq) == 0, "queue should be empty after balanced exchange");

    u64 sc = 0, rc = 0;
    msg_queue_stats(&mq, &sc, &rc);
    TEST_ASSERT(sc == num_msgs, "send_count should match total messages");
    TEST_ASSERT(rc == num_msgs, "recv_count should match total messages");

    msg_queue_cleanup(&mq);
}

int main(void)
{
    printf("========================================\n");
    printf("HFSSS Message Queue Tests\n");
    printf("========================================\n");

    test_invalid_args();
    test_send_busy_zero_timeout();
    test_recv_noent_zero_timeout();
    test_send_timeout_full();
    test_recv_timeout_empty();
    test_ring_wraparound();
    test_stats();
    test_producer_consumer();

    printf("\n========================================\n");
    printf("Test Summary\n");
    printf("========================================\n");
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    printf("========================================\n");

    if (tests_failed == 0) {
        printf("\n  [SUCCESS] All tests passed!\n");
        return 0;
    }

    printf("\n  [FAILURE] Some tests failed!\n");
    return 1;
}
