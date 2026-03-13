#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sssim.h"
#include "common/log.h"

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

static void test_sssim_init_cleanup(void)
{
    struct sssim_ctx ctx;
    struct sssim_config config;
    int ret;

    print_separator();
    printf("Test: SSSIM Init & Cleanup\n");
    print_separator();

    sssim_config_default(&config);

    /* Use smaller configuration for faster testing */
    config.page_size = 4096;    /* Match LBA size */
    config.spare_size = 64;
    config.channel_count = 2;
    config.chips_per_channel = 2;
    config.dies_per_chip = 1;
    config.planes_per_die = 1;
    config.blocks_per_plane = 64;
    config.pages_per_block = 64;
    config.total_lbas = 1024;  /* 4MB */

    ret = sssim_init(&ctx, &config);
    TEST_ASSERT(ret == HFSSS_OK, "sssim_init succeeds");

    sssim_cleanup(&ctx);
    TEST_ASSERT(!ctx.initialized, "ctx.initialized is false after cleanup");

    printf("\n");
}

static void test_sssim_read_write(void)
{
    struct sssim_ctx ctx;
    struct sssim_config config;
    u8 *write_buf, *read_buf;
    u32 lba_size;
    int ret;
    int i;

    print_separator();
    printf("Test: SSSIM Read & Write\n");
    print_separator();

    sssim_config_default(&config);

    /* Small configuration */
    config.page_size = 4096;    /* Match LBA size */
    config.spare_size = 64;
    config.channel_count = 2;
    config.chips_per_channel = 2;
    config.dies_per_chip = 1;
    config.planes_per_die = 1;
    config.blocks_per_plane = 64;
    config.pages_per_block = 64;
    config.total_lbas = 1024;

    lba_size = config.lba_size;
    write_buf = (u8 *)malloc(lba_size * 4);
    read_buf = (u8 *)malloc(lba_size * 4);

    /* Fill write buffer with pattern */
    for (i = 0; i < (int)(lba_size * 4); i++) {
        write_buf[i] = (u8)(i & 0xFF);
    }

    ret = sssim_init(&ctx, &config);
    TEST_ASSERT(ret == HFSSS_OK, "sssim_init succeeds");

    /* Write 4 LBAs */
    memset(read_buf, 0, lba_size * 4);
    ret = sssim_write(&ctx, 0, 4, write_buf);
    TEST_ASSERT(ret == HFSSS_OK, "Write 4 LBAs succeeds");

    /* Read back and verify */
    ret = sssim_read(&ctx, 0, 4, read_buf);
    TEST_ASSERT(ret == HFSSS_OK, "Read 4 LBAs succeeds");
    TEST_ASSERT(memcmp(write_buf, read_buf, lba_size * 4) == 0,
                "Read data matches written data");

    /* Test non-zero LBA */
    for (i = 0; i < (int)lba_size; i++) {
        write_buf[i] = (u8)(0xAA);
    }
    ret = sssim_write(&ctx, 100, 1, write_buf);
    TEST_ASSERT(ret == HFSSS_OK, "Write at LBA 100 succeeds");

    memset(read_buf, 0, lba_size);
    ret = sssim_read(&ctx, 100, 1, read_buf);
    TEST_ASSERT(ret == HFSSS_OK, "Read from LBA 100 succeeds");
    TEST_ASSERT(memcmp(write_buf, read_buf, lba_size) == 0,
                "Data at LBA 100 matches");

    /* Flush */
    ret = sssim_flush(&ctx);
    TEST_ASSERT(ret == HFSSS_OK, "Flush succeeds");

    sssim_cleanup(&ctx);

    free(write_buf);
    free(read_buf);
    printf("\n");
}

static void test_sssim_trim(void)
{
    struct sssim_ctx ctx;
    struct sssim_config config;
    u8 *write_buf, *read_buf;
    u32 lba_size;
    int ret;
    int i;

    print_separator();
    printf("Test: SSSIM Trim\n");
    print_separator();

    sssim_config_default(&config);

    /* Small configuration */
    config.page_size = 4096;    /* Match LBA size */
    config.spare_size = 64;
    config.channel_count = 2;
    config.chips_per_channel = 2;
    config.dies_per_chip = 1;
    config.planes_per_die = 1;
    config.blocks_per_plane = 64;
    config.pages_per_block = 64;
    config.total_lbas = 1024;

    lba_size = config.lba_size;
    write_buf = (u8 *)malloc(lba_size * 10);
    read_buf = (u8 *)malloc(lba_size * 10);

    for (i = 0; i < (int)(lba_size * 10); i++) {
        write_buf[i] = (u8)(i & 0xFF);
    }

    ret = sssim_init(&ctx, &config);
    TEST_ASSERT(ret == HFSSS_OK, "sssim_init succeeds");

    /* Write some data */
    ret = sssim_write(&ctx, 10, 10, write_buf);
    TEST_ASSERT(ret == HFSSS_OK, "Write 10 LBAs succeeds");

    /* Trim LBAs 12-15 */
    ret = sssim_trim(&ctx, 12, 4);
    TEST_ASSERT(ret == HFSSS_OK, "Trim 4 LBAs succeeds");

    sssim_cleanup(&ctx);

    free(write_buf);
    free(read_buf);
    printf("\n");
}

static void test_sssim_stats(void)
{
    struct sssim_ctx ctx;
    struct sssim_config config;
    struct ftl_stats stats;
    u8 *buf;
    u32 lba_size;
    int ret;

    print_separator();
    printf("Test: SSSIM Statistics\n");
    print_separator();

    sssim_config_default(&config);

    /* Small configuration */
    config.page_size = 4096;    /* Match LBA size */
    config.spare_size = 64;
    config.channel_count = 2;
    config.chips_per_channel = 2;
    config.dies_per_chip = 1;
    config.planes_per_die = 1;
    config.blocks_per_plane = 64;
    config.pages_per_block = 64;
    config.total_lbas = 1024;

    lba_size = config.lba_size;
    buf = (u8 *)malloc(lba_size);
    memset(buf, 0x55, lba_size);

    ret = sssim_init(&ctx, &config);
    TEST_ASSERT(ret == HFSSS_OK, "sssim_init succeeds");

    /* Reset stats */
    sssim_reset_stats(&ctx);

    /* Do some operations */
    ret = sssim_write(&ctx, 0, 1, buf);
    TEST_ASSERT(ret == HFSSS_OK, "Write 1 LBA succeeds");

    ret = sssim_read(&ctx, 0, 1, buf);
    TEST_ASSERT(ret == HFSSS_OK, "Read 1 LBA succeeds");

    /* Get stats */
    sssim_get_stats(&ctx, &stats);
    TEST_ASSERT(stats.write_count >= 1, "Write count >= 1");
    TEST_ASSERT(stats.read_count >= 1, "Read count >= 1");
    TEST_ASSERT(stats.write_bytes >= lba_size, "Write bytes >= LBA size");
    TEST_ASSERT(stats.read_bytes >= lba_size, "Read bytes >= LBA size");

    sssim_cleanup(&ctx);

    free(buf);
    printf("\n");
}

int main(void)
{
    /* No log context used in this simple test */

    print_separator();
    printf("HFSSS Top-Level Simulator Tests\n");
    print_separator();
    printf("\n");

    test_sssim_init_cleanup();
    test_sssim_read_write();
    test_sssim_trim();
    test_sssim_stats();

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
