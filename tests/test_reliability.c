/* Tests for the system reliability module (REQ-113, REQ-114, REQ-135, REQ-136, REQ-138). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

#include "reliability.h"
#include "common.h"

/* ------------------------------------------------------------------ */
/* Minimal test framework                                               */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;

#define TEST_ASSERT(cond) \
    do { \
        if (cond) { \
            printf("  PASS: %s\n", #cond); \
            g_pass++; \
        } else { \
            printf("  FAIL: %s  (line %d)\n", #cond, __LINE__); \
            g_fail++; \
        } \
    } while (0)

#define TEST_BEGIN(name) \
    do { printf("\n[TEST] %s\n", name); } while (0)

/* ------------------------------------------------------------------ */
/* Temporary file helpers                                               */
/* ------------------------------------------------------------------ */

#define TMP_PRIMARY "/tmp/hfsss_test_mirror_primary.bin"
#define TMP_BACKUP  "/tmp/hfsss_test_mirror_backup.bin"

static void cleanup_tmp_files(void)
{
    remove(TMP_PRIMARY);
    remove(TMP_BACKUP);
    remove(TMP_PRIMARY ".tmp");
    remove(TMP_BACKUP  ".tmp");
}

/* ------------------------------------------------------------------ */
/* REQ-113: Token bucket tests                                          */
/* ------------------------------------------------------------------ */

static void test_token_bucket_init_cleanup(void)
{
    TEST_BEGIN("token_bucket init/cleanup");
    struct ns_token_bucket tb;
    int rc = ns_token_bucket_init(&tb, 1, 1000, 100);
    TEST_ASSERT(rc == HFSSS_OK);
    TEST_ASSERT(tb.initialized == true);
    TEST_ASSERT(tb.capacity == 1000);
    TEST_ASSERT(tb.refill_rate == 100);
    TEST_ASSERT(tb.nsid == 1);
    TEST_ASSERT(tb.tokens == 1000);

    ns_token_bucket_cleanup(&tb);
    TEST_ASSERT(tb.initialized == false);
}

static void test_token_bucket_consume_succeeds(void)
{
    TEST_BEGIN("token_bucket consume succeeds when tokens available");
    struct ns_token_bucket tb;
    ns_token_bucket_init(&tb, 1, 500, 10);

    bool ok = ns_token_bucket_consume(&tb, 200);
    TEST_ASSERT(ok == true);
    TEST_ASSERT(tb.tokens <= 300); /* may be slightly more due to refill */
}

static void test_token_bucket_consume_fails_empty(void)
{
    TEST_BEGIN("token_bucket consume fails when empty");
    struct ns_token_bucket tb;
    ns_token_bucket_init(&tb, 2, 100, 0); /* zero refill rate */

    /* Drain all tokens */
    bool drained = ns_token_bucket_consume(&tb, 100);
    TEST_ASSERT(drained == true);

    /* Now bucket should be empty */
    bool ok = ns_token_bucket_consume(&tb, 1);
    TEST_ASSERT(ok == false);

    ns_token_bucket_cleanup(&tb);
}

static void test_token_bucket_refill(void)
{
    TEST_BEGIN("token_bucket refill adds tokens up to capacity");
    struct ns_token_bucket tb;
    ns_token_bucket_init(&tb, 3, 1000, 0);

    /* Drain and then manually set last_refill_ns far in the past */
    ns_token_bucket_consume(&tb, 1000);
    TEST_ASSERT(tb.tokens == 0);

    /* Simulate 10 seconds elapsed by moving last_refill_ns back */
    tb.last_refill_ns -= 10ULL * 1000000000ULL;
    tb.refill_rate = 100; /* 100 tokens/sec */

    ns_token_bucket_refill(&tb);
    /* Should have received 1000 tokens (capped at capacity) */
    TEST_ASSERT(tb.tokens == 1000);

    ns_token_bucket_cleanup(&tb);
}

static void test_token_bucket_available(void)
{
    TEST_BEGIN("token_bucket available returns correct count");
    struct ns_token_bucket tb;
    ns_token_bucket_init(&tb, 4, 200, 0);

    uint64_t avail = ns_token_bucket_available(&tb);
    TEST_ASSERT(avail == 200);

    ns_token_bucket_consume(&tb, 50);
    avail = ns_token_bucket_available(&tb);
    TEST_ASSERT(avail == 150);

    ns_token_bucket_cleanup(&tb);
}

/* ------------------------------------------------------------------ */
/* REQ-113: Channel depth limiter tests                                 */
/* ------------------------------------------------------------------ */

static void test_channel_depth_init(void)
{
    TEST_BEGIN("channel_depth init");
    struct channel_depth_limiter lim;
    int rc = channel_depth_limiter_init(&lim, 0, 8);
    TEST_ASSERT(rc == HFSSS_OK);
    TEST_ASSERT(lim.initialized == true);
    TEST_ASSERT(lim.max_depth == 8);
    TEST_ASSERT(lim.current_depth == 0);
    TEST_ASSERT(lim.channel_id == 0);
}

static void test_channel_depth_acquire_increments(void)
{
    TEST_BEGIN("channel_depth_acquire increments depth");
    struct channel_depth_limiter lim;
    channel_depth_limiter_init(&lim, 0, 4);

    bool ok = channel_depth_acquire(&lim);
    TEST_ASSERT(ok == true);
    TEST_ASSERT(lim.current_depth == 1);

    channel_depth_acquire(&lim);
    TEST_ASSERT(lim.current_depth == 2);
}

static void test_channel_depth_acquire_fails_when_full(void)
{
    TEST_BEGIN("channel_depth_acquire fails when full");
    struct channel_depth_limiter lim;
    channel_depth_limiter_init(&lim, 1, 2);

    channel_depth_acquire(&lim);
    channel_depth_acquire(&lim);
    TEST_ASSERT(lim.current_depth == 2);

    bool ok = channel_depth_acquire(&lim);
    TEST_ASSERT(ok == false);
    TEST_ASSERT(lim.current_depth == 2);
}

static void test_channel_depth_release(void)
{
    TEST_BEGIN("channel_depth_release decrements depth");
    struct channel_depth_limiter lim;
    channel_depth_limiter_init(&lim, 2, 4);

    channel_depth_acquire(&lim);
    channel_depth_acquire(&lim);
    TEST_ASSERT(lim.current_depth == 2);

    channel_depth_release(&lim);
    TEST_ASSERT(lim.current_depth == 1);

    channel_depth_release(&lim);
    TEST_ASSERT(lim.current_depth == 0);

    /* Release below zero is safe */
    channel_depth_release(&lim);
    TEST_ASSERT(lim.current_depth == 0);
}

static void test_channel_depth_is_full(void)
{
    TEST_BEGIN("channel_depth_is_full returns correct value");
    struct channel_depth_limiter lim;
    channel_depth_limiter_init(&lim, 3, 1);

    TEST_ASSERT(channel_depth_is_full(&lim) == false);
    channel_depth_acquire(&lim);
    TEST_ASSERT(channel_depth_is_full(&lim) == true);
    channel_depth_release(&lim);
    TEST_ASSERT(channel_depth_is_full(&lim) == false);
}

/* ------------------------------------------------------------------ */
/* REQ-114: CRC32 tests                                                 */
/* ------------------------------------------------------------------ */

static void test_crc32_known_value(void)
{
    TEST_BEGIN("crc32_compute: known input -> known output");
    /* CRC32 of "123456789" must equal 0xCBF43926 */
    const char *input = "123456789";
    uint32_t crc = crc32_compute(input, strlen(input));
    printf("  crc32(\"123456789\") = 0x%08X (expect 0xCBF43926)\n", crc);
    TEST_ASSERT(crc == 0xCBF43926u);
}

/* ------------------------------------------------------------------ */
/* REQ-114: Meta mirror write/read/verify tests                         */
/* ------------------------------------------------------------------ */

static void test_meta_mirror_write_creates_files(void)
{
    TEST_BEGIN("meta_mirror_write creates both files");
    cleanup_tmp_files();

    const char *data = "hello mirror";
    int rc = meta_mirror_write(TMP_PRIMARY, TMP_BACKUP, data, (uint32_t)strlen(data));
    TEST_ASSERT(rc == HFSSS_OK);

    /* Check files exist by opening them */
    FILE *fp = fopen(TMP_PRIMARY, "rb");
    FILE *fb = fopen(TMP_BACKUP,  "rb");
    TEST_ASSERT(fp != NULL);
    TEST_ASSERT(fb != NULL);
    if (fp) fclose(fp);
    if (fb) fclose(fb);

    cleanup_tmp_files();
}

static void test_meta_mirror_read_primary(void)
{
    TEST_BEGIN("meta_mirror_read reads primary correctly");
    cleanup_tmp_files();

    const char *payload = "test_payload_data";
    uint32_t wlen = (uint32_t)strlen(payload);
    meta_mirror_write(TMP_PRIMARY, TMP_BACKUP, payload, wlen);

    char buf[256] = {0};
    uint32_t rlen = 0;
    bool used_backup = true;
    int rc = meta_mirror_read(TMP_PRIMARY, TMP_BACKUP, buf, sizeof(buf),
                               &rlen, &used_backup);
    TEST_ASSERT(rc == HFSSS_OK);
    TEST_ASSERT(used_backup == false);
    TEST_ASSERT(rlen == wlen);
    TEST_ASSERT(memcmp(buf, payload, wlen) == 0);

    cleanup_tmp_files();
}

static void test_meta_mirror_read_fallback_to_backup(void)
{
    TEST_BEGIN("meta_mirror_read falls back to backup when primary corrupted");
    cleanup_tmp_files();

    const char *payload = "important_metadata";
    uint32_t wlen = (uint32_t)strlen(payload);
    meta_mirror_write(TMP_PRIMARY, TMP_BACKUP, payload, wlen);

    /* Corrupt primary by overwriting it with garbage */
    FILE *fp = fopen(TMP_PRIMARY, "wb");
    if (fp) {
        fwrite("CORRUPTED!!!!", 13, 1, fp);
        fclose(fp);
    }

    char buf[256] = {0};
    uint32_t rlen = 0;
    bool used_backup = false;
    int rc = meta_mirror_read(TMP_PRIMARY, TMP_BACKUP, buf, sizeof(buf),
                               &rlen, &used_backup);
    TEST_ASSERT(rc == HFSSS_OK);
    TEST_ASSERT(used_backup == true);
    TEST_ASSERT(rlen == wlen);
    TEST_ASSERT(memcmp(buf, payload, wlen) == 0);

    cleanup_tmp_files();
}

static void test_meta_mirror_verify_passes(void)
{
    TEST_BEGIN("meta_mirror_verify passes when both consistent");
    cleanup_tmp_files();

    const char *payload = "consistent_data";
    meta_mirror_write(TMP_PRIMARY, TMP_BACKUP, payload, (uint32_t)strlen(payload));

    int rc = meta_mirror_verify(TMP_PRIMARY, TMP_BACKUP);
    TEST_ASSERT(rc == HFSSS_OK);

    cleanup_tmp_files();
}

static void test_meta_mirror_verify_fails_when_both_corrupted(void)
{
    TEST_BEGIN("meta_mirror_verify fails when both corrupted");
    cleanup_tmp_files();

    /* Write garbage to both files */
    FILE *fp = fopen(TMP_PRIMARY, "wb");
    if (fp) { fwrite("GARBAGE", 7, 1, fp); fclose(fp); }
    FILE *fb = fopen(TMP_BACKUP, "wb");
    if (fb) { fwrite("GARBAGE", 7, 1, fb); fclose(fb); }

    int rc = meta_mirror_verify(TMP_PRIMARY, TMP_BACKUP);
    TEST_ASSERT(rc != HFSSS_OK);

    cleanup_tmp_files();
}

/* ------------------------------------------------------------------ */
/* REQ-136: buf_checksum tests                                          */
/* ------------------------------------------------------------------ */

static void test_buf_checksum_deterministic(void)
{
    TEST_BEGIN("buf_checksum deterministic for same input");
    const char *data = "deterministic_test_data_block";
    uint64_t c1 = buf_checksum(data, strlen(data));
    uint64_t c2 = buf_checksum(data, strlen(data));
    TEST_ASSERT(c1 == c2);
    TEST_ASSERT(c1 != 0);
}

static void test_buf_checksum_differs_for_different_input(void)
{
    TEST_BEGIN("buf_checksum different for different input");
    const char *a = "AAAAAAAAAAAAAAAA";
    const char *b = "BBBBBBBBBBBBBBBB";
    uint64_t ca = buf_checksum(a, strlen(a));
    uint64_t cb = buf_checksum(b, strlen(b));
    TEST_ASSERT(ca != cb);
}

/* ------------------------------------------------------------------ */
/* REQ-136: integrity_test_run tests                                    */
/* ------------------------------------------------------------------ */

static void test_integrity_100_blocks_3_passes(void)
{
    TEST_BEGIN("integrity_test_run: 100 blocks, 3 passes, all_passed = true");
    struct integrity_test_cfg cfg = {
        .capacity_bytes = 4096ULL * 100,
        .block_size     = 4096,
        .num_blocks     = 100,
        .num_passes     = 3,
        .seed           = 0xDEADBEEFULL,
    };
    struct integrity_test_result res;
    int rc = integrity_test_run(&cfg, &res);
    TEST_ASSERT(rc == HFSSS_OK);
    TEST_ASSERT(res.all_passed == true);
    TEST_ASSERT(res.blocks_failed == 0);
    TEST_ASSERT(res.blocks_written == 300);
    TEST_ASSERT(res.blocks_verified == 300);
    TEST_ASSERT(res.passes_completed == 3);
}

static void test_integrity_zero_blocks(void)
{
    TEST_BEGIN("integrity_test_run with 0 blocks returns OK immediately");
    struct integrity_test_cfg cfg = {
        .capacity_bytes = 0,
        .block_size     = 4096,
        .num_blocks     = 0,
        .num_passes     = 5,
        .seed           = 0,
    };
    struct integrity_test_result res;
    int rc = integrity_test_run(&cfg, &res);
    TEST_ASSERT(rc == HFSSS_OK);
    TEST_ASSERT(res.blocks_written == 0);
    TEST_ASSERT(res.passes_completed == 0);
}

/* ------------------------------------------------------------------ */
/* NULL safety tests                                                    */
/* ------------------------------------------------------------------ */

static void test_null_safety_token_bucket(void)
{
    TEST_BEGIN("NULL safety: token_bucket APIs");
    /* None of these should crash */
    int rc = ns_token_bucket_init(NULL, 0, 100, 10);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL);

    ns_token_bucket_cleanup(NULL);    /* must not crash */
    TEST_ASSERT(true);

    bool ok = ns_token_bucket_consume(NULL, 1);
    TEST_ASSERT(ok == false);

    ns_token_bucket_refill(NULL);     /* must not crash */
    TEST_ASSERT(true);

    uint64_t a = ns_token_bucket_available(NULL);
    TEST_ASSERT(a == 0);
}

static void test_null_safety_channel_depth(void)
{
    TEST_BEGIN("NULL safety: channel_depth APIs");
    int rc = channel_depth_limiter_init(NULL, 0, 4);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL);

    bool ok = channel_depth_acquire(NULL);
    TEST_ASSERT(ok == false);

    channel_depth_release(NULL);  /* must not crash */
    TEST_ASSERT(true);

    bool full = channel_depth_is_full(NULL);
    TEST_ASSERT(full == true); /* NULL treated as full / unavailable */
}

static void test_null_safety_meta_mirror(void)
{
    TEST_BEGIN("NULL safety: meta_mirror APIs");
    int rc;

    rc = meta_mirror_write(NULL, TMP_BACKUP, "x", 1);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL);

    rc = meta_mirror_write(TMP_PRIMARY, NULL, "x", 1);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL);

    rc = meta_mirror_write(TMP_PRIMARY, TMP_BACKUP, NULL, 1);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL);

    char buf[64];
    uint32_t len;
    bool ub;
    rc = meta_mirror_read(NULL, TMP_BACKUP, buf, sizeof(buf), &len, &ub);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL);

    rc = meta_mirror_read(TMP_PRIMARY, NULL, buf, sizeof(buf), &len, &ub);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL);

    rc = meta_mirror_verify(NULL, TMP_BACKUP);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL);

    rc = meta_mirror_verify(TMP_PRIMARY, NULL);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL);

    /* integrity_test_run NULL safety */
    rc = integrity_test_run(NULL, NULL);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("========================================\n");
    printf("test_reliability\n");
    printf("========================================\n");

    /* REQ-113: token bucket */
    test_token_bucket_init_cleanup();
    test_token_bucket_consume_succeeds();
    test_token_bucket_consume_fails_empty();
    test_token_bucket_refill();
    test_token_bucket_available();

    /* REQ-113: channel depth limiter */
    test_channel_depth_init();
    test_channel_depth_acquire_increments();
    test_channel_depth_acquire_fails_when_full();
    test_channel_depth_release();
    test_channel_depth_is_full();

    /* REQ-114: CRC32 */
    test_crc32_known_value();

    /* REQ-114: meta mirror */
    test_meta_mirror_write_creates_files();
    test_meta_mirror_read_primary();
    test_meta_mirror_read_fallback_to_backup();
    test_meta_mirror_verify_passes();
    test_meta_mirror_verify_fails_when_both_corrupted();

    /* REQ-136: buf_checksum */
    test_buf_checksum_deterministic();
    test_buf_checksum_differs_for_different_input();

    /* REQ-136: integrity test */
    test_integrity_100_blocks_3_passes();
    test_integrity_zero_blocks();

    /* NULL safety */
    test_null_safety_token_bucket();
    test_null_safety_channel_depth();
    test_null_safety_meta_mirror();

    printf("\n========================================\n");
    printf("Total: %d  PASS: %d  FAIL: %d\n", g_pass + g_fail, g_pass, g_fail);
    printf("========================================\n");

    return (g_fail == 0) ? 0 : 1;
}
