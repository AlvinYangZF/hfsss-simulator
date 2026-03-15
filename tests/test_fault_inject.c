/*
 * Tests for the fault injection framework (REQ-132, REQ-133, REQ-134).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "fault_inject.h"
#include "common.h"

/* -------------------------------------------------------------------------
 * Minimal test harness
 * ---------------------------------------------------------------------- */

static int g_pass = 0;
static int g_fail = 0;

#define TEST_ASSERT(cond, name)                                     \
    do {                                                            \
        if (cond) {                                                 \
            printf("  PASS: %s\n", (name));                        \
            g_pass++;                                               \
        } else {                                                    \
            printf("  FAIL: %s  (line %d)\n", (name), __LINE__);  \
            g_fail++;                                               \
        }                                                           \
    } while (0)

/* Full-wildcard convenience address. */
static const struct fault_addr ANY_ADDR = {
    FAULT_WILDCARD, FAULT_WILDCARD, FAULT_WILDCARD,
    FAULT_WILDCARD, FAULT_WILDCARD, FAULT_WILDCARD
};

/* A concrete address for exact-match tests. */
static const struct fault_addr ADDR_1 = { 0, 1, 0, 0, 42, 7 };

/* -------------------------------------------------------------------------
 * Test cases
 * ---------------------------------------------------------------------- */

/* 1. init / cleanup */
static void test_init_cleanup(void)
{
    printf("[1] init/cleanup\n");
    struct fault_registry reg;
    int rc = fault_registry_init(&reg);
    TEST_ASSERT(rc == HFSSS_OK,          "init returns OK");
    TEST_ASSERT(reg.initialized == true, "initialized flag set");
    TEST_ASSERT(reg.count == 0,          "count is 0 after init");
    TEST_ASSERT(reg.type_present == 0,   "type_present is 0 after init");

    fault_registry_cleanup(&reg);
    TEST_ASSERT(reg.initialized == false, "initialized flag cleared after cleanup");
}

/* 2. add fault returns valid id */
static void test_add_returns_id(void)
{
    printf("[2] add fault returns valid id\n");
    struct fault_registry reg;
    fault_registry_init(&reg);

    int id = fault_inject_add(&reg, FAULT_READ_ERROR, &ANY_ADDR,
                               FAULT_PERSIST_STICKY, 1.0);
    TEST_ASSERT(id > 0, "returned id > 0");
    TEST_ASSERT(reg.count == 1, "count is 1");
    TEST_ASSERT((reg.type_present & FAULT_READ_ERROR) != 0, "type_present updated");

    fault_registry_cleanup(&reg);
}

/* 3. add multiple faults, count increases */
static void test_add_multiple(void)
{
    printf("[3] add multiple faults\n");
    struct fault_registry reg;
    fault_registry_init(&reg);

    fault_inject_add(&reg, FAULT_BAD_BLOCK,     &ANY_ADDR, FAULT_PERSIST_STICKY, 1.0);
    fault_inject_add(&reg, FAULT_READ_ERROR,    &ANY_ADDR, FAULT_PERSIST_STICKY, 1.0);
    fault_inject_add(&reg, FAULT_PROGRAM_ERROR, &ANY_ADDR, FAULT_PERSIST_STICKY, 1.0);

    TEST_ASSERT(reg.count == 3, "count is 3");
    TEST_ASSERT((reg.type_present & FAULT_BAD_BLOCK)     != 0, "BAD_BLOCK present");
    TEST_ASSERT((reg.type_present & FAULT_READ_ERROR)    != 0, "READ_ERROR present");
    TEST_ASSERT((reg.type_present & FAULT_PROGRAM_ERROR) != 0, "PROGRAM_ERROR present");

    fault_registry_cleanup(&reg);
}

/* 4. registry full returns -1 */
static void test_registry_full(void)
{
    printf("[4] registry full\n");
    struct fault_registry reg;
    fault_registry_init(&reg);

    for (int i = 0; i < FAULT_REGISTRY_MAX; i++)
        fault_inject_add(&reg, FAULT_READ_ERROR, &ANY_ADDR, FAULT_PERSIST_STICKY, 1.0);

    int rc = fault_inject_add(&reg, FAULT_ERASE_ERROR, &ANY_ADDR,
                               FAULT_PERSIST_STICKY, 1.0);
    TEST_ASSERT(rc == HFSSS_ERR, "returns HFSSS_ERR when full");

    fault_registry_cleanup(&reg);
}

/* 5. fault_check returns NULL when registry empty */
static void test_check_empty(void)
{
    printf("[5] fault_check on empty registry\n");
    struct fault_registry reg;
    fault_registry_init(&reg);

    struct fault_entry *e = fault_check(&reg, FAULT_READ_ERROR, &ADDR_1);
    TEST_ASSERT(e == NULL, "returns NULL on empty registry");

    fault_registry_cleanup(&reg);
}

/* 6. fast exit via type_present bitmask */
static void test_check_fast_exit(void)
{
    printf("[6] fault_check fast exit via type_present\n");
    struct fault_registry reg;
    fault_registry_init(&reg);

    /* Only a BAD_BLOCK fault is registered. */
    fault_inject_add(&reg, FAULT_BAD_BLOCK, &ANY_ADDR, FAULT_PERSIST_STICKY, 1.0);

    struct fault_entry *e = fault_check(&reg, FAULT_READ_ERROR, &ADDR_1);
    TEST_ASSERT(e == NULL, "READ_ERROR check returns NULL (only BAD_BLOCK registered)");

    fault_registry_cleanup(&reg);
}

/* 7. fault_check matches exact address */
static void test_check_exact_addr(void)
{
    printf("[7] fault_check exact address match\n");
    struct fault_registry reg;
    fault_registry_init(&reg);

    fault_inject_add(&reg, FAULT_READ_ERROR, &ADDR_1, FAULT_PERSIST_STICKY, 1.0);

    struct fault_entry *e = fault_check(&reg, FAULT_READ_ERROR, &ADDR_1);
    TEST_ASSERT(e != NULL, "matches exact address");

    /* A different address should not match. */
    struct fault_addr other = { 1, 0, 0, 0, 0, 0 };
    e = fault_check(&reg, FAULT_READ_ERROR, &other);
    TEST_ASSERT(e == NULL, "different address does not match");

    fault_registry_cleanup(&reg);
}

/* 8. wildcard channel matches any channel */
static void test_check_wildcard_channel(void)
{
    printf("[8] wildcard channel\n");
    struct fault_registry reg;
    fault_registry_init(&reg);

    struct fault_addr pattern = { FAULT_WILDCARD, 1, 0, 0, 42, 7 };
    fault_inject_add(&reg, FAULT_READ_ERROR, &pattern, FAULT_PERSIST_STICKY, 1.0);

    struct fault_addr probe0 = { 0, 1, 0, 0, 42, 7 };
    struct fault_addr probe9 = { 9, 1, 0, 0, 42, 7 };

    TEST_ASSERT(fault_check(&reg, FAULT_READ_ERROR, &probe0) != NULL,
                "channel 0 matches wildcard channel");
    TEST_ASSERT(fault_check(&reg, FAULT_READ_ERROR, &probe9) != NULL,
                "channel 9 matches wildcard channel");

    fault_registry_cleanup(&reg);
}

/* 9. wildcard block matches any block */
static void test_check_wildcard_block(void)
{
    printf("[9] wildcard block\n");
    struct fault_registry reg;
    fault_registry_init(&reg);

    struct fault_addr pattern = { 0, 0, 0, 0, FAULT_WILDCARD, 0 };
    fault_inject_add(&reg, FAULT_ERASE_ERROR, &pattern, FAULT_PERSIST_STICKY, 1.0);

    struct fault_addr probe_a = { 0, 0, 0, 0,   0, 0 };
    struct fault_addr probe_b = { 0, 0, 0, 0, 999, 0 };

    TEST_ASSERT(fault_check(&reg, FAULT_ERASE_ERROR, &probe_a) != NULL,
                "block 0 matches wildcard block");
    TEST_ASSERT(fault_check(&reg, FAULT_ERASE_ERROR, &probe_b) != NULL,
                "block 999 matches wildcard block");

    fault_registry_cleanup(&reg);
}

/* 10. full wildcard matches any address */
static void test_check_full_wildcard(void)
{
    printf("[10] full wildcard matches any address\n");
    struct fault_registry reg;
    fault_registry_init(&reg);

    fault_inject_add(&reg, FAULT_BAD_BLOCK, &ANY_ADDR, FAULT_PERSIST_STICKY, 1.0);

    struct fault_addr probes[] = {
        { 0, 0, 0, 0,   0,   0 },
        { 3, 7, 2, 1, 500, 127 },
        { UINT32_MAX - 1, 0, 0, 0, 0, 0 },
    };

    for (int i = 0; i < 3; i++) {
        TEST_ASSERT(fault_check(&reg, FAULT_BAD_BLOCK, &probes[i]) != NULL,
                    "full wildcard matches arbitrary address");
    }

    fault_registry_cleanup(&reg);
}

/* 11. one-shot fault triggers once then deactivates */
static void test_one_shot(void)
{
    printf("[11] one-shot fault\n");
    struct fault_registry reg;
    fault_registry_init(&reg);

    fault_inject_add(&reg, FAULT_READ_ERROR, &ANY_ADDR, FAULT_PERSIST_ONE_SHOT, 1.0);

    struct fault_entry *e1 = fault_check(&reg, FAULT_READ_ERROR, &ADDR_1);
    TEST_ASSERT(e1 != NULL,         "first check triggers one-shot");
    TEST_ASSERT(e1->hit_count == 1, "hit_count == 1 after first trigger");

    struct fault_entry *e2 = fault_check(&reg, FAULT_READ_ERROR, &ADDR_1);
    TEST_ASSERT(e2 == NULL, "second check returns NULL (already deactivated)");
    TEST_ASSERT(reg.count == 0, "count is 0 after one-shot consumed");

    fault_registry_cleanup(&reg);
}

/* 12. sticky fault triggers multiple times */
static void test_sticky(void)
{
    printf("[12] sticky fault\n");
    struct fault_registry reg;
    fault_registry_init(&reg);

    fault_inject_add(&reg, FAULT_READ_ERROR, &ANY_ADDR, FAULT_PERSIST_STICKY, 1.0);

    for (int i = 0; i < 5; i++) {
        struct fault_entry *e = fault_check(&reg, FAULT_READ_ERROR, &ADDR_1);
        TEST_ASSERT(e != NULL, "sticky fault triggers repeatedly");
    }

    /* After 5 checks, the entry is still active. */
    struct fault_entry *last = fault_check(&reg, FAULT_READ_ERROR, &ADDR_1);
    TEST_ASSERT(last != NULL && last->hit_count == 6, "hit_count accumulates");

    fault_registry_cleanup(&reg);
}

/* 13. probability 0.0 never triggers */
static void test_prob_zero(void)
{
    printf("[13] probability=0.0 never triggers\n");
    struct fault_registry reg;
    fault_registry_init(&reg);

    fault_inject_add(&reg, FAULT_READ_ERROR, &ANY_ADDR, FAULT_PERSIST_STICKY, 0.0);

    bool any_triggered = false;
    for (int i = 0; i < 1000; i++) {
        if (fault_check(&reg, FAULT_READ_ERROR, &ADDR_1))
            any_triggered = true;
    }
    TEST_ASSERT(!any_triggered, "probability=0.0 never triggers (1000 attempts)");

    fault_registry_cleanup(&reg);
}

/* 14. probability 1.0 always triggers */
static void test_prob_one(void)
{
    printf("[14] probability=1.0 always triggers\n");
    struct fault_registry reg;
    fault_registry_init(&reg);

    fault_inject_add(&reg, FAULT_READ_ERROR, &ANY_ADDR, FAULT_PERSIST_STICKY, 1.0);

    bool all_triggered = true;
    for (int i = 0; i < 100; i++) {
        if (!fault_check(&reg, FAULT_READ_ERROR, &ADDR_1))
            all_triggered = false;
    }
    TEST_ASSERT(all_triggered, "probability=1.0 always triggers (100 attempts)");

    fault_registry_cleanup(&reg);
}

/* 15. fault_apply_bit_flip flips correct bits */
static void test_bit_flip_apply(void)
{
    printf("[15] fault_apply_bit_flip\n");

    uint8_t buf[16];
    memset(buf, 0x00, sizeof(buf));

    uint64_t mask = 0xDEADBEEFCAFEBABEULL;
    fault_apply_bit_flip(buf, sizeof(buf), mask);

    uint64_t *w = (uint64_t *)buf;
    TEST_ASSERT(w[0] == mask, "first word XOR'd with mask");
    TEST_ASSERT(w[1] == mask, "second word XOR'd with mask");

    /* Applying twice should restore original. */
    fault_apply_bit_flip(buf, sizeof(buf), mask);
    TEST_ASSERT(w[0] == 0 && w[1] == 0, "double XOR restores original");
}

/* 16. set_bit_flip updates existing entry */
static void test_set_bit_flip(void)
{
    printf("[16] fault_inject_set_bit_flip\n");
    struct fault_registry reg;
    fault_registry_init(&reg);

    int id = fault_inject_add(&reg, FAULT_BIT_FLIP, &ANY_ADDR,
                               FAULT_PERSIST_STICKY, 1.0);
    TEST_ASSERT(id > 0, "fault added");

    uint64_t mask = 0x1122334455667788ULL;
    int rc = fault_inject_set_bit_flip(&reg, (uint32_t)id, mask);
    TEST_ASSERT(rc == HFSSS_OK, "set_bit_flip returns OK");

    /* Verify the mask was stored. */
    struct fault_entry *e = fault_check(&reg, FAULT_BIT_FLIP, &ADDR_1);
    TEST_ASSERT(e != NULL && e->bit_flip_mask == mask, "mask stored correctly");

    fault_registry_cleanup(&reg);
}

/* 17. set_disturb / set_aging update entry fields */
static void test_set_disturb_aging(void)
{
    printf("[17] set_disturb / set_aging\n");
    struct fault_registry reg;
    fault_registry_init(&reg);

    int id = fault_inject_add(&reg, FAULT_READ_DISTURB, &ANY_ADDR,
                               FAULT_PERSIST_STICKY, 1.0);

    int rc_d = fault_inject_set_disturb(&reg, (uint32_t)id, 3.5);
    int rc_a = fault_inject_set_aging  (&reg, (uint32_t)id, 2.1);

    TEST_ASSERT(rc_d == HFSSS_OK, "set_disturb returns OK");
    TEST_ASSERT(rc_a == HFSSS_OK, "set_aging returns OK");

    struct fault_entry *e = fault_check(&reg, FAULT_READ_DISTURB, &ADDR_1);
    TEST_ASSERT(e != NULL, "entry found after set_disturb/aging");
    TEST_ASSERT(e->disturb_factor == 3.5, "disturb_factor stored");
    TEST_ASSERT(e->aging_factor   == 2.1, "aging_factor stored");

    fault_registry_cleanup(&reg);
}

/* 18. remove by id; subsequent check returns NULL */
static void test_remove_by_id(void)
{
    printf("[18] remove by id\n");
    struct fault_registry reg;
    fault_registry_init(&reg);

    int id = fault_inject_add(&reg, FAULT_PROGRAM_ERROR, &ANY_ADDR,
                               FAULT_PERSIST_STICKY, 1.0);
    TEST_ASSERT(reg.count == 1, "count == 1 before remove");

    int rc = fault_inject_remove(&reg, (uint32_t)id);
    TEST_ASSERT(rc == HFSSS_OK, "remove returns OK");
    TEST_ASSERT(reg.count == 0, "count == 0 after remove");
    TEST_ASSERT((reg.type_present & FAULT_PROGRAM_ERROR) == 0,
                "type_present cleared");

    struct fault_entry *e = fault_check(&reg, FAULT_PROGRAM_ERROR, &ADDR_1);
    TEST_ASSERT(e == NULL, "check after remove returns NULL");

    /* Remove non-existent id. */
    rc = fault_inject_remove(&reg, 9999);
    TEST_ASSERT(rc == HFSSS_ERR_NOENT, "remove unknown id returns HFSSS_ERR_NOENT");

    fault_registry_cleanup(&reg);
}

/* 19. clear_all deactivates all, type_present=0 */
static void test_clear_all(void)
{
    printf("[19] clear_all\n");
    struct fault_registry reg;
    fault_registry_init(&reg);

    fault_inject_add(&reg, FAULT_BAD_BLOCK,     &ANY_ADDR, FAULT_PERSIST_STICKY, 1.0);
    fault_inject_add(&reg, FAULT_READ_ERROR,    &ANY_ADDR, FAULT_PERSIST_STICKY, 1.0);
    fault_inject_add(&reg, FAULT_PROGRAM_ERROR, &ANY_ADDR, FAULT_PERSIST_STICKY, 1.0);

    fault_inject_clear_all(&reg);

    TEST_ASSERT(reg.count == 0,          "count == 0 after clear_all");
    TEST_ASSERT(reg.type_present == 0,   "type_present == 0 after clear_all");
    TEST_ASSERT(fault_check(&reg, FAULT_BAD_BLOCK, &ADDR_1) == NULL,
                "check after clear_all returns NULL");

    fault_registry_cleanup(&reg);
}

/* 20. fault_registry_to_json produces valid JSON array */
static void test_to_json(void)
{
    printf("[20] fault_registry_to_json\n");
    struct fault_registry reg;
    fault_registry_init(&reg);

    /* Empty registry. */
    char buf[4096];
    int rc = fault_registry_to_json(&reg, buf, sizeof(buf));
    TEST_ASSERT(rc == HFSSS_OK, "json from empty registry returns OK");
    TEST_ASSERT(buf[0] == '[' && buf[1] == ']', "empty registry gives []");

    /* Registry with one entry. */
    fault_inject_add(&reg, FAULT_READ_ERROR, &ADDR_1, FAULT_PERSIST_STICKY, 1.0);
    rc = fault_registry_to_json(&reg, buf, sizeof(buf));
    TEST_ASSERT(rc == HFSSS_OK, "json from 1-entry registry returns OK");
    TEST_ASSERT(strstr(buf, "READ_ERROR") != NULL, "type name in JSON");
    TEST_ASSERT(buf[0] == '[', "starts with [");
    TEST_ASSERT(strchr(buf, ']') != NULL, "ends with ]");

    fault_registry_cleanup(&reg);
}

/* 21. NULL safety on all APIs */
static void test_null_safety(void)
{
    printf("[21] NULL safety\n");

    TEST_ASSERT(fault_registry_init(NULL) == HFSSS_ERR_INVAL,
                "init(NULL) returns INVAL");

    /* cleanup(NULL) must not crash */
    fault_registry_cleanup(NULL);
    TEST_ASSERT(true, "cleanup(NULL) does not crash");

    TEST_ASSERT(fault_inject_add(NULL, FAULT_READ_ERROR, &ANY_ADDR,
                                  FAULT_PERSIST_STICKY, 1.0) == HFSSS_ERR_INVAL,
                "add(NULL reg) returns INVAL");

    /* Check on uninitialized reg pointer. */
    TEST_ASSERT(fault_check(NULL, FAULT_READ_ERROR, &ADDR_1) == NULL,
                "check(NULL reg) returns NULL");

    struct fault_registry reg;
    fault_registry_init(&reg);
    fault_inject_add(&reg, FAULT_READ_ERROR, &ANY_ADDR, FAULT_PERSIST_STICKY, 1.0);

    TEST_ASSERT(fault_check(&reg, FAULT_READ_ERROR, NULL) == NULL,
                "check(NULL addr) returns NULL");

    TEST_ASSERT(fault_inject_set_bit_flip(NULL, 1, 0xFFULL) == HFSSS_ERR_INVAL,
                "set_bit_flip(NULL reg) returns INVAL");
    TEST_ASSERT(fault_inject_set_disturb(NULL, 1, 1.0) == HFSSS_ERR_INVAL,
                "set_disturb(NULL reg) returns INVAL");
    TEST_ASSERT(fault_inject_set_aging(NULL, 1, 1.0) == HFSSS_ERR_INVAL,
                "set_aging(NULL reg) returns INVAL");
    TEST_ASSERT(fault_inject_remove(NULL, 1) == HFSSS_ERR_INVAL,
                "remove(NULL reg) returns INVAL");

    /* clear_all(NULL) must not crash */
    fault_inject_clear_all(NULL);
    TEST_ASSERT(true, "clear_all(NULL) does not crash");

    TEST_ASSERT(fault_registry_to_json(NULL, NULL, 0) == HFSSS_ERR_INVAL,
                "to_json(NULL) returns INVAL");

    TEST_ASSERT(fault_entry_from_json(NULL, NULL) == HFSSS_ERR_INVAL,
                "from_json(NULL) returns INVAL");

    /* apply_bit_flip(NULL) must not crash */
    fault_apply_bit_flip(NULL, 0, 0xFFULL);
    TEST_ASSERT(true, "apply_bit_flip(NULL) does not crash");

    /* power marker only — NULL path */
    TEST_ASSERT(fault_power_write_marker_only(NULL) == HFSSS_ERR_INVAL,
                "power_write_marker_only(NULL) returns INVAL");

    fault_registry_cleanup(&reg);
}

/* Extra: power marker write (REQ-133, no _exit) */
static void test_power_marker(void)
{
    printf("[22] power fault marker write\n");

    const char *path = "/tmp/hfsss_test_crash_marker.txt";

    /* Remove stale file if present. */
    remove(path);

    int rc = fault_power_write_marker_only(path);
    TEST_ASSERT(rc == HFSSS_OK, "marker write returns OK");

    FILE *f = fopen(path, "r");
    TEST_ASSERT(f != NULL, "crash marker file created");
    if (f) {
        char line[32] = {0};
        fgets(line, sizeof(line), f);
        fclose(f);
        TEST_ASSERT(strncmp(line, "CRASH", 5) == 0, "marker contains CRASH");
    }
    remove(path);
}

/* Extra: fault_entry_from_json round-trip */
static void test_json_roundtrip(void)
{
    printf("[23] JSON round-trip\n");
    struct fault_registry reg;
    fault_registry_init(&reg);

    fault_inject_add(&reg, FAULT_BIT_FLIP, &ADDR_1, FAULT_PERSIST_STICKY, 1.0);

    char buf[2048];
    int rc = fault_registry_to_json(&reg, buf, sizeof(buf));
    TEST_ASSERT(rc == HFSSS_OK, "to_json succeeds");

    /* The JSON contains one object; extract it. */
    char *start = strchr(buf, '{');
    char *end   = strrchr(buf, '}');
    TEST_ASSERT(start != NULL && end != NULL, "JSON contains an object");

    if (start && end) {
        size_t len = (size_t)(end - start) + 1;
        char obj[2048];
        if (len < sizeof(obj)) {
            memcpy(obj, start, len);
            obj[len] = '\0';

            struct fault_entry out;
            rc = fault_entry_from_json(obj, &out);
            TEST_ASSERT(rc == HFSSS_OK, "from_json returns OK");
            TEST_ASSERT(out.type == FAULT_BIT_FLIP, "type round-trips");
            TEST_ASSERT(out.addr.block == ADDR_1.block, "block addr round-trips");
        }
    }

    fault_registry_cleanup(&reg);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void)
{
    printf("========================================\n");
    printf("test_fault_inject\n");
    printf("========================================\n");

    test_init_cleanup();
    test_add_returns_id();
    test_add_multiple();
    test_registry_full();
    test_check_empty();
    test_check_fast_exit();
    test_check_exact_addr();
    test_check_wildcard_channel();
    test_check_wildcard_block();
    test_check_full_wildcard();
    test_one_shot();
    test_sticky();
    test_prob_zero();
    test_prob_one();
    test_bit_flip_apply();
    test_set_bit_flip();
    test_set_disturb_aging();
    test_remove_by_id();
    test_clear_all();
    test_to_json();
    test_null_safety();
    test_power_marker();
    test_json_roundtrip();

    printf("========================================\n");
    printf("Total: %d PASS, %d FAIL\n", g_pass, g_fail);
    printf("========================================\n");

    return (g_fail == 0) ? 0 : 1;
}
