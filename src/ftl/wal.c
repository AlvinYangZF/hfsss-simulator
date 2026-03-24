#include "ftl/wal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* wal_init / wal_cleanup                                              */
/* ------------------------------------------------------------------ */

int wal_init(struct wal_ctx *ctx, u32 max_records)
{
    if (!ctx || max_records == 0) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    if (max_records > WAL_MAX_RECORDS) {
        max_records = WAL_MAX_RECORDS;
    }

    ctx->records = (struct wal_record *)calloc(max_records,
                                               sizeof(struct wal_record));
    if (!ctx->records) {
        return HFSSS_ERR_NOMEM;
    }

    ctx->capacity = max_records;
    ctx->count = 0;
    ctx->next_sequence = 1;
    ctx->initialized = true;

    return HFSSS_OK;
}

void wal_cleanup(struct wal_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    free(ctx->records);
    memset(ctx, 0, sizeof(*ctx));
}

/* ------------------------------------------------------------------ */
/* wal_append                                                          */
/* ------------------------------------------------------------------ */

int wal_append(struct wal_ctx *ctx, enum wal_rec_type type,
               u64 lba, u64 ppn, const void *meta, u32 meta_len)
{
    struct wal_record *rec;

    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    if (ctx->count >= ctx->capacity) {
        return HFSSS_ERR_NOSPC;
    }

    rec = &ctx->records[ctx->count];
    memset(rec, 0, sizeof(*rec));

    rec->magic = WAL_RECORD_MAGIC;
    rec->type = (u32)type;
    rec->sequence = ctx->next_sequence++;
    rec->lba = lba;
    rec->ppn = ppn;

    if (meta && meta_len > 0) {
        u32 copy_len = meta_len > sizeof(rec->meta) ?
                       sizeof(rec->meta) : meta_len;
        memcpy(rec->meta, meta, copy_len);
    }

    rec->end_marker = 0;
    rec->crc32 = hfsss_crc32(rec, offsetof(struct wal_record, crc32));

    ctx->count++;
    return HFSSS_OK;
}

/* ------------------------------------------------------------------ */
/* wal_commit                                                          */
/* ------------------------------------------------------------------ */

int wal_commit(struct wal_ctx *ctx)
{
    struct wal_record *rec;

    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    if (ctx->count >= ctx->capacity) {
        return HFSSS_ERR_NOSPC;
    }

    rec = &ctx->records[ctx->count];
    memset(rec, 0, sizeof(*rec));

    rec->magic = WAL_RECORD_MAGIC;
    rec->type = (u32)WAL_REC_COMMIT;
    rec->sequence = ctx->next_sequence++;
    rec->end_marker = WAL_COMMIT_MARKER;
    rec->crc32 = hfsss_crc32(rec, offsetof(struct wal_record, crc32));

    ctx->count++;
    return HFSSS_OK;
}

/* ------------------------------------------------------------------ */
/* wal_replay                                                          */
/* ------------------------------------------------------------------ */

int wal_replay(const struct wal_ctx *ctx, wal_replay_cb callback,
               void *user_data)
{
    u32 i;
    u32 replayed = 0;

    if (!ctx || !ctx->initialized || !callback) {
        return HFSSS_ERR_INVAL;
    }

    for (i = 0; i < ctx->count; i++) {
        const struct wal_record *rec = &ctx->records[i];
        u32 expected_crc;
        int ret;

        /* Validate magic */
        if (rec->magic != WAL_RECORD_MAGIC) {
            break;
        }

        /* Validate CRC */
        expected_crc = hfsss_crc32(rec,
                                   offsetof(struct wal_record, crc32));
        if (expected_crc != rec->crc32) {
            break;
        }

        /* Skip commit markers in replay */
        if (rec->type == (u32)WAL_REC_COMMIT) {
            continue;
        }

        ret = callback(rec, user_data);
        if (ret != HFSSS_OK) {
            return ret;
        }

        replayed++;
    }

    return HFSSS_OK;
}

/* ------------------------------------------------------------------ */
/* wal_get_committed_seq                                               */
/* ------------------------------------------------------------------ */

u64 wal_get_committed_seq(const struct wal_ctx *ctx)
{
    u64 last_commit = 0;
    u32 i;

    if (!ctx || !ctx->initialized) {
        return 0;
    }

    for (i = 0; i < ctx->count; i++) {
        const struct wal_record *rec = &ctx->records[i];
        if (rec->magic != WAL_RECORD_MAGIC) {
            break;
        }
        if (rec->type == (u32)WAL_REC_COMMIT &&
            rec->end_marker == WAL_COMMIT_MARKER) {
            last_commit = rec->sequence;
        }
    }

    return last_commit;
}

/* ------------------------------------------------------------------ */
/* wal_truncate                                                        */
/* ------------------------------------------------------------------ */

int wal_truncate(struct wal_ctx *ctx, u64 up_to_seq)
{
    u32 keep_from = 0;
    u32 i;

    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    /* Find the first record with sequence > up_to_seq */
    for (i = 0; i < ctx->count; i++) {
        if (ctx->records[i].sequence > up_to_seq) {
            break;
        }
        keep_from = i + 1;
    }

    if (keep_from == 0) {
        return HFSSS_OK;
    }

    if (keep_from >= ctx->count) {
        ctx->count = 0;
        return HFSSS_OK;
    }

    /* Shift remaining records to the front */
    u32 remaining = ctx->count - keep_from;
    memmove(ctx->records, ctx->records + keep_from,
            remaining * sizeof(struct wal_record));
    ctx->count = remaining;

    return HFSSS_OK;
}

/* ------------------------------------------------------------------ */
/* wal_get_count / wal_reset                                           */
/* ------------------------------------------------------------------ */

u32 wal_get_count(const struct wal_ctx *ctx)
{
    if (!ctx || !ctx->initialized) {
        return 0;
    }
    return ctx->count;
}

void wal_reset(struct wal_ctx *ctx)
{
    if (!ctx || !ctx->initialized) {
        return;
    }
    ctx->count = 0;
}

/* ------------------------------------------------------------------ */
/* wal_save / wal_load (file persistence)                              */
/* ------------------------------------------------------------------ */

int wal_save(const struct wal_ctx *ctx, const char *filepath)
{
    FILE *f;

    if (!ctx || !ctx->initialized || !filepath) {
        return HFSSS_ERR_INVAL;
    }

    f = fopen(filepath, "wb");
    if (!f) {
        return HFSSS_ERR_IO;
    }

    if (ctx->count > 0) {
        if (fwrite(ctx->records, sizeof(struct wal_record),
                   ctx->count, f) != ctx->count) {
            fclose(f);
            return HFSSS_ERR_IO;
        }
    }

    fclose(f);
    return HFSSS_OK;
}

int wal_load(struct wal_ctx *ctx, const char *filepath)
{
    FILE *f;
    struct wal_record rec;

    if (!ctx || !ctx->initialized || !filepath) {
        return HFSSS_ERR_INVAL;
    }

    f = fopen(filepath, "rb");
    if (!f) {
        return HFSSS_ERR_NOENT;
    }

    ctx->count = 0;

    while (fread(&rec, sizeof(rec), 1, f) == 1) {
        if (rec.magic != WAL_RECORD_MAGIC) {
            break;
        }

        u32 expected_crc = hfsss_crc32(&rec,
                                       offsetof(struct wal_record, crc32));
        if (expected_crc != rec.crc32) {
            break;
        }

        if (ctx->count >= ctx->capacity) {
            break;
        }

        memcpy(&ctx->records[ctx->count], &rec, sizeof(rec));
        ctx->count++;

        if (rec.sequence >= ctx->next_sequence) {
            ctx->next_sequence = rec.sequence + 1;
        }
    }

    fclose(f);
    return HFSSS_OK;
}
