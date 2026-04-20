#ifndef __HFSSS_SECURITY_H
#define __HFSSS_SECURITY_H

#include "common/common.h"

#define SEC_KEY_LEN      32   /* 256-bit keys */
#define SEC_WRAPPED_LEN  48   /* wrapped DEK size */
#define SEC_MAX_NS       32
#define SEC_KEY_MAGIC    0x4B455953U  /* "KEYS" */

/* Key states */
enum key_state {
    KEY_EMPTY     = 0,
    KEY_ACTIVE    = 1,
    KEY_SUSPENDED = 2,
    KEY_DESTROYED = 3,
};

/* Crypto context per namespace */
struct crypto_ctx {
    u8  data_key[SEC_KEY_LEN];
    u8  tweak_key[SEC_KEY_LEN];
    u32 nsid;
    bool active;
};

/* Key entry in persistent key table */
struct key_entry {
    u8  wrapped_dek[SEC_WRAPPED_LEN];
    u32 nsid;
    u32 state;     /* enum key_state */
    u32 reserved[2];
};

/* Persistent key table (stored in NOR) */
struct key_table {
    u32 magic;
    u32 version;
    u8  master_key_wrapped[SEC_WRAPPED_LEN];
    struct key_entry entries[SEC_MAX_NS];
    u32 crc32;
};

/* Firmware signature & FW_SIG_MAGIC now live in <common/boot.h> so the
 * boot sequence can verify images without pulling in the controller
 * layer. Include boot.h here so existing callers (test_security.c etc.)
 * keep seeing the same declarations through this header. */
#include "common/boot.h"

/* AES-XTS simulation (XOR-based placeholder) */
void crypto_xts_encrypt(const struct crypto_ctx *ctx, u64 sector,
                        const u8 *plain, u8 *cipher, u32 len);
void crypto_xts_decrypt(const struct crypto_ctx *ctx, u64 sector,
                        const u8 *cipher, u8 *plain, u32 len);

/* Crypto context management */
int  crypto_ctx_init(struct crypto_ctx *ctx, u32 nsid, const u8 *dek);
void crypto_ctx_cleanup(struct crypto_ctx *ctx);

/* Key generation and derivation */
void sec_generate_random_key(u8 *key, u32 len);
void sec_hkdf_derive(const u8 mk[SEC_KEY_LEN], u32 nsid,
                     u8 kek_out[SEC_KEY_LEN]);

/* DEK wrapping / unwrapping */
void sec_dek_wrap(const u8 kek[SEC_KEY_LEN], const u8 dek[SEC_KEY_LEN],
                  u8 wrapped[SEC_WRAPPED_LEN]);
int  sec_dek_unwrap(const u8 kek[SEC_KEY_LEN],
                    const u8 wrapped[SEC_WRAPPED_LEN],
                    u8 dek_out[SEC_KEY_LEN]);

/* Key table management (REQ-165).
 *
 * Persistence is NOR-backed with dual-copy + UPLP-safe update:
 * each save stamps a monotonic generation number and writes to
 * slot B first, then slot A. A crash between the two writes leaves
 * one slot holding the new generation and the other holding the
 * previous generation — `key_table_load` picks whichever slot has
 * the highest generation with a valid CRC.
 *
 * The canonical save/load API takes `struct nor_dev *` directly;
 * there is no file-backed path. Callers that want to persist a
 * key table outside of NOR must serialize the struct themselves.
 *
 * Requires NOR_PART_KEYS to be sized to at least two 64 KB sectors.
 */
#define SEC_NOR_KEYS_MAGIC       0x4E4B4559U  /* "NKEY" */
#define SEC_NOR_KEYS_SLOT_A_REL  0U
#define SEC_NOR_KEYS_SLOT_B_REL  (64U * 1024U)  /* second NOR sector */

struct key_table_nor_slot {
    u32              slot_magic;
    u32              generation;
    struct key_table body;
    u32              slot_crc32;  /* CRC over everything above */
};

struct nor_dev;
int key_table_init(struct key_table *kt);
int key_table_save(const struct key_table *kt, struct nor_dev *nor);
int key_table_load(struct key_table *kt, struct nor_dev *nor);

/* Crypto erase */
int crypto_erase_ns(struct key_table *kt, u32 nsid,
                    const u8 mk[SEC_KEY_LEN]);

/* Secure boot verification lives in common/boot.h. */

#endif /* __HFSSS_SECURITY_H */
