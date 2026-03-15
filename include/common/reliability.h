#ifndef HFSSS_RELIABILITY_H
#define HFSSS_RELIABILITY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------
 * REQ-113: Multi-level flow control
 * ------------------------------------------------------------------ */

/* Per-namespace token bucket */
struct ns_token_bucket {
    uint64_t tokens;          /* current tokens */
    uint64_t capacity;        /* max burst */
    uint64_t refill_rate;     /* tokens per second */
    uint64_t last_refill_ns;  /* monotonic timestamp */
    uint32_t nsid;
    bool     initialized;
};

/* NAND channel queue depth limiter */
struct channel_depth_limiter {
    uint32_t max_depth;       /* max outstanding ops per channel */
    uint32_t current_depth;   /* current outstanding ops */
    uint32_t channel_id;
    bool     initialized;
};

int  ns_token_bucket_init(struct ns_token_bucket *tb,
                           uint32_t nsid,
                           uint64_t capacity,
                           uint64_t refill_rate_per_sec);
void ns_token_bucket_cleanup(struct ns_token_bucket *tb);
bool ns_token_bucket_consume(struct ns_token_bucket *tb, uint64_t tokens);
void ns_token_bucket_refill(struct ns_token_bucket *tb);
uint64_t ns_token_bucket_available(const struct ns_token_bucket *tb);

int  channel_depth_limiter_init(struct channel_depth_limiter *lim,
                                 uint32_t channel_id, uint32_t max_depth);
bool channel_depth_acquire(struct channel_depth_limiter *lim);
void channel_depth_release(struct channel_depth_limiter *lim);
bool channel_depth_is_full(const struct channel_depth_limiter *lim);

/* ------------------------------------------------------------------
 * REQ-114: Metadata redundancy
 * ------------------------------------------------------------------ */

#define META_MIRROR_MAGIC  0xFEEDBEEFu

struct meta_mirror_header {
    uint32_t magic;
    uint32_t crc32;         /* CRC of the payload that follows */
    uint64_t sequence;      /* monotonically increasing write counter */
    uint32_t payload_len;
    uint8_t  _pad[12];      /* 32-byte header */
};

/* Write payload to both primary and backup paths atomically (write backup first) */
int  meta_mirror_write(const char *primary_path,
                        const char *backup_path,
                        const void *payload, uint32_t len);

/* Read: prefer primary; fall back to backup on CRC mismatch; sets *used_backup */
int  meta_mirror_read(const char *primary_path,
                       const char *backup_path,
                       void *payload_out, uint32_t maxlen,
                       uint32_t *len_out, bool *used_backup);

/* Verify both copies are consistent; returns HFSSS_OK or error code */
int  meta_mirror_verify(const char *primary_path, const char *backup_path);

/* CRC32 (simple table-based, no external dependency) */
uint32_t crc32_compute(const void *data, size_t len);

/* ------------------------------------------------------------------
 * REQ-136: End-to-end data integrity
 * ------------------------------------------------------------------ */

struct integrity_test_cfg {
    uint64_t capacity_bytes;   /* simulated device capacity */
    uint32_t block_size;       /* typically 4096 */
    uint32_t num_blocks;       /* number of blocks to test */
    uint32_t num_passes;       /* write/read/verify cycles */
    uint64_t seed;             /* PRNG seed for deterministic data */
};

struct integrity_test_result {
    uint64_t blocks_written;
    uint64_t blocks_verified;
    uint64_t blocks_failed;
    uint32_t passes_completed;
    bool     all_passed;
};

/* Run write -> read -> compare using in-memory buffers; simulates end-to-end path */
int  integrity_test_run(const struct integrity_test_cfg *cfg,
                         struct integrity_test_result *result);

/* Simple checksum for buffer comparison (XOR-fold + length) */
uint64_t buf_checksum(const void *buf, size_t len);

#endif /* HFSSS_RELIABILITY_H */
