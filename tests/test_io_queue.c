#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "ftl/io_queue.h"

static int total = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    total++; \
    if (cond) { printf("  [PASS] %s\n", msg); passed++; } \
    else      { printf("  [FAIL] %s\n", msg); failed++; } \
} while (0)

static void test_basic(void)
{
    printf("\n=== IO Ring Basic ===\n");

    struct io_ring ring;
    int ret = io_ring_init(&ring, sizeof(struct io_request),
                           IO_RING_DEFAULT_CAPACITY);
    TEST_ASSERT(ret == HFSSS_OK, "init succeeds");
    TEST_ASSERT(io_ring_is_empty(&ring), "initially empty");
    TEST_ASSERT(!io_ring_is_full(&ring), "initially not full");

    struct io_request req = { .opcode = IO_OP_WRITE, .lba = 42, .count = 1 };
    bool ok = io_ring_push(&ring, &req);
    TEST_ASSERT(ok, "push succeeds");
    TEST_ASSERT(!io_ring_is_empty(&ring), "not empty after push");
    TEST_ASSERT(io_ring_count(&ring) == 1, "count is 1");

    struct io_request out;
    ok = io_ring_pop(&ring, &out);
    TEST_ASSERT(ok, "pop succeeds");
    TEST_ASSERT(out.lba == 42, "popped LBA correct");
    TEST_ASSERT(out.opcode == IO_OP_WRITE, "popped opcode correct");
    TEST_ASSERT(io_ring_is_empty(&ring), "empty after pop");

    /* Pop from empty */
    ok = io_ring_pop(&ring, &out);
    TEST_ASSERT(!ok, "pop from empty returns false");

    io_ring_cleanup(&ring);
}

static void test_fill_drain(void)
{
    printf("\n=== IO Ring Fill/Drain ===\n");

    struct io_ring ring;
    io_ring_init(&ring, sizeof(u64), 16);

    /* Fill to capacity - 1 (one slot always empty in SPSC) */
    u32 pushed = 0;
    for (u32 i = 0; i < 100; i++) {
        u64 val = i;
        if (io_ring_push(&ring, &val)) pushed++;
    }
    TEST_ASSERT(pushed == 15, "pushed 15 items into 16-slot ring");
    TEST_ASSERT(io_ring_is_full(&ring), "ring is full");

    /* Drain all */
    u32 popped = 0;
    u64 val;
    while (io_ring_pop(&ring, &val)) popped++;
    TEST_ASSERT(popped == 15, "popped 15 items");
    TEST_ASSERT(io_ring_is_empty(&ring), "ring is empty");

    io_ring_cleanup(&ring);
}

/* SPSC concurrent: 1 producer thread, 1 consumer thread */
struct spsc_arg {
    struct io_ring *ring;
    u32 count;
    u64 checksum;
};

static void *producer_thread(void *arg)
{
    struct spsc_arg *a = (struct spsc_arg *)arg;
    for (u32 i = 0; i < a->count; i++) {
        u64 val = i;
        while (!io_ring_push(a->ring, &val)) {
            /* Spin until space available */
        }
        a->checksum += val;
    }
    return NULL;
}

static void *consumer_thread(void *arg)
{
    struct spsc_arg *a = (struct spsc_arg *)arg;
    for (u32 i = 0; i < a->count; i++) {
        u64 val;
        while (!io_ring_pop(a->ring, &val)) {
            /* Spin until data available */
        }
        a->checksum += val;
    }
    return NULL;
}

static void test_spsc_concurrent(void)
{
    printf("\n=== IO Ring SPSC Concurrent (100K ops) ===\n");

    struct io_ring ring;
    io_ring_init(&ring, sizeof(u64), 1024);

    struct spsc_arg prod = { .ring = &ring, .count = 100000, .checksum = 0 };
    struct spsc_arg cons = { .ring = &ring, .count = 100000, .checksum = 0 };

    pthread_t pt, ct;
    pthread_create(&pt, NULL, producer_thread, &prod);
    pthread_create(&ct, NULL, consumer_thread, &cons);
    pthread_join(pt, NULL);
    pthread_join(ct, NULL);

    TEST_ASSERT(prod.checksum == cons.checksum,
                "producer and consumer checksums match");
    TEST_ASSERT(io_ring_is_empty(&ring), "ring empty after test");

    printf("  Checksum: %llu\n", (unsigned long long)prod.checksum);

    io_ring_cleanup(&ring);
}

int main(void)
{
    printf("========================================\n");
    printf("IO Queue (Lock-Free Ring) Tests\n");
    printf("========================================\n");

    test_basic();
    test_fill_drain();
    test_spsc_concurrent();

    printf("\n========================================\n");
    printf("Results: %d/%d passed, %d failed\n", passed, total, failed);
    printf("========================================\n");
    return failed > 0 ? 1 : 0;
}
