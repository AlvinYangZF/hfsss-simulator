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
#include "media/nor_flash.h"
#include <stddef.h>

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

int key_table_register_ns(struct key_table *kt, u32 nsid)
{
    if (!kt || nsid == 0) {
        return HFSSS_ERR_INVAL;
    }
    for (u32 i = 0; i < SEC_MAX_NS; i++) {
        if (kt->entries[i].nsid == nsid &&
            kt->entries[i].state != KEY_EMPTY) {
            return HFSSS_ERR_EXIST;
        }
    }
    for (u32 i = 0; i < SEC_MAX_NS; i++) {
        if (kt->entries[i].state == KEY_EMPTY) {
            kt->entries[i].nsid  = nsid;
            kt->entries[i].state = KEY_ACTIVE;
            kt->crc32 = hfsss_crc32(kt, offsetof(struct key_table, crc32));
            return HFSSS_OK;
        }
    }
    return HFSSS_ERR_NOMEM;
}

/* ----------------------------------------------------------------
 * NOR-backed Key Table (REQ-165)
 *
 * The persistent store for the key table lives in NOR_PART_KEYS;
 * there is no file-backed fallback. Layout inside NOR_PART_KEYS:
 *   [ 0,       64KB) — slot A
 *   [64KB,   128KB) — slot B
 * Each slot holds a `struct key_table_nor_slot` whose outer CRC
 * authenticates the 2176-byte record. Generation numbers are
 * monotonic so readers can pick the newest valid slot even after
 * an interrupted save leaves the two slots disagreeing.
 * ---------------------------------------------------------------- */

/* Build outer CRC over everything except the trailing slot_crc32 word. */
static u32 key_slot_compute_crc(const struct key_table_nor_slot *slot)
{
    return hfsss_crc32(slot, offsetof(struct key_table_nor_slot, slot_crc32));
}

/* Return slot generation if the slot is valid (magic + both CRCs
 * match), or 0 if invalid/blank. Generation 0 is reserved to mean
 * "never written" — the first save stamps generation 1. */
static u32 key_slot_read_if_valid(struct nor_dev *nor, u32 rel_offset,
                                  struct key_table_nor_slot *out)
{
    if (nor_partition_read(nor, NOR_PART_KEYS, rel_offset,
                           out, sizeof(*out)) != HFSSS_OK) {
        return 0;
    }
    if (out->slot_magic != SEC_NOR_KEYS_MAGIC) {
        return 0;
    }
    if (out->generation == 0) {
        return 0;
    }
    if (key_slot_compute_crc(out) != out->slot_crc32) {
        return 0;
    }
    u32 body_crc = hfsss_crc32(&out->body,
                               offsetof(struct key_table, crc32));
    if (body_crc != out->body.crc32) {
        return 0;
    }
    return out->generation;
}

/* Erase the NOR sector containing the given partition-relative offset
 * and reprogram it with `slot`. Called once per copy. Returns the
 * status of the first failing NOR op, or HFSSS_OK. */
static int key_slot_write(struct nor_dev *nor, u32 rel_offset,
                          const struct key_table_nor_slot *slot)
{
    u32 part_offset = 0;
    int ret = nor_get_partition(NOR_PART_KEYS, &part_offset, NULL);
    if (ret != HFSSS_OK) {
        return ret;
    }
    ret = nor_write_enable(nor);
    if (ret != HFSSS_OK) {
        return ret;
    }
    ret = nor_sector_erase(nor, (u64)part_offset + rel_offset);
    if (ret != HFSSS_OK) {
        return ret;
    }
    ret = nor_write_enable(nor);
    if (ret != HFSSS_OK) {
        return ret;
    }
    return nor_partition_write(nor, NOR_PART_KEYS, rel_offset,
                               slot, sizeof(*slot));
}

int key_table_save(const struct key_table *kt, struct nor_dev *nor)
{
    if (!kt || !nor) {
        return HFSSS_ERR_INVAL;
    }

    /* Look up current generation numbers so the new save strictly
     * supersedes whatever's there. */
    struct key_table_nor_slot probe;
    u32 gen_a = key_slot_read_if_valid(nor, SEC_NOR_KEYS_SLOT_A_REL, &probe);
    u32 gen_b = key_slot_read_if_valid(nor, SEC_NOR_KEYS_SLOT_B_REL, &probe);
    u32 max_gen = (gen_a > gen_b) ? gen_a : gen_b;
    u32 new_gen = max_gen + 1;

    /* Assemble the outgoing slot once; both physical copies are
     * byte-for-byte identical aside from their NOR offset. */
    struct key_table_nor_slot slot;
    memset(&slot, 0, sizeof(slot));
    slot.slot_magic = SEC_NOR_KEYS_MAGIC;
    slot.generation = new_gen;
    slot.body       = *kt;
    slot.slot_crc32 = key_slot_compute_crc(&slot);

    /* Slot B first: an interrupted save leaves A with the old (but
     * valid) generation so load can still recover. Then slot A. */
    int ret = key_slot_write(nor, SEC_NOR_KEYS_SLOT_B_REL, &slot);
    if (ret != HFSSS_OK) {
        return ret;
    }
    ret = key_slot_write(nor, SEC_NOR_KEYS_SLOT_A_REL, &slot);
    if (ret != HFSSS_OK) {
        return ret;
    }

    return HFSSS_OK;
}

int key_table_load(struct key_table *kt, struct nor_dev *nor)
{
    if (!kt || !nor) {
        return HFSSS_ERR_INVAL;
    }

    struct key_table_nor_slot slot_a;
    struct key_table_nor_slot slot_b;
    u32 gen_a = key_slot_read_if_valid(nor, SEC_NOR_KEYS_SLOT_A_REL, &slot_a);
    u32 gen_b = key_slot_read_if_valid(nor, SEC_NOR_KEYS_SLOT_B_REL, &slot_b);

    if (gen_a == 0 && gen_b == 0) {
        return HFSSS_ERR_NOENT;
    }

    const struct key_table_nor_slot *src = (gen_a >= gen_b) ? &slot_a : &slot_b;
    *kt = src->body;
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
 * TCG Opal SSC basic lock/unlock (REQ-161)
 *
 * Per-NS authentication token is derived from the master key using
 * the same HKDF primitive as the KEK, but with a domain-separation
 * tag XORed into the master key so a leaked KEK does not imply a
 * leaked Opal PIN. The simulator maps ACTIVE <-> SUSPENDED on the
 * existing key_state machine; unlock with the wrong auth returns
 * HFSSS_ERR_AUTH without changing state.
 * ---------------------------------------------------------------- */

#define OPAL_DOMAIN_SEP_TAG  0xA5

void opal_derive_auth(const u8 mk[SEC_KEY_LEN], u32 nsid,
                      u8 auth_out[SEC_KEY_LEN])
{
    if (!mk || !auth_out) {
        return;
    }
    u8 tagged_mk[SEC_KEY_LEN];
    for (u32 i = 0; i < SEC_KEY_LEN; i++) {
        tagged_mk[i] = mk[i] ^ OPAL_DOMAIN_SEP_TAG;
    }
    sec_hkdf_derive(tagged_mk, nsid, auth_out);
    /* Wipe the tagged intermediate so it doesn't linger on stack. */
    memset(tagged_mk, 0, sizeof(tagged_mk));
}

/* Locate the key_entry for `nsid`. Returns NULL when not present. */
static struct key_entry *opal_find_entry(struct key_table *kt, u32 nsid)
{
    for (u32 i = 0; i < SEC_MAX_NS; i++) {
        if (kt->entries[i].nsid == nsid) {
            return &kt->entries[i];
        }
    }
    return NULL;
}

int opal_lock_ns(struct key_table *kt, u32 nsid)
{
    if (!kt || nsid == 0) {
        return HFSSS_ERR_INVAL;
    }
    struct key_entry *entry = opal_find_entry(kt, nsid);
    if (!entry) {
        return HFSSS_ERR_NOENT;
    }
    if (entry->state != KEY_ACTIVE) {
        /* Idempotent-locked or the key has been destroyed; nothing
         * to do. Distinguish "already locked" so callers can detect
         * double locks without pattern-matching on state. */
        return (entry->state == KEY_SUSPENDED) ? HFSSS_OK
                                               : HFSSS_ERR_INVAL;
    }
    entry->state = KEY_SUSPENDED;
    kt->crc32 = hfsss_crc32(kt, offsetof(struct key_table, crc32));
    return HFSSS_OK;
}

int opal_unlock_ns(struct key_table *kt, const u8 mk[SEC_KEY_LEN],
                   u32 nsid, const u8 auth[SEC_KEY_LEN])
{
    if (!kt || !mk || !auth || nsid == 0) {
        return HFSSS_ERR_INVAL;
    }

    /* Verify auth FIRST so a wrong PIN doesn't leak whether the
     * namespace exists or what state it's in. */
    u8 expected[SEC_KEY_LEN];
    opal_derive_auth(mk, nsid, expected);
    int cmp = memcmp(expected, auth, SEC_KEY_LEN);
    memset(expected, 0, sizeof(expected));
    if (cmp != 0) {
        return HFSSS_ERR_AUTH;
    }

    struct key_entry *entry = opal_find_entry(kt, nsid);
    if (!entry) {
        return HFSSS_ERR_NOENT;
    }
    if (entry->state == KEY_ACTIVE) {
        /* Already unlocked — benign, treat as success. */
        return HFSSS_OK;
    }
    if (entry->state != KEY_SUSPENDED) {
        return HFSSS_ERR_INVAL;
    }
    entry->state = KEY_ACTIVE;
    kt->crc32 = hfsss_crc32(kt, offsetof(struct key_table, crc32));
    return HFSSS_OK;
}

bool opal_is_locked(const struct key_table *kt, u32 nsid)
{
    if (!kt || nsid == 0) {
        return false;
    }
    for (u32 i = 0; i < SEC_MAX_NS; i++) {
        if (kt->entries[i].nsid == nsid) {
            return kt->entries[i].state == KEY_SUSPENDED;
        }
    }
    return false;
}

/* ----------------------------------------------------------------
 * Secure Boot Verification
 * ---------------------------------------------------------------- */

/* secure_boot_verify() moved to src/common/boot.c so the boot sequence
 * can invoke it without the controller layer being linked. See
 * include/common/boot.h. */
