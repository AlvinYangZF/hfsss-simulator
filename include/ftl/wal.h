#ifndef __HFSSS_WAL_H
#define __HFSSS_WAL_H

#include "common/common.h"

/*
 * Write-Ahead Log (WAL) for crash recovery.
 *
 * Each record is 64 bytes, compatible with the format already parsed
 * in boot.c power_mgmt_recover_wal().
 */

#define WAL_RECORD_MAGIC   0x12345678U
#define WAL_COMMIT_MARKER  0xDEADBEEFU
#define WAL_RECORD_SIZE    64
#define WAL_MAX_RECORDS    (16 * 1024)

/* WAL record types */
enum wal_rec_type {
    WAL_REC_L2P_UPDATE  = 1,
    WAL_REC_TRIM        = 2,
    WAL_REC_BBT_UPDATE  = 3,
    WAL_REC_SMART_UPDATE = 4,
    WAL_REC_COMMIT      = 0xFF,
};

/* WAL record (64 bytes, packed) */
struct wal_record {
    u32 magic;           /* WAL_RECORD_MAGIC */
    u32 type;            /* enum wal_rec_type */
    u64 sequence;        /* monotonic sequence number */
    u64 lba;             /* logical block address */
    u64 ppn;             /* physical page number */
    u8  meta[16];        /* type-specific metadata */
    u32 crc32;           /* CRC-32 over bytes [0..55] */
    u32 end_marker;      /* 0 for data records, WAL_COMMIT_MARKER for commit */
};

/* WAL replay callback: invoked for each valid record during replay */
typedef int (*wal_replay_cb)(const struct wal_record *rec, void *user_data);

/* WAL context */
struct wal_ctx {
    struct wal_record *records;
    u32 capacity;
    u32 count;
    u64 next_sequence;
    bool initialized;
};

/* API */
int  wal_init(struct wal_ctx *ctx, u32 max_records);
void wal_cleanup(struct wal_ctx *ctx);

int  wal_append(struct wal_ctx *ctx, enum wal_rec_type type,
                u64 lba, u64 ppn, const void *meta, u32 meta_len);
int  wal_commit(struct wal_ctx *ctx);

int  wal_replay(const struct wal_ctx *ctx, wal_replay_cb callback,
                void *user_data);

u64  wal_get_committed_seq(const struct wal_ctx *ctx);
int  wal_truncate(struct wal_ctx *ctx, u64 up_to_seq);
u32  wal_get_count(const struct wal_ctx *ctx);
void wal_reset(struct wal_ctx *ctx);

/* File persistence (for clean power cycle integration) */
int  wal_save(const struct wal_ctx *ctx, const char *filepath);
int  wal_load(struct wal_ctx *ctx, const char *filepath);

#endif /* __HFSSS_WAL_H */
