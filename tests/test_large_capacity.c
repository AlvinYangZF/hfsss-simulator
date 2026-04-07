#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "pcie/nvme_uspace.h"

static int total = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg)                                                                                         \
    do {                                                                                                               \
        total++;                                                                                                       \
        if (cond) {                                                                                                    \
            printf("  [PASS] %s\n", msg);                                                                              \
            passed++;                                                                                                  \
        } else {                                                                                                       \
            printf("  [FAIL] %s\n", msg);                                                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

/*
 * Test: Initialize a 1 GB geometry, verify L2P addressing,
 * write+read boundary LBAs through the full NVMe uspace path.
 *
 * Geometry: 4ch * 2chip * 2die * 2plane * 256blk * 256pg * 4096B
 *         = 4*2*2*2*256*256 = 1,048,576 pages
 *         = 4 GB raw NAND
 *         = ~256K usable LBAs at 4K with OP
 *
 * Kept small enough for CI runners (~1 GB RSS) while still
 * exercising capacity-scaling code paths beyond the default geometry.
 */

static void test_large_geometry_init(void)
{
    printf("\n=== Large Capacity Init (1 GB) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;

    nvme_uspace_config_default(&config);
    config.sssim_cfg.page_size = 4096;
    config.sssim_cfg.spare_size = 64;
    config.sssim_cfg.channel_count = 4;
    config.sssim_cfg.chips_per_channel = 2;
    config.sssim_cfg.dies_per_chip = 2;
    config.sssim_cfg.planes_per_die = 2;
    config.sssim_cfg.blocks_per_plane = 256;
    config.sssim_cfg.pages_per_block = 256;

    /* 1 GB = 262,144 LBAs at 4K */
    uint64_t total_lbas = 1ULL * 1024 * 1024 * 1024 / 4096;
    config.sssim_cfg.total_lbas = total_lbas;
    config.sssim_cfg.lba_size = 4096;

    TEST_ASSERT(total_lbas == 262144, "1 GB = 262,144 LBAs");

    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_uspace_dev_init succeeds for 1 GB");

    ret = nvme_uspace_dev_start(&dev);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_uspace_dev_start succeeds");

    /* Verify namespace size */
    struct nvme_identify_ns id_ns;
    ret = nvme_uspace_identify_ns(&dev, 1, &id_ns);
    TEST_ASSERT(ret == HFSSS_OK, "identify_ns succeeds");
    TEST_ASSERT(id_ns.nsze >= total_lbas, "namespace size >= 1 GB LBAs");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
}

static void test_boundary_io(void)
{
    printf("\n=== Boundary I/O (1 GB) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config config;

    nvme_uspace_config_default(&config);
    config.sssim_cfg.page_size = 4096;
    config.sssim_cfg.spare_size = 64;
    config.sssim_cfg.channel_count = 4;
    config.sssim_cfg.chips_per_channel = 2;
    config.sssim_cfg.dies_per_chip = 2;
    config.sssim_cfg.planes_per_die = 2;
    config.sssim_cfg.blocks_per_plane = 256;
    config.sssim_cfg.pages_per_block = 256;

    uint64_t total_lbas = 262144; /* 1 GB */
    config.sssim_cfg.total_lbas = total_lbas;
    config.sssim_cfg.lba_size = 4096;

    int ret = nvme_uspace_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "init for boundary I/O");
    nvme_uspace_dev_start(&dev);

    uint8_t wbuf[4096], rbuf[4096];
    uint64_t test_lbas[] = {
        0,              /* first LBA */
        total_lbas / 2, /* middle LBA */
        total_lbas - 1, /* last LBA */
        200000,         /* arbitrary high LBA */
    };
    int num_tests = sizeof(test_lbas) / sizeof(test_lbas[0]);

    for (int i = 0; i < num_tests; i++) {
        uint64_t lba = test_lbas[i];

        /* Fill with pattern based on LBA */
        memset(wbuf, (uint8_t)(lba & 0xFF), 4096);
        wbuf[0] = (uint8_t)((lba >> 8) & 0xFF);
        wbuf[1] = (uint8_t)((lba >> 16) & 0xFF);

        ret = nvme_uspace_write(&dev, 1, lba, 1, wbuf);
        char msg[128];
        snprintf(msg, sizeof(msg), "write LBA %" PRIu64 " succeeds", lba);
        TEST_ASSERT(ret == HFSSS_OK, msg);

        memset(rbuf, 0, 4096);
        ret = nvme_uspace_read(&dev, 1, lba, 1, rbuf);
        snprintf(msg, sizeof(msg), "read LBA %" PRIu64 " succeeds", lba);
        TEST_ASSERT(ret == HFSSS_OK, msg);

        snprintf(msg, sizeof(msg), "data verify LBA %" PRIu64, lba);
        TEST_ASSERT(memcmp(wbuf, rbuf, 4096) == 0, msg);
    }

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
}

int main(void)
{
    printf("========================================\n");
    printf("Large Capacity Tests (1 GB)\n");
    printf("========================================\n");

    test_large_geometry_init();
    test_boundary_io();

    printf("\n========================================\n");
    printf("Results: %d/%d passed, %d failed\n", passed, total, failed);
    printf("========================================\n");
    return failed > 0 ? 1 : 0;
}
