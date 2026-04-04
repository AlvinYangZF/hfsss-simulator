#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "vhost/nbd_async.h"

static int total_tests = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    total_tests++; \
    if (cond) { printf("  [PASS] %s\n", msg); passed++; } \
    else      { printf("  [FAIL] %s\n", msg); failed++; } \
} while (0)

static void test_pool_init_cleanup(void)
{
    printf("\n=== Inflight Pool Init/Cleanup ===\n");
    struct inflight_pool pool;
    int ret = inflight_pool_init(&pool, 64);
    TEST_ASSERT(ret == 0, "init with 64 slots succeeds");
    TEST_ASSERT(pool.capacity == 64, "capacity is 64");
    inflight_pool_cleanup(&pool);
}

static void test_alloc_free_cycle(void)
{
    printf("\n=== Alloc/Free Cycle ===\n");
    struct inflight_pool pool;
    inflight_pool_init(&pool, 8);

    /* Alloc all 8 slots */
    struct inflight_slot *slots[8];
    for (int i = 0; i < 8; i++) {
        slots[i] = inflight_alloc(&pool);
        TEST_ASSERT(slots[i] != NULL, "alloc slot succeeds");
    }

    /* 9th alloc should fail (pool exhausted) */
    struct inflight_slot *overflow = inflight_alloc(&pool);
    TEST_ASSERT(overflow == NULL, "alloc fails when pool exhausted");

    /* Free one, then alloc should succeed */
    inflight_free(&pool, slots[0]);
    struct inflight_slot *reused = inflight_alloc(&pool);
    TEST_ASSERT(reused != NULL, "alloc succeeds after free");

    /* Cleanup */
    for (int i = 1; i < 8; i++) inflight_free(&pool, slots[i]);
    inflight_free(&pool, reused);
    inflight_pool_cleanup(&pool);
}

static void test_slot_id_lookup(void)
{
    printf("\n=== Slot ID Lookup ===\n");
    struct inflight_pool pool;
    inflight_pool_init(&pool, 16);

    struct inflight_slot *s = inflight_alloc(&pool);
    TEST_ASSERT(s != NULL, "alloc succeeds");

    uint32_t id = s->slot_id;
    struct inflight_slot *found = inflight_get(&pool, id);
    TEST_ASSERT(found == s, "lookup by slot_id returns same pointer");

    inflight_free(&pool, s);
    inflight_pool_cleanup(&pool);
}

int main(void)
{
    printf("========================================\n");
    printf("Inflight Pool Tests\n");
    printf("========================================\n");

    test_pool_init_cleanup();
    test_alloc_free_cycle();
    test_slot_id_lookup();

    printf("\n========================================\n");
    printf("Results: %d/%d passed, %d failed\n", passed, total_tests, failed);
    printf("========================================\n");
    return failed > 0 ? 1 : 0;
}
