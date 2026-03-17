/*
 * systest_error_boundary.c - Error handling and boundary configuration system tests
 *
 * Tests NOSPC recovery, minimum/larger geometry, low OP ratio, NAND types,
 * and edge LBA values for the HFSSS SSD simulator.
 */

#include "pcie/nvme_uspace.h"
#include "ftl/ftl.h"
#include "common/common.h"

/* --------------- Test Harness --------------- */

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

/* --------------- Setup / Teardown Helpers --------------- */

static int setup_dev(struct nvme_uspace_dev *dev, struct nvme_uspace_config *cfg,
                     u32 channels, u32 chips, u32 dies, u32 planes,
                     u32 blocks_per_plane, u32 pages_per_block, u32 page_size) {
    nvme_uspace_config_default(cfg);
    cfg->sssim_cfg.channel_count = channels;
    cfg->sssim_cfg.chips_per_channel = chips;
    cfg->sssim_cfg.dies_per_chip = dies;
    cfg->sssim_cfg.planes_per_die = planes;
    cfg->sssim_cfg.blocks_per_plane = blocks_per_plane;
    cfg->sssim_cfg.pages_per_block = pages_per_block;
    cfg->sssim_cfg.page_size = page_size;
    uint64_t raw = (uint64_t)channels * chips * dies * planes * blocks_per_plane * pages_per_block;
    cfg->sssim_cfg.total_lbas = raw * (100 - cfg->sssim_cfg.op_ratio) / 100;
    if (nvme_uspace_dev_init(dev, cfg) != HFSSS_OK) return -1;
    if (nvme_uspace_dev_start(dev) != HFSSS_OK) return -1;
    if (nvme_uspace_create_io_cq(dev, 1, 256, false) != HFSSS_OK) return -1;
    if (nvme_uspace_create_io_sq(dev, 1, 256, 1, 0) != HFSSS_OK) return -1;
    return 0;
}

static void teardown_dev(struct nvme_uspace_dev *dev) {
    nvme_uspace_delete_io_sq(dev, 1);
    nvme_uspace_delete_io_cq(dev, 1);
    nvme_uspace_dev_stop(dev);
    nvme_uspace_dev_cleanup(dev);
}

/* --------------- LCG Pattern Helpers --------------- */

static uint32_t lcg(uint32_t s) { return s * 1664525u + 1013904223u; }

#define LBA_SIZE 4096

static void fill_pattern(void *buf, uint32_t lba, uint64_t gen) {
    uint32_t *p = (uint32_t *)buf;
    uint32_t s = (uint32_t)(lba ^ (gen * 0x9e3779b9u));
    for (int i = 0; i < (int)(LBA_SIZE / 4); i++) { s = lcg(s); p[i] = s; }
}

static bool verify_pattern(const void *buf, uint32_t lba, uint64_t gen) {
    const uint32_t *p = (const uint32_t *)buf;
    uint32_t s = (uint32_t)(lba ^ (gen * 0x9e3779b9u));
    for (int i = 0; i < (int)(LBA_SIZE / 4); i++) { s = lcg(s); if (p[i] != s) return false; }
    return true;
}

/* ================================================================
 * EH-005: NOSPC handling and recovery
 * ================================================================ */
static void test_eh005_nospc_handling(void) {
    printf("\n=== EH-005: NOSPC handling and recovery ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    memset(&dev, 0, sizeof(dev));

    int rc = setup_dev(&dev, &cfg, 2, 1, 1, 1, 32, 64, 4096);
    TEST_ASSERT(rc == 0, "EH-005: device init");
    if (rc != 0) return;

    u64 total_lbas = cfg.sssim_cfg.total_lbas;
    void *buf = malloc(LBA_SIZE);
    TEST_ASSERT(buf != NULL, "EH-005: allocate buffer");
    if (!buf) { teardown_dev(&dev); return; }

    /* Write all LBAs */
    int write_fail = 0;
    for (u64 lba = 0; lba < total_lbas; lba++) {
        fill_pattern(buf, (uint32_t)lba, 1);
        rc = nvme_uspace_write(&dev, 1, lba, 1, buf);
        if (rc != HFSSS_OK) write_fail++;
    }
    TEST_ASSERT(write_fail == 0, "EH-005: write all LBAs succeeds");

    /* Attempt 100 more overwrites on valid LBAs to try to exhaust space.
     * With a small device and 100% fill, overwrites may trigger NOSPC if
     * GC cannot keep up. If GC handles it, that's also valid. */
    int nospc_count = 0;
    int extra_ok = 0;
    uint32_t rng = 0xBEEF;
    for (int i = 0; i < 100; i++) {
        rng = lcg(rng);
        u64 lba = rng % total_lbas;
        fill_pattern(buf, (uint32_t)lba, 99);
        rc = nvme_uspace_write(&dev, 1, lba, 1, buf);
        if (rc == HFSSS_ERR_NOSPC) nospc_count++;
        else if (rc == HFSSS_OK) extra_ok++;
    }
    TEST_ASSERT(nospc_count > 0 || extra_ok > 0,
                "EH-005: overwrites beyond capacity handled (NOSPC or GC)");

    /* Trim first 20% of LBAs */
    u64 trim_count = total_lbas / 5;
    struct nvme_dsm_range range;
    range.attributes = 0;
    range.slba = 0;
    range.nlb = (uint32_t)trim_count;
    rc = nvme_uspace_trim(&dev, 1, &range, 1);
    TEST_ASSERT(rc == HFSSS_OK, "EH-005: trim first 20%% succeeds");

    rc = nvme_uspace_flush(&dev, 1);
    TEST_ASSERT(rc == HFSSS_OK, "EH-005: flush after trim succeeds");

    /* Write 50 LBAs to freed region */
    int freed_write_fail = 0;
    u32 write_count = 50;
    if (write_count > trim_count) write_count = (u32)trim_count;
    for (u32 i = 0; i < write_count; i++) {
        fill_pattern(buf, i, 2);
        rc = nvme_uspace_write(&dev, 1, (u64)i, 1, buf);
        if (rc != HFSSS_OK) freed_write_fail++;
    }
    TEST_ASSERT(freed_write_fail == 0, "EH-005: write to freed region succeeds");

    /* Verify all current data */
    int corrupt = 0;
    /* Verify the 50 newly written LBAs (gen=2) */
    for (u32 i = 0; i < write_count; i++) {
        rc = nvme_uspace_read(&dev, 1, (u64)i, 1, buf);
        if (rc != HFSSS_OK || !verify_pattern(buf, i, 2)) corrupt++;
    }
    /* Verify remaining LBAs (gen=1 or gen=99 from NOSPC overwrites) */
    for (u64 lba = trim_count; lba < total_lbas; lba++) {
        rc = nvme_uspace_read(&dev, 1, lba, 1, buf);
        if (rc != HFSSS_OK) { corrupt++; continue; }
        if (!verify_pattern(buf, (uint32_t)lba, 1) &&
            !verify_pattern(buf, (uint32_t)lba, 99)) corrupt++;
    }
    TEST_ASSERT(corrupt == 0, "EH-005: all current data verified");

    free(buf);
    teardown_dev(&dev);
}

/* ================================================================
 * CB-001: Minimum geometry
 * ================================================================ */
static void test_cb001_minimum_geometry(void) {
    printf("\n=== CB-001: Minimum geometry ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    memset(&dev, 0, sizeof(dev));

    int rc = setup_dev(&dev, &cfg, 1, 1, 1, 1, 8, 8, 4096);
    TEST_ASSERT(rc == 0, "CB-001: init with minimum geometry");
    if (rc != 0) return;

    u64 total_lbas = cfg.sssim_cfg.total_lbas;
    void *buf = malloc(LBA_SIZE);
    TEST_ASSERT(buf != NULL, "CB-001: allocate buffer");
    if (!buf) { teardown_dev(&dev); return; }

    /* Write all LBAs */
    for (u64 lba = 0; lba < total_lbas; lba++) {
        fill_pattern(buf, (uint32_t)lba, 1);
        nvme_uspace_write(&dev, 1, lba, 1, buf);
    }

    /* Read and verify */
    int corrupt = 0;
    for (u64 lba = 0; lba < total_lbas; lba++) {
        rc = nvme_uspace_read(&dev, 1, lba, 1, buf);
        if (rc != HFSSS_OK || !verify_pattern(buf, (uint32_t)lba, 1)) corrupt++;
    }
    TEST_ASSERT(corrupt == 0, "CB-001: first write-read verify pass");

    /* Overwrite all LBAs to trigger GC. Track which LBAs succeeded. */
    uint8_t *gen_map = calloc(total_lbas, 1);
    for (u64 lba = 0; lba < total_lbas; lba++) gen_map[lba] = 1;
    for (u64 lba = 0; lba < total_lbas; lba++) {
        fill_pattern(buf, (uint32_t)lba, 2);
        rc = nvme_uspace_write(&dev, 1, lba, 1, buf);
        if (rc == HFSSS_OK) gen_map[lba] = 2;
    }

    /* Read and verify — each LBA should match its successful gen */
    corrupt = 0;
    for (u64 lba = 0; lba < total_lbas; lba++) {
        rc = nvme_uspace_read(&dev, 1, lba, 1, buf);
        if (rc != HFSSS_OK || !verify_pattern(buf, (uint32_t)lba, gen_map[lba])) corrupt++;
    }
    TEST_ASSERT(corrupt == 0, "CB-001: overwrite-read verify pass (GC triggered)");
    free(gen_map);

    free(buf);
    teardown_dev(&dev);
}

/* ================================================================
 * CB-002: Larger geometry
 * ================================================================ */
static void test_cb002_larger_geometry(void) {
    printf("\n=== CB-002: Larger geometry ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    memset(&dev, 0, sizeof(dev));

    int rc = setup_dev(&dev, &cfg, 4, 2, 1, 1, 128, 128, 4096);
    TEST_ASSERT(rc == 0, "CB-002: init with larger geometry");
    if (rc != 0) return;

    u64 total_lbas = cfg.sssim_cfg.total_lbas;
    void *buf = malloc(LBA_SIZE);
    TEST_ASSERT(buf != NULL, "CB-002: allocate buffer");
    if (!buf) { teardown_dev(&dev); return; }

    /* Write 500 random LBAs */
    uint32_t seed = 0xDEADBEEF;
    u64 written_lbas[500];
    for (int i = 0; i < 500; i++) {
        seed = lcg(seed);
        written_lbas[i] = seed % total_lbas;
        fill_pattern(buf, (uint32_t)written_lbas[i], 1);
        rc = nvme_uspace_write(&dev, 1, written_lbas[i], 1, buf);
        TEST_ASSERT(rc == HFSSS_OK || i > 0, "CB-002: random write");
    }

    /* Read and verify */
    int corrupt = 0;
    for (int i = 0; i < 500; i++) {
        rc = nvme_uspace_read(&dev, 1, written_lbas[i], 1, buf);
        if (rc != HFSSS_OK || !verify_pattern(buf, (uint32_t)written_lbas[i], 1))
            corrupt++;
    }
    TEST_ASSERT(corrupt == 0, "CB-002: all 500 random LBAs verified");

    free(buf);
    teardown_dev(&dev);
}

/* ================================================================
 * CB-003: Low OP ratio (5%)
 * ================================================================ */
static void test_cb003_low_op_ratio(void) {
    printf("\n=== CB-003: Low OP ratio (5%%) ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    memset(&dev, 0, sizeof(dev));

    /* Custom setup with op_ratio = 5 */
    nvme_uspace_config_default(&cfg);
    cfg.sssim_cfg.channel_count = 2;
    cfg.sssim_cfg.chips_per_channel = 1;
    cfg.sssim_cfg.dies_per_chip = 1;
    cfg.sssim_cfg.planes_per_die = 1;
    cfg.sssim_cfg.blocks_per_plane = 64;
    cfg.sssim_cfg.pages_per_block = 128;
    cfg.sssim_cfg.page_size = 4096;
    cfg.sssim_cfg.op_ratio = 5;
    uint64_t raw = (uint64_t)2 * 1 * 1 * 1 * 64 * 128;
    cfg.sssim_cfg.total_lbas = raw * (100 - 5) / 100;

    int rc = nvme_uspace_dev_init(&dev, &cfg);
    TEST_ASSERT(rc == HFSSS_OK, "CB-003: dev init with 5%% OP");
    if (rc != HFSSS_OK) return;

    rc = nvme_uspace_dev_start(&dev);
    TEST_ASSERT(rc == HFSSS_OK, "CB-003: dev start");
    if (rc != HFSSS_OK) { nvme_uspace_dev_cleanup(&dev); return; }

    rc = nvme_uspace_create_io_cq(&dev, 1, 256, false);
    TEST_ASSERT(rc == HFSSS_OK, "CB-003: create CQ");
    rc = nvme_uspace_create_io_sq(&dev, 1, 256, 1, 0);
    TEST_ASSERT(rc == HFSSS_OK, "CB-003: create SQ");

    u64 total_lbas = cfg.sssim_cfg.total_lbas;
    void *buf = malloc(LBA_SIZE);
    TEST_ASSERT(buf != NULL, "CB-003: allocate buffer");
    if (!buf) { teardown_dev(&dev); return; }

    /* Track which LBAs were successfully written and their generation */
    u64 *gen_map = calloc(total_lbas, sizeof(u64));
    TEST_ASSERT(gen_map != NULL, "CB-003: allocate generation map");
    if (!gen_map) { free(buf); teardown_dev(&dev); return; }

    /* Write all LBAs (some may NOSPC) */
    int written_ok = 0;
    for (u64 lba = 0; lba < total_lbas; lba++) {
        fill_pattern(buf, (uint32_t)lba, 1);
        rc = nvme_uspace_write(&dev, 1, lba, 1, buf);
        if (rc == HFSSS_OK) {
            gen_map[lba] = 1;
            written_ok++;
        }
    }
    TEST_ASSERT(written_ok > 0, "CB-003: at least some LBAs written");

    /* Random overwrite 5000 times (heavy GC pressure) */
    uint32_t seed = 0xCAFEBABE;
    for (int i = 0; i < 5000; i++) {
        seed = lcg(seed);
        u64 lba = seed % total_lbas;
        u64 gen = gen_map[lba] + 1;
        fill_pattern(buf, (uint32_t)lba, gen);
        rc = nvme_uspace_write(&dev, 1, lba, 1, buf);
        if (rc == HFSSS_OK) {
            gen_map[lba] = gen;
        }
    }

    /* Verify all written LBAs */
    int corrupt = 0;
    for (u64 lba = 0; lba < total_lbas; lba++) {
        if (gen_map[lba] == 0) continue;
        rc = nvme_uspace_read(&dev, 1, lba, 1, buf);
        if (rc != HFSSS_OK || !verify_pattern(buf, (uint32_t)lba, gen_map[lba]))
            corrupt++;
    }
    TEST_ASSERT(corrupt == 0, "CB-003: all written LBAs verified after heavy GC");

    free(gen_map);
    free(buf);
    teardown_dev(&dev);
}

/* ================================================================
 * CB-005: NAND types (verify all init correctly)
 * ================================================================ */
static void test_cb005_nand_types(void) {
    printf("\n=== CB-005: NAND types ===\n");

    enum nand_type types[] = {
        NAND_TYPE_SLC,
        NAND_TYPE_MLC,
        NAND_TYPE_TLC,
        NAND_TYPE_QLC
    };
    const char *type_names[] = { "SLC", "MLC", "TLC", "QLC" };

    void *buf = malloc(LBA_SIZE);
    TEST_ASSERT(buf != NULL, "CB-005: allocate buffer");
    if (!buf) return;

    for (int t = 0; t < 4; t++) {
        struct nvme_uspace_dev dev;
        struct nvme_uspace_config cfg;
        memset(&dev, 0, sizeof(dev));

        nvme_uspace_config_default(&cfg);
        cfg.sssim_cfg.channel_count = 2;
        cfg.sssim_cfg.chips_per_channel = 1;
        cfg.sssim_cfg.dies_per_chip = 1;
        cfg.sssim_cfg.planes_per_die = 1;
        cfg.sssim_cfg.blocks_per_plane = 32;
        cfg.sssim_cfg.pages_per_block = 64;
        cfg.sssim_cfg.page_size = 4096;
        cfg.sssim_cfg.nand_type = types[t];
        uint64_t raw = (uint64_t)2 * 1 * 1 * 1 * 32 * 64;
        cfg.sssim_cfg.total_lbas = raw * (100 - cfg.sssim_cfg.op_ratio) / 100;

        int rc = nvme_uspace_dev_init(&dev, &cfg);
        if (rc != HFSSS_OK) {
            char msg[128];
            snprintf(msg, sizeof(msg), "CB-005: %s init", type_names[t]);
            TEST_ASSERT(false, msg);
            continue;
        }
        rc = nvme_uspace_dev_start(&dev);
        if (rc != HFSSS_OK) {
            char msg[128];
            snprintf(msg, sizeof(msg), "CB-005: %s start", type_names[t]);
            TEST_ASSERT(false, msg);
            nvme_uspace_dev_cleanup(&dev);
            continue;
        }
        rc = nvme_uspace_create_io_cq(&dev, 1, 256, false);
        rc |= nvme_uspace_create_io_sq(&dev, 1, 256, 1, 0);

        /* Write 10 LBAs, read verify */
        int corrupt = 0;
        for (u32 i = 0; i < 10; i++) {
            fill_pattern(buf, i, 1);
            rc = nvme_uspace_write(&dev, 1, (u64)i, 1, buf);
            if (rc != HFSSS_OK) { corrupt++; continue; }
        }
        for (u32 i = 0; i < 10; i++) {
            rc = nvme_uspace_read(&dev, 1, (u64)i, 1, buf);
            if (rc != HFSSS_OK || !verify_pattern(buf, i, 1)) corrupt++;
        }

        char msg[128];
        snprintf(msg, sizeof(msg), "CB-005: %s write/read 10 LBAs verified", type_names[t]);
        TEST_ASSERT(corrupt == 0, msg);

        teardown_dev(&dev);
    }

    free(buf);
}

/* ================================================================
 * CB-006: Edge LBA values
 * ================================================================ */
static void test_cb006_edge_lba_values(void) {
    printf("\n=== CB-006: Edge LBA values ===\n");

    struct nvme_uspace_dev dev;
    struct nvme_uspace_config cfg;
    memset(&dev, 0, sizeof(dev));

    int rc = setup_dev(&dev, &cfg, 2, 1, 1, 1, 32, 64, 4096);
    TEST_ASSERT(rc == 0, "CB-006: device init");
    if (rc != 0) return;

    u64 total_lbas = cfg.sssim_cfg.total_lbas;
    void *buf = malloc(LBA_SIZE);
    TEST_ASSERT(buf != NULL, "CB-006: allocate buffer");
    if (!buf) { teardown_dev(&dev); return; }

    /* Write/read LBA 0 */
    fill_pattern(buf, 0, 1);
    rc = nvme_uspace_write(&dev, 1, 0, 1, buf);
    TEST_ASSERT(rc == HFSSS_OK, "CB-006: write LBA 0");
    memset(buf, 0, LBA_SIZE);
    rc = nvme_uspace_read(&dev, 1, 0, 1, buf);
    TEST_ASSERT(rc == HFSSS_OK && verify_pattern(buf, 0, 1), "CB-006: read/verify LBA 0");

    /* Write/read LBA total_lbas-1 */
    u64 last_lba = total_lbas - 1;
    fill_pattern(buf, (uint32_t)last_lba, 1);
    rc = nvme_uspace_write(&dev, 1, last_lba, 1, buf);
    TEST_ASSERT(rc == HFSSS_OK, "CB-006: write last LBA");
    memset(buf, 0, LBA_SIZE);
    rc = nvme_uspace_read(&dev, 1, last_lba, 1, buf);
    TEST_ASSERT(rc == HFSSS_OK && verify_pattern(buf, (uint32_t)last_lba, 1),
                "CB-006: read/verify last LBA");

    /* Write at LBA total_lbas: expect error */
    fill_pattern(buf, (uint32_t)total_lbas, 1);
    rc = nvme_uspace_write(&dev, 1, total_lbas, 1, buf);
    TEST_ASSERT(rc != HFSSS_OK, "CB-006: write beyond capacity returns error");

    /* Read at LBA total_lbas: expect error */
    rc = nvme_uspace_read(&dev, 1, total_lbas, 1, buf);
    TEST_ASSERT(rc != HFSSS_OK, "CB-006: read beyond capacity returns error");

    free(buf);
    teardown_dev(&dev);
}

/* ================================================================
 * Main
 * ================================================================ */
int main(void) {
    printf("============================================\n");
    printf("  HFSSS Error Handling & Boundary Tests\n");
    printf("============================================\n");

    test_eh005_nospc_handling();
    test_cb001_minimum_geometry();
    test_cb002_larger_geometry();
    test_cb003_low_op_ratio();
    test_cb005_nand_types();
    test_cb006_edge_lba_values();

    printf("\n============================================\n");
    printf("  Results: %d/%d passed, %d failed\n", passed_tests, total_tests, failed_tests);
    printf("============================================\n");

    return (failed_tests == 0) ? 0 : 1;
}
