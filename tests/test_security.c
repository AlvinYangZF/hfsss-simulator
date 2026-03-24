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
static void test_key_table_persistence(void)
{
    struct key_table kt_orig;
    struct key_table kt_loaded;
    const char *path = "/tmp/hfsss_test_keytable.bin";
    int ret;

    print_separator();
    printf("Test: Key Table Init/Save/Load Round-Trip\n");
    print_separator();

    ret = key_table_init(&kt_orig);
    TEST_ASSERT(ret == HFSSS_OK, "key_table_init succeeds");
    TEST_ASSERT(kt_orig.magic == SEC_KEY_MAGIC, "key table magic is correct");
    TEST_ASSERT(kt_orig.version == 1, "key table version is 1");

    ret = key_table_save(&kt_orig, path);
    TEST_ASSERT(ret == HFSSS_OK, "key_table_save succeeds");

    memset(&kt_loaded, 0xFF, sizeof(kt_loaded));
    ret = key_table_load(&kt_loaded, path);
    TEST_ASSERT(ret == HFSSS_OK, "key_table_load succeeds");
    TEST_ASSERT(memcmp(&kt_orig, &kt_loaded, sizeof(struct key_table)) == 0,
                "loaded key table matches original");

    unlink(path);
    printf("\n");
}

/* ----------------------------------------------------------------
 * Key Table Load With Corrupt File
 * ---------------------------------------------------------------- */
static void test_key_table_corrupt(void)
{
    struct key_table kt;
    const char *path = "/tmp/hfsss_test_keytable_corrupt.bin";
    FILE *fp;
    int ret;

    print_separator();
    printf("Test: Key Table Load With Corrupt File\n");
    print_separator();

    /* Write garbage data */
    fp = fopen(path, "wb");
    TEST_ASSERT(fp != NULL, "create corrupt test file");
    if (fp) {
        u8 garbage[sizeof(struct key_table)];
        memset(garbage, 0xDE, sizeof(garbage));
        fwrite(garbage, sizeof(garbage), 1, fp);
        fclose(fp);
    }

    ret = key_table_load(&kt, path);
    TEST_ASSERT(ret == HFSSS_ERR_CRYPTO,
                "key_table_load detects corrupt file");

    unlink(path);
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
    test_key_table_persistence();
    test_key_table_corrupt();
    test_crypto_erase();
    test_crypto_erase_data_loss();
    test_secure_boot_valid();
    test_secure_boot_tampered();

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
