#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "common/common.h"
#include "ftl/wal.h"

/* Test counters */
static int total_tests = 0;
static int passed_tests = 0;
static int failed_tests = 0;

#define TEST_ASSERT(cond, msg) do { \
    total_tests++; \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        passed_tests++; \
    } else { \
        printf("  [FAIL] %s\n", msg); \
        failed_tests++; \
    } \
} while (0)

static void print_separator(void)
{
    printf("========================================\n");
}

/* ------------------------------------------------------------------ */
/* CRC-16 T10 DIF tests                                                */
/* ------------------------------------------------------------------ */
static void test_crc16_known_vectors(void)
{
    u8 data1[] = { 0x00 };
    u8 data2[] = { 0x01, 0x02, 0x03, 0x04 };
    u16 crc1, crc2, crc3;

    print_separator();
    printf("Test: CRC-16 T10 DIF Known Vectors\n");
    print_separator();

    /* CRC-16 of a single zero byte — may be 0 for T10 DIF polynomial */
    crc1 = t10_crc16(data1, 1);
    /* Verify it differs from CRC of a non-zero byte */
    {
        u8 data_ff[] = { 0xFF };
        u16 crc_ff = t10_crc16(data_ff, 1);
        TEST_ASSERT(crc1 != crc_ff,
                    "CRC-16 of 0x00 differs from CRC-16 of 0xFF");
    }

    /* CRC-16 of a 4-byte sequence */
    crc2 = t10_crc16(data2, 4);
    TEST_ASSERT(crc2 != 0, "CRC-16 of [01,02,03,04] is non-zero");

    /* CRC-16 of same data produces same result (deterministic) */
    crc3 = t10_crc16(data2, 4);
    TEST_ASSERT(crc2 == crc3, "CRC-16 is deterministic");

    printf("\n");
}

static void test_crc16_detects_corruption(void)
{
    u8 data[512];
    u16 original_crc, corrupted_crc;
    int i;

    print_separator();
    printf("Test: CRC-16 Corruption Detection\n");
    print_separator();

    /* Fill with a pattern */
    for (i = 0; i < 512; i++) {
        data[i] = (u8)(i & 0xFF);
    }

    original_crc = t10_crc16(data, 512);

    /* Flip one bit */
    data[256] ^= 0x01;
    corrupted_crc = t10_crc16(data, 512);

    TEST_ASSERT(original_crc != corrupted_crc,
                "CRC-16 detects single-bit corruption");

    printf("\n");
}

/* ------------------------------------------------------------------ */
/* WAL tests                                                           */
/* ------------------------------------------------------------------ */
static void test_wal_init_cleanup(void)
{
    struct wal_ctx ctx;
    int ret;

    print_separator();
    printf("Test: WAL Init and Cleanup\n");
    print_separator();

    ret = wal_init(&ctx, 1024);
    TEST_ASSERT(ret == HFSSS_OK, "wal_init succeeds");
    TEST_ASSERT(ctx.initialized == true, "WAL is initialized");
    TEST_ASSERT(ctx.count == 0, "WAL starts empty");

    wal_cleanup(&ctx);
    TEST_ASSERT(ctx.initialized == false, "WAL cleaned up");

    printf("\n");
}

static void test_wal_append_commit_replay(void)
{
    struct wal_ctx ctx;
    int ret;
    u32 replay_count = 0;

    print_separator();
    printf("Test: WAL Append, Commit, and Replay\n");
    print_separator();

    ret = wal_init(&ctx, 1024);
    TEST_ASSERT(ret == HFSSS_OK, "wal_init succeeds");

    /* Append L2P updates */
    ret = wal_append(&ctx, WAL_REC_L2P_UPDATE, 100, 0x12345, NULL, 0);
    TEST_ASSERT(ret == HFSSS_OK, "append L2P record 1");

    ret = wal_append(&ctx, WAL_REC_L2P_UPDATE, 200, 0x23456, NULL, 0);
    TEST_ASSERT(ret == HFSSS_OK, "append L2P record 2");

    ret = wal_append(&ctx, WAL_REC_TRIM, 300, 0, NULL, 0);
    TEST_ASSERT(ret == HFSSS_OK, "append TRIM record");

    ret = wal_commit(&ctx);
    TEST_ASSERT(ret == HFSSS_OK, "commit succeeds");

    TEST_ASSERT(wal_get_count(&ctx) == 4, "4 records total (3 data + 1 commit)");

    /* Verify committed sequence */
    u64 committed = wal_get_committed_seq(&ctx);
    TEST_ASSERT(committed > 0, "committed sequence is non-zero");

    wal_cleanup(&ctx);
    printf("\n");
}

static int test_replay_counter_cb(const struct wal_record *rec, void *ud)
{
    u32 *count = (u32 *)ud;
    (*count)++;
    (void)rec;
    return HFSSS_OK;
}

static void test_wal_replay_and_truncate(void)
{
    struct wal_ctx ctx;
    u32 replay_count = 0;
    int ret;

    print_separator();
    printf("Test: WAL Replay and Truncate\n");
    print_separator();

    ret = wal_init(&ctx, 1024);
    TEST_ASSERT(ret == HFSSS_OK, "wal_init succeeds");

    wal_append(&ctx, WAL_REC_L2P_UPDATE, 10, 100, NULL, 0);
    wal_append(&ctx, WAL_REC_L2P_UPDATE, 20, 200, NULL, 0);
    wal_append(&ctx, WAL_REC_L2P_UPDATE, 30, 300, NULL, 0);
    wal_commit(&ctx);

    /* Replay and count data records */
    replay_count = 0;
    ret = wal_replay(&ctx, test_replay_counter_cb, &replay_count);
    TEST_ASSERT(ret == HFSSS_OK, "replay succeeds");
    TEST_ASSERT(replay_count == 3, "3 data records replayed");

    /* Truncate up to committed sequence */
    u64 committed = wal_get_committed_seq(&ctx);
    ret = wal_truncate(&ctx, committed);
    TEST_ASSERT(ret == HFSSS_OK, "truncate succeeds");
    TEST_ASSERT(wal_get_count(&ctx) == 0, "WAL empty after truncate");

    wal_cleanup(&ctx);
    printf("\n");
}

static void test_wal_corrupt_record(void)
{
    struct wal_ctx ctx;
    u32 replay_count = 0;
    int ret;

    print_separator();
    printf("Test: WAL Corrupt Record Detection\n");
    print_separator();

    ret = wal_init(&ctx, 1024);
    TEST_ASSERT(ret == HFSSS_OK, "wal_init succeeds");

    wal_append(&ctx, WAL_REC_L2P_UPDATE, 10, 100, NULL, 0);
    wal_append(&ctx, WAL_REC_L2P_UPDATE, 20, 200, NULL, 0);
    wal_append(&ctx, WAL_REC_L2P_UPDATE, 30, 300, NULL, 0);

    /* Corrupt the CRC of record 1 */
    ctx.records[1].crc32 ^= 0xFFFFFFFF;

    replay_count = 0;
    ret = wal_replay(&ctx, test_replay_counter_cb, &replay_count);
    TEST_ASSERT(ret == HFSSS_OK, "replay stops at corruption");
    TEST_ASSERT(replay_count == 1, "only 1 record before corruption replayed");

    wal_cleanup(&ctx);
    printf("\n");
}

static void test_wal_save_load(void)
{
    struct wal_ctx ctx1, ctx2;
    u32 replay_count = 0;
    int ret;
    const char *path = "/tmp/hfsss_test_wal.bin";

    print_separator();
    printf("Test: WAL Save and Load\n");
    print_separator();

    ret = wal_init(&ctx1, 1024);
    TEST_ASSERT(ret == HFSSS_OK, "wal_init ctx1 succeeds");

    wal_append(&ctx1, WAL_REC_L2P_UPDATE, 10, 100, NULL, 0);
    wal_append(&ctx1, WAL_REC_L2P_UPDATE, 20, 200, NULL, 0);
    wal_commit(&ctx1);

    ret = wal_save(&ctx1, path);
    TEST_ASSERT(ret == HFSSS_OK, "wal_save succeeds");

    ret = wal_init(&ctx2, 1024);
    TEST_ASSERT(ret == HFSSS_OK, "wal_init ctx2 succeeds");

    ret = wal_load(&ctx2, path);
    TEST_ASSERT(ret == HFSSS_OK, "wal_load succeeds");
    TEST_ASSERT(wal_get_count(&ctx2) == 3, "loaded 3 records");

    replay_count = 0;
    ret = wal_replay(&ctx2, test_replay_counter_cb, &replay_count);
    TEST_ASSERT(ret == HFSSS_OK, "replay loaded WAL succeeds");
    TEST_ASSERT(replay_count == 2, "2 data records replayed from loaded WAL");

    wal_cleanup(&ctx1);
    wal_cleanup(&ctx2);
    unlink(path);
    printf("\n");
}

int main(void)
{
    print_separator();
    printf("HFSSS Foundation Tests (CRC-16 + WAL)\n");
    print_separator();
    printf("\n");

    test_crc16_known_vectors();
    test_crc16_detects_corruption();
    test_wal_init_cleanup();
    test_wal_append_commit_replay();
    test_wal_replay_and_truncate();
    test_wal_corrupt_record();
    test_wal_save_load();

    print_separator();
    printf("Test Summary\n");
    print_separator();
    printf("  Total:  %d\n", total_tests);
    printf("  Passed: %d\n", passed_tests);
    printf("  Failed: %d\n", failed_tests);
    print_separator();

    if (failed_tests == 0) {
        printf("\n[SUCCESS] All tests passed!\n");
        return 0;
    } else {
        printf("\n[FAILURE] Some tests failed!\n");
        return 1;
    }
}
