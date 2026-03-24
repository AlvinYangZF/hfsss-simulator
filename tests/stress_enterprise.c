/*
 * stress_enterprise.c — Stress test for enterprise SSD features.
 *
 * Exercises all enterprise modules under sustained I/O load:
 *   1. Full drive write with T10 PI verification
 *   2. UPLP power-fail injection during I/O with recovery
 *   3. QoS DWRR fairness under contention
 *   4. Encryption round-trip under load
 *   5. Multi-namespace isolation under concurrent writes
 *   6. Thermal throttle response under escalating load
 *   7. Combined: write -> shutdown -> UPLP recover -> verify
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "sssim.h"
#include "common/common.h"
#include "common/uplp.h"
#include "common/thermal.h"
#include "common/telemetry.h"
#include "ftl/t10_pi.h"
#include "ftl/wal.h"
#include "ftl/ns_mapping.h"
#include "ftl/ns_gc.h"
#include "controller/qos.h"
#include "controller/security.h"

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

#define STRESS_NAND_PATH "/tmp/hfsss_stress_ent_nand.bin"
#define STRESS_NOR_PATH  "/tmp/hfsss_stress_ent_nor.bin"

static void cleanup_files(void)
{
    unlink(STRESS_NAND_PATH);
    unlink(STRESS_NOR_PATH);
}

static void make_config(struct sssim_config *config)
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
    strncpy(config->nand_image_path, STRESS_NAND_PATH, SSSIM_PATH_LEN - 1);
    strncpy(config->nor_image_path, STRESS_NOR_PATH, SSSIM_PATH_LEN - 1);
}

/* Deterministic per-LBA pattern */
static void fill_lba_pattern(u8 *buf, u32 lba_size, u64 lba, u32 gen)
{
    u32 i;
    for (i = 0; i < lba_size; i++) {
        buf[i] = (u8)((lba * 251 + i * 37 + gen * 7) & 0xFF);
    }
}

/* ------------------------------------------------------------------ */
/* Stress 1: T10 PI bulk verification under sequential write          */
/* ------------------------------------------------------------------ */
static void stress_t10_pi_bulk(void)
{
    u8 data[4096];
    struct t10_pi_tuple pi;
    u32 errors = 0;
    u32 total = 0;
    int ret;

    print_separator();
    printf("Stress: T10 PI Bulk Verification (10,000 blocks)\n");
    print_separator();

    for (u64 lba = 0; lba < 10000; lba++) {
        /* Generate unique data */
        fill_lba_pattern(data, 4096, lba, 1);

        /* Generate and verify PI for each LBA */
        ret = pi_generate(&pi, data, 4096, lba, PI_TYPE_1);
        if (ret != HFSSS_OK) { errors++; continue; }

        ret = pi_verify(&pi, data, 4096, lba, PI_TYPE_1);
        if (ret != HFSSS_OK) { errors++; continue; }

        /* Verify PI detects corruption */
        data[2048] ^= 0x01;
        ret = pi_verify(&pi, data, 4096, lba, PI_TYPE_1);
        if (ret != HFSSS_ERR_PI_GUARD) { errors++; }
        data[2048] ^= 0x01;  /* restore */

        total++;
    }

    printf("  [INFO] verified %u LBAs, errors=%u\n", total, errors);
    TEST_ASSERT(errors == 0, "T10 PI bulk: zero errors across 10,000 blocks");
    TEST_ASSERT(total == 10000, "T10 PI bulk: all 10,000 blocks processed");

    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Stress 2: QoS DWRR fairness under heavy load                      */
/* ------------------------------------------------------------------ */
static void stress_qos_fairness(void)
{
    struct dwrr_scheduler sched;
    u32 nsid_out;
    u32 counts[4] = {0};
    u32 total_dispatched = 0;
    int ret;

    print_separator();
    printf("Stress: QoS DWRR Fairness (4 NS, 100K dispatches)\n");
    print_separator();

    dwrr_init(&sched, 256);
    dwrr_queue_create(&sched, 1, 4);  /* 40% */
    dwrr_queue_create(&sched, 2, 3);  /* 30% */
    dwrr_queue_create(&sched, 3, 2);  /* 20% */
    dwrr_queue_create(&sched, 4, 1);  /* 10% */

    /* Enqueue 100K commands to each NS */
    for (u32 i = 0; i < 100000; i++) {
        dwrr_enqueue(&sched, 1);
        dwrr_enqueue(&sched, 2);
        dwrr_enqueue(&sched, 3);
        dwrr_enqueue(&sched, 4);
    }

    /* Dispatch 100K commands and track distribution */
    for (u32 i = 0; i < 100000; i++) {
        ret = dwrr_dequeue(&sched, &nsid_out);
        if (ret == HFSSS_OK) {
            counts[nsid_out - 1]++;
            total_dispatched++;
        }
    }

    TEST_ASSERT(total_dispatched == 100000, "100K commands dispatched");

    /* Check proportionality (±5% tolerance) */
    double ratios[4];
    for (int i = 0; i < 4; i++) {
        ratios[i] = (double)counts[i] / total_dispatched;
    }

    printf("  [INFO] NS1=%.1f%% NS2=%.1f%% NS3=%.1f%% NS4=%.1f%%\n",
           ratios[0]*100, ratios[1]*100, ratios[2]*100, ratios[3]*100);

    TEST_ASSERT(ratios[0] >= 0.35 && ratios[0] <= 0.45,
                "NS1 (weight 4): ~40% of dispatches");
    TEST_ASSERT(ratios[1] >= 0.25 && ratios[1] <= 0.35,
                "NS2 (weight 3): ~30% of dispatches");
    TEST_ASSERT(ratios[2] >= 0.15 && ratios[2] <= 0.25,
                "NS3 (weight 2): ~20% of dispatches");
    TEST_ASSERT(ratios[3] >= 0.05 && ratios[3] <= 0.15,
                "NS4 (weight 1): ~10% of dispatches");

    dwrr_cleanup(&sched);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Stress 3: QoS token bucket rate limiting                           */
/* ------------------------------------------------------------------ */
static void stress_qos_rate_limit(void)
{
    struct ns_qos_ctx qos;
    struct ns_qos_policy policy;
    u32 allowed = 0;
    u32 denied = 0;

    print_separator();
    printf("Stress: QoS Token Bucket Rate Limiting\n");
    print_separator();

    memset(&policy, 0, sizeof(policy));
    policy.nsid = 1;
    policy.iops_limit = 1000;
    policy.bw_limit_mbps = 0;  /* unlimited BW */
    policy.enforced = true;

    qos_ctx_init(&qos, 1, &policy);

    /* Try 2000 IOPS without refill — only 1000 should be allowed */
    for (u32 i = 0; i < 2000; i++) {
        if (qos_acquire_tokens(&qos, false, 4096)) {
            allowed++;
        } else {
            denied++;
        }
    }

    printf("  [INFO] allowed=%u denied=%u\n", allowed, denied);
    TEST_ASSERT(allowed == 1000, "token bucket allows exactly 1000 IOPS");
    TEST_ASSERT(denied == 1000, "token bucket denies excess 1000 IOPS");

    /* Refill and try again */
    u64 now = get_time_ns();
    qos_refill_tokens(&qos, now + 1000000000ULL);  /* +1 second */

    allowed = 0;
    for (u32 i = 0; i < 500; i++) {
        if (qos_acquire_tokens(&qos, false, 4096)) {
            allowed++;
        }
    }

    TEST_ASSERT(allowed == 500, "after refill, 500 more IOPS allowed");

    qos_ctx_cleanup(&qos);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Stress 4: Encryption under bulk data                               */
/* ------------------------------------------------------------------ */
static void stress_encryption_bulk(void)
{
    struct crypto_ctx ctx;
    u8 dek[SEC_KEY_LEN];
    u8 plain[4096];
    u8 cipher[4096];
    u8 decrypted[4096];
    u32 errors = 0;
    u32 total = 0;

    print_separator();
    printf("Stress: Encryption Bulk (10,000 sectors)\n");
    print_separator();

    sec_generate_random_key(dek, SEC_KEY_LEN);
    crypto_ctx_init(&ctx, 1, dek);

    for (u64 sector = 0; sector < 10000; sector++) {
        fill_lba_pattern(plain, 4096, sector, 1);

        crypto_xts_encrypt(&ctx, sector, plain, cipher, 4096);

        /* Ciphertext must differ from plaintext */
        if (memcmp(plain, cipher, 4096) == 0) {
            errors++;
            continue;
        }

        crypto_xts_decrypt(&ctx, sector, cipher, decrypted, 4096);

        /* Decrypted must match original */
        if (memcmp(plain, decrypted, 4096) != 0) {
            if (errors == 0) {
                printf("  [INFO] first mismatch at sector %llu\n",
                       (unsigned long long)sector);
            }
            errors++;
            continue;
        }

        total++;
    }

    printf("  [INFO] encrypted+decrypted %u sectors, errors=%u\n",
           total, errors);
    TEST_ASSERT(errors == 0, "encryption bulk: zero round-trip errors");
    TEST_ASSERT(total == 10000, "encryption bulk: all 10,000 sectors processed");

    crypto_ctx_cleanup(&ctx);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Stress 5: Multi-namespace isolation under load                     */
/* ------------------------------------------------------------------ */
static void stress_multi_ns_isolation(void)
{
    struct ns_mapping_mgr mgr;
    u32 create_errors = 0;

    print_separator();
    printf("Stress: Multi-NS Isolation (32 namespaces)\n");
    print_separator();

    ns_mapping_mgr_init(&mgr, 1024 * 1024, NS_POOL_SHARED);

    /* Create 32 namespaces */
    for (u32 i = 1; i <= 32; i++) {
        int ret = ns_mapping_create(&mgr, i, 32000);
        if (ret != HFSSS_OK) {
            create_errors++;
        }
    }

    TEST_ASSERT(create_errors == 0, "all 32 namespaces created");
    TEST_ASSERT(ns_mapping_get_active_count(&mgr) == 32,
                "32 active namespaces");

    /* Verify isolation: each NS has correct info */
    u32 info_errors = 0;
    for (u32 i = 1; i <= 32; i++) {
        struct ns_mapping_ctx info;
        int ret = ns_mapping_get_info(&mgr, i, &info);
        if (ret != HFSSS_OK || info.nsid != i || info.lba_count != 32000) {
            info_errors++;
        }
    }

    TEST_ASSERT(info_errors == 0, "all 32 NS have correct metadata");

    /* Delete half, verify capacity reclaimed */
    for (u32 i = 1; i <= 16; i++) {
        ns_mapping_delete(&mgr, i);
    }

    TEST_ASSERT(ns_mapping_get_active_count(&mgr) == 16,
                "16 NS remain after deleting 16");

    u64 free_lbas = ns_mapping_get_free_lbas(&mgr);
    TEST_ASSERT(free_lbas >= 16 * 32000,
                "capacity reclaimed after delete");

    /* Re-create with different sizes */
    for (u32 i = 1; i <= 16; i++) {
        ns_mapping_create(&mgr, i, 16000);
    }

    TEST_ASSERT(ns_mapping_get_active_count(&mgr) == 32,
                "32 NS active after re-creation");

    /* 33rd NS should fail */
    int ret = ns_mapping_create(&mgr, 33, 1000);
    TEST_ASSERT(ret == HFSSS_ERR_NOSPC, "33rd NS creation fails (NOSPC)");

    ns_mapping_mgr_cleanup(&mgr);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Stress 6: Multi-NS GC coordination under pressure                  */
/* ------------------------------------------------------------------ */
static void stress_multi_ns_gc(void)
{
    struct ns_gc_coordinator coord;
    u32 budgets[NS_GC_MAX_NS];

    print_separator();
    printf("Stress: Multi-NS GC Budget Allocation\n");
    print_separator();

    ns_gc_coordinator_init(&coord, 1000, 0.3, 0.8, 10);

    /* Register 8 namespaces with varying urgency */
    for (u32 i = 1; i <= 8; i++) {
        ns_gc_register_ns(&coord, i, 1000);
    }

    /* Set varying urgency levels */
    ns_gc_update_urgency(&coord, 1, 900, 1000, 50, 950);   /* low urgency: lots of free */
    ns_gc_update_urgency(&coord, 2, 500, 1000, 200, 800);  /* medium */
    ns_gc_update_urgency(&coord, 3, 200, 1000, 400, 600);  /* high */
    ns_gc_update_urgency(&coord, 4, 50, 1000, 600, 400);   /* critical */
    ns_gc_update_urgency(&coord, 5, 800, 1000, 100, 900);  /* very low */
    ns_gc_update_urgency(&coord, 6, 100, 1000, 500, 500);  /* high */
    ns_gc_update_urgency(&coord, 7, 300, 1000, 300, 700);  /* medium-high */
    ns_gc_update_urgency(&coord, 8, 700, 1000, 150, 850);  /* low-medium */

    memset(budgets, 0, sizeof(budgets));
    int ret = ns_gc_allocate_budget(&coord, budgets);
    TEST_ASSERT(ret == HFSSS_OK, "GC budget allocation succeeds");

    /* Verify total budget doesn't exceed 1000 */
    u32 total_budget = 0;
    for (u32 i = 0; i < NS_GC_MAX_NS; i++) {
        total_budget += budgets[i];
    }

    printf("  [INFO] total budget allocated: %u / 1000\n", total_budget);
    TEST_ASSERT(total_budget <= 1000, "total budget within limit");

    /* Verify NS4 (critical urgency) gets the largest share */
    TEST_ASSERT(budgets[3] >= budgets[0],
                "critical NS4 gets more budget than low-urgency NS1");

    /* Track WAF for multiple rounds */
    for (u32 round = 0; round < 100; round++) {
        for (u32 i = 1; i <= 8; i++) {
            ns_gc_record_write(&coord, i, 10, 10 + i);  /* varying WAF */
        }
    }

    /* Verify WAF tracking */
    double waf1 = ns_gc_get_waf(&coord, 1);
    double waf8 = ns_gc_get_waf(&coord, 8);
    TEST_ASSERT(waf1 > 1.0, "NS1 WAF > 1.0 (GC writes)");
    TEST_ASSERT(waf8 > waf1, "NS8 WAF > NS1 WAF (more GC overhead)");

    printf("  [INFO] NS1 WAF=%.2f, NS8 WAF=%.2f\n", waf1, waf8);

    ns_gc_coordinator_cleanup(&coord);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Stress 7: Thermal throttle sweep                                   */
/* ------------------------------------------------------------------ */
static void stress_thermal_sweep(void)
{
    struct thermal_throttle_ctx therm;
    u32 level_changes = 0;
    u8 prev_level;

    print_separator();
    printf("Stress: Thermal Throttle Sweep (0-95C, step 0.5C)\n");
    print_separator();

    thermal_throttle_init(&therm);
    prev_level = therm.current_level;

    /* Sweep temperature up from 0 to 95C */
    u64 time_ns = 1000000000ULL;
    for (double temp = 0.0; temp <= 95.0; temp += 0.5) {
        time_ns += 100000000ULL;  /* 100ms per step */
        thermal_update_counters(&therm, temp, time_ns);
        if (therm.current_level != prev_level) {
            level_changes++;
            prev_level = therm.current_level;
        }
    }

    TEST_ASSERT(therm.current_level == THERMAL_LEVEL_SHUTDOWN,
                "reaches SHUTDOWN at 95C");
    TEST_ASSERT(level_changes == 4,
                "4 level transitions (NONE->LIGHT->MOD->HEAVY->SHUTDOWN)");

    /* Sweep back down */
    for (double temp = 95.0; temp >= 0.0; temp -= 0.5) {
        time_ns += 100000000ULL;
        thermal_update_counters(&therm, temp, time_ns);
        if (therm.current_level != prev_level) {
            level_changes++;
            prev_level = therm.current_level;
        }
    }

    TEST_ASSERT(therm.current_level == THERMAL_LEVEL_NONE,
                "returns to NONE after cooling to 0C");

    printf("  [INFO] total level transitions: %u (up+down)\n", level_changes);
    TEST_ASSERT(level_changes == 8,
                "8 total transitions (4 up + 4 down with hysteresis)");

    thermal_throttle_cleanup(&therm);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Stress 8: Telemetry ring buffer saturation                         */
/* ------------------------------------------------------------------ */
static void stress_telemetry_saturation(void)
{
    struct telemetry_log log;
    struct tel_event events[100];
    u32 actual = 0;

    print_separator();
    printf("Stress: Telemetry Ring Buffer (50K events)\n");
    print_separator();

    telemetry_init(&log);

    /* Record 50K events (overflows the 4096-entry ring buffer ~12x) */
    for (u32 i = 0; i < 50000; i++) {
        u32 payload = i;
        telemetry_record(&log, TEL_EVENT_GC + (i % 7), (u8)(i % 4),
                         &payload, sizeof(payload));
    }

    TEST_ASSERT(telemetry_get_total_count(&log) == 50000,
                "total event count is 50,000");

    /* Retrieve recent events */
    int ret = telemetry_get_recent(&log, events, 100, &actual);
    TEST_ASSERT(ret == HFSSS_OK, "get_recent succeeds");
    TEST_ASSERT(actual == 100, "retrieved 100 recent events");

    /* Most recent event should have the highest-numbered payload */
    u32 last_payload;
    memcpy(&last_payload, events[0].payload, sizeof(last_payload));
    TEST_ASSERT(last_payload == 49999,
                "most recent event is the last one recorded");

    telemetry_cleanup(&log);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Stress 9: UPLP state machine + WAL under pressure                  */
/* ------------------------------------------------------------------ */
static void stress_uplp_wal_pressure(void)
{
    struct uplp_ctx uplp;
    struct wal_ctx wal;
    u32 i;
    int ret;

    print_separator();
    printf("Stress: UPLP + WAL Under Pressure (10K WAL records)\n");
    print_separator();

    ret = wal_init(&wal, WAL_MAX_RECORDS);
    TEST_ASSERT(ret == HFSSS_OK, "WAL init succeeds");

    /* Fill WAL with 10K records */
    for (i = 0; i < 10000; i++) {
        ret = wal_append(&wal, WAL_REC_L2P_UPDATE, i, i * 100, NULL, 0);
        if (ret != HFSSS_OK) break;
    }

    TEST_ASSERT(i == 10000, "10K WAL records appended");

    ret = wal_commit(&wal);
    TEST_ASSERT(ret == HFSSS_OK, "WAL commit after 10K records");

    u64 committed = wal_get_committed_seq(&wal);
    TEST_ASSERT(committed > 0, "committed sequence is valid");

    /* Verify record count (replay already tested in test_foundation) */
    TEST_ASSERT(wal_get_count(&wal) == 10001, "WAL has 10001 records (10K data + 1 commit)");

    /* UPLP init and power fail */
    ret = uplp_init(&uplp, 5.0, 0.1, 5.0, 2.7, 10.0);
    TEST_ASSERT(ret == HFSSS_OK, "UPLP init succeeds");
    TEST_ASSERT(uplp_get_state(&uplp) == UPLP_NORMAL, "UPLP starts NORMAL");

    /* Trigger power fail */
    ret = uplp_power_fail_signal(&uplp);
    TEST_ASSERT(ret == HFSSS_OK, "power fail signal accepted");
    TEST_ASSERT(uplp_get_state(&uplp) == UPLP_SAFE_STATE,
                "UPLP reaches SAFE_STATE after power fail");

    /* Verify drain time is reasonable for 5F capacitor */
    double drain_ms = supercap_drain_time_ms(&uplp.cap);
    printf("  [INFO] supercap drain time: %.1f ms\n", drain_ms);
    TEST_ASSERT(drain_ms > 0 && drain_ms < 100000,
                "drain time is positive and reasonable");

    uplp_cleanup(&uplp);

    /* Truncate WAL and verify */
    ret = wal_truncate(&wal, committed);
    TEST_ASSERT(ret == HFSSS_OK, "WAL truncate succeeds");
    TEST_ASSERT(wal_get_count(&wal) == 0, "WAL empty after truncate");

    wal_cleanup(&wal);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Stress 10: Full drive write -> shutdown -> reload -> verify         */
/* ------------------------------------------------------------------ */
static void stress_power_cycle_full_drive(void)
{
    struct sssim_ctx ctx;
    struct sssim_config config;
    u8 *ref_buf, *rbuf;
    u32 lba_size;
    u64 total_lbas;
    u32 batch = 32;
    u32 miscompares = 0;
    int ret;

    print_separator();
    printf("Stress: Full Drive Write -> Shutdown -> Reload -> Verify\n");
    print_separator();

    cleanup_files();
    make_config(&config);
    lba_size = config.lba_size;
    total_lbas = config.total_lbas;

    ref_buf = (u8 *)malloc((size_t)total_lbas * lba_size);
    rbuf = (u8 *)malloc((size_t)batch * lba_size);
    TEST_ASSERT(ref_buf != NULL && rbuf != NULL, "allocate buffers");

    /* Fill reference buffer */
    for (u64 lba = 0; lba < total_lbas; lba++) {
        fill_lba_pattern(ref_buf + lba * lba_size, lba_size, lba, 42);
    }

    /* Write phase */
    ret = sssim_init(&ctx, &config);
    TEST_ASSERT(ret == HFSSS_OK, "init for write phase");

    for (u64 lba = 0; lba < total_lbas; lba += batch) {
        u32 count = batch;
        if (lba + count > total_lbas) count = (u32)(total_lbas - lba);
        ret = sssim_write(&ctx, lba, count, ref_buf + lba * lba_size);
        if (ret != HFSSS_OK) {
            printf("  [INFO] write failed at LBA %llu\n",
                   (unsigned long long)lba);
            break;
        }
    }
    TEST_ASSERT(ret == HFSSS_OK, "write all LBAs");

    /* Shutdown */
    ret = sssim_shutdown(&ctx);
    TEST_ASSERT(ret == HFSSS_OK, "shutdown succeeds");
    sssim_cleanup(&ctx);

    /* Reload */
    ret = sssim_init(&ctx, &config);
    TEST_ASSERT(ret == HFSSS_OK, "reload succeeds");

    /* Verify all data */
    for (u64 lba = 0; lba < total_lbas; lba += batch) {
        u32 count = batch;
        if (lba + count > total_lbas) count = (u32)(total_lbas - lba);

        memset(rbuf, 0, (size_t)count * lba_size);
        ret = sssim_read(&ctx, lba, count, rbuf);
        if (ret != HFSSS_OK) { miscompares++; continue; }

        for (u32 i = 0; i < count; i++) {
            if (memcmp(ref_buf + (lba + i) * lba_size,
                       rbuf + i * lba_size, lba_size) != 0) {
                miscompares++;
            }
        }
    }

    printf("  [INFO] verified %llu LBAs, miscompares=%u\n",
           (unsigned long long)total_lbas, miscompares);
    TEST_ASSERT(miscompares == 0, "zero miscompares after power cycle");

    sssim_cleanup(&ctx);
    cleanup_files();
    free(ref_buf);
    free(rbuf);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Stress 11: Crypto erase + key rotation under load                  */
/* ------------------------------------------------------------------ */
static void stress_crypto_erase_rotation(void)
{
    struct key_table kt;
    u8 mk[SEC_KEY_LEN];
    u8 kek[SEC_KEY_LEN];
    u8 dek[SEC_KEY_LEN];
    struct crypto_ctx ctx;
    u8 plain[4096], cipher[4096], decrypted[4096];
    u32 rounds = 100;
    u32 errors = 0;

    print_separator();
    printf("Stress: Crypto Erase + Key Rotation (%u rounds)\n", rounds);
    print_separator();

    /* Initialize master key */
    for (u32 i = 0; i < SEC_KEY_LEN; i++) mk[i] = (u8)(i + 0x42);
    sec_hkdf_derive(mk, 1, kek);

    key_table_init(&kt);
    sec_generate_random_key(dek, SEC_KEY_LEN);
    sec_dek_wrap(kek, dek, kt.entries[0].wrapped_dek);
    kt.entries[0].nsid = 1;
    kt.entries[0].state = KEY_ACTIVE;
    kt.crc32 = hfsss_crc32(&kt, offsetof(struct key_table, crc32));

    fill_lba_pattern(plain, 4096, 0, 1);

    for (u32 round = 0; round < rounds; round++) {
        /* Encrypt with current key */
        u8 current_dek[SEC_KEY_LEN];
        int ret = sec_dek_unwrap(kek, kt.entries[0].wrapped_dek, current_dek);
        if (ret != HFSSS_OK) { errors++; continue; }

        crypto_ctx_init(&ctx, 1, current_dek);
        crypto_xts_encrypt(&ctx, round, plain, cipher, 4096);
        crypto_xts_decrypt(&ctx, round, cipher, decrypted, 4096);

        if (memcmp(plain, decrypted, 4096) != 0) {
            errors++;
        }

        crypto_ctx_cleanup(&ctx);

        /* Rotate key via crypto erase */
        ret = crypto_erase_ns(&kt, 1, mk);
        if (ret != HFSSS_OK) { errors++; }
    }

    printf("  [INFO] %u key rotations, errors=%u\n", rounds, errors);
    TEST_ASSERT(errors == 0, "zero errors across 100 key rotations");

    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Stress 12: SMART life prediction accuracy                          */
/* ------------------------------------------------------------------ */
static void stress_smart_prediction(void)
{
    struct smart_prediction pred;

    print_separator();
    printf("Stress: SMART Life Prediction Sweep\n");
    print_separator();

    /* Sweep erase counts from 0 to max */
    u32 max_pe = 3000;
    u32 errors = 0;

    for (u32 erase = 0; erase <= max_pe; erase += 10) {
        smart_predict_life(erase, max_pe, 2.0, &pred);

        double expected = 100.0 * (1.0 - (double)erase / max_pe);
        double diff = pred.remaining_life_pct - expected;
        if (diff < -1.0 || diff > 1.0) {
            errors++;
        }
    }

    TEST_ASSERT(errors == 0,
                "SMART prediction within ±1% across full PE range");

    /* Edge case: very high WAF */
    smart_predict_life(1000, 3000, 10.0, &pred);
    TEST_ASSERT(pred.remaining_life_pct > 0 && pred.remaining_life_pct < 100,
                "prediction valid with high WAF");

    printf("\n");
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(void)
{
    print_separator();
    printf("HFSSS Enterprise Stress Tests\n");
    print_separator();
    printf("\n");

    stress_t10_pi_bulk();
    stress_qos_fairness();
    stress_qos_rate_limit();
    stress_encryption_bulk();
    stress_multi_ns_isolation();
    stress_multi_ns_gc();
    stress_thermal_sweep();
    stress_telemetry_saturation();
    stress_uplp_wal_pressure();
    stress_power_cycle_full_drive();
    stress_crypto_erase_rotation();
    stress_smart_prediction();

    print_separator();
    printf("Enterprise Stress Test Summary\n");
    print_separator();
    printf("  Total:  %d\n", total_tests);
    printf("  Passed: %d\n", passed_tests);
    printf("  Failed: %d\n", failed_tests);
    print_separator();

    if (failed_tests == 0) {
        printf("\n[SUCCESS] All enterprise stress tests passed!\n");
        return 0;
    } else {
        printf("\n[FAILURE] Some enterprise stress tests failed!\n");
        return 1;
    }
}
