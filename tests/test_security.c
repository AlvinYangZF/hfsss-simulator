/*
 * HFSSS Security and Encryption Module Tests
 *
 * Covers: AES-XTS simulation, key hierarchy, DEK wrap/unwrap,
 * key table persistence, crypto erase, secure boot verification.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "common/common.h"
#include "controller/security.h"
#include "media/nor_flash.h"

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

/* ----------------------------------------------------------------
 * Encrypt / Decrypt Round-Trip
 * ---------------------------------------------------------------- */
static void test_crypto_roundtrip(void)
{
    struct crypto_ctx ctx;
    u8 dek[SEC_KEY_LEN];
    u8 plain[512];
    u8 cipher[512];
    u8 decrypted[512];
    int ret;

    print_separator();
    printf("Test: Crypto Encrypt/Decrypt Round-Trip\n");
    print_separator();

    /* Fill DEK and plaintext with known patterns */
    for (u32 i = 0; i < SEC_KEY_LEN; i++) {
        dek[i] = (u8)(i * 7 + 3);
    }
    for (u32 i = 0; i < 512; i++) {
        plain[i] = (u8)(i & 0xFF);
    }

    ret = crypto_ctx_init(&ctx, 1, dek);
    TEST_ASSERT(ret == HFSSS_OK, "crypto_ctx_init succeeds");
    TEST_ASSERT(ctx.active == true, "crypto context is active after init");

    crypto_xts_encrypt(&ctx, 0, plain, cipher, 512);
    crypto_xts_decrypt(&ctx, 0, cipher, decrypted, 512);

    TEST_ASSERT(memcmp(plain, decrypted, 512) == 0,
                "decrypt(encrypt(plain)) == plain");

    crypto_ctx_cleanup(&ctx);
    TEST_ASSERT(ctx.active == false, "crypto context inactive after cleanup");

    printf("\n");
}

/* ----------------------------------------------------------------
 * Different Sectors Produce Different Ciphertext
 * ---------------------------------------------------------------- */
static void test_crypto_different_sectors(void)
{
    struct crypto_ctx ctx;
    u8 dek[SEC_KEY_LEN];
    u8 plain[256];
    u8 cipher_s0[256];
    u8 cipher_s1[256];

    print_separator();
    printf("Test: Different Sectors Produce Different Ciphertext\n");
    print_separator();

    for (u32 i = 0; i < SEC_KEY_LEN; i++) {
        dek[i] = (u8)(i + 0x10);
    }
    memset(plain, 0x42, 256);

    crypto_ctx_init(&ctx, 1, dek);

    crypto_xts_encrypt(&ctx, 0, plain, cipher_s0, 256);
    crypto_xts_encrypt(&ctx, 1, plain, cipher_s1, 256);

    TEST_ASSERT(memcmp(cipher_s0, cipher_s1, 256) != 0,
                "same plaintext at different sectors produces different ciphertext");

    /* Both should still decrypt correctly */
    u8 dec0[256], dec1[256];
    crypto_xts_decrypt(&ctx, 0, cipher_s0, dec0, 256);
    crypto_xts_decrypt(&ctx, 1, cipher_s1, dec1, 256);

    TEST_ASSERT(memcmp(plain, dec0, 256) == 0,
                "sector 0 decrypts correctly");
    TEST_ASSERT(memcmp(plain, dec1, 256) == 0,
                "sector 1 decrypts correctly");

    crypto_ctx_cleanup(&ctx);
    printf("\n");
}

/* ----------------------------------------------------------------
 * Ciphertext Differs From Plaintext
 * ---------------------------------------------------------------- */
static void test_crypto_transforms_data(void)
{
    struct crypto_ctx ctx;
    u8 dek[SEC_KEY_LEN];
    u8 plain[128];
    u8 cipher[128];

    print_separator();
    printf("Test: Encryption Transforms Data\n");
    print_separator();

    for (u32 i = 0; i < SEC_KEY_LEN; i++) {
        dek[i] = (u8)(i + 1);
    }
    for (u32 i = 0; i < 128; i++) {
        plain[i] = (u8)(i);
    }

    crypto_ctx_init(&ctx, 1, dek);
    crypto_xts_encrypt(&ctx, 5, plain, cipher, 128);

    TEST_ASSERT(memcmp(plain, cipher, 128) != 0,
                "ciphertext differs from plaintext");

    crypto_ctx_cleanup(&ctx);
    printf("\n");
}

/* ----------------------------------------------------------------
 * Key Generation
 * ---------------------------------------------------------------- */
static void test_key_generation(void)
{
    u8 key1[SEC_KEY_LEN];
    u8 key2[SEC_KEY_LEN];
    u8 zero[SEC_KEY_LEN];
    bool all_same;

    print_separator();
    printf("Test: Key Generation\n");
    print_separator();

    memset(zero, 0, SEC_KEY_LEN);

    sec_generate_random_key(key1, SEC_KEY_LEN);
    TEST_ASSERT(memcmp(key1, zero, SEC_KEY_LEN) != 0,
                "generated key is non-zero");

    sec_generate_random_key(key2, SEC_KEY_LEN);

    /* Two random keys should differ (extremely high probability) */
    all_same = (memcmp(key1, key2, SEC_KEY_LEN) == 0);
    TEST_ASSERT(!all_same,
                "two independently generated keys differ");

    printf("\n");
}

/* ----------------------------------------------------------------
 * HKDF Derivation
 * ---------------------------------------------------------------- */
static void test_hkdf_derivation(void)
{
    u8 mk[SEC_KEY_LEN];
    u8 kek1[SEC_KEY_LEN];
    u8 kek2[SEC_KEY_LEN];
    u8 kek1b[SEC_KEY_LEN];

    print_separator();
    printf("Test: HKDF Key Derivation\n");
    print_separator();

    for (u32 i = 0; i < SEC_KEY_LEN; i++) {
        mk[i] = (u8)(i * 3 + 0x55);
    }

    sec_hkdf_derive(mk, 1, kek1);
    sec_hkdf_derive(mk, 2, kek2);
    sec_hkdf_derive(mk, 1, kek1b);

    TEST_ASSERT(memcmp(kek1, kek2, SEC_KEY_LEN) != 0,
                "different NSID produces different KEK");
    TEST_ASSERT(memcmp(kek1, kek1b, SEC_KEY_LEN) == 0,
                "same NSID produces same KEK (deterministic)");

    printf("\n");
}

/* ----------------------------------------------------------------
 * DEK Wrap / Unwrap Round-Trip
 * ---------------------------------------------------------------- */
static void test_dek_wrap_unwrap(void)
{
    u8 kek[SEC_KEY_LEN];
    u8 dek[SEC_KEY_LEN];
    u8 wrapped[SEC_WRAPPED_LEN];
    u8 unwrapped[SEC_KEY_LEN];
    int ret;

    print_separator();
    printf("Test: DEK Wrap/Unwrap Round-Trip\n");
    print_separator();

    for (u32 i = 0; i < SEC_KEY_LEN; i++) {
        kek[i] = (u8)(i + 0x20);
        dek[i] = (u8)(i * 5 + 0x11);
    }

    sec_dek_wrap(kek, dek, wrapped);

    ret = sec_dek_unwrap(kek, wrapped, unwrapped);
    TEST_ASSERT(ret == HFSSS_OK, "DEK unwrap with correct KEK succeeds");
    TEST_ASSERT(memcmp(dek, unwrapped, SEC_KEY_LEN) == 0,
                "unwrapped DEK matches original");

    printf("\n");
}

/* ----------------------------------------------------------------
 * DEK Unwrap With Wrong KEK
 * ---------------------------------------------------------------- */
static void test_dek_unwrap_wrong_kek(void)
{
    u8 kek[SEC_KEY_LEN];
    u8 wrong_kek[SEC_KEY_LEN];
    u8 dek[SEC_KEY_LEN];
    u8 wrapped[SEC_WRAPPED_LEN];
    u8 unwrapped[SEC_KEY_LEN];
    int ret;

    print_separator();
    printf("Test: DEK Unwrap With Wrong KEK\n");
    print_separator();

    for (u32 i = 0; i < SEC_KEY_LEN; i++) {
        kek[i] = (u8)(i + 0x20);
        wrong_kek[i] = (u8)(i + 0x30);
        dek[i] = (u8)(i * 5 + 0x11);
    }

    sec_dek_wrap(kek, dek, wrapped);

    ret = sec_dek_unwrap(wrong_kek, wrapped, unwrapped);
    TEST_ASSERT(ret == HFSSS_ERR_AUTH,
                "DEK unwrap with wrong KEK returns HFSSS_ERR_AUTH");

    printf("\n");
}

/* ----------------------------------------------------------------
 * Key Table Init / Save / Load Round-Trip
 * ---------------------------------------------------------------- */
/* Init happy path + magic/version sanity. Round-trip and corruption
 * coverage live in the NOR-backed tests further down, where the
 * canonical save/load APIs are exercised against real NOR state. */
static void test_key_table_init_fields(void)
{
    struct key_table kt_orig;
    int ret;

    print_separator();
    printf("Test: Key Table Init Fields\n");
    print_separator();

    ret = key_table_init(&kt_orig);
    TEST_ASSERT(ret == HFSSS_OK, "key_table_init succeeds");
    TEST_ASSERT(kt_orig.magic == SEC_KEY_MAGIC, "key table magic is correct");
    TEST_ASSERT(kt_orig.version == 1, "key table version is 1");
    for (u32 i = 0; i < SEC_MAX_NS; i++) {
        TEST_ASSERT(kt_orig.entries[i].state == KEY_EMPTY,
                    "entry initialized as KEY_EMPTY");
        TEST_ASSERT(kt_orig.entries[i].nsid == 0,
                    "entry nsid zeroed");
    }
    u32 expected_crc = hfsss_crc32(&kt_orig,
                                   offsetof(struct key_table, crc32));
    TEST_ASSERT(kt_orig.crc32 == expected_crc,
                "body CRC covers everything except crc32 field");
    printf("\n");
}

/* ----------------------------------------------------------------
 * Crypto Erase: Old DEK Destroyed, New DEK Generated
 * ---------------------------------------------------------------- */
static void test_crypto_erase(void)
{
    struct key_table kt;
    u8 mk[SEC_KEY_LEN];
    u8 kek[SEC_KEY_LEN];
    u8 dek[SEC_KEY_LEN];
    u8 old_wrapped[SEC_WRAPPED_LEN];
    int ret;

    print_separator();
    printf("Test: Crypto Erase\n");
    print_separator();

    /* Set up master key */
    for (u32 i = 0; i < SEC_KEY_LEN; i++) {
        mk[i] = (u8)(i + 0x80);
    }

    key_table_init(&kt);

    /* Manually create an active entry for NSID=1 */
    sec_generate_random_key(dek, SEC_KEY_LEN);
    sec_hkdf_derive(mk, 1, kek);
    sec_dek_wrap(kek, dek, kt.entries[0].wrapped_dek);
    kt.entries[0].nsid = 1;
    kt.entries[0].state = KEY_ACTIVE;
    kt.crc32 = hfsss_crc32(&kt, offsetof(struct key_table, crc32));

    /* Save old wrapped DEK for comparison */
    memcpy(old_wrapped, kt.entries[0].wrapped_dek, SEC_WRAPPED_LEN);

    ret = crypto_erase_ns(&kt, 1, mk);
    TEST_ASSERT(ret == HFSSS_OK, "crypto_erase_ns succeeds");
    TEST_ASSERT(kt.entries[0].state == KEY_ACTIVE,
                "entry state is KEY_ACTIVE after erase (new key)");
    TEST_ASSERT(memcmp(old_wrapped, kt.entries[0].wrapped_dek,
                       SEC_WRAPPED_LEN) != 0,
                "wrapped DEK changed after crypto erase");

    /* Verify the new wrapped DEK is valid (can unwrap) */
    u8 new_dek[SEC_KEY_LEN];
    ret = sec_dek_unwrap(kek, kt.entries[0].wrapped_dek, new_dek);
    TEST_ASSERT(ret == HFSSS_OK,
                "new wrapped DEK can be unwrapped with correct KEK");

    memset(kek, 0, SEC_KEY_LEN);
    memset(new_dek, 0, SEC_KEY_LEN);
    printf("\n");
}

/* ----------------------------------------------------------------
 * Crypto Erase: Decrypt With Old Key Produces Wrong Data
 * ---------------------------------------------------------------- */
static void test_crypto_erase_data_loss(void)
{
    struct key_table kt;
    struct crypto_ctx ctx_old, ctx_new;
    u8 mk[SEC_KEY_LEN];
    u8 kek[SEC_KEY_LEN];
    u8 dek_old[SEC_KEY_LEN];
    u8 dek_new[SEC_KEY_LEN];
    u8 plain[256];
    u8 cipher[256];
    u8 wrong_decrypt[256];
    int ret;

    print_separator();
    printf("Test: Crypto Erase Data Loss Verification\n");
    print_separator();

    for (u32 i = 0; i < SEC_KEY_LEN; i++) {
        mk[i] = (u8)(i + 0x80);
    }
    for (u32 i = 0; i < 256; i++) {
        plain[i] = (u8)(i & 0xFF);
    }

    key_table_init(&kt);

    /* Create old DEK and encrypt data */
    sec_generate_random_key(dek_old, SEC_KEY_LEN);
    sec_hkdf_derive(mk, 1, kek);
    sec_dek_wrap(kek, dek_old, kt.entries[0].wrapped_dek);
    kt.entries[0].nsid = 1;
    kt.entries[0].state = KEY_ACTIVE;
    kt.crc32 = hfsss_crc32(&kt, offsetof(struct key_table, crc32));

    crypto_ctx_init(&ctx_old, 1, dek_old);
    crypto_xts_encrypt(&ctx_old, 0, plain, cipher, 256);

    /* Crypto erase: old DEK is gone, new DEK is generated */
    ret = crypto_erase_ns(&kt, 1, mk);
    TEST_ASSERT(ret == HFSSS_OK, "crypto erase succeeds");

    /* Unwrap new DEK */
    ret = sec_dek_unwrap(kek, kt.entries[0].wrapped_dek, dek_new);
    TEST_ASSERT(ret == HFSSS_OK, "unwrap new DEK succeeds");

    /* Verify old and new DEK are different */
    TEST_ASSERT(memcmp(dek_old, dek_new, SEC_KEY_LEN) != 0,
                "new DEK differs from old DEK after crypto erase");

    /* Try decrypting old ciphertext with new DEK */
    crypto_ctx_init(&ctx_new, 1, dek_new);
    crypto_xts_decrypt(&ctx_new, 0, cipher, wrong_decrypt, 256);

    TEST_ASSERT(memcmp(plain, wrong_decrypt, 256) != 0,
                "decrypt with new key produces wrong data (old data unrecoverable)");

    crypto_ctx_cleanup(&ctx_old);
    crypto_ctx_cleanup(&ctx_new);
    memset(kek, 0, SEC_KEY_LEN);
    printf("\n");
}

/* ----------------------------------------------------------------
 * Secure Boot: Valid Signature
 * ---------------------------------------------------------------- */
static void test_secure_boot_valid(void)
{
    u8 image[1024];
    struct fw_signature sig;
    bool result;

    print_separator();
    printf("Test: Secure Boot Valid Signature\n");
    print_separator();

    /* Create a test firmware image */
    for (u32 i = 0; i < 1024; i++) {
        image[i] = (u8)(i * 3 + 17);
    }

    sig.magic = FW_SIG_MAGIC;
    sig.fw_version = 100;
    sig.image_crc32 = hfsss_crc32(image, 1024);
    sig.reserved = 0;

    result = secure_boot_verify(image, 1024, &sig);
    TEST_ASSERT(result == true, "valid firmware signature passes verification");

    printf("\n");
}

/* ----------------------------------------------------------------
 * Secure Boot: Tampered Image
 * ---------------------------------------------------------------- */
static void test_secure_boot_tampered(void)
{
    u8 image[1024];
    struct fw_signature sig;
    bool result;

    print_separator();
    printf("Test: Secure Boot Tampered Image\n");
    print_separator();

    for (u32 i = 0; i < 1024; i++) {
        image[i] = (u8)(i * 3 + 17);
    }

    sig.magic = FW_SIG_MAGIC;
    sig.fw_version = 100;
    sig.image_crc32 = hfsss_crc32(image, 1024);
    sig.reserved = 0;

    /* Tamper with the image */
    image[512] ^= 0x01;

    result = secure_boot_verify(image, 1024, &sig);
    TEST_ASSERT(result == false,
                "tampered firmware image fails verification");

    /* Also test bad magic */
    image[512] ^= 0x01;  /* restore */
    sig.magic = 0xDEADBEEF;
    result = secure_boot_verify(image, 1024, &sig);
    TEST_ASSERT(result == false,
                "invalid signature magic fails verification");

    printf("\n");
}

/* ----------------------------------------------------------------
 * NOR-backed key table: round-trip save/load (REQ-165)
 * ---------------------------------------------------------------- */
static void with_tmp_nor_dev(void (*body)(struct nor_dev *))
{
    char img_path[64];
    snprintf(img_path, sizeof(img_path),
             "/tmp/hfsss_test_nor_keys_%d.bin", (int)getpid());
    unlink(img_path);

    struct nor_dev nor;
    int ret = nor_dev_init(&nor, img_path);
    TEST_ASSERT(ret == HFSSS_OK, "nor-key: nor_dev_init");

    body(&nor);

    nor_dev_cleanup(&nor);
    unlink(img_path);
}

static void body_nor_roundtrip(struct nor_dev *nor)
{
    struct key_table kt_orig;
    struct key_table kt_loaded;
    int ret;

    ret = key_table_init(&kt_orig);
    TEST_ASSERT(ret == HFSSS_OK, "nor-key: key_table_init");
    kt_orig.entries[3].nsid  = 7;
    kt_orig.entries[3].state = KEY_ACTIVE;
    /* Recompute body CRC so save/load authenticates the mutation. */
    kt_orig.crc32 = hfsss_crc32(&kt_orig, offsetof(struct key_table, crc32));

    /* Empty-NOR load: neither slot exists yet. */
    ret = key_table_load(&kt_loaded, nor);
    TEST_ASSERT(ret == HFSSS_ERR_NOENT,
                "nor-key: load_nor on blank NOR returns NOENT");

    ret = key_table_save(&kt_orig, nor);
    TEST_ASSERT(ret == HFSSS_OK, "nor-key: save_nor OK");

    memset(&kt_loaded, 0xAB, sizeof(kt_loaded));
    ret = key_table_load(&kt_loaded, nor);
    TEST_ASSERT(ret == HFSSS_OK, "nor-key: load_nor OK after save");
    TEST_ASSERT(memcmp(&kt_loaded, &kt_orig, sizeof(kt_orig)) == 0,
                "nor-key: round-tripped body matches original");
}

static void test_key_table_nor_roundtrip(void)
{
    printf("Test: NOR-backed key table round-trip (REQ-165)\n");
    with_tmp_nor_dev(body_nor_roundtrip);
    printf("\n");
}

/* Helper: peek at a slot's generation field directly. */
static u32 slot_peek_generation(struct nor_dev *nor, u32 rel_offset)
{
    struct key_table_nor_slot slot;
    if (nor_partition_read(nor, NOR_PART_KEYS, rel_offset,
                           &slot, sizeof(slot)) != HFSSS_OK) {
        return 0;
    }
    if (slot.slot_magic != SEC_NOR_KEYS_MAGIC) {
        return 0;
    }
    return slot.generation;
}

static void body_nor_generation_advance(struct nor_dev *nor)
{
    struct key_table kt;
    key_table_init(&kt);

    /* First save -> generation 1 in both slots. */
    TEST_ASSERT(key_table_save(&kt, nor) == HFSSS_OK,
                "nor-key: first save OK");
    TEST_ASSERT(slot_peek_generation(nor, SEC_NOR_KEYS_SLOT_A_REL) == 1,
                "nor-key: slot A has generation 1 after first save");
    TEST_ASSERT(slot_peek_generation(nor, SEC_NOR_KEYS_SLOT_B_REL) == 1,
                "nor-key: slot B has generation 1 after first save");

    /* Second save -> generation 2. */
    TEST_ASSERT(key_table_save(&kt, nor) == HFSSS_OK,
                "nor-key: second save OK");
    TEST_ASSERT(slot_peek_generation(nor, SEC_NOR_KEYS_SLOT_A_REL) == 2,
                "nor-key: generation advances on each save");
    TEST_ASSERT(slot_peek_generation(nor, SEC_NOR_KEYS_SLOT_B_REL) == 2,
                "nor-key: both slots reach new generation after save");
}

static void test_key_table_nor_generation_advance(void)
{
    printf("Test: NOR key table monotonic generation (REQ-165)\n");
    with_tmp_nor_dev(body_nor_generation_advance);
    printf("\n");
}

/* Corrupt slot A after a save; load must recover from slot B. */
static void body_nor_slot_a_corruption(struct nor_dev *nor)
{
    struct key_table kt;
    key_table_init(&kt);
    kt.entries[0].nsid  = 11;
    kt.entries[0].state = KEY_ACTIVE;
    kt.crc32 = hfsss_crc32(&kt, offsetof(struct key_table, crc32));
    TEST_ASSERT(key_table_save(&kt, nor) == HFSSS_OK,
                "nor-key: baseline save OK");

    /* Destroy slot A: rewrite its magic header with garbage by
     * erasing its sector (blank NOR has magic 0xFFFFFFFF). */
    u32 part_off = 0;
    nor_get_partition(NOR_PART_KEYS, &part_off, NULL);
    TEST_ASSERT(nor_write_enable(nor) == HFSSS_OK,
                "nor-key: write_enable before manual corrupt");
    TEST_ASSERT(nor_sector_erase(nor,
                                 (u64)part_off + SEC_NOR_KEYS_SLOT_A_REL) == HFSSS_OK,
                "nor-key: slot A erase (corruption)");
    TEST_ASSERT(slot_peek_generation(nor, SEC_NOR_KEYS_SLOT_A_REL) == 0,
                "nor-key: slot A now blank/invalid");
    TEST_ASSERT(slot_peek_generation(nor, SEC_NOR_KEYS_SLOT_B_REL) != 0,
                "nor-key: slot B still intact");

    struct key_table kt_loaded;
    memset(&kt_loaded, 0, sizeof(kt_loaded));
    TEST_ASSERT(key_table_load(&kt_loaded, nor) == HFSSS_OK,
                "nor-key: load still OK after slot A corrupted");
    TEST_ASSERT(memcmp(&kt_loaded, &kt, sizeof(kt)) == 0,
                "nor-key: slot B recovered the saved body");
}

static void test_key_table_nor_slot_a_corruption_falls_back_to_b(void)
{
    printf("Test: NOR key table slot-A corruption recovery (REQ-165)\n");
    with_tmp_nor_dev(body_nor_slot_a_corruption);
    printf("\n");
}

/* Corrupt slot B; load should still succeed from slot A. */
static void body_nor_slot_b_corruption(struct nor_dev *nor)
{
    struct key_table kt;
    key_table_init(&kt);
    kt.entries[1].nsid  = 22;
    kt.entries[1].state = KEY_ACTIVE;
    kt.crc32 = hfsss_crc32(&kt, offsetof(struct key_table, crc32));
    TEST_ASSERT(key_table_save(&kt, nor) == HFSSS_OK,
                "nor-key: baseline save OK");

    u32 part_off = 0;
    nor_get_partition(NOR_PART_KEYS, &part_off, NULL);
    nor_write_enable(nor);
    nor_sector_erase(nor, (u64)part_off + SEC_NOR_KEYS_SLOT_B_REL);

    struct key_table kt_loaded;
    memset(&kt_loaded, 0, sizeof(kt_loaded));
    TEST_ASSERT(key_table_load(&kt_loaded, nor) == HFSSS_OK,
                "nor-key: load OK after slot B corrupted");
    TEST_ASSERT(memcmp(&kt_loaded, &kt, sizeof(kt)) == 0,
                "nor-key: slot A recovered the saved body");
}

static void test_key_table_nor_slot_b_corruption_still_loads_a(void)
{
    printf("Test: NOR key table slot-B corruption recovery (REQ-165)\n");
    with_tmp_nor_dev(body_nor_slot_b_corruption);
    printf("\n");
}

/* Both slots corrupt -> load returns NOENT. */
static void body_nor_both_corrupt(struct nor_dev *nor)
{
    struct key_table kt;
    key_table_init(&kt);
    TEST_ASSERT(key_table_save(&kt, nor) == HFSSS_OK,
                "nor-key: baseline save OK");

    u32 part_off = 0;
    nor_get_partition(NOR_PART_KEYS, &part_off, NULL);
    nor_write_enable(nor);
    nor_sector_erase(nor, (u64)part_off + SEC_NOR_KEYS_SLOT_A_REL);
    nor_write_enable(nor);
    nor_sector_erase(nor, (u64)part_off + SEC_NOR_KEYS_SLOT_B_REL);

    struct key_table kt_loaded;
    TEST_ASSERT(key_table_load(&kt_loaded, nor) == HFSSS_ERR_NOENT,
                "nor-key: both-slots-corrupt returns NOENT");
}

static void test_key_table_nor_both_corrupt(void)
{
    printf("Test: NOR key table both slots corrupt (REQ-165)\n");
    with_tmp_nor_dev(body_nor_both_corrupt);
    printf("\n");
}

/* Simulate a crash between slot B and slot A writes: slot B should
 * hold the new generation, slot A the previous. Load must pick the
 * higher generation. */
static void body_nor_interrupted_save(struct nor_dev *nor)
{
    /* Baseline save puts gen=1 into both slots. */
    struct key_table kt_old;
    key_table_init(&kt_old);
    kt_old.entries[5].nsid = 99;
    kt_old.entries[5].state = KEY_SUSPENDED;
    kt_old.crc32 = hfsss_crc32(&kt_old, offsetof(struct key_table, crc32));
    TEST_ASSERT(key_table_save(&kt_old, nor) == HFSSS_OK,
                "nor-key: baseline gen=1 save OK");
    TEST_ASSERT(slot_peek_generation(nor, SEC_NOR_KEYS_SLOT_A_REL) == 1,
                "nor-key: slot A starts at gen=1");

    /* Prepare a new table that WOULD be gen=2 after save. */
    struct key_table kt_new;
    key_table_init(&kt_new);
    kt_new.entries[5].nsid = 100;
    kt_new.entries[5].state = KEY_ACTIVE;
    kt_new.crc32 = hfsss_crc32(&kt_new, offsetof(struct key_table, crc32));

    /* Manually reproduce the "slot B updated, slot A still old"
     * intermediate state. Build a slot record for kt_new with gen=2
     * and program it into slot B only. */
    struct key_table_nor_slot crash_slot;
    memset(&crash_slot, 0, sizeof(crash_slot));
    crash_slot.slot_magic = SEC_NOR_KEYS_MAGIC;
    crash_slot.generation = 2;
    crash_slot.body       = kt_new;
    crash_slot.slot_crc32 = hfsss_crc32(
        &crash_slot, offsetof(struct key_table_nor_slot, slot_crc32));

    u32 part_off = 0;
    nor_get_partition(NOR_PART_KEYS, &part_off, NULL);
    nor_write_enable(nor);
    nor_sector_erase(nor, (u64)part_off + SEC_NOR_KEYS_SLOT_B_REL);
    nor_write_enable(nor);
    TEST_ASSERT(nor_partition_write(nor, NOR_PART_KEYS,
                                    SEC_NOR_KEYS_SLOT_B_REL,
                                    &crash_slot,
                                    sizeof(crash_slot)) == HFSSS_OK,
                "nor-key: manually wrote gen=2 to slot B (simulated crash)");
    TEST_ASSERT(slot_peek_generation(nor, SEC_NOR_KEYS_SLOT_A_REL) == 1,
                "nor-key: slot A still at gen=1 (pre-crash)");
    TEST_ASSERT(slot_peek_generation(nor, SEC_NOR_KEYS_SLOT_B_REL) == 2,
                "nor-key: slot B at gen=2 (post-partial-save)");

    /* Load should pick gen=2 (the newer). */
    struct key_table kt_loaded;
    memset(&kt_loaded, 0, sizeof(kt_loaded));
    TEST_ASSERT(key_table_load(&kt_loaded, nor) == HFSSS_OK,
                "nor-key: load picks higher gen after crash");
    TEST_ASSERT(kt_loaded.entries[5].nsid == 100,
                "nor-key: loaded body is kt_new (highest gen wins)");
    TEST_ASSERT(kt_loaded.entries[5].state == KEY_ACTIVE,
                "nor-key: loaded state matches kt_new");
}

static void test_key_table_nor_interrupted_save(void)
{
    printf("Test: NOR key table interrupted save / crash recovery (REQ-165)\n");
    with_tmp_nor_dev(body_nor_interrupted_save);
    printf("\n");
}

/* ----------------------------------------------------------------
 * TCG Opal SSC lock/unlock (REQ-161)
 * ---------------------------------------------------------------- */

/* Seed a key_table with one ACTIVE namespace for the Opal tests. */
static void seed_active_ns(struct key_table *kt, u32 nsid)
{
    key_table_init(kt);
    kt->entries[0].nsid  = nsid;
    kt->entries[0].state = KEY_ACTIVE;
    kt->crc32 = hfsss_crc32(kt, offsetof(struct key_table, crc32));
}

static void test_opal_lock_transitions_active_to_suspended(void)
{
    printf("Test: Opal lock transitions ACTIVE -> SUSPENDED (REQ-161)\n");

    struct key_table kt;
    seed_active_ns(&kt, 7);

    TEST_ASSERT(opal_is_locked(&kt, 7) == false,
                "opal: fresh ACTIVE namespace reports unlocked");
    int rc = opal_lock_ns(&kt, 7);
    TEST_ASSERT(rc == HFSSS_OK, "opal: lock_ns OK on ACTIVE namespace");
    TEST_ASSERT(opal_is_locked(&kt, 7) == true,
                "opal: namespace reports locked after lock");
    TEST_ASSERT(kt.entries[0].state == KEY_SUSPENDED,
                "opal: key_state is SUSPENDED after lock");

    /* Locking an already-locked NS is idempotent. */
    rc = opal_lock_ns(&kt, 7);
    TEST_ASSERT(rc == HFSSS_OK,
                "opal: re-locking an already-locked NS is idempotent");
    TEST_ASSERT(kt.entries[0].state == KEY_SUSPENDED,
                "opal: state stays SUSPENDED on redundant lock");

    printf("\n");
}

static void test_opal_unlock_with_correct_auth_restores_active(void)
{
    printf("Test: Opal unlock with correct auth -> ACTIVE (REQ-161)\n");

    u8 mk[SEC_KEY_LEN];
    for (u32 i = 0; i < SEC_KEY_LEN; i++) {
        mk[i] = (u8)(0x30 + (i * 7));
    }

    struct key_table kt;
    seed_active_ns(&kt, 11);
    opal_lock_ns(&kt, 11);
    TEST_ASSERT(opal_is_locked(&kt, 11) == true,
                "opal: baseline locked");

    u8 auth[SEC_KEY_LEN];
    opal_derive_auth(mk, 11, auth);
    int rc = opal_unlock_ns(&kt, mk, 11, auth);
    TEST_ASSERT(rc == HFSSS_OK,
                "opal: unlock with correct auth returns OK");
    TEST_ASSERT(opal_is_locked(&kt, 11) == false,
                "opal: namespace unlocked after correct auth");
    TEST_ASSERT(kt.entries[0].state == KEY_ACTIVE,
                "opal: key_state back to ACTIVE after unlock");

    /* Unlocking an already-unlocked NS with the right auth is a
     * no-op success (matches TCG Opal §5.3 redundant-unlock). */
    rc = opal_unlock_ns(&kt, mk, 11, auth);
    TEST_ASSERT(rc == HFSSS_OK,
                "opal: redundant unlock with correct auth is benign");

    printf("\n");
}

static void test_opal_unlock_with_wrong_auth_rejects(void)
{
    printf("Test: Opal unlock with wrong auth rejects (REQ-161)\n");

    u8 mk[SEC_KEY_LEN];
    for (u32 i = 0; i < SEC_KEY_LEN; i++) {
        mk[i] = (u8)(0x60 + i);
    }

    struct key_table kt;
    seed_active_ns(&kt, 13);
    opal_lock_ns(&kt, 13);

    u8 bad_auth[SEC_KEY_LEN];
    for (u32 i = 0; i < SEC_KEY_LEN; i++) {
        bad_auth[i] = 0xFF;
    }
    int rc = opal_unlock_ns(&kt, mk, 13, bad_auth);
    TEST_ASSERT(rc == HFSSS_ERR_AUTH,
                "opal: unlock with wrong auth returns ERR_AUTH");
    TEST_ASSERT(opal_is_locked(&kt, 13) == true,
                "opal: namespace stays locked after wrong auth");
    TEST_ASSERT(kt.entries[0].state == KEY_SUSPENDED,
                "opal: key_state still SUSPENDED after rejected unlock");

    /* Unlock with an auth derived for a DIFFERENT nsid also rejects. */
    u8 wrong_ns_auth[SEC_KEY_LEN];
    opal_derive_auth(mk, 99 /* wrong nsid */, wrong_ns_auth);
    rc = opal_unlock_ns(&kt, mk, 13, wrong_ns_auth);
    TEST_ASSERT(rc == HFSSS_ERR_AUTH,
                "opal: auth derived for a different nsid is rejected");
    TEST_ASSERT(opal_is_locked(&kt, 13) == true,
                "opal: namespace still locked after cross-nsid attempt");

    printf("\n");
}

static void test_opal_derive_auth_is_deterministic_and_ns_unique(void)
{
    printf("Test: Opal auth derivation is deterministic + NS-unique (REQ-161)\n");

    u8 mk[SEC_KEY_LEN];
    for (u32 i = 0; i < SEC_KEY_LEN; i++) {
        mk[i] = (u8)(i * 3 + 1);
    }

    u8 a1[SEC_KEY_LEN];
    u8 a2[SEC_KEY_LEN];
    opal_derive_auth(mk, 5, a1);
    opal_derive_auth(mk, 5, a2);
    TEST_ASSERT(memcmp(a1, a2, SEC_KEY_LEN) == 0,
                "opal: same (mk, nsid) -> same auth (deterministic)");

    u8 a_ns5[SEC_KEY_LEN];
    u8 a_ns6[SEC_KEY_LEN];
    opal_derive_auth(mk, 5, a_ns5);
    opal_derive_auth(mk, 6, a_ns6);
    TEST_ASSERT(memcmp(a_ns5, a_ns6, SEC_KEY_LEN) != 0,
                "opal: different nsid -> different auth (ns-unique)");

    /* Different MK -> different auth. */
    u8 mk2[SEC_KEY_LEN];
    for (u32 i = 0; i < SEC_KEY_LEN; i++) {
        mk2[i] = mk[i] ^ 0xFFu;
    }
    u8 a_mk2[SEC_KEY_LEN];
    opal_derive_auth(mk2, 5, a_mk2);
    TEST_ASSERT(memcmp(a_ns5, a_mk2, SEC_KEY_LEN) != 0,
                "opal: different MK -> different auth");

    printf("\n");
}

static void test_opal_lock_unknown_nsid_returns_noent(void)
{
    printf("Test: Opal lock/unlock unknown nsid -> NOENT (REQ-161)\n");

    u8 mk[SEC_KEY_LEN];
    for (u32 i = 0; i < SEC_KEY_LEN; i++) {
        mk[i] = (u8)i;
    }

    struct key_table kt;
    key_table_init(&kt);
    /* No NS populated — every nsid should be "unknown". */
    int rc = opal_lock_ns(&kt, 42);
    TEST_ASSERT(rc == HFSSS_ERR_NOENT,
                "opal: lock_ns on unknown nsid returns NOENT");

    u8 auth[SEC_KEY_LEN];
    opal_derive_auth(mk, 42, auth);
    rc = opal_unlock_ns(&kt, mk, 42, auth);
    TEST_ASSERT(rc == HFSSS_ERR_NOENT,
                "opal: unlock_ns on unknown nsid returns NOENT "
                "(after auth verified)");

    /* Invalid args rejected up front. */
    TEST_ASSERT(opal_lock_ns(NULL, 42) == HFSSS_ERR_INVAL,
                "opal: NULL kt rejected");
    TEST_ASSERT(opal_lock_ns(&kt, 0) == HFSSS_ERR_INVAL,
                "opal: nsid=0 rejected");

    printf("\n");
}

/* ----------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------- */
int main(void)
{
    print_separator();
    printf("HFSSS Security and Encryption Tests\n");
    print_separator();
    printf("\n");

    test_crypto_roundtrip();
    test_crypto_different_sectors();
    test_crypto_transforms_data();
    test_key_generation();
    test_hkdf_derivation();
    test_dek_wrap_unwrap();
    test_dek_unwrap_wrong_kek();
    test_key_table_init_fields();
    test_crypto_erase();
    test_crypto_erase_data_loss();
    test_secure_boot_valid();
    test_secure_boot_tampered();
    test_key_table_nor_roundtrip();
    test_key_table_nor_generation_advance();
    test_key_table_nor_slot_a_corruption_falls_back_to_b();
    test_key_table_nor_slot_b_corruption_still_loads_a();
    test_key_table_nor_both_corrupt();
    test_key_table_nor_interrupted_save();
    test_opal_lock_transitions_active_to_suspended();
    test_opal_unlock_with_correct_auth_restores_active();
    test_opal_unlock_with_wrong_auth_rejects();
    test_opal_derive_auth_is_deterministic_and_ns_unique();
    test_opal_lock_unknown_nsid_returns_noent();

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
