#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "common/hfsss_config.h"
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
 * Defaults
 * ------------------------------------------------------------------ */
static void test_defaults(void) {
    separator();
    printf("Test: hfsss_config_defaults\n");
    separator();

    struct hfsss_config cfg;
    hfsss_config_defaults(&cfg);

    TEST_ASSERT(cfg.nand.channel_count > 0, "default channel_count > 0");
    TEST_ASSERT(cfg.nand.page_size > 0, "default page_size > 0");
    TEST_ASSERT((cfg.nand.page_size & (cfg.nand.page_size - 1)) == 0,
                "default page_size is a power of 2");
    TEST_ASSERT(cfg.gc.threshold_pct > 0, "default gc threshold > 0");
    TEST_ASSERT(strcmp(cfg.gc.policy, "greedy") == 0 ||
                strcmp(cfg.gc.policy, "cost_benefit") == 0,
                "default gc policy is valid");
    TEST_ASSERT(cfg.oob.enabled, "oob enabled by default");
    TEST_ASSERT(cfg.oob.sock_path[0] != '\0', "oob sock_path set");
    TEST_ASSERT(cfg.perf.temp_warn_celsius < cfg.perf.temp_crit_celsius,
                "warn temp < crit temp");
    TEST_ASSERT(cfg.persist.checkpoint_interval_s > 0,
                "checkpoint interval > 0");
    TEST_ASSERT(!cfg.loaded, "loaded=false after defaults");
}

/* ------------------------------------------------------------------
 * Validate
 * ------------------------------------------------------------------ */
static void test_validate(void) {
    separator();
    printf("Test: hfsss_config_validate\n");
    separator();

    struct hfsss_config cfg;
    hfsss_config_defaults(&cfg);
    char errbuf[256];

    TEST_ASSERT(hfsss_config_validate(&cfg, errbuf, sizeof(errbuf)) == HFSSS_OK,
                "defaults pass validation");

    /* Invalid: channel_count = 0 */
    cfg.nand.channel_count = 0;
    TEST_ASSERT(hfsss_config_validate(&cfg, errbuf, sizeof(errbuf)) != HFSSS_OK,
                "validation fails with channel_count=0");
    TEST_ASSERT(strlen(errbuf) > 0, "errbuf populated on validation failure");
    hfsss_config_defaults(&cfg);

    /* Invalid: page_size not power of 2 */
    cfg.nand.page_size = 5000;
    TEST_ASSERT(hfsss_config_validate(&cfg, errbuf, sizeof(errbuf)) != HFSSS_OK,
                "validation fails with non-power-of-2 page_size");
    hfsss_config_defaults(&cfg);

    /* Invalid: warn >= crit */
    cfg.perf.temp_warn_celsius = 80;
    cfg.perf.temp_crit_celsius = 70;
    TEST_ASSERT(hfsss_config_validate(&cfg, errbuf, sizeof(errbuf)) != HFSSS_OK,
                "validation fails when warn_temp >= crit_temp");

    /* NULL safety */
    TEST_ASSERT(hfsss_config_validate(NULL, errbuf, sizeof(errbuf)) != HFSSS_OK,
                "validate(NULL) returns error");
}

/* ------------------------------------------------------------------
 * Save and load round-trip
 * ------------------------------------------------------------------ */
static void test_save_load(void) {
    separator();
    printf("Test: config save and load round-trip\n");
    separator();

    const char *path = "/tmp/hfsss_test_config.yaml";
    struct hfsss_config orig;
    hfsss_config_defaults(&orig);

    /* Modify some values */
    orig.nand.channel_count    = 4;
    orig.nand.page_size        = 8192;
    orig.gc.threshold_pct      = 15;
    strncpy(orig.gc.policy, "cost_benefit", sizeof(orig.gc.policy) - 1);
    orig.oob.max_clients       = 8;
    orig.persist.checkpoint_interval_s = 60;

    int ret = hfsss_config_save(path, &orig);
    TEST_ASSERT(ret == HFSSS_OK, "config_save returns OK");

    struct hfsss_config loaded;
    char errbuf[256] = {0};
    ret = hfsss_config_load(path, &loaded, errbuf, sizeof(errbuf));
    TEST_ASSERT(ret == HFSSS_OK, "config_load returns OK");
    TEST_ASSERT(loaded.loaded, "loaded flag set");

    TEST_ASSERT(loaded.nand.channel_count == 4, "channel_count round-trips");
    TEST_ASSERT(loaded.nand.page_size     == 8192, "page_size round-trips");
    TEST_ASSERT(loaded.gc.threshold_pct   == 15, "gc.threshold_pct round-trips");
    TEST_ASSERT(strcmp(loaded.gc.policy, "cost_benefit") == 0,
                "gc.policy round-trips");
    TEST_ASSERT(loaded.oob.max_clients    == 8, "oob.max_clients round-trips");
    TEST_ASSERT(loaded.persist.checkpoint_interval_s == 60,
                "checkpoint_interval_s round-trips");

    remove(path);
}

/* ------------------------------------------------------------------
 * Load from non-existent file
 * ------------------------------------------------------------------ */
static void test_load_noent(void) {
    separator();
    printf("Test: load from non-existent file\n");
    separator();

    struct hfsss_config cfg;
    char errbuf[256];
    int ret = hfsss_config_load("/tmp/hfsss_does_not_exist_xyz.yaml",
                                &cfg, errbuf, sizeof(errbuf));
    TEST_ASSERT(ret == HFSSS_ERR_NOENT, "load returns NOENT for missing file");
    TEST_ASSERT(strlen(errbuf) > 0, "errbuf populated");
}

/* ------------------------------------------------------------------
 * Load with unknown keys (should warn, not fail)
 * ------------------------------------------------------------------ */
static void test_load_unknown_keys(void) {
    separator();
    printf("Test: unknown keys produce warning, not error\n");
    separator();

    const char *path = "/tmp/hfsss_test_unknown.yaml";
    FILE *f = fopen(path, "w");
    if (!f) {
        printf("  [SKIP] cannot create temp file\n");
        return;
    }
    fprintf(f, "nand:\n");
    fprintf(f, "  channel_count: 2\n");
    fprintf(f, "  unknown_key_xyz: 999\n");
    fprintf(f, "unknown_section:\n");
    fprintf(f, "  foo: bar\n");
    fclose(f);

    struct hfsss_config cfg;
    char errbuf[256] = {0};
    int ret = hfsss_config_load(path, &cfg, errbuf, sizeof(errbuf));
    TEST_ASSERT(ret == HFSSS_OK, "load succeeds despite unknown keys");
    TEST_ASSERT(cfg.nand.channel_count == 2,
                "known key channel_count parsed correctly");

    remove(path);
}

/* ------------------------------------------------------------------
 * Get/set by dotted key
 * ------------------------------------------------------------------ */
static void test_get_set(void) {
    separator();
    printf("Test: get/set by dotted key\n");
    separator();

    struct hfsss_config cfg;
    hfsss_config_defaults(&cfg);

    /* Get */
    char val[64];
    int ret = hfsss_config_get_str(&cfg, "nand.channel_count", val, sizeof(val));
    TEST_ASSERT(ret == HFSSS_OK, "get nand.channel_count returns OK");
    TEST_ASSERT(atoi(val) == (int)cfg.nand.channel_count,
                "get returns correct value");

    ret = hfsss_config_get_str(&cfg, "gc.policy", val, sizeof(val));
    TEST_ASSERT(ret == HFSSS_OK, "get gc.policy returns OK");
    TEST_ASSERT(strlen(val) > 0, "gc.policy value non-empty");

    ret = hfsss_config_get_str(&cfg, "nonexistent.key", val, sizeof(val));
    TEST_ASSERT(ret == HFSSS_ERR_NOENT, "get nonexistent key returns NOENT");

    /* Set */
    ret = hfsss_config_set_str(&cfg, "nand.channel_count", "16");
    TEST_ASSERT(ret == HFSSS_OK, "set nand.channel_count returns OK");
    TEST_ASSERT(cfg.nand.channel_count == 16, "channel_count updated");

    ret = hfsss_config_set_str(&cfg, "gc.policy", "cost_benefit");
    TEST_ASSERT(ret == HFSSS_OK, "set gc.policy returns OK");
    TEST_ASSERT(strcmp(cfg.gc.policy, "cost_benefit") == 0, "gc.policy updated");

    ret = hfsss_config_set_str(&cfg, "nonexistent.key", "value");
    TEST_ASSERT(ret == HFSSS_ERR_NOENT, "set nonexistent key returns NOENT");
}

/* ------------------------------------------------------------------
 * NULL safety
 * ------------------------------------------------------------------ */
static void test_null_safety(void) {
    separator();
    printf("Test: NULL safety\n");
    separator();

    char errbuf[64];
    char val[64];

    hfsss_config_defaults(NULL);  /* should not crash */
    TEST_ASSERT(true, "defaults(NULL) does not crash");

    TEST_ASSERT(hfsss_config_load(NULL, NULL, errbuf, sizeof(errbuf)) == HFSSS_ERR_INVAL,
                "load(NULL, NULL) returns INVAL");
    TEST_ASSERT(hfsss_config_save(NULL, NULL) == HFSSS_ERR_INVAL,
                "save(NULL, NULL) returns INVAL");
    TEST_ASSERT(hfsss_config_get_str(NULL, "key", val, sizeof(val)) == HFSSS_ERR_INVAL,
                "get_str(NULL) returns INVAL");
    TEST_ASSERT(hfsss_config_set_str(NULL, "key", "val") == HFSSS_ERR_INVAL,
                "set_str(NULL) returns INVAL");
}

/* ------------------------------------------------------------------
 * Full NAND geometry round-trip
 * ------------------------------------------------------------------ */
static void test_full_nand_roundtrip(void) {
    separator();
    printf("Test: full NAND geometry round-trip\n");
    separator();

    const char *path = "/tmp/hfsss_test_nand_rt.yaml";
    struct hfsss_config orig;
    hfsss_config_defaults(&orig);

    /* Override every NAND field to non-default values */
    orig.nand.channel_count     = 32;
    orig.nand.chips_per_channel = 4;
    orig.nand.dies_per_chip     = 8;
    orig.nand.planes_per_die    = 4;
    orig.nand.blocks_per_plane  = 4096;
    orig.nand.pages_per_block   = 1024;
    orig.nand.page_size         = 32768;
    orig.nand.spare_size        = 512;
    orig.nand.op_ratio_pct      = 28;

    int ret = hfsss_config_save(path, &orig);
    TEST_ASSERT(ret == HFSSS_OK, "save returns OK");

    struct hfsss_config loaded;
    char errbuf[256] = {0};
    ret = hfsss_config_load(path, &loaded, errbuf, sizeof(errbuf));
    TEST_ASSERT(ret == HFSSS_OK, "load returns OK");

    TEST_ASSERT(loaded.nand.channel_count     == 32,    "channel_count 32 round-trips");
    TEST_ASSERT(loaded.nand.chips_per_channel == 4,     "chips_per_channel 4 round-trips");
    TEST_ASSERT(loaded.nand.dies_per_chip     == 8,     "dies_per_chip 8 round-trips");
    TEST_ASSERT(loaded.nand.planes_per_die    == 4,     "planes_per_die 4 round-trips");
    TEST_ASSERT(loaded.nand.blocks_per_plane  == 4096,  "blocks_per_plane 4096 round-trips");
    TEST_ASSERT(loaded.nand.pages_per_block   == 1024,  "pages_per_block 1024 round-trips");
    TEST_ASSERT(loaded.nand.page_size         == 32768, "page_size 32768 round-trips");
    TEST_ASSERT(loaded.nand.spare_size        == 512,   "spare_size 512 round-trips");
    TEST_ASSERT(loaded.nand.op_ratio_pct      == 28,    "op_ratio_pct 28 round-trips");

    remove(path);
}

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */
int main(void) {
    separator();
    printf("HFSSS Config Parser Tests\n");
    separator();

    test_defaults();
    test_validate();
    test_save_load();
    test_load_noent();
    test_load_unknown_keys();
    test_get_set();
    test_null_safety();
    test_full_nand_roundtrip();

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
