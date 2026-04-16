#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <inttypes.h>
#include "ftl/taa.h"

static int total = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    total++; \
    if (cond) { printf("  [PASS] %s\n", msg); passed++; } \
    else      { printf("  [FAIL] %s\n", msg); failed++; } \
} while (0)

static void test_init_cleanup(void)
{
    printf("\n=== TAA Init/Cleanup ===\n");

    struct taa_ctx ctx;
    int ret = taa_init(&ctx, 1024, 2048, 4);
    TEST_ASSERT(ret == HFSSS_OK, "taa_init succeeds");
    TEST_ASSERT(ctx.num_shards == 4, "4 shards");
    TEST_ASSERT(ctx.lbas_per_shard == 256, "256 LBAs per shard");

    taa_cleanup(&ctx);
    TEST_ASSERT(ctx.initialized == false, "cleanup resets initialized");

    /* NULL safety */
    ret = taa_init(NULL, 1024, 2048, 4);
    TEST_ASSERT(ret != HFSSS_OK, "init(NULL) fails");
    taa_cleanup(NULL);
    TEST_ASSERT(1, "cleanup(NULL) does not crash");
}

static void test_lookup_insert_remove(void)
{
    printf("\n=== TAA Lookup/Insert/Remove ===\n");

    struct taa_ctx ctx;
    taa_init(&ctx, 1024, 2048, 4);

    union ppn ppn;
    ppn.raw = 0;
    ppn.bits.channel = 1;
    ppn.bits.block = 10;
    ppn.bits.page = 5;

    /* Insert */
    int ret = taa_insert(&ctx, 42, ppn);
    TEST_ASSERT(ret == HFSSS_OK, "insert LBA 42");

    /* Lookup */
    union ppn out;
    ret = taa_lookup(&ctx, 42, &out);
    TEST_ASSERT(ret == HFSSS_OK, "lookup LBA 42 succeeds");
    TEST_ASSERT(out.raw == ppn.raw, "lookup returns correct PPN");

    /* Lookup unmapped */
    ret = taa_lookup(&ctx, 999, &out);
    TEST_ASSERT(ret == HFSSS_ERR_NOENT, "lookup unmapped returns NOENT");

    /* Remove */
    ret = taa_remove(&ctx, 42);
    TEST_ASSERT(ret == HFSSS_OK, "remove LBA 42");
    ret = taa_lookup(&ctx, 42, &out);
    TEST_ASSERT(ret == HFSSS_ERR_NOENT, "removed LBA returns NOENT");

    /* Valid count */
    TEST_ASSERT(taa_get_valid_count(&ctx) == 0, "valid count is 0");

    taa_cleanup(&ctx);
}

static void test_update(void)
{
    printf("\n=== TAA Update ===\n");

    struct taa_ctx ctx;
    taa_init(&ctx, 1024, 2048, 4);

    union ppn ppn1, ppn2, old;
    ppn1.raw = 0x100;
    ppn2.raw = 0x200;

    taa_insert(&ctx, 50, ppn1);

    int ret = taa_update(&ctx, 50, ppn2, &old);
    TEST_ASSERT(ret == HFSSS_OK, "update succeeds");
    TEST_ASSERT(old.raw == ppn1.raw, "old PPN returned");

    union ppn out;
    taa_lookup(&ctx, 50, &out);
    TEST_ASSERT(out.raw == ppn2.raw, "lookup returns new PPN");

    TEST_ASSERT(taa_get_valid_count(&ctx) == 1, "valid count is 1");

    taa_cleanup(&ctx);
}

static void test_update_if_equal(void)
{
    printf("\n=== TAA update_if_equal (GC CAS) ===\n");

    struct taa_ctx ctx;
    taa_init(&ctx, 1024, 2048, 4);

    union ppn src, dst, other, cur;
    src.raw = 0x100;
    dst.raw = 0x200;
    other.raw = 0x300;

    taa_insert(&ctx, 50, src);

    /* Happy path: L2P still equals src → CAS swaps it to dst. */
    bool updated = false;
    int rc = taa_update_if_equal(&ctx, 50, src, dst, &updated);
    TEST_ASSERT(rc == HFSSS_OK, "CAS happy path rc OK");
    TEST_ASSERT(updated == true, "CAS happy path reports updated");
    taa_lookup(&ctx, 50, &cur);
    TEST_ASSERT(cur.raw == dst.raw, "CAS happy path installs dst_ppn");

    /* Conflict path: simulate a concurrent host write that moved L2P to
     * `other` between the GC read and CAS. The CAS must leave L2P alone. */
    taa_update(&ctx, 50, other, NULL);
    updated = true;
    rc = taa_update_if_equal(&ctx, 50, dst, /* new */ src, &updated);
    TEST_ASSERT(rc == HFSSS_OK, "CAS conflict path rc OK");
    TEST_ASSERT(updated == false, "CAS conflict path reports no swap");
    taa_lookup(&ctx, 50, &cur);
    TEST_ASSERT(cur.raw == other.raw, "CAS conflict path preserves host write");

    /* NULL updated_out must be accepted. */
    rc = taa_update_if_equal(&ctx, 50, other, dst, NULL);
    TEST_ASSERT(rc == HFSSS_OK, "CAS NULL out ptr accepted");
    taa_lookup(&ctx, 50, &cur);
    TEST_ASSERT(cur.raw == dst.raw, "CAS NULL out ptr still swaps");

    /* Empty slot: CAS must not install on an unmapped LBA. */
    updated = true;
    rc = taa_update_if_equal(&ctx, 999, src, dst, &updated);
    TEST_ASSERT(rc == HFSSS_OK, "CAS on empty slot rc OK");
    TEST_ASSERT(updated == false, "CAS on empty slot does not insert");
    rc = taa_lookup(&ctx, 999, &cur);
    TEST_ASSERT(rc != HFSSS_OK, "empty slot remains empty");

    taa_cleanup(&ctx);
}

static void test_cross_shard(void)
{
    printf("\n=== TAA Cross-Shard ===\n");

    struct taa_ctx ctx;
    taa_init(&ctx, 1024, 2048, 4);

    /* Insert into different shards (shard = lba / 256) */
    union ppn ppn;
    ppn.raw = 0;

    ppn.bits.page = 1;
    taa_insert(&ctx, 0, ppn);      /* shard 0 */
    ppn.bits.page = 2;
    taa_insert(&ctx, 256, ppn);    /* shard 1 */
    ppn.bits.page = 3;
    taa_insert(&ctx, 512, ppn);    /* shard 2 */
    ppn.bits.page = 4;
    taa_insert(&ctx, 768, ppn);    /* shard 3 */

    TEST_ASSERT(taa_get_valid_count(&ctx) == 4, "4 entries across 4 shards");

    union ppn out;
    taa_lookup(&ctx, 0, &out);
    TEST_ASSERT(out.bits.page == 1, "shard 0 correct");
    taa_lookup(&ctx, 256, &out);
    TEST_ASSERT(out.bits.page == 2, "shard 1 correct");
    taa_lookup(&ctx, 512, &out);
    TEST_ASSERT(out.bits.page == 3, "shard 2 correct");
    taa_lookup(&ctx, 768, &out);
    TEST_ASSERT(out.bits.page == 4, "shard 3 correct");

    taa_cleanup(&ctx);
}

/* Concurrent access test — 4 threads each own a shard */
struct thread_arg {
    struct taa_ctx *ctx;
    u32 thread_id;
    u32 ops;
    int errors;
};

static void *concurrent_worker(void *arg)
{
    struct thread_arg *ta = (struct thread_arg *)arg;
    struct taa_ctx *ctx = ta->ctx;
    u32 base = ta->thread_id * 256;
    ta->errors = 0;

    for (u32 i = 0; i < ta->ops; i++) {
        u64 lba = base + (i % 256);
        union ppn ppn;
        ppn.raw = 0;
        ppn.bits.channel = ta->thread_id;
        ppn.bits.page = i % 256;

        int ret = taa_insert(ctx, lba, ppn);
        if (ret != HFSSS_OK) { ta->errors++; continue; }

        union ppn out;
        ret = taa_lookup(ctx, lba, &out);
        if (ret != HFSSS_OK || out.raw != ppn.raw) { ta->errors++; }
    }
    return NULL;
}

static void test_concurrent(void)
{
    printf("\n=== TAA Concurrent Access (4 threads) ===\n");

    struct taa_ctx ctx;
    taa_init(&ctx, 1024, 2048, 4);

    pthread_t threads[4];
    struct thread_arg args[4];

    for (int i = 0; i < 4; i++) {
        args[i].ctx = &ctx;
        args[i].thread_id = i;
        args[i].ops = 10000;
        args[i].errors = 0;
        pthread_create(&threads[i], NULL, concurrent_worker, &args[i]);
    }

    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    int total_errors = 0;
    for (int i = 0; i < 4; i++) {
        total_errors += args[i].errors;
    }

    TEST_ASSERT(total_errors == 0, "0 errors across 40K concurrent ops");

    u64 lookups, conflicts;
    taa_get_stats(&ctx, &lookups, &conflicts);
    printf("  TAA stats: %" PRIu64 " lookups, %" PRIu64 " conflicts\n",
           lookups, conflicts);

    taa_cleanup(&ctx);
}

int main(void)
{
    printf("========================================\n");
    printf("TAA (Table Access Accelerator) Tests\n");
    printf("========================================\n");

    test_init_cleanup();
    test_lookup_insert_remove();
    test_update();
    test_update_if_equal();
    test_cross_shard();
    test_concurrent();

    printf("\n========================================\n");
    printf("Results: %d/%d passed, %d failed\n", passed, total, failed);
    printf("========================================\n");
    return failed > 0 ? 1 : 0;
}
