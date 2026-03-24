# Security and Encryption Low-Level Design

## Revision History

| Version | Date       | Author | Description     |
|---------|------------|--------|-----------------|
| V1.0    | 2026-03-23 | HFSSS  | Initial release |

## Table of Contents

1. [Overview](#1-overview)
2. [Requirements Traceability](#2-requirements-traceability)
3. [AES-XTS Encryption Pipeline](#3-aes-xts-encryption-pipeline)
4. [Key Hierarchy](#4-key-hierarchy)
5. [TCG Opal SSC Simulation](#5-tcg-opal-ssc-simulation)
6. [Secure Erase Flows](#6-secure-erase-flows)
7. [Key Storage in NOR Flash](#7-key-storage-in-nor-flash)
8. [Secure Boot Chain](#8-secure-boot-chain)
9. [Architecture Decision Records](#9-architecture-decision-records)
10. [Test Plan](#10-test-plan)

---

## 1. Overview

**Purpose**: Simulate data-at-rest encryption for enterprise SSDs, including AES-XTS encryption pipeline, key hierarchy management, TCG Opal SSC locking, secure erase, and secure boot chain verification.

**Scope**: This module provides a simulation-grade implementation of encryption and security features. Actual cryptographic operations use XOR-based placeholders with the same interface as real AES, enabling full firmware path testing without the computational overhead of real encryption.

**References**: TCG Opal SSC 2.0 (Storage Security Subsystem Class), NIST SP 800-38E (XTS-AES), IEEE 1619 (Standard for Cryptographic Protection of Data on Block-Oriented Storage Devices).

---

## 2. Requirements Traceability

| Req ID  | Description | Priority | Target |
|---------|-------------|----------|--------|
| REQ-159 | AES-XTS encryption pipeline: encrypt on write, decrypt on read | P0 | V3.0 |
| REQ-160 | Key hierarchy: Master Key -> KEK -> DEK, three-level management | P0 | V3.0 |
| REQ-161 | TCG Opal SSC simulation: locking ranges, authority management, MBR shadowing | P0 | V3.0 |
| REQ-162 | Secure erase: crypto erase (DEK destroy), block erase, sanitize | P0 | V3.0 |
| REQ-163 | Key storage in NOR Flash: protected partition, dual-copy power-safe updates | P0 | V3.0 |
| REQ-164 | Secure boot chain: ROM -> bootloader signature -> FW signature -> boot | P1 | V3.0 |
| REQ-165 | NVMe Format with SES field: SES=1 (user data erase), SES=2 (crypto erase) | P0 | V3.0 |

---

## 3. AES-XTS Encryption Pipeline

### 3.1 Algorithm

AES-XTS per IEEE 1619 uses two AES keys: a data key and a tweak key. The tweak is computed from the sector number, providing unique encryption per sector.

```
Tweak computation: tweak = AES_encrypt(tweak_key, sector_number)
Encryption: ciphertext = XTS_encrypt(data_key, tweak, plaintext)
Decryption: plaintext = XTS_decrypt(data_key, tweak, ciphertext)
```

### 3.2 Data Path Integration

**Write path**:
```
host_data -> controller_encrypt(data_key, tweak, plaintext) -> ciphertext -> NAND_write
```

**Read path**:
```
NAND_read -> ciphertext -> controller_decrypt(data_key, tweak, ciphertext) -> plaintext -> host
```

### 3.3 Simulation Implementation

Actual AES is not required for simulation purposes. The implementation uses an XOR-based placeholder with the same interface:

```c
struct crypto_ctx {
    uint8_t data_key[32];    /* 256-bit data encryption key */
    uint8_t tweak_key[32];   /* 256-bit tweak key */
    bool    active;          /* true = encryption enabled for this NS */
    uint32_t nsid;           /* associated namespace */
};

/*
 * Simulation-grade "encryption": XOR data with key-derived stream.
 * Same interface as real AES-XTS; swappable with real crypto library.
 */
void crypto_xts_encrypt(const struct crypto_ctx *ctx, uint64_t sector_num,
                         const uint8_t *plaintext, uint8_t *ciphertext, uint32_t len) {
    /* Generate tweak from sector number */
    uint8_t tweak[16];
    memset(tweak, 0, sizeof(tweak));
    memcpy(tweak, &sector_num, 8);
    /* XOR with key-derived stream (simulation placeholder) */
    for (uint32_t i = 0; i < len; i++)
        ciphertext[i] = plaintext[i] ^ ctx->data_key[i % 32] ^ tweak[i % 16];
}

void crypto_xts_decrypt(const struct crypto_ctx *ctx, uint64_t sector_num,
                         const uint8_t *ciphertext, uint8_t *plaintext, uint32_t len) {
    /* XTS decrypt is identical to encrypt (XOR is self-inverse) */
    crypto_xts_encrypt(ctx, sector_num, ciphertext, plaintext, len);
}
```

---

## 4. Key Hierarchy

### 4.1 Three-Level Hierarchy

```
Level 1: Master Key (MK)
    |
    +-- stored in NOR Flash protected partition
    +-- loaded at boot
    +-- never exposed outside the controller
    |
Level 2: Namespace Key Encryption Key (KEK)
    |
    +-- one per namespace
    +-- derived: KEK = HKDF_sim(MK, NSID)
    +-- used only to wrap/unwrap DEKs
    |
Level 3: Data Encryption Key (DEK)
    |
    +-- one per namespace
    +-- randomly generated at namespace creation
    +-- wrapped with KEK for persistent storage
    +-- used for AES-XTS data encryption/decryption
```

### 4.2 Key Table

```c
#define MAX_NS 32

enum key_state {
    KEY_STATE_EMPTY     = 0,
    KEY_STATE_ACTIVE    = 1,
    KEY_STATE_LOCKED    = 2,
    KEY_STATE_DESTROYED = 3,
};

struct key_entry {
    uint8_t  wrapped_dek[48];  /* DEK encrypted with KEK (32B DEK + 16B IV/tag) */
    uint32_t nsid;
    enum key_state state;
    uint8_t  reserved[8];
};

struct key_table {
    struct key_entry entries[MAX_NS];
    uint32_t version;          /* incremented on each modification */
    uint32_t crc32;            /* CRC of entire table */
};
```

### 4.3 HKDF Simulation

```c
/* Simulated HKDF: derive KEK from MK and NSID */
void hkdf_sim(const uint8_t mk[32], uint32_t nsid, uint8_t kek_out[32]) {
    /* Simple derivation for simulation (not cryptographically secure) */
    for (int i = 0; i < 32; i++)
        kek_out[i] = mk[i] ^ (uint8_t)(nsid + i);
}
```

### 4.4 DEK Wrap/Unwrap

```c
void dek_wrap(const uint8_t kek[32], const uint8_t dek[32], uint8_t wrapped[48]) {
    /* Simulation: XOR DEK with KEK, append 16-byte verification tag */
    for (int i = 0; i < 32; i++)
        wrapped[i] = dek[i] ^ kek[i];
    /* Tag = CRC32(dek) repeated 4 times */
    uint32_t tag = crc32(dek, 32);
    memcpy(wrapped + 32, &tag, 4);
    memcpy(wrapped + 36, &tag, 4);
    memcpy(wrapped + 40, &tag, 4);
    memcpy(wrapped + 44, &tag, 4);
}

int dek_unwrap(const uint8_t kek[32], const uint8_t wrapped[48], uint8_t dek_out[32]) {
    for (int i = 0; i < 32; i++)
        dek_out[i] = wrapped[i] ^ kek[i];
    /* Verify tag */
    uint32_t expected_tag = crc32(dek_out, 32);
    uint32_t stored_tag;
    memcpy(&stored_tag, wrapped + 32, 4);
    return (expected_tag == stored_tag) ? 0 : -EACCES;
}
```

---

## 5. TCG Opal SSC Simulation

### 5.1 Locking Ranges

Locking ranges map to namespaces:
- Range 0 = Global (all namespaces)
- Range N = Namespace N

```c
struct opal_locking_range {
    uint32_t range_id;        /* 0 = global, N = NS N */
    bool     read_locked;     /* true = reads return zeros */
    bool     write_locked;    /* true = writes rejected */
    bool     read_lock_enabled;
    bool     write_lock_enabled;
    uint64_t start_lba;       /* range start (0 for entire NS) */
    uint64_t length_lba;      /* range length (0 = entire NS) */
};
```

### 5.2 Basic Commands

| Command | Description |
|---------|-------------|
| StartSession | Open a session with specified SP (AdminSP or LockingSP) |
| Authenticate | Verify credentials for specified authority |
| Set | Modify locking range read/write lock state |
| Get | Query locking range state |
| RevertSP | Reset the Security Provider to factory defaults |

### 5.3 Authority Management

```c
#define OPAL_MAX_USERS 9  /* Admin1 + User1-User8 */

struct opal_authority {
    char     name[16];        /* "Admin1", "User1", ..., "User8" */
    uint8_t  password_hash[32]; /* Simulated hash of password */
    bool     enabled;
    bool     authenticated;   /* true if currently authenticated in session */
    uint32_t allowed_ranges;  /* bitmask of accessible locking ranges */
};

struct opal_ctx {
    struct opal_locking_range ranges[MAX_NS + 1]; /* range 0 + NS ranges */
    struct opal_authority     authorities[OPAL_MAX_USERS];
    bool     session_active;
    uint32_t session_sp;      /* 0 = AdminSP, 1 = LockingSP */
    uint32_t session_auth;    /* authenticated authority index */
    bool     mbr_shadow_active; /* true = return zeros for locked ranges */
    pthread_mutex_t lock;
};
```

### 5.4 MBR Shadowing

When a locking range is read-locked, reads to that range return zeros (MBR shadow). This simulates the pre-boot authentication requirement:

```c
int opal_check_read_access(struct opal_ctx *ctx, uint32_t nsid, uint64_t lba) {
    uint32_t range_id = nsid; /* range N = NS N */
    if (ctx->ranges[range_id].read_locked && ctx->mbr_shadow_active)
        return -EACCES;  /* Return zeros to host */
    return 0;  /* Allow read */
}
```

---

## 6. Secure Erase Flows

### 6.1 Crypto Erase

Destroy the DEK for target namespace; generate new DEK; all old data becomes unreadable:

```c
int crypto_erase(struct key_table *kt, struct crypto_ctx *ctx, uint32_t nsid) {
    /* 1. Destroy old DEK */
    memset(&kt->entries[nsid].wrapped_dek, 0, 48);
    kt->entries[nsid].state = KEY_STATE_DESTROYED;

    /* 2. Generate new random DEK */
    uint8_t new_dek[32];
    generate_random_key(new_dek, 32);

    /* 3. Wrap new DEK with KEK */
    uint8_t kek[32];
    hkdf_sim(master_key, nsid, kek);
    dek_wrap(kek, new_dek, kt->entries[nsid].wrapped_dek);
    kt->entries[nsid].state = KEY_STATE_ACTIVE;

    /* 4. Update crypto context */
    memcpy(ctx->data_key, new_dek, 32);

    /* 5. Persist updated key table to NOR */
    key_table_persist(kt);

    /* Old data encrypted with old DEK is now unreadable */
    return 0;
}
```

### 6.2 Block Erase

Issue NAND erase to all blocks belonging to target namespace:

```c
int block_erase_namespace(uint32_t nsid) {
    /* Iterate all blocks mapped to this namespace and erase them */
    for_each_block_in_namespace(nsid, block) {
        nand_erase_block(block);
    }
    return 0;
}
```

### 6.3 Sanitize

Both crypto erase AND block erase for maximum security:

```c
int sanitize(struct key_table *kt, struct crypto_ctx *ctx, uint32_t nsid) {
    crypto_erase(kt, ctx, nsid);
    block_erase_namespace(nsid);
    return 0;
}
```

### 6.4 NVMe Format with SES Field

| SES Value | Action |
|-----------|--------|
| 0 | No secure erase |
| 1 | User data erase (block erase) |
| 2 | Crypto erase (DEK destroy + regenerate) |

```c
int nvme_format_ses(uint32_t nsid, uint8_t ses) {
    switch (ses) {
        case 0: return format_namespace(nsid);          /* no erase */
        case 1: return block_erase_namespace(nsid);     /* user data erase */
        case 2: return crypto_erase(kt, ctx, nsid);     /* crypto erase */
        default: return -EINVAL;
    }
}
```

---

## 7. Key Storage in NOR Flash

### 7.1 Protected Partition Layout

```
Offset  Size    Field
0       4       magic[4]          = "KEYS"
4       4       version[4]        = partition format version
8       48      mk_wrapped[48]    = Master Key (wrapped with device-unique key)
56      2048    key_table[MAX_NS * 64]  = key entries
2104    4       crc32[4]          = CRC of bytes 0-2103
```

Total: 2108 bytes, fits within one 64KB NOR sector.

### 7.2 Sector Erase Alignment

The key partition is aligned to NOR sector boundaries. Writes use the read-modify-erase-reprogram cycle from LLD_14.

### 7.3 Dual-Copy Power-Safe Updates

To protect against torn writes during key table updates:

```c
int key_table_persist(struct key_table *kt) {
    /* 1. Write new copy to slot B */
    kt->version++;
    kt->crc32 = crc32(kt, sizeof(*kt) - 4);
    nor_partition_write(NOR_PART_CONFIG, KEY_SLOT_B_OFFSET, kt, sizeof(*kt));

    /* 2. Verify slot B write */
    struct key_table verify;
    nor_partition_read(NOR_PART_CONFIG, KEY_SLOT_B_OFFSET, &verify, sizeof(verify));
    if (verify.crc32 != kt->crc32) return -EIO;

    /* 3. Invalidate slot A (write new copy there too) */
    nor_partition_write(NOR_PART_CONFIG, KEY_SLOT_A_OFFSET, kt, sizeof(*kt));

    return 0;
}

int key_table_load(struct key_table *kt) {
    struct key_table a, b;
    bool a_valid = (nor_read_and_verify_crc(KEY_SLOT_A_OFFSET, &a) == 0);
    bool b_valid = (nor_read_and_verify_crc(KEY_SLOT_B_OFFSET, &b) == 0);

    if (a_valid && b_valid)
        *kt = (a.version >= b.version) ? a : b;  /* use newer */
    else if (a_valid) *kt = a;
    else if (b_valid) *kt = b;
    else return -ENODATA;  /* both corrupt */

    return 0;
}
```

---

## 8. Secure Boot Chain

### 8.1 Verification Flow

```
ROM (immutable) -> verify bootloader signature -> verify FW slot signature -> boot FW
```

### 8.2 Signature Simulation

Real RSA/ECDSA is not needed for simulation. CRC-based signature with the same flow:

```c
struct fw_signature {
    uint32_t magic;         /* "SIGN" */
    uint32_t crc32;         /* CRC of firmware image */
    uint32_t fw_version;
    uint8_t  reserved[20];
};

bool secure_boot_verify(const uint8_t *image, uint32_t size,
                         const struct fw_signature *sig) {
    if (sig->magic != 0x5349474E) return false;  /* "SIGN" */
    uint32_t computed_crc = crc32(image, size);
    return (computed_crc == sig->crc32);
}
```

### 8.3 Boot Abort on Verification Failure

If signature verification fails, the system enters recovery mode:

```c
int secure_boot_init(struct boot_ctx *ctx) {
    struct fw_signature sig;
    nor_partition_read(NOR_PART_FW_SLOT_A, SIG_OFFSET, &sig, sizeof(sig));

    if (!secure_boot_verify(fw_image_a, fw_size_a, &sig)) {
        /* Try Slot B */
        nor_partition_read(NOR_PART_FW_SLOT_B, SIG_OFFSET, &sig, sizeof(sig));
        if (!secure_boot_verify(fw_image_b, fw_size_b, &sig)) {
            log_error("secure boot: both slots failed verification");
            return -ENODEV;  /* enter recovery mode */
        }
    }
    return 0;
}
```

---

## 9. Architecture Decision Records

### ADR-001: XOR-Based Crypto Simulation

**Context**: Real AES-XTS is computationally expensive and unnecessary for firmware path testing.

**Decision**: Use XOR-based placeholder encryption with identical interfaces to real AES-XTS. The crypto_ctx structure and function signatures match exactly, allowing drop-in replacement with a real crypto library (e.g., OpenSSL) for hardware validation.

**Rationale**: The simulation goal is to test the encryption data path, key management, and locking behavior, not to validate cryptographic strength. XOR provides the same data transformation semantics (encrypt/decrypt are inverse operations) with negligible CPU overhead.

### ADR-002: Three-Level Key Hierarchy

**Context**: Enterprise SSDs use hierarchical key management to enable per-namespace crypto erase without exposing the master key.

**Decision**: Implement MK -> KEK -> DEK hierarchy matching TCG Enterprise/Opal specifications. MK stored in NOR, KEK derived per-namespace, DEK randomly generated and wrapped with KEK.

**Rationale**: This hierarchy enables crypto erase (destroy DEK, generate new one) without touching MK or other namespaces' keys. It also enables key rotation per-namespace without full-device re-encryption.

### ADR-003: CRC-Based Secure Boot Simulation

**Context**: Real secure boot uses RSA/ECDSA signatures which require a PKI infrastructure.

**Decision**: Use CRC32 as the signature mechanism. Same verification flow (ROM -> bootloader -> firmware) but with CRC instead of RSA.

**Rationale**: The simulation tests the boot chain control flow (fallback to Slot B on failure, recovery mode on both failures) rather than cryptographic signature validity. CRC provides a simple, deterministic verification mechanism suitable for testing.

---

## 10. Test Plan

| Test ID | Description | Verification Point |
|---------|-------------|-------------------|
| SE-001 | AES-XTS encrypt then decrypt | Plaintext recovered exactly |
| SE-002 | Different sector numbers produce different ciphertext | Same plaintext, different sectors -> different output |
| SE-003 | Encryption disabled namespace | Data written/read without transformation |
| SE-004 | Key hierarchy: MK -> KEK derivation | KEK differs per NSID |
| SE-005 | DEK wrap then unwrap | Recovered DEK matches original |
| SE-006 | DEK unwrap with wrong KEK | Returns -EACCES |
| SE-007 | Key table persist and reload | All entries match after restart |
| SE-008 | Key table dual-copy: corrupt slot A | Loads from slot B successfully |
| SE-009 | Key table both slots corrupt | Returns -ENODATA |
| SE-010 | Crypto erase: old data unreadable | Read after crypto erase returns different data |
| SE-011 | Crypto erase: new writes readable | New data written after crypto erase is readable |
| SE-012 | Block erase: all NS blocks erased | All blocks for NS read as 0xFF |
| SE-013 | Sanitize: crypto + block erase | Both DEK destroyed and blocks erased |
| SE-014 | NVMe Format SES=1 | Block erase performed |
| SE-015 | NVMe Format SES=2 | Crypto erase performed |
| SE-016 | Opal locking range: read lock | Reads to locked range return zeros |
| SE-017 | Opal locking range: write lock | Writes to locked range rejected |
| SE-018 | Opal authentication: correct password | Session authenticated |
| SE-019 | Opal authentication: wrong password | Session rejected |
| SE-020 | Opal authority: User1 accesses allowed range | Access granted |
| SE-021 | Opal authority: User1 accesses disallowed range | Access denied |
| SE-022 | MBR shadowing: locked range returns zeros | Host receives zero-filled data |
| SE-023 | Secure boot: valid signature | Boot proceeds normally |
| SE-024 | Secure boot: Slot A invalid, Slot B valid | Falls back to Slot B |
| SE-025 | Secure boot: both invalid | Enters recovery mode (-ENODEV) |
| SE-026 | Key table power-safe update | Power loss during update: valid slot remains |
| SE-027 | Concurrent encryption: multi-thread | No data corruption; thread-safe |

---

**Document Statistics**:
- Requirements covered: 7 (REQ-159 through REQ-165)
- New header files: `include/controller/security.h`, `include/controller/opal.h`
- New source files: `src/controller/crypto_sim.c`, `src/controller/key_mgmt.c`, `src/controller/opal_sim.c`, `src/controller/secure_boot.c`
- Function interfaces: 35+
- Test cases: 27

## Appendix: Cross-References

| Reference | Document |
|-----------|----------|
| NOR Flash key storage | LLD_14_NOR_FLASH |
| Bootloader verification | LLD_09_BOOTLOADER |
| NVMe Format command | LLD_01_PCIE_NVMe_EMULATION |
| T10 DIF/PI data integrity | LLD_11_FTL_RELIABILITY |
| Power loss during key update | LLD_17_POWER_LOSS_PROTECTION |
