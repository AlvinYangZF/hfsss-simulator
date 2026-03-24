# FTL Reliability Mechanisms Low-Level Design

## Revision History

| Version | Date       | Author | Description                        |
|---------|------------|--------|------------------------------------|
| V1.0    | 2026-03-15 | HFSSS  | Initial release                    |
| V1.1    | 2026-03-23 | HFSSS  | Added T10 DIF/PI Integration section |

## Table of Contents

1. [Module Overview](#1-module-overview)
2. [Requirements Traceability](#2-requirements-traceability)
3. [Data Structure Design](#3-data-structure-design)
4. [Header File Design](#4-header-file-design)
5. [Function Interface Design](#5-function-interface-design)
6. [Flow Diagrams](#6-flow-diagrams)
7. [T10 DIF/PI Integration](#7-t10-difpi-integration)
8. [Integration Notes](#8-integration-notes)
9. [Test Plan](#9-test-plan)

---

## 1. Module Overview

The FTL Reliability module provides a comprehensive data reliability assurance system for the simulator, covering five major functional domains: command lifecycle tracking, read/write retry, multi-level flow control, data redundancy protection, and NVMe error reporting. This module does not alter the main I/O data path logic but embeds itself as hook points, providing observable and verifiable reliability behavior under fault injection, stress testing, and abnormal scenarios.

**REQ-110 -- Command State Machine**: Maintains an independent state context (`ftl_cmd_ctx`) for each FTL command, with state sequence `RECEIVED -> PARSING -> L2P_LOOKUP -> NAND_QUEUED -> EXECUTING -> ECC_CHECK -> COMPLETE` (or `ERROR`). Each state transition records a nanosecond-precision timestamp for latency analysis and trace export.

**REQ-111 -- Read Retry Mechanism**: On read failure, FTL first attempts soft-decision LDPC decoding, then applies up to 15 voltage offsets sequentially, re-reading and decoding the same physical page. If all retries fail, it is treated as an uncorrectable error (UCE), an entry is appended to the NVMe error log page, and the SMART `media_errors` counter is updated.

**REQ-112 -- Write Retry and Write Verification**: On program failure, FTL retries up to `WRITE_RETRY_MAX_ATTEMPTS` (3) times within the same block; after the first failure, it synchronously allocates a new target page (`backup_ppn`) via `ftl_alloc_page()` in a spare block. If write verification is enabled (`verify_after_write = true`), data is read back and compared after each successful program.

**REQ-113 -- Multi-Level Flow Control**: Two layers: (1) per-namespace token bucket (`ns_flow_ctrl`) limiting bandwidth per NS; (2) NAND channel queue depth limit (`channel_flow_ctrl`) preventing new commands when the channel queue is full.

**REQ-114 -- RAID-like Data Protection**: L2P table maintains dual copies (DRAM primary + file shadow), shadow synced every 64MB of host writes. BBT uses NOR Flash dual-mirror (slot_a / slot_b). Optional die-level XOR parity protection.

**REQ-115 -- NVMe Error Handling**: Complete NVMe status code system (SCT/SC), error log page (Log Page 0x03) as 64-entry ring buffer, thread-safe append with monotonically increasing `error_count`.

**Requirements Coverage**: REQ-110, REQ-111, REQ-112, REQ-113, REQ-114, REQ-115.

---

## 2. Requirements Traceability

| Req ID  | Description | Priority | Target |
|---------|-------------|----------|--------|
| REQ-110 | Command state machine with ns-precision timestamps | P0 | V2.0 |
| REQ-111 | Read retry: soft LDPC + 15 voltage offsets; UCE -> error log + SMART | P0 | V2.0 |
| REQ-112 | Write retry: 3 attempts, backup block allocation, optional write verify | P0 | V2.0 |
| REQ-113 | Multi-level flow control: per-NS token bucket + channel queue depth | P0 | V2.0 |
| REQ-114 | RAID-like protection: dual L2P, dual BBT NOR mirror, optional XOR parity | P0 | V2.0 |
| REQ-115 | NVMe error handling: SCT/SC codes, error log page 0x03, SMART sync | P0 | V2.0 |

---

## 3. Data Structure Design

### 3.1 Command State Machine (REQ-110)

```c
enum ftl_cmd_state {
    FTL_CMD_RECEIVED = 0, FTL_CMD_PARSING, FTL_CMD_L2P_LOOKUP,
    FTL_CMD_NAND_QUEUED, FTL_CMD_EXECUTING, FTL_CMD_ECC_CHECK,
    FTL_CMD_COMPLETE, FTL_CMD_ERROR, FTL_CMD_STATE_COUNT
};

struct ftl_cmd_ctx {
    uint32_t cmd_id;
    uint16_t sq_id;
    enum ftl_cmd_state state;
    uint64_t lba;
    uint32_t len_sectors;
    uint64_t submit_ns;
    uint64_t state_enter_ns[FTL_CMD_STATE_COUNT];
    int      status_code;
    void    *priv;
};
```

### 3.2 Read Retry (REQ-111)

```c
#define READ_RETRY_MAX_ATTEMPTS 15

struct read_retry_ctx {
    uint8_t  attempt;
    int8_t   voltage_offsets[READ_RETRY_MAX_ATTEMPTS];
    bool     soft_ldpc_attempted;
    uint64_t ppn;
    uint8_t *buf;
};
```

### 3.3 Write Retry (REQ-112)

```c
#define WRITE_RETRY_MAX_ATTEMPTS 3

struct write_retry_ctx {
    uint8_t  attempt;
    uint64_t original_ppn;
    uint64_t backup_ppn;
    bool     verify_after_write;
};
```

### 3.4 Multi-Level Flow Control (REQ-113)

```c
struct ns_flow_ctrl {
    uint32_t ns_id;
    uint64_t tokens;
    uint64_t token_rate_per_sec;
    uint64_t token_max;
    uint64_t last_refill_ns;
    pthread_mutex_t lock;
};

struct channel_flow_ctrl {
    uint8_t  channel_id;
    uint32_t queue_depth_max;
    _Atomic uint32_t queue_depth_current;
};

struct ftl_flow_ctrl {
    struct ns_flow_ctrl      ns[MAX_NAMESPACES];
    struct channel_flow_ctrl ch[MAX_CHANNELS];
};
```

### 3.5 RAID-like Data Protection (REQ-114)

```c
struct l2p_redundancy {
    uint64_t *primary;
    uint64_t *shadow;
    uint64_t  shadow_seq;
    pthread_rwlock_t lock;
};

struct bbt_redundancy {
    uint8_t  *slot_a;
    uint8_t  *slot_b;
    uint32_t  crc_a;
    uint32_t  crc_b;
};

struct xor_parity_group {
    uint8_t  die_count;
    uint64_t parity_ppn;
    uint64_t data_ppns[8];
};
```

### 3.6 NVMe Error Log Page (REQ-115)

```c
struct nvme_error_log_entry {
    uint64_t error_count;
    uint16_t sq_id;
    uint16_t cmd_id;
    uint16_t status_field;
    uint16_t param_error_loc;
    uint64_t lba;
    uint32_t ns;
    uint8_t  vs_info;
    uint8_t  trtype;
    uint8_t  reserved[2];
    uint64_t cs;
    uint16_t trtype_spec_info;
    uint8_t  reserved2[22];
} __attribute__((packed));

#define NVME_ERROR_LOG_ENTRIES 64

struct nvme_error_log {
    struct nvme_error_log_entry entries[NVME_ERROR_LOG_ENTRIES];
    uint32_t head;
    uint32_t count;
    pthread_mutex_t lock;
};
```

---

## 4. Header File Design

Full header path: `include/ftl/ftl_reliability.h`

Key function prototypes:

```c
struct ftl_cmd_ctx *ftl_cmd_alloc(uint32_t cmd_id, uint16_t sq_id);
void ftl_cmd_free(struct ftl_cmd_ctx *ctx);
int ftl_cmd_transition(struct ftl_cmd_ctx *ctx, enum ftl_cmd_state next_state);

int read_retry_execute(struct read_retry_ctx *ctx, uint64_t ppn, uint8_t *buf, uint32_t len);
int write_retry_execute(struct write_retry_ctx *ctx, uint64_t ppn, const uint8_t *data, uint32_t len);

int ns_flow_ctrl_acquire(struct ftl_flow_ctrl *fc, uint32_t ns_id, uint64_t bytes);
void ns_flow_ctrl_refill(struct ftl_flow_ctrl *fc, uint32_t ns_id);
bool channel_flow_ctrl_check(const struct ftl_flow_ctrl *fc, uint8_t ch_id);
void channel_flow_ctrl_inc(struct ftl_flow_ctrl *fc, uint8_t ch_id);
void channel_flow_ctrl_dec(struct ftl_flow_ctrl *fc, uint8_t ch_id);

int l2p_shadow_sync(struct l2p_redundancy *red);
uint32_t l2p_redundancy_check(const struct l2p_redundancy *red);
int l2p_restore_from_shadow(struct l2p_redundancy *red);
int bbt_write_both_slots(struct bbt_redundancy *red, const uint8_t *bbt, uint32_t len);
int bbt_verify_and_repair(struct bbt_redundancy *red, uint32_t len);
int xor_parity_compute_and_write(struct xor_parity_group *grp);
int xor_parity_recover(struct xor_parity_group *grp, uint8_t failed_die_idx, uint64_t dst_ppn);

void nvme_error_log_append(struct nvme_error_log *log, const struct nvme_error_log_entry *entry);
uint32_t nvme_error_log_get_page(const struct nvme_error_log *log,
                                 struct nvme_error_log_entry *buf, uint32_t num_entries);
```

---

## 5. Function Interface Design

### 5.1 Command State Machine

- `ftl_cmd_alloc`: Acquire from pool, set state=RECEIVED, record timestamp
- `ftl_cmd_free`: Assert COMPLETE or ERROR state, return to pool
- `ftl_cmd_transition`: Validate legal transition, update state and timestamp

### 5.2 Read Retry: `read_retry_execute`

1. Attempt initial `hal_nand_read` -> success returns 0
2. Try soft-decision LDPC decode
3. Loop 15 voltage offset retries, each with read + ECC check
4. On UCE: append to nvme_error_log, increment SMART media_errors, return -EIO

### 5.3 Write Retry: `write_retry_execute`

1. Attempt program at target_ppn
2. If verify_after_write: read back and compare
3. On first failure: allocate backup_ppn via ftl_alloc_page()
4. After 3 failures: mark original block bad, return -EIO

### 5.4 Namespace Token Bucket

Token acquisition blocks via condition-wait when depleted; timer thread periodically refills based on elapsed time and configured rate.

### 5.5 L2P Shadow Sync

Copies primary to shadow under write lock, increments shadow_seq, computes CRC32, msync to disk.

### 5.6 BBT Dual-Slot Write and Repair

Writes to both NOR slots with CRC; on verification, repairs corrupted slot from valid one. Both corrupt returns -EIO.

### 5.7 NVMe Error Log

Ring buffer append under mutex; Get Page copies most recent entries in reverse chronological order.

---

## 6. Flow Diagrams

### 6.1 Read Retry Decision Tree

```
hal_nand_read(ppn)
    |
    +-- Success -> return 0
    +-- Failure -> Soft LDPC decode
                    |
                    +-- Success -> return 0
                    +-- Failure -> Voltage offset retry loop (15 attempts)
                                    |
                                    +-- ECC OK -> restore voltage, return 0
                                    +-- All fail -> UCE: error_log_append
                                                        + smart_increment
                                                        + return -EIO
```

### 6.2 Write Retry and Backup Block Allocation

```
write_retry_execute(ppn, data)
    |
    +-- program(target_ppn) -> Success?
    |   +-- Yes + verify? -> readback match? -> Update L2P, return 0
    |   +-- No -> attempt==0? -> allocate backup_ppn, retry
    |             attempt >= 3 -> bbt_mark_block_bad, return -EIO
```

---

## 7. T10 DIF/PI Integration

### 7.1 Overview

T10 Data Integrity Field (DIF) / Protection Information (PI) provides end-to-end data integrity verification between the host and the storage media. The HFSSS simulator implements PI Type 1, Type 2, and Type 3 to enable enterprise-grade data integrity testing.

### 7.2 PI Format Definitions

```c
struct t10_pi_tuple {
    u16 guard;      /* CRC-16 over the user data sector (512B or 4KB) */
    u16 app_tag;    /* Application tag (user-defined) */
    u32 ref_tag;    /* Reference tag (typically LBA-derived) */
};
```

**PI Types**:

| Type | Guard Tag | Application Tag | Reference Tag |
|------|-----------|-----------------|---------------|
| Type 1 | CRC-16 of data sector | User-defined, checked if non-0xFFFF | Must equal LBA & 0xFFFFFFFF; increments per sector |
| Type 2 | CRC-16 of data sector | User-defined, checked if non-0xFFFF | Must equal Initial Ref Tag from command + sector offset |
| Type 3 | CRC-16 of data sector | User-defined, checked if non-0xFFFF | Not checked (opaque to controller) |

### 7.3 PI Insertion Point in Write Path

PI is inserted by the controller **before** the FTL processes the write:

```
Host write command (with PRACT/PRCHK bits set in CDW12)
    |
    v
Controller: pi_generate(data, lba, len) -> compute CRC-16, fill ref_tag from LBA
    |
    v
FTL: L2P lookup, allocate target page
    |
    v
NAND program: write user_data + PI tuple to page (PI stored in OOB area)
```

### 7.4 PI Verification Point in Read Path

PI is verified by the controller **after** the FTL completes the read:

```
FTL: L2P lookup -> NAND read (user_data + PI from OOB)
    |
    v
Controller: pi_verify(data, pi_tuple, expected_lba)
    |
    +-- Guard tag CRC mismatch -> NVMe status: Guard Check Error (SCT=2, SC=0x82)
    +-- Reference tag mismatch -> NVMe status: Reference Tag Check Error (SCT=2, SC=0x83)
    +-- Application tag mismatch -> NVMe status: Application Tag Check Error (SCT=2, SC=0x84)
    +-- All pass -> return data to host
```

### 7.5 PI Handling During GC

During garbage collection, PI metadata must be preserved when migrating valid pages:

```
GC valid page migration:
  1. Read source page: user_data + PI tuple from OOB
  2. DO NOT regenerate PI (preserve original guard/ref/app tags)
  3. Write to destination page: user_data + original PI tuple
  4. Update L2P mapping only
  5. PI integrity verified on next host read
```

This ensures that PI metadata remains consistent with the original write, even after GC relocation.

### 7.6 PI Error Reporting

| Error Condition | NVMe Status Code Type (SCT) | Status Code (SC) | Description |
|----------------|----------------------------|-------------------|-------------|
| Guard tag CRC mismatch | 0x02 (Media/Data Integrity) | 0x82 | End-to-end Guard Check Error |
| Reference tag mismatch | 0x02 (Media/Data Integrity) | 0x83 | End-to-end Reference Tag Check Error |
| Application tag mismatch | 0x02 (Media/Data Integrity) | 0x84 | End-to-end Application Tag Check Error |

All PI check failures are also appended to the NVMe error log (Log Page 0x03) and increment the SMART `media_errors` counter.

### 7.7 PI Data Structures and API

```c
/* PI configuration per namespace */
struct ns_pi_config {
    uint8_t  pi_type;        /* 0=disabled, 1=Type1, 2=Type2, 3=Type3 */
    bool     pi_enabled;
    uint16_t guard_seed;     /* Initial CRC-16 seed (typically 0xFFFF) */
};

/* PI generation: called on write path before FTL */
int pi_generate(struct t10_pi_tuple *pi_out, const uint8_t *data,
                uint32_t data_len, uint64_t lba, uint8_t pi_type);

/* PI verification: called on read path after FTL */
int pi_verify(const struct t10_pi_tuple *pi, const uint8_t *data,
              uint32_t data_len, uint64_t expected_lba, uint8_t pi_type);

/* CRC-16 computation (T10 DIF standard polynomial x^16+x^15+x^11+x^1+1) */
uint16_t t10_pi_crc16(const uint8_t *data, uint32_t len, uint16_t seed);
```

---

## 8. Integration Notes

- `nand.c` calls `read_retry_execute()` transparently on ECC failure
- `ftl.c` calls `write_retry_execute()` on program failure, handling backup allocation internally
- Flow control gates are checked before NAND queue submission
- L2P shadow sync triggered every 64MB of host writes on background thread
- NVMe error log maintained globally, queried via Get Log Page (LID=0x03)
- PI generation/verification integrated into the controller command processing pipeline

---

## 9. Test Plan

| Test ID | Description | Verification Point |
|---------|-------------|-------------------|
| FR-001 | ECC error triggers read retry | attempt increments to 15; returns -EIO |
| FR-002 | Soft LDPC succeeds on first try | soft_ldpc_attempted==true; return 0 |
| FR-003 | Read retry succeeds at attempt 7 | return 0; attempt==6; voltage restored |
| FR-004 | All 15 retries fail (UCE) | error_log.count +1; media_errors incremented |
| FR-005 | UCE error log entry completeness | status_field SCT=2, SC=0x81 correct |
| FR-006 | Write retry: backup block allocated on first failure | backup_ppn != PPN_INVALID |
| FR-007 | Write retry: backup succeeds | L2P updated to backup_ppn; return 0 |
| FR-008 | Write retry 3x all fail | bbt_mark_block_bad called; return -EIO |
| FR-009 | Write verify: readback mismatch triggers retry | Next attempt incremented |
| FR-010 | Token bucket: over-rate commands blocked | ns_flow_ctrl_acquire blocks > 0 |
| FR-011 | Token bucket: timer refill resumes | Command resumes within 1ms |
| FR-012 | Token bucket: multi-namespace isolation | NS-1 overload does not affect NS-2 |
| FR-013 | Channel queue full blocks new commands | channel_flow_ctrl_check returns false |
| FR-014 | Channel command completion resumes waiting | Waiting command dispatched after dec() |
| FR-015 | L2P shadow sync passes verification | l2p_redundancy_check() returns 0 |
| FR-016 | L2P primary corruption detected | l2p_redundancy_check() returns mismatches > 0 |
| FR-017 | L2P restore from shadow succeeds | l2p_redundancy_check() returns 0 after restore |
| FR-018 | BBT slot_a corrupt, repaired from slot_b | bbt_verify_and_repair() returns 0 |
| FR-019 | BBT both slots corrupt | bbt_verify_and_repair() returns -EIO |
| FR-020 | Error log entry 65 wraps ring | head wraps; count==65 |
| FR-021 | Error log Get Page order | Most recent first |
| FR-022 | Command state machine illegal transition | ftl_cmd_transition returns -EINVAL |
| FR-023 | State timestamps monotonically increasing | Each timestamp > previous |
| FR-024 | PI Type 1: guard tag CRC mismatch detected | Status SCT=2, SC=0x82 returned |
| FR-025 | PI Type 1: reference tag mismatch detected | Status SCT=2, SC=0x83 returned |
| FR-026 | PI preserved during GC migration | Read after GC: PI verification passes |
| FR-027 | PI disabled namespace: no PI checks | Reads succeed without PI verification |

---

**Document Statistics**:
- Requirements covered: 6 (REQ-110 through REQ-115) + T10 DIF/PI integration
- New header files: `include/ftl/ftl_reliability.h`
- Function interfaces: 35+
- Test cases: 27 (FR-001 through FR-027)

## Appendix: Cross-References

| Reference | Document |
|-----------|----------|
| NAND media layer | LLD_03_NAND_MEDIA |
| OOB error reporting | LLD_07_OOB_MANAGEMENT |
| NOR Flash BBT storage | LLD_14_NOR_FLASH |
| Security encryption | LLD_19_SECURITY_ENCRYPTION |
