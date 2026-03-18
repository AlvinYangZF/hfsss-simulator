#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sssim.h"
#include "common/boot.h"

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

#define TEST_NAND_PATH "/tmp/hfsss_test_pc_nand.bin"
#define TEST_NOR_PATH  "/tmp/hfsss_test_pc_nor.bin"

static void cleanup_files(void)
{
    unlink(TEST_NAND_PATH);
    unlink(TEST_NOR_PATH);
}

static void make_config(struct sssim_config *config, bool with_persist)
{
    sssim_config_default(config);

    config->page_size = 4096;
    config->spare_size = 64;
    config->channel_count = 2;
    config->chips_per_channel = 2;
    config->dies_per_chip = 1;
    config->planes_per_die = 1;
    config->blocks_per_plane = 64;
    config->pages_per_block = 64;
    config->total_lbas = 1024;

    if (with_persist) {
        strncpy(config->nand_image_path, TEST_NAND_PATH,
                SSSIM_PATH_LEN - 1);
        strncpy(config->nor_image_path, TEST_NOR_PATH,
                SSSIM_PATH_LEN - 1);
    }
}

/* Write a repeating byte pattern to a buffer */
static void fill_pattern(u8 *buf, u32 size, u8 pattern)
{
    memset(buf, pattern, size);
}

/* Generate a unique, deterministic pattern for a given LBA */
static void fill_lba_pattern(u8 *buf, u32 lba_size, u64 lba)
{
    u32 i;
    for (i = 0; i < lba_size; i++) {
        buf[i] = (u8)((lba * 251 + i) & 0xFF);
    }
}

/* ------------------------------------------------------------------
 * Test: basic write -> shutdown -> reload -> read-verify
 * ------------------------------------------------------------------ */
static void test_basic_round_trip(void)
{
    struct sssim_ctx ctx;
    struct sssim_config config;
    u8 *wbuf, *rbuf;
    u32 lba_size;
    int ret;

    print_separator();
    printf("Test: Clean Power Cycle — Basic Round Trip\n");
    print_separator();

    cleanup_files();
    make_config(&config, true);
    lba_size = config.lba_size;
    wbuf = (u8 *)malloc(lba_size * 10);
    rbuf = (u8 *)malloc(lba_size * 10);

    /* --- Write phase --- */
    ret = sssim_init(&ctx, &config);
    TEST_ASSERT(ret == HFSSS_OK, "init succeeds (write phase)");

    fill_pattern(wbuf, lba_size * 5, 0xAA);
    ret = sssim_write(&ctx, 0, 5, wbuf);
    TEST_ASSERT(ret == HFSSS_OK, "write LBAs 0-4 pattern 0xAA");

    fill_pattern(wbuf + lba_size * 5, lba_size * 5, 0xBB);
    ret = sssim_write(&ctx, 50, 5, wbuf + lba_size * 5);
    TEST_ASSERT(ret == HFSSS_OK, "write LBAs 50-54 pattern 0xBB");

    ret = sssim_shutdown(&ctx);
    TEST_ASSERT(ret == HFSSS_OK, "shutdown succeeds");

    sssim_cleanup(&ctx);

    /* --- Reload phase --- */
    ret = sssim_init(&ctx, &config);
    TEST_ASSERT(ret == HFSSS_OK, "init succeeds (reload phase)");

    memset(rbuf, 0, lba_size * 10);
    ret = sssim_read(&ctx, 0, 5, rbuf);
    TEST_ASSERT(ret == HFSSS_OK, "read LBAs 0-4 after reload");
    TEST_ASSERT(memcmp(rbuf, wbuf, lba_size * 5) == 0,
                "LBAs 0-4 data matches 0xAA pattern");

    memset(rbuf, 0, lba_size * 5);
    ret = sssim_read(&ctx, 50, 5, rbuf);
    TEST_ASSERT(ret == HFSSS_OK, "read LBAs 50-54 after reload");
    TEST_ASSERT(memcmp(rbuf, wbuf + lba_size * 5, lba_size * 5) == 0,
                "LBAs 50-54 data matches 0xBB pattern");

    sssim_cleanup(&ctx);
    cleanup_files();
    free(wbuf);
    free(rbuf);
    printf("\n");
}

/* ------------------------------------------------------------------
 * Test: trim persistence across power cycle
 * ------------------------------------------------------------------ */
static void test_trim_persistence(void)
{
    struct sssim_ctx ctx;
    struct sssim_config config;
    u8 *wbuf, *rbuf;
    u32 lba_size;
    int ret;

    print_separator();
    printf("Test: Clean Power Cycle — Trim Persistence\n");
    print_separator();

    cleanup_files();
    make_config(&config, true);
    lba_size = config.lba_size;
    wbuf = (u8 *)malloc(lba_size * 10);
    rbuf = (u8 *)malloc(lba_size * 10);

    /* Write 10 LBAs then trim the middle ones */
    ret = sssim_init(&ctx, &config);
    TEST_ASSERT(ret == HFSSS_OK, "init succeeds");

    fill_pattern(wbuf, lba_size * 10, 0xCC);
    ret = sssim_write(&ctx, 10, 10, wbuf);
    TEST_ASSERT(ret == HFSSS_OK, "write LBAs 10-19");

    ret = sssim_trim(&ctx, 13, 4);
    TEST_ASSERT(ret == HFSSS_OK, "trim LBAs 13-16");

    ret = sssim_shutdown(&ctx);
    TEST_ASSERT(ret == HFSSS_OK, "shutdown succeeds");
    sssim_cleanup(&ctx);

    /* Reload and verify */
    ret = sssim_init(&ctx, &config);
    TEST_ASSERT(ret == HFSSS_OK, "init succeeds (reload)");

    /* Non-trimmed LBAs should be readable */
    memset(rbuf, 0, lba_size * 3);
    ret = sssim_read(&ctx, 10, 3, rbuf);
    TEST_ASSERT(ret == HFSSS_OK, "read non-trimmed LBAs 10-12");
    TEST_ASSERT(memcmp(rbuf, wbuf, lba_size * 3) == 0,
                "LBAs 10-12 data intact after reload");

    memset(rbuf, 0, lba_size * 3);
    ret = sssim_read(&ctx, 17, 3, rbuf);
    TEST_ASSERT(ret == HFSSS_OK, "read non-trimmed LBAs 17-19");
    TEST_ASSERT(memcmp(rbuf, wbuf + lba_size * 7, lba_size * 3) == 0,
                "LBAs 17-19 data intact after reload");

    sssim_cleanup(&ctx);
    cleanup_files();
    free(wbuf);
    free(rbuf);
    printf("\n");
}

/* ------------------------------------------------------------------
 * Test: write after reload persists across second power cycle
 * ------------------------------------------------------------------ */
static void test_write_after_reload(void)
{
    struct sssim_ctx ctx;
    struct sssim_config config;
    u8 *wbuf, *rbuf;
    u32 lba_size;
    int ret;

    print_separator();
    printf("Test: Clean Power Cycle — Write After Reload\n");
    print_separator();

    cleanup_files();
    make_config(&config, true);
    lba_size = config.lba_size;
    wbuf = (u8 *)malloc(lba_size);
    rbuf = (u8 *)malloc(lba_size);

    /* First cycle: write pattern A */
    ret = sssim_init(&ctx, &config);
    TEST_ASSERT(ret == HFSSS_OK, "first init");

    fill_pattern(wbuf, lba_size, 0xDD);
    ret = sssim_write(&ctx, 0, 1, wbuf);
    TEST_ASSERT(ret == HFSSS_OK, "write pattern 0xDD to LBA 0");

    ret = sssim_shutdown(&ctx);
    TEST_ASSERT(ret == HFSSS_OK, "first shutdown");
    sssim_cleanup(&ctx);

    /* Second cycle: reload and overwrite with pattern B */
    ret = sssim_init(&ctx, &config);
    TEST_ASSERT(ret == HFSSS_OK, "second init (reload)");

    fill_pattern(wbuf, lba_size, 0xEE);
    ret = sssim_write(&ctx, 0, 1, wbuf);
    TEST_ASSERT(ret == HFSSS_OK, "overwrite LBA 0 with 0xEE");

    ret = sssim_shutdown(&ctx);
    TEST_ASSERT(ret == HFSSS_OK, "second shutdown");
    sssim_cleanup(&ctx);

    /* Third cycle: verify overwritten data */
    ret = sssim_init(&ctx, &config);
    TEST_ASSERT(ret == HFSSS_OK, "third init (reload)");

    memset(rbuf, 0, lba_size);
    ret = sssim_read(&ctx, 0, 1, rbuf);
    TEST_ASSERT(ret == HFSSS_OK, "read LBA 0 after second reload");
    TEST_ASSERT(memcmp(rbuf, wbuf, lba_size) == 0,
                "LBA 0 has 0xEE pattern from second write");

    sssim_cleanup(&ctx);
    cleanup_files();
    free(wbuf);
    free(rbuf);
    printf("\n");
}

/* ------------------------------------------------------------------
 * Test: init without persistence files works as fresh SSD
 * ------------------------------------------------------------------ */
static void test_no_file_fallback(void)
{
    struct sssim_ctx ctx;
    struct sssim_config config;
    u8 *wbuf, *rbuf;
    u32 lba_size;
    int ret;

    print_separator();
    printf("Test: Clean Power Cycle — No-File Fallback\n");
    print_separator();

    cleanup_files();
    make_config(&config, true);
    lba_size = config.lba_size;
    wbuf = (u8 *)malloc(lba_size);
    rbuf = (u8 *)malloc(lba_size);

    /* Init with persist paths but no files on disk */
    ret = sssim_init(&ctx, &config);
    TEST_ASSERT(ret == HFSSS_OK, "init with no persist files succeeds");

    /* Basic write/read works on fresh SSD */
    fill_pattern(wbuf, lba_size, 0xFF);
    wbuf[0] = 0x42;
    ret = sssim_write(&ctx, 0, 1, wbuf);
    TEST_ASSERT(ret == HFSSS_OK, "write on fresh SSD succeeds");

    memset(rbuf, 0, lba_size);
    ret = sssim_read(&ctx, 0, 1, rbuf);
    TEST_ASSERT(ret == HFSSS_OK, "read on fresh SSD succeeds");
    TEST_ASSERT(memcmp(rbuf, wbuf, lba_size) == 0,
                "data matches on fresh SSD");

    sssim_cleanup(&ctx);
    cleanup_files();
    free(wbuf);
    free(rbuf);
    printf("\n");
}

/* ------------------------------------------------------------------
 * Test: init without persistence paths (empty strings)
 * ------------------------------------------------------------------ */
static void test_no_persist_config(void)
{
    struct sssim_ctx ctx;
    struct sssim_config config;
    int ret;

    print_separator();
    printf("Test: Clean Power Cycle — No Persistence Config\n");
    print_separator();

    make_config(&config, false);

    ret = sssim_init(&ctx, &config);
    TEST_ASSERT(ret == HFSSS_OK, "init without persist paths succeeds");

    /* Shutdown should succeed (no-op for file saves) */
    ret = sssim_shutdown(&ctx);
    TEST_ASSERT(ret == HFSSS_OK, "shutdown without persist paths succeeds");

    sssim_cleanup(&ctx);
    printf("\n");
}

/* ------------------------------------------------------------------
 * Test: write entire drive -> shutdown -> reload -> read-verify all
 * ------------------------------------------------------------------ */
static void test_full_drive_round_trip(void)
{
    struct sssim_ctx ctx;
    struct sssim_config config;
    u8 *ref_buf, *rbuf;
    u32 lba_size;
    u64 total_lbas;
    u64 lba;
    u32 batch = 32;
    u32 miscompares = 0;
    int ret;

    print_separator();
    printf("Test: Clean Power Cycle — Full Drive Round Trip\n");
    print_separator();

    cleanup_files();
    make_config(&config, true);
    lba_size = config.lba_size;
    total_lbas = config.total_lbas;

    /* Allocate reference buffer for entire drive */
    ref_buf = (u8 *)malloc((size_t)total_lbas * lba_size);
    rbuf = (u8 *)malloc((size_t)batch * lba_size);
    TEST_ASSERT(ref_buf != NULL && rbuf != NULL,
                "allocate reference and read buffers");

    /* Fill reference buffer with unique per-LBA patterns */
    for (lba = 0; lba < total_lbas; lba++) {
        fill_lba_pattern(ref_buf + lba * lba_size, lba_size, lba);
    }

    /* --- Write phase: write entire drive in batches --- */
    ret = sssim_init(&ctx, &config);
    TEST_ASSERT(ret == HFSSS_OK, "init succeeds (write phase)");

    for (lba = 0; lba < total_lbas; lba += batch) {
        u32 count = batch;
        if (lba + count > total_lbas) {
            count = (u32)(total_lbas - lba);
        }
        ret = sssim_write(&ctx, lba, count,
                          ref_buf + lba * lba_size);
        if (ret != HFSSS_OK) {
            printf("  [INFO] write failed at LBA %llu, ret=%d\n",
                   (unsigned long long)lba, ret);
            break;
        }
    }
    TEST_ASSERT(ret == HFSSS_OK, "write all LBAs succeeds");

    printf("  [INFO] wrote %llu LBAs (%llu KB)\n",
           (unsigned long long)total_lbas,
           (unsigned long long)(total_lbas * lba_size / 1024));

    ret = sssim_shutdown(&ctx);
    TEST_ASSERT(ret == HFSSS_OK, "shutdown succeeds");
    sssim_cleanup(&ctx);

    /* --- Reload phase --- */
    ret = sssim_init(&ctx, &config);
    TEST_ASSERT(ret == HFSSS_OK, "init succeeds (reload phase)");

    /* --- Verify phase: read entire drive and compare --- */
    for (lba = 0; lba < total_lbas; lba += batch) {
        u32 count = batch;
        u32 i;
        if (lba + count > total_lbas) {
            count = (u32)(total_lbas - lba);
        }

        memset(rbuf, 0, (size_t)count * lba_size);
        ret = sssim_read(&ctx, lba, count, rbuf);
        if (ret != HFSSS_OK) {
            printf("  [INFO] read failed at LBA %llu, ret=%d\n",
                   (unsigned long long)lba, ret);
            miscompares++;
            continue;
        }

        for (i = 0; i < count; i++) {
            u8 *expected = ref_buf + (lba + i) * lba_size;
            u8 *actual = rbuf + i * lba_size;
            if (memcmp(expected, actual, lba_size) != 0) {
                if (miscompares == 0) {
                    /* Print details for first miscompare only */
                    u32 j;
                    for (j = 0; j < lba_size; j++) {
                        if (expected[j] != actual[j]) {
                            printf("  [INFO] first miscompare: "
                                   "LBA=%llu offset=%u "
                                   "expected=0x%02X actual=0x%02X\n",
                                   (unsigned long long)(lba + i), j,
                                   expected[j], actual[j]);
                            break;
                        }
                    }
                }
                miscompares++;
            }
        }
    }

    printf("  [INFO] verified %llu LBAs, miscompares=%u\n",
           (unsigned long long)total_lbas, miscompares);
    TEST_ASSERT(miscompares == 0,
                "zero miscompares across all LBAs");

    sssim_cleanup(&ctx);
    cleanup_files();
    free(ref_buf);
    free(rbuf);
    printf("\n");
}

int main(void)
{
    print_separator();
    printf("HFSSS Clean Power Cycle Tests\n");
    print_separator();
    printf("\n");

    test_basic_round_trip();
    test_trim_persistence();
    test_write_after_reload();
    test_no_file_fallback();
    test_no_persist_config();
    test_full_drive_round_trip();

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
