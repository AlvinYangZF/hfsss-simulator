/*
 * test_vhost_proto.c - Unit tests for vhost-user protocol definitions
 *
 * Tests wire-format sizes, layout assumptions, and feature bit values used
 * by the HFSSS vhost-user-blk server.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "vhost_user_proto.h"
#include "vhost/vhost_user_blk.h"

/* -------------------------------------------------------------------------
 * Simple test framework
 * ---------------------------------------------------------------------- */

static int total = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    total++; \
    if (cond) { printf("  [PASS] %s\n", msg); passed++; } \
    else       { printf("  [FAIL] %s\n", msg); failed++; } \
} while (0)

/* -------------------------------------------------------------------------
 * Test 1: 512-byte to 4096-byte sector conversion
 *
 * virtio-blk uses 512-byte sectors; the NVMe device uses 4096-byte LBAs.
 * Conversion: lba = virtio_sector / (4096 / 512) = sector / 8
 * ---------------------------------------------------------------------- */

static void test_sector_conversion(void)
{
    printf("test_sector_conversion:\n");

    /* Helper: virtio sector -> NVMe LBA */
#define SECTOR_TO_LBA(s)  ((s) / (4096 / 512))

    TEST_ASSERT(SECTOR_TO_LBA(0)  == 0, "sector 0 -> lba 0");
    TEST_ASSERT(SECTOR_TO_LBA(8)  == 1, "sector 8 -> lba 1");
    TEST_ASSERT(SECTOR_TO_LBA(16) == 2, "sector 16 -> lba 2");
    TEST_ASSERT(SECTOR_TO_LBA(24) == 3, "sector 24 -> lba 3");
    TEST_ASSERT(SECTOR_TO_LBA(80) == 10, "sector 80 -> lba 10");

#undef SECTOR_TO_LBA
}

/* -------------------------------------------------------------------------
 * Test 2: virtio_blk_config is correctly sized for the wire format
 *
 * Expected layout (bytes):
 *   capacity          8
 *   size_max          4
 *   seg_max           4
 *   cylinders         2
 *   heads             1
 *   sectors           1
 *   blk_size          4
 *   physical_block_exp 1
 *   alignment_offset  1
 *   min_io_size       2
 *   opt_io_size       4
 *   Total            32
 * ---------------------------------------------------------------------- */

static void test_virtio_blk_config_size(void)
{
    printf("test_virtio_blk_config_size:\n");

    TEST_ASSERT(sizeof(struct virtio_blk_config) == 32,
                "sizeof(virtio_blk_config) == 32 bytes");

    TEST_ASSERT(offsetof(struct virtio_blk_config, capacity) == 0,
                "capacity at offset 0");
    TEST_ASSERT(offsetof(struct virtio_blk_config, size_max) == 8,
                "size_max at offset 8");
    TEST_ASSERT(offsetof(struct virtio_blk_config, seg_max) == 12,
                "seg_max at offset 12");
    TEST_ASSERT(offsetof(struct virtio_blk_config, blk_size) == 20,
                "blk_size at offset 20");
}

/* -------------------------------------------------------------------------
 * Test 3: vhost_user_msg header is 12 bytes (request + flags + size)
 * ---------------------------------------------------------------------- */

static void test_vhost_msg_size(void)
{
    printf("test_vhost_msg_size:\n");

    TEST_ASSERT(VHOST_USER_HDR_SIZE == 12,
                "VHOST_USER_HDR_SIZE == 12");

    TEST_ASSERT(offsetof(struct vhost_user_msg, request) == 0,
                "request field at offset 0");
    TEST_ASSERT(offsetof(struct vhost_user_msg, flags) == 4,
                "flags field at offset 4");
    TEST_ASSERT(offsetof(struct vhost_user_msg, size) == 8,
                "size field at offset 8");
    TEST_ASSERT(offsetof(struct vhost_user_msg, payload) == 12,
                "payload at offset 12 (== HDR_SIZE)");
}

/* -------------------------------------------------------------------------
 * Test 4: virtio_blk_outhdr layout matches wire format
 *
 * type   at offset 0  (uint32_t)
 * ioprio at offset 4  (uint32_t)
 * sector at offset 8  (uint64_t)
 * ---------------------------------------------------------------------- */

static void test_virtio_blk_outhdr_layout(void)
{
    printf("test_virtio_blk_outhdr_layout:\n");

    TEST_ASSERT(offsetof(struct virtio_blk_outhdr, type)   == 0,
                "type at offset 0");
    TEST_ASSERT(offsetof(struct virtio_blk_outhdr, ioprio) == 4,
                "ioprio at offset 4");
    TEST_ASSERT(offsetof(struct virtio_blk_outhdr, sector) == 8,
                "sector at offset 8");
    TEST_ASSERT(sizeof(struct virtio_blk_outhdr) == 16,
                "sizeof(virtio_blk_outhdr) == 16");
}

/* -------------------------------------------------------------------------
 * Test 5: Feature bit values are correct powers of 2
 * ---------------------------------------------------------------------- */

static void test_feature_bits(void)
{
    printf("test_feature_bits:\n");

    /* Virtio-blk feature bits */
    TEST_ASSERT(VIRTIO_BLK_F_SIZE_MAX == 1,   "VIRTIO_BLK_F_SIZE_MAX == 1");
    TEST_ASSERT(VIRTIO_BLK_F_SEG_MAX  == 2,   "VIRTIO_BLK_F_SEG_MAX == 2");
    TEST_ASSERT(VIRTIO_BLK_F_GEOMETRY == 4,   "VIRTIO_BLK_F_GEOMETRY == 4");
    TEST_ASSERT(VIRTIO_BLK_F_BLK_SIZE == 6,   "VIRTIO_BLK_F_BLK_SIZE == 6");
    TEST_ASSERT(VIRTIO_BLK_F_FLUSH    == 9,   "VIRTIO_BLK_F_FLUSH == 9");
    TEST_ASSERT(VIRTIO_F_VERSION_1    == 32,  "VIRTIO_F_VERSION_1 == 32");

    /* vhost-user protocol feature bits */
    TEST_ASSERT(VHOST_USER_F_PROTOCOL_FEATURES == 30,
                "VHOST_USER_F_PROTOCOL_FEATURES == 30");
    TEST_ASSERT(VHOST_USER_PROTOCOL_F_MQ     == 0,
                "VHOST_USER_PROTOCOL_F_MQ == 0");
    TEST_ASSERT(VHOST_USER_PROTOCOL_F_CONFIG == 9,
                "VHOST_USER_PROTOCOL_F_CONFIG == 9");
}

/* -------------------------------------------------------------------------
 * Test 6: GPA -> HVA translation concept
 *
 * gpa_to_hva is a static helper inside vhost_user_blk.c.  We verify the
 * equivalent arithmetic here with a mock memory region.
 * ---------------------------------------------------------------------- */

static void test_gpa_translation(void)
{
    printf("test_gpa_translation:\n");

    /* Mock a single memory region: GPA [0x1000, 0x3000) -> host buffer */
    static uint8_t host_buf[0x2000];

    struct vhost_mem_region reg = {
        .guest_phys  = 0x1000,
        .size        = 0x2000,
        .mmap_addr   = host_buf,
        .mmap_offset = 0,
    };

    /* Inline the translation logic from gpa_to_hva */
    uint64_t gpa  = 0x1500;
    void    *hva  = NULL;

    if (gpa >= reg.guest_phys && gpa < reg.guest_phys + reg.size) {
        hva = (uint8_t *)reg.mmap_addr + (gpa - reg.guest_phys) - reg.mmap_offset;
    }

    TEST_ASSERT(hva != NULL,
                "GPA 0x1500 inside region resolves to non-NULL HVA");
    TEST_ASSERT(hva == (uint8_t *)host_buf + 0x500,
                "GPA 0x1500 -> host_buf + 0x500");

    /* GPA outside region should not resolve */
    gpa = 0x5000;
    hva = NULL;
    if (gpa >= reg.guest_phys && gpa < reg.guest_phys + reg.size) {
        hva = (uint8_t *)reg.mmap_addr + (gpa - reg.guest_phys) - reg.mmap_offset;
    }
    TEST_ASSERT(hva == NULL, "GPA 0x5000 outside region -> NULL");
}

/* -------------------------------------------------------------------------
 * Test 7: virtio-blk request type constants
 * ---------------------------------------------------------------------- */

static void test_request_type_mapping(void)
{
    printf("test_request_type_mapping:\n");

    TEST_ASSERT(VIRTIO_BLK_T_IN    == 0, "VIRTIO_BLK_T_IN == 0");
    TEST_ASSERT(VIRTIO_BLK_T_OUT   == 1, "VIRTIO_BLK_T_OUT == 1");
    TEST_ASSERT(VIRTIO_BLK_T_FLUSH == 4, "VIRTIO_BLK_T_FLUSH == 4");

    TEST_ASSERT(VIRTIO_BLK_S_OK    == 0, "VIRTIO_BLK_S_OK == 0");
    TEST_ASSERT(VIRTIO_BLK_S_IOERR == 1, "VIRTIO_BLK_S_IOERR == 1");
    TEST_ASSERT(VIRTIO_BLK_S_UNSUPP == 2, "VIRTIO_BLK_S_UNSUPP == 2");
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
    printf("========================================\n");
    printf("HFSSS vhost-user protocol unit tests\n");
    printf("========================================\n\n");

    test_sector_conversion();
    printf("\n");
    test_virtio_blk_config_size();
    printf("\n");
    test_vhost_msg_size();
    printf("\n");
    test_virtio_blk_outhdr_layout();
    printf("\n");
    test_feature_bits();
    printf("\n");
    test_gpa_translation();
    printf("\n");
    test_request_type_mapping();

    printf("\n========================================\n");
    printf("Results: %d/%d passed", passed, total);
    if (failed > 0) {
        printf(", %d FAILED", failed);
    }
    printf("\n========================================\n");

    return (failed == 0) ? 0 : 1;
}
