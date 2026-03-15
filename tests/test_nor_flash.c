#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#ifndef HFSSS_NOR_TEST_MODE
#  define HFSSS_NOR_TEST_MODE
#endif
#include "media/nor_flash.h"
#include "common/common.h"

static int total = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    total++; \
    if (cond) { printf("  [PASS] %s\n", msg); passed++; } \
    else       { printf("  [FAIL] %s\n", msg); failed++; } \
} while (0)

static void separator(void) {
    printf("========================================\n");
}

/* ------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */
static void test_init_cleanup(void) {
    separator();
    printf("Test: nor_dev_init / cleanup (malloc path)\n");
    separator();

    struct nor_dev dev;
    int ret = nor_dev_init(&dev, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "nor_dev_init(NULL path) returns OK");
    TEST_ASSERT(dev.initialized, "dev.initialized set");
    TEST_ASSERT(dev.image != NULL, "image buffer allocated");
    TEST_ASSERT(!dev.use_mmap, "use_mmap is false for malloc path");
    TEST_ASSERT(dev.image_size == NOR_IMAGE_SIZE_IMPL, "image_size matches NOR_IMAGE_SIZE_IMPL");

    /* Check erased state: first and last byte should be 0xFF */
    TEST_ASSERT(dev.image[0] == 0xFF, "first byte initialised to 0xFF");
    TEST_ASSERT(dev.image[dev.image_size - 1] == 0xFF, "last byte initialised to 0xFF");

    nor_dev_cleanup(&dev);
    TEST_ASSERT(!dev.initialized, "initialized cleared after cleanup");
}

static void test_init_null(void) {
    separator();
    printf("Test: nor_dev_init NULL device\n");
    separator();

    int ret = nor_dev_init(NULL, NULL);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "nor_dev_init(NULL dev) returns INVAL");
}

/* ------------------------------------------------------------------
 * Read / Program (AND-semantics)
 * ------------------------------------------------------------------ */
static void test_read_write_basic(void) {
    separator();
    printf("Test: basic read/program round-trip\n");
    separator();

    struct nor_dev dev;
    nor_dev_init(&dev, NULL);

    uint8_t buf[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0x00, 0x11, 0x22, 0x33};
    int ret = nor_program(&dev, 0, buf, sizeof(buf));
    TEST_ASSERT(ret == HFSSS_OK, "nor_program at offset 0 returns OK");

    uint8_t rbuf[8] = {0};
    ret = nor_read(&dev, 0, rbuf, sizeof(rbuf));
    TEST_ASSERT(ret == HFSSS_OK, "nor_read at offset 0 returns OK");

    /* AND-semantics: 0xFF & 0xAA = 0xAA */
    TEST_ASSERT(rbuf[0] == 0xAA, "first byte matches programmed value");
    TEST_ASSERT(rbuf[4] == 0x00, "zero byte stays zero");

    nor_dev_cleanup(&dev);
}

static void test_and_semantics(void) {
    separator();
    printf("Test: program AND-semantics (bits can only be cleared)\n");
    separator();

    struct nor_dev dev;
    nor_dev_init(&dev, NULL);

    /* Write 0xF0 → image = 0xFF & 0xF0 = 0xF0 */
    uint8_t first = 0xF0;
    nor_program(&dev, 0, &first, 1);
    uint8_t val;
    nor_read(&dev, 0, &val, 1);
    TEST_ASSERT(val == 0xF0, "first program: 0xFF & 0xF0 = 0xF0");

    /* Write 0x0F → image = 0xF0 & 0x0F = 0x00 */
    uint8_t second = 0x0F;
    nor_program(&dev, 0, &second, 1);
    nor_read(&dev, 0, &val, 1);
    TEST_ASSERT(val == 0x00, "second program: 0xF0 & 0x0F = 0x00");

    /* Write 0xFF → image stays 0x00 (cannot set bits without erase) */
    uint8_t third = 0xFF;
    nor_program(&dev, 0, &third, 1);
    nor_read(&dev, 0, &val, 1);
    TEST_ASSERT(val == 0x00, "third program: 0x00 & 0xFF = 0x00 (no bit set)");

    nor_dev_cleanup(&dev);
}

/* ------------------------------------------------------------------
 * Sector erase
 * ------------------------------------------------------------------ */
static void test_sector_erase(void) {
    separator();
    printf("Test: sector erase restores 0xFF\n");
    separator();

    struct nor_dev dev;
    nor_dev_init(&dev, NULL);

    /* Program some bytes then erase */
    uint8_t wbuf[32];
    memset(wbuf, 0x00, sizeof(wbuf));
    nor_program(&dev, 0, wbuf, sizeof(wbuf));

    uint8_t rbuf[32];
    nor_read(&dev, 0, rbuf, sizeof(rbuf));
    TEST_ASSERT(rbuf[0] == 0x00, "bytes are 0x00 after program");

    int ret = nor_sector_erase(&dev, 0);
    TEST_ASSERT(ret == HFSSS_OK, "sector_erase returns OK");

    nor_read(&dev, 0, rbuf, sizeof(rbuf));
    TEST_ASSERT(rbuf[0] == 0xFF, "first byte 0xFF after erase");
    TEST_ASSERT(rbuf[31] == 0xFF, "last byte 0xFF after erase");

    uint32_t sec0_erase_count = dev.sectors[0].erase_count;
    TEST_ASSERT(sec0_erase_count == 1, "sector 0 erase_count incremented to 1");

    nor_dev_cleanup(&dev);
}

static void test_sector_erase_alignment(void) {
    separator();
    printf("Test: sector erase aligns to sector boundary\n");
    separator();

    struct nor_dev dev;
    nor_dev_init(&dev, NULL);

    /* Program at mid-sector offset */
    uint8_t wbuf[8];
    memset(wbuf, 0x42, sizeof(wbuf));
    nor_program(&dev, NOR_SECTOR_SIZE / 2, wbuf, sizeof(wbuf));

    /* Erase using mid-sector address; should erase whole sector */
    int ret = nor_sector_erase(&dev, NOR_SECTOR_SIZE / 2);
    TEST_ASSERT(ret == HFSSS_OK, "sector_erase with mid-sector offset returns OK");

    uint8_t rbuf[8];
    nor_read(&dev, NOR_SECTOR_SIZE / 2, rbuf, sizeof(rbuf));
    TEST_ASSERT(rbuf[0] == 0xFF, "byte restored to 0xFF after aligned erase");

    nor_dev_cleanup(&dev);
}

/* ------------------------------------------------------------------
 * Chip erase
 * ------------------------------------------------------------------ */
static void test_chip_erase(void) {
    separator();
    printf("Test: chip erase\n");
    separator();

    struct nor_dev dev;
    nor_dev_init(&dev, NULL);

    /* Program every byte in sector 0 and sector 1 */
    uint8_t wbuf[NOR_SECTOR_SIZE];
    memset(wbuf, 0x00, sizeof(wbuf));
    nor_program(&dev, 0, wbuf, NOR_SECTOR_SIZE);
    nor_program(&dev, NOR_SECTOR_SIZE, wbuf, NOR_SECTOR_SIZE);

    int ret = nor_chip_erase(&dev);
    TEST_ASSERT(ret == HFSSS_OK, "chip_erase returns OK");

    uint8_t val;
    nor_read(&dev, 0, &val, 1);
    TEST_ASSERT(val == 0xFF, "sector 0 byte 0 is 0xFF after chip erase");

    nor_read(&dev, NOR_SECTOR_SIZE, &val, 1);
    TEST_ASSERT(val == 0xFF, "sector 1 byte 0 is 0xFF after chip erase");

    TEST_ASSERT(dev.sectors[0].erase_count == 1, "sector 0 erase_count = 1");
    TEST_ASSERT(dev.sectors[1].erase_count == 1, "sector 1 erase_count = 1");

    nor_dev_cleanup(&dev);
}

/* ------------------------------------------------------------------
 * PE cycle limit
 * ------------------------------------------------------------------ */
static void test_pe_limit_erase(void) {
    separator();
    printf("Test: PE cycle limit on sector erase\n");
    separator();

    struct nor_dev dev;
    nor_dev_init(&dev, NULL);

    /* Force sector 0 to limit */
    dev.sectors[0].erase_count = NOR_PE_CYCLE_LIMIT;

    int ret = nor_sector_erase(&dev, 0);
    TEST_ASSERT(ret == HFSSS_ERR_IO, "sector_erase at PE limit returns ERR_IO");

    nor_dev_cleanup(&dev);
}

static void test_pe_limit_program(void) {
    separator();
    printf("Test: PE cycle limit enforced on program\n");
    separator();

    struct nor_dev dev;
    nor_dev_init(&dev, NULL);

    /* Force sector 0 to limit */
    dev.sectors[0].erase_count = NOR_PE_CYCLE_LIMIT;

    uint8_t buf[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    int ret = nor_program(&dev, 0, buf, sizeof(buf));
    TEST_ASSERT(ret == HFSSS_ERR_IO, "program into PE-limit sector returns ERR_IO");

    nor_dev_cleanup(&dev);
}

/* ------------------------------------------------------------------
 * Out-of-range checks
 * ------------------------------------------------------------------ */
static void test_out_of_range(void) {
    separator();
    printf("Test: out-of-range access returns INVAL\n");
    separator();

    struct nor_dev dev;
    nor_dev_init(&dev, NULL);

    uint8_t buf[4] = {0};
    int ret = nor_read(&dev, dev.image_size - 2, buf, 4);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "read straddling end returns INVAL");

    ret = nor_program(&dev, dev.image_size - 2, buf, 4);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "program straddling end returns INVAL");

    ret = nor_sector_erase(&dev, dev.image_size);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "sector_erase beyond image returns INVAL");

    nor_dev_cleanup(&dev);
}

/* ------------------------------------------------------------------
 * Partition helpers
 * ------------------------------------------------------------------ */
static void test_partition_layout(void) {
    separator();
    printf("Test: partition layout offsets and sizes\n");
    separator();

    uint32_t off, sz;

    nor_get_partition(NOR_PART_BOOTLOADER, &off, &sz);
    TEST_ASSERT(off == 0, "bootloader offset = 0");
    TEST_ASSERT(sz == 4u * 1024u * 1024u, "bootloader size = 4 MB");

    nor_get_partition(NOR_PART_FW_SLOT_A, &off, &sz);
    TEST_ASSERT(off == 4u * 1024u * 1024u, "fw_slot_a offset = 4 MB");
    TEST_ASSERT(sz == 64u * 1024u * 1024u, "fw_slot_a size = 64 MB");

    nor_get_partition(NOR_PART_FW_SLOT_B, &off, &sz);
    TEST_ASSERT(off == 68u * 1024u * 1024u, "fw_slot_b offset = 68 MB");

    nor_get_partition(NOR_PART_CONFIG, &off, &sz);
    TEST_ASSERT(off == 132u * 1024u * 1024u, "config offset = 132 MB");

    nor_get_partition(NOR_PART_SYSINFO, &off, &sz);
    TEST_ASSERT(sz == 4u * 1024u * 1024u, "sysinfo size = 4 MB");

    /* Invalid partition id */
    int ret = nor_get_partition(NOR_PART_COUNT, &off, &sz);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "nor_get_partition with invalid id returns INVAL");
}

static void test_partition_read_only(void) {
    separator();
    printf("Test: bootloader partition is read-only at runtime\n");
    separator();

    struct nor_dev dev;
    nor_dev_init(&dev, NULL);

    uint8_t buf[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    int ret = nor_partition_write(&dev, NOR_PART_BOOTLOADER, 0, buf, sizeof(buf));
    TEST_ASSERT(ret == HFSSS_ERR_NOTSUPP, "write to bootloader returns NOTSUPP");

    nor_dev_cleanup(&dev);
}

static void test_partition_read_write(void) {
    separator();
    printf("Test: partition read/write within config partition\n");
    separator();

    struct nor_dev dev;
    nor_dev_init(&dev, NULL);

    /* Config partition is writable — but only within image_size (test mode = 8MB) */
    /* In test mode (8 MB image), config @ 132 MB is beyond image — skip write test */
    uint32_t config_off, config_sz;
    nor_get_partition(NOR_PART_CONFIG, &config_off, &config_sz);

    if (config_off + 4 <= (uint32_t)dev.image_size) {
        uint8_t wbuf[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        int ret = nor_partition_write(&dev, NOR_PART_CONFIG, 0, wbuf, sizeof(wbuf));
        TEST_ASSERT(ret == HFSSS_OK, "partition_write to config returns OK");

        uint8_t rbuf[4] = {0};
        ret = nor_partition_read(&dev, NOR_PART_CONFIG, 0, rbuf, sizeof(rbuf));
        TEST_ASSERT(ret == HFSSS_OK, "partition_read from config returns OK");
        TEST_ASSERT(rbuf[0] == (0xFF & 0xDE), "config read-back byte 0 correct");
    } else {
        /* In test mode, config partition is out of range — verify it returns INVAL */
        uint8_t wbuf[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        int ret = nor_partition_write(&dev, NOR_PART_CONFIG, 0, wbuf, sizeof(wbuf));
        TEST_ASSERT(ret == HFSSS_ERR_INVAL, "partition_write out of range in test mode returns INVAL");
    }

    nor_dev_cleanup(&dev);
}

/* ------------------------------------------------------------------
 * Status / ID helpers
 * ------------------------------------------------------------------ */
static void test_status_and_id(void) {
    separator();
    printf("Test: status register and vendor/device ID\n");
    separator();

    struct nor_dev dev;
    nor_dev_init(&dev, NULL);

    /* Default: no WEL set, never busy */
    uint8_t status = nor_read_status(&dev);
    TEST_ASSERT(!(status & NOR_STATUS_BUSY), "BUSY bit never set in sim");

    int ret = nor_write_enable(&dev);
    TEST_ASSERT(ret == HFSSS_OK, "nor_write_enable returns OK");
    TEST_ASSERT(dev.write_enabled, "write_enabled flag set");
    status = nor_read_status(&dev);
    TEST_ASSERT(status & NOR_STATUS_WEL, "WEL bit set after write_enable");

    ret = nor_reset(&dev);
    TEST_ASSERT(ret == HFSSS_OK, "nor_reset returns OK");
    TEST_ASSERT(!dev.write_enabled, "write_enabled cleared after reset");
    status = nor_read_status(&dev);
    TEST_ASSERT(!(status & NOR_STATUS_WEL), "WEL bit cleared after reset");

    uint8_t vid = 0;
    uint16_t did = 0;
    nor_read_id(&dev, &vid, &did);
    TEST_ASSERT(vid == NOR_VENDOR_ID, "vendor ID matches NOR_VENDOR_ID");
    TEST_ASSERT(did == NOR_DEVICE_ID, "device ID matches NOR_DEVICE_ID");

    nor_dev_cleanup(&dev);
}

/* ------------------------------------------------------------------
 * Sync (no-op on malloc path)
 * ------------------------------------------------------------------ */
static void test_sync(void) {
    separator();
    printf("Test: nor_sync on malloc device\n");
    separator();

    struct nor_dev dev;
    nor_dev_init(&dev, NULL);

    int ret = nor_sync(&dev);
    TEST_ASSERT(ret == HFSSS_OK, "nor_sync on malloc device returns OK");

    nor_dev_cleanup(&dev);
}

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */
int main(void) {
    separator();
    printf("HFSSS NOR Flash Tests\n");
    separator();

    test_init_cleanup();
    test_init_null();
    test_read_write_basic();
    test_and_semantics();
    test_sector_erase();
    test_sector_erase_alignment();
    test_chip_erase();
    test_pe_limit_erase();
    test_pe_limit_program();
    test_out_of_range();
    test_partition_layout();
    test_partition_read_only();
    test_partition_read_write();
    test_status_and_id();
    test_sync();

    separator();
    printf("Test Summary\n");
    separator();
    printf("  Total:  %d\n", total);
    printf("  Passed: %d\n", passed);
    printf("  Failed: %d\n", failed);
    separator();

    if (failed == 0) {
        printf("\n  [SUCCESS] All tests passed!\n");
        return 0;
    }
    printf("\n  [FAILURE] Some tests failed!\n");
    return 1;
}
