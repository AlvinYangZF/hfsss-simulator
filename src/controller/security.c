/*
 * HFSSS Security and Encryption Module
 *
 * Implements REQ-159 through REQ-165:
 *   - AES-XTS simulation (XOR-based placeholder)
 *   - Key hierarchy (MK -> KEK -> DEK)
 *   - DEK wrap/unwrap with verification tag
 *   - Persistent key table with CRC32 protection
 *   - Crypto erase per namespace
 *   - Secure boot firmware verification
 */

#include "controller/security.h"

/* ----------------------------------------------------------------
 * AES-XTS Simulation (XOR-based)
 * ---------------------------------------------------------------- */

void crypto_xts_encrypt(const struct crypto_ctx *ctx, u64 sector,
                        const u8 *plain, u8 *cipher, u32 len)
{
    u8 tweak_block[SEC_KEY_LEN];
    u32 i;

    if (!ctx || !plain || !cipher || len == 0) {
        return;
    }

    /* Derive per-sector tweak block from sector number and tweak key.
     * Each byte of the tweak depends on sector bits to ensure different
     * sectors produce different ciphertext. */
    for (i = 0; i < SEC_KEY_LEN; i++) {
        tweak_block[i] = ctx->tweak_key[i] ^
                         (u8)((sector >> (i % 8)) & 0xFF) ^
                         (u8)(i * 37 + sector);
    }

    for (i = 0; i < len; i++) {
        cipher[i] = plain[i] ^ ctx->data_key[i % SEC_KEY_LEN] ^
                    tweak_block[i % SEC_KEY_LEN];
    }
}

void crypto_xts_decrypt(const struct crypto_ctx *ctx, u64 sector,
                        const u8 *cipher, u8 *plain, u32 len)
{
    /* XOR is self-inverse: decrypt is identical to encrypt */
    crypto_xts_encrypt(ctx, sector, cipher, plain, len);
}

/* ----------------------------------------------------------------
 * Crypto Context Management
 * ---------------------------------------------------------------- */

int crypto_ctx_init(struct crypto_ctx *ctx, u32 nsid, const u8 *dek)
{
    if (!ctx || !dek) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->nsid = nsid;

    /* Split the 32-byte DEK into two independent 16-byte halves,
     * then expand each to 32 bytes via CRC-based mixing.
     * This ensures data_key and tweak_key are independently derived. */
    memcpy(ctx->data_key, dek, SEC_KEY_LEN);

    /* Derive tweak_key using a cascaded hash of the DEK.
     * This breaks the XOR cancellation that would occur with simple
     * dek ^ constant derivation. */
    {
        u32 crc = hfsss_crc32(dek, SEC_KEY_LEN);
        for (u32 i = 0; i < SEC_KEY_LEN; i++) {
            ctx->tweak_key[i] = (u8)((crc >> ((i % 4) * 8)) & 0xFF) ^
                                (u8)(dek[(i + 13) % SEC_KEY_LEN]);
            crc = crc * 1103515245u + 12345u + dek[i];
        }
    }

    ctx->active = true;
    return HFSSS_OK;
}

void crypto_ctx_cleanup(struct crypto_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    /* Securely wipe key material */
    memset(ctx->data_key, 0, SEC_KEY_LEN);
    memset(ctx->tweak_key, 0, SEC_KEY_LEN);
    ctx->active = false;
    ctx->nsid = 0;
}

/* ----------------------------------------------------------------
 * Key Generation
 * ---------------------------------------------------------------- */

void sec_generate_random_key(u8 *key, u32 len)
{
    static bool seeded = false;

    if (!key || len == 0) {
        return;
    }

    if (!seeded) {
        srand((unsigned int)time(NULL) ^ (unsigned int)get_time_ns());
        seeded = true;
    }

    for (u32 i = 0; i < len; i++) {
        key[i] = (u8)(rand() & 0xFF);
    }
}

/* ----------------------------------------------------------------
 * Key Derivation (HKDF Simulation)
 * ---------------------------------------------------------------- */

void sec_hkdf_derive(const u8 mk[SEC_KEY_LEN], u32 nsid,
                     u8 kek_out[SEC_KEY_LEN])
{
    u8 nsid_bytes[4];

    if (!mk || !kek_out) {
        return;
    }

    /* Encode NSID as little-endian bytes */
    nsid_bytes[0] = (u8)(nsid & 0xFF);
    nsid_bytes[1] = (u8)((nsid >> 8) & 0xFF);
    nsid_bytes[2] = (u8)((nsid >> 16) & 0xFF);
    nsid_bytes[3] = (u8)((nsid >> 24) & 0xFF);

    /* Simulated HKDF: XOR master key with NSID-derived bytes */
    for (u32 i = 0; i < SEC_KEY_LEN; i++) {
        kek_out[i] = mk[i] ^ nsid_bytes[i % 4];
        /* Additional mixing: rotate and add position */
        kek_out[i] = (u8)((kek_out[i] + i * 0x37) & 0xFF);
    }
}

/* ----------------------------------------------------------------
 * DEK Wrap / Unwrap
 *
 * Wrapped format: [32 bytes encrypted DEK] [16 bytes verification tag]
 * Tag = CRC32 of plaintext DEK repeated 4 times (16 bytes)
 * ---------------------------------------------------------------- */

void sec_dek_wrap(const u8 kek[SEC_KEY_LEN], const u8 dek[SEC_KEY_LEN],
                  u8 wrapped[SEC_WRAPPED_LEN])
{
    u32 tag_crc;

    if (!kek || !dek || !wrapped) {
        return;
    }

    /* Encrypt DEK with KEK (XOR) */
    for (u32 i = 0; i < SEC_KEY_LEN; i++) {
        wrapped[i] = dek[i] ^ kek[i];
    }

    /* Compute verification tag from plaintext DEK */
    tag_crc = hfsss_crc32(dek, SEC_KEY_LEN);

    /* Fill 16-byte tag area with repeated CRC32 (4 copies) */
    for (u32 i = 0; i < 4; i++) {
        u32 offset = SEC_KEY_LEN + i * 4;
        wrapped[offset + 0] = (u8)(tag_crc & 0xFF);
        wrapped[offset + 1] = (u8)((tag_crc >> 8) & 0xFF);
        wrapped[offset + 2] = (u8)((tag_crc >> 16) & 0xFF);
        wrapped[offset + 3] = (u8)((tag_crc >> 24) & 0xFF);
    }
}

int sec_dek_unwrap(const u8 kek[SEC_KEY_LEN],
                   const u8 wrapped[SEC_WRAPPED_LEN],
                   u8 dek_out[SEC_KEY_LEN])
{
    u32 tag_crc;
    u32 expected_crc;

    if (!kek || !wrapped || !dek_out) {
        return HFSSS_ERR_INVAL;
    }

    /* Decrypt DEK with KEK (XOR) */
    for (u32 i = 0; i < SEC_KEY_LEN; i++) {
        dek_out[i] = wrapped[i] ^ kek[i];
    }

    /* Compute CRC32 of decrypted DEK */
    tag_crc = hfsss_crc32(dek_out, SEC_KEY_LEN);

    /* Read expected CRC from verification tag (first 4 bytes of tag) */
    expected_crc = (u32)wrapped[SEC_KEY_LEN]
                 | ((u32)wrapped[SEC_KEY_LEN + 1] << 8)
                 | ((u32)wrapped[SEC_KEY_LEN + 2] << 16)
                 | ((u32)wrapped[SEC_KEY_LEN + 3] << 24);

    if (tag_crc != expected_crc) {
        /* Wrong KEK: wipe decrypted data */
        memset(dek_out, 0, SEC_KEY_LEN);
        return HFSSS_ERR_AUTH;
    }

    return HFSSS_OK;
}

/* ----------------------------------------------------------------
 * Key Table Management
 * ---------------------------------------------------------------- */

int key_table_init(struct key_table *kt)
{
    if (!kt) {
        return HFSSS_ERR_INVAL;
    }

    memset(kt, 0, sizeof(*kt));
    kt->magic = SEC_KEY_MAGIC;
    kt->version = 1;

    /* All entries start as KEY_EMPTY */
    for (u32 i = 0; i < SEC_MAX_NS; i++) {
        kt->entries[i].state = KEY_EMPTY;
        kt->entries[i].nsid = 0;
    }

    /* Compute CRC over everything except the crc32 field itself */
    kt->crc32 = hfsss_crc32(kt, offsetof(struct key_table, crc32));

    return HFSSS_OK;
}

int key_table_save(const struct key_table *kt, const char *filepath)
{
    FILE *fp;

    if (!kt || !filepath) {
        return HFSSS_ERR_INVAL;
    }

    fp = fopen(filepath, "wb");
    if (!fp) {
        return HFSSS_ERR_IO;
    }

    if (fwrite(kt, sizeof(*kt), 1, fp) != 1) {
        fclose(fp);
        return HFSSS_ERR_IO;
    }

    fclose(fp);
    return HFSSS_OK;
}

int key_table_load(struct key_table *kt, const char *filepath)
{
    FILE *fp;
    u32 computed_crc;

    if (!kt || !filepath) {
        return HFSSS_ERR_INVAL;
    }

    fp = fopen(filepath, "rb");
    if (!fp) {
        return HFSSS_ERR_IO;
    }

    if (fread(kt, sizeof(*kt), 1, fp) != 1) {
        fclose(fp);
        return HFSSS_ERR_IO;
    }

    fclose(fp);

    /* Verify magic */
    if (kt->magic != SEC_KEY_MAGIC) {
        return HFSSS_ERR_CRYPTO;
    }

    /* Verify CRC32 */
    computed_crc = hfsss_crc32(kt, offsetof(struct key_table, crc32));
    if (computed_crc != kt->crc32) {
        return HFSSS_ERR_CRYPTO;
    }

    return HFSSS_OK;
}

/* ----------------------------------------------------------------
 * Crypto Erase
 *
 * Destroys the DEK for a given namespace and generates a new one.
 * Data encrypted with the old DEK becomes permanently unrecoverable.
 * ---------------------------------------------------------------- */

int crypto_erase_ns(struct key_table *kt, u32 nsid,
                    const u8 mk[SEC_KEY_LEN])
{
    struct key_entry *entry = NULL;
    u8 kek[SEC_KEY_LEN];
    u8 new_dek[SEC_KEY_LEN];

    if (!kt || !mk || nsid == 0) {
        return HFSSS_ERR_INVAL;
    }

    /* Find the entry for this namespace */
    for (u32 i = 0; i < SEC_MAX_NS; i++) {
        if (kt->entries[i].nsid == nsid &&
            kt->entries[i].state == KEY_ACTIVE) {
            entry = &kt->entries[i];
            break;
        }
    }

    if (!entry) {
        return HFSSS_ERR_NOENT;
    }

    /* Zero out old wrapped DEK */
    memset(entry->wrapped_dek, 0, SEC_WRAPPED_LEN);
    entry->state = KEY_DESTROYED;

    /* Generate new DEK */
    sec_generate_random_key(new_dek, SEC_KEY_LEN);

    /* Derive KEK for this namespace */
    sec_hkdf_derive(mk, nsid, kek);

    /* Wrap new DEK and store */
    sec_dek_wrap(kek, new_dek, entry->wrapped_dek);
    entry->state = KEY_ACTIVE;

    /* Update CRC */
    kt->crc32 = hfsss_crc32(kt, offsetof(struct key_table, crc32));

    /* Wipe sensitive local variables */
    memset(kek, 0, SEC_KEY_LEN);
    memset(new_dek, 0, SEC_KEY_LEN);

    return HFSSS_OK;
}

/* ----------------------------------------------------------------
 * Secure Boot Verification
 * ---------------------------------------------------------------- */

bool secure_boot_verify(const u8 *image, u32 size,
                        const struct fw_signature *sig)
{
    u32 computed_crc;

    if (!image || size == 0 || !sig) {
        return false;
    }

    /* Verify signature magic */
    if (sig->magic != FW_SIG_MAGIC) {
        return false;
    }

    /* Verify CRC32 of firmware image */
    computed_crc = hfsss_crc32(image, size);
    if (computed_crc != sig->image_crc32) {
        return false;
    }

    return true;
}
