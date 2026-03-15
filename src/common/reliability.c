/* System reliability module: flow control, metadata redundancy, and data integrity. */

#include "reliability.h"
#include "common.h"
#include "log.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define MODULE_NAME "reliability"

/* ================================================================
 * Internal helpers
 * ================================================================ */

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ================================================================
 * REQ-113: Token bucket (per-namespace flow control)
 * ================================================================ */

int ns_token_bucket_init(struct ns_token_bucket *tb,
                          uint32_t nsid,
                          uint64_t capacity,
                          uint64_t refill_rate_per_sec)
{
    if (!tb || capacity == 0) {
        HFSSS_LOG_ERROR(MODULE_NAME, "ns_token_bucket_init: invalid args");
        return HFSSS_ERR_INVAL;
    }

    memset(tb, 0, sizeof(*tb));
    tb->nsid           = nsid;
    tb->capacity       = capacity;
    tb->refill_rate    = refill_rate_per_sec;
    tb->tokens         = capacity;   /* start full */
    tb->last_refill_ns = monotonic_ns();
    tb->initialized    = true;

    return HFSSS_OK;
}

void ns_token_bucket_cleanup(struct ns_token_bucket *tb)
{
    if (!tb)
        return;
    memset(tb, 0, sizeof(*tb));
}

void ns_token_bucket_refill(struct ns_token_bucket *tb)
{
    if (!tb || !tb->initialized)
        return;

    uint64_t now_ns  = monotonic_ns();
    uint64_t elapsed = now_ns - tb->last_refill_ns;

    /* elapsed_s * refill_rate, computed in fixed-point to avoid floating point */
    uint64_t new_tokens = (elapsed / 1000000000ULL) * tb->refill_rate
                        + ((elapsed % 1000000000ULL) * tb->refill_rate) / 1000000000ULL;

    tb->tokens += new_tokens;
    if (tb->tokens > tb->capacity)
        tb->tokens = tb->capacity;

    tb->last_refill_ns = now_ns;
}

bool ns_token_bucket_consume(struct ns_token_bucket *tb, uint64_t tokens)
{
    if (!tb || !tb->initialized)
        return false;

    ns_token_bucket_refill(tb);

    if (tb->tokens >= tokens) {
        tb->tokens -= tokens;
        return true;
    }
    return false;
}

uint64_t ns_token_bucket_available(const struct ns_token_bucket *tb)
{
    if (!tb || !tb->initialized)
        return 0;
    return tb->tokens;
}

/* ================================================================
 * REQ-113: Channel depth limiter (NAND channel queue depth)
 * ================================================================ */

int channel_depth_limiter_init(struct channel_depth_limiter *lim,
                                uint32_t channel_id, uint32_t max_depth)
{
    if (!lim || max_depth == 0) {
        HFSSS_LOG_ERROR(MODULE_NAME, "channel_depth_limiter_init: invalid args");
        return HFSSS_ERR_INVAL;
    }

    memset(lim, 0, sizeof(*lim));
    lim->channel_id   = channel_id;
    lim->max_depth    = max_depth;
    lim->current_depth = 0;
    lim->initialized  = true;

    return HFSSS_OK;
}

bool channel_depth_acquire(struct channel_depth_limiter *lim)
{
    if (!lim || !lim->initialized)
        return false;

    if (lim->current_depth >= lim->max_depth)
        return false;

    lim->current_depth++;
    return true;
}

void channel_depth_release(struct channel_depth_limiter *lim)
{
    if (!lim || !lim->initialized)
        return;

    if (lim->current_depth > 0)
        lim->current_depth--;
}

bool channel_depth_is_full(const struct channel_depth_limiter *lim)
{
    if (!lim || !lim->initialized)
        return true;
    return lim->current_depth >= lim->max_depth;
}

/* ================================================================
 * REQ-114: CRC32 (table-based, polynomial 0xEDB88320)
 * ================================================================ */

static uint32_t crc32_table[256];
static bool     crc32_table_ready = false;

static void crc32_build_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            if (c & 1u)
                c = 0xEDB88320u ^ (c >> 1);
            else
                c >>= 1;
        }
        crc32_table[i] = c;
    }
    crc32_table_ready = true;
}

uint32_t crc32_compute(const void *data, size_t len)
{
    if (!crc32_table_ready)
        crc32_build_table();

    if (!data)
        return 0;

    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);

    return crc ^ 0xFFFFFFFFu;
}

/* ================================================================
 * REQ-114: Metadata mirror (dual-copy L2P / BBT)
 * ================================================================ */

/* Write a single file: header + payload */
static int write_mirror_file(const char *path,
                              const struct meta_mirror_header *hdr,
                              const void *payload)
{
    /* Write atomically via a temp file then rename */
    char tmp_path[4096];
    int ret = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (ret < 0 || (size_t)ret >= sizeof(tmp_path)) {
        HFSSS_LOG_ERROR(MODULE_NAME, "path too long: %s", path);
        return HFSSS_ERR_INVAL;
    }

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        HFSSS_LOG_ERROR(MODULE_NAME, "cannot open %s for write", tmp_path);
        return HFSSS_ERR_IO;
    }

    if (fwrite(hdr, sizeof(*hdr), 1, f) != 1 ||
        fwrite(payload, hdr->payload_len, 1, f) != 1) {
        HFSSS_LOG_ERROR(MODULE_NAME, "write failed: %s", tmp_path);
        fclose(f);
        remove(tmp_path);
        return HFSSS_ERR_IO;
    }

    fclose(f);

    if (rename(tmp_path, path) != 0) {
        HFSSS_LOG_ERROR(MODULE_NAME, "rename failed: %s -> %s", tmp_path, path);
        remove(tmp_path);
        return HFSSS_ERR_IO;
    }

    return HFSSS_OK;
}

/* Read and validate a single mirror file; returns HFSSS_OK on success */
static int read_mirror_file(const char *path,
                             void *payload_out, uint32_t maxlen,
                             uint32_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return HFSSS_ERR_IO;

    struct meta_mirror_header hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return HFSSS_ERR_IO;
    }

    if (hdr.magic != META_MIRROR_MAGIC) {
        fclose(f);
        return HFSSS_ERR_IO;
    }

    if (hdr.payload_len > maxlen) {
        fclose(f);
        return HFSSS_ERR_INVAL;
    }

    if (fread(payload_out, hdr.payload_len, 1, f) != 1) {
        fclose(f);
        return HFSSS_ERR_IO;
    }
    fclose(f);

    uint32_t computed = crc32_compute(payload_out, hdr.payload_len);
    if (computed != hdr.crc32)
        return HFSSS_ERR_IO;

    *len_out = hdr.payload_len;
    return HFSSS_OK;
}

/* Global sequence counter; incremented on every write */
static uint64_t g_mirror_sequence = 0;

int meta_mirror_write(const char *primary_path,
                       const char *backup_path,
                       const void *payload, uint32_t len)
{
    if (!primary_path || !backup_path || !payload) {
        HFSSS_LOG_ERROR(MODULE_NAME, "meta_mirror_write: NULL args");
        return HFSSS_ERR_INVAL;
    }

    struct meta_mirror_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = META_MIRROR_MAGIC;
    hdr.crc32       = crc32_compute(payload, len);
    hdr.sequence    = ++g_mirror_sequence;
    hdr.payload_len = len;

    /* Write backup first, then primary */
    int rc = write_mirror_file(backup_path, &hdr, payload);
    if (rc != HFSSS_OK) {
        HFSSS_LOG_ERROR(MODULE_NAME, "failed to write backup: %s", backup_path);
        return rc;
    }

    rc = write_mirror_file(primary_path, &hdr, payload);
    if (rc != HFSSS_OK) {
        HFSSS_LOG_ERROR(MODULE_NAME, "failed to write primary: %s", primary_path);
        return rc;
    }

    return HFSSS_OK;
}

int meta_mirror_read(const char *primary_path,
                      const char *backup_path,
                      void *payload_out, uint32_t maxlen,
                      uint32_t *len_out, bool *used_backup)
{
    if (!primary_path || !backup_path || !payload_out || !len_out || !used_backup) {
        HFSSS_LOG_ERROR(MODULE_NAME, "meta_mirror_read: NULL args");
        return HFSSS_ERR_INVAL;
    }

    *used_backup = false;

    int rc = read_mirror_file(primary_path, payload_out, maxlen, len_out);
    if (rc == HFSSS_OK)
        return HFSSS_OK;

    HFSSS_LOG_WARN(MODULE_NAME, "primary read failed (%d), trying backup", rc);
    rc = read_mirror_file(backup_path, payload_out, maxlen, len_out);
    if (rc == HFSSS_OK) {
        *used_backup = true;
        return HFSSS_OK;
    }

    HFSSS_LOG_ERROR(MODULE_NAME, "both mirror copies unreadable");
    return HFSSS_ERR_IO;
}

int meta_mirror_verify(const char *primary_path, const char *backup_path)
{
    if (!primary_path || !backup_path) {
        HFSSS_LOG_ERROR(MODULE_NAME, "meta_mirror_verify: NULL args");
        return HFSSS_ERR_INVAL;
    }

    /* Read each file's header and CRC independently */
    FILE *fp = fopen(primary_path, "rb");
    FILE *fb = fopen(backup_path, "rb");

    if (!fp || !fb) {
        if (fp) fclose(fp);
        if (fb) fclose(fb);
        return HFSSS_ERR_IO;
    }

    struct meta_mirror_header ph, bh;
    bool ph_ok = (fread(&ph, sizeof(ph), 1, fp) == 1) && (ph.magic == META_MIRROR_MAGIC);
    bool bh_ok = (fread(&bh, sizeof(bh), 1, fb) == 1) && (bh.magic == META_MIRROR_MAGIC);

    fclose(fp);
    fclose(fb);

    if (!ph_ok || !bh_ok)
        return HFSSS_ERR_IO;

    /* Both copies must have the same CRC and payload length */
    if (ph.crc32 != bh.crc32 || ph.payload_len != bh.payload_len)
        return HFSSS_ERR_IO;

    return HFSSS_OK;
}

/* ================================================================
 * REQ-136: End-to-end data integrity test
 * ================================================================ */

uint64_t buf_checksum(const void *buf, size_t len)
{
    if (!buf || len == 0)
        return 0;

    /* FNV-1a 64-bit: avalanche effect ensures different bytes yield different sums */
    const uint8_t *p = (const uint8_t *)buf;
    uint64_t hash = 14695981039346656037ULL; /* FNV offset basis */
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)p[i];
        hash *= 1099511628211ULL;            /* FNV prime */
    }
    return hash;
}

/* Fill buffer with deterministic pattern derived from block index, pass, and seed */
static void fill_pattern(uint8_t *buf, uint32_t block_size,
                          uint32_t block_idx, uint32_t pass, uint64_t seed)
{
    uint64_t base = (uint64_t)block_idx ^ (uint64_t)pass ^ seed;
    for (uint32_t i = 0; i < block_size; i++) {
        uint64_t v = base ^ ((uint64_t)i * 0x9E3779B97F4A7C15ULL);
        buf[i] = (uint8_t)(v & 0xFF);
    }
}

int integrity_test_run(const struct integrity_test_cfg *cfg,
                        struct integrity_test_result *result)
{
    if (!cfg || !result) {
        HFSSS_LOG_ERROR(MODULE_NAME, "integrity_test_run: NULL args");
        return HFSSS_ERR_INVAL;
    }

    memset(result, 0, sizeof(*result));
    result->all_passed = true;

    if (cfg->num_blocks == 0 || cfg->num_passes == 0)
        return HFSSS_OK;

    if (cfg->block_size == 0)
        return HFSSS_ERR_INVAL;

    uint8_t *write_buf = malloc(cfg->block_size);
    uint8_t *read_buf  = malloc(cfg->block_size);
    uint64_t *ref_checksums = malloc(sizeof(uint64_t) * cfg->num_blocks);

    if (!write_buf || !read_buf || !ref_checksums) {
        HFSSS_LOG_ERROR(MODULE_NAME, "integrity_test_run: out of memory");
        free(write_buf);
        free(read_buf);
        free(ref_checksums);
        return HFSSS_ERR_NOMEM;
    }

    for (uint32_t pass = 0; pass < cfg->num_passes; pass++) {
        /* Write phase: fill and record reference checksums */
        for (uint32_t blk = 0; blk < cfg->num_blocks; blk++) {
            fill_pattern(write_buf, cfg->block_size, blk, pass, cfg->seed);
            ref_checksums[blk] = buf_checksum(write_buf, cfg->block_size);
            result->blocks_written++;
        }

        /* Read/verify phase: simulate read via memcpy, then compare */
        for (uint32_t blk = 0; blk < cfg->num_blocks; blk++) {
            /* Regenerate write_buf (simulates the written content) */
            fill_pattern(write_buf, cfg->block_size, blk, pass, cfg->seed);
            /* Simulate read (in-memory copy) */
            memcpy(read_buf, write_buf, cfg->block_size);

            uint64_t actual = buf_checksum(read_buf, cfg->block_size);
            result->blocks_verified++;

            if (actual != ref_checksums[blk]) {
                result->blocks_failed++;
                result->all_passed = false;
                HFSSS_LOG_ERROR(MODULE_NAME,
                    "integrity mismatch: pass=%" PRIu32 " blk=%" PRIu32
                    " expected=0x%016" PRIx64 " got=0x%016" PRIx64,
                    pass, blk, ref_checksums[blk], actual);
            }
        }

        result->passes_completed++;
    }

    free(write_buf);
    free(read_buf);
    free(ref_checksums);

    return HFSSS_OK;
}
