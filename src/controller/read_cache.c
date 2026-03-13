#include "controller/read_cache.h"
#include <stdlib.h>
#include <string.h>

int rc_init(struct read_cache_ctx *ctx, u32 max_entries)
{
    u32 i;
    int ret;

    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    if (max_entries > RC_MAX_ENTRIES) {
        max_entries = RC_MAX_ENTRIES;
    }

    memset(ctx, 0, sizeof(*ctx));

    ret = mutex_init(&ctx->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    ctx->entries = (struct rc_entry *)calloc(max_entries, sizeof(struct rc_entry));
    if (!ctx->entries) {
        mutex_cleanup(&ctx->lock);
        return HFSSS_ERR_NOMEM;
    }

    ctx->data_pool = (u8 *)calloc(max_entries, RC_ENTRY_SIZE);
    if (!ctx->data_pool) {
        free(ctx->entries);
        mutex_cleanup(&ctx->lock);
        return HFSSS_ERR_NOMEM;
    }

    for (i = 0; i < max_entries; i++) {
        ctx->entries[i].data = &ctx->data_pool[i * RC_ENTRY_SIZE];
        ctx->entries[i].len = RC_ENTRY_SIZE;
        if (i == 0) {
            ctx->entries[i].prev = NULL;
        } else {
            ctx->entries[i].prev = &ctx->entries[i - 1];
        }
        if (i == max_entries - 1) {
            ctx->entries[i].next = NULL;
        } else {
            ctx->entries[i].next = &ctx->entries[i + 1];
        }
    }

    ctx->entry_count = max_entries;
    ctx->lru_head = &ctx->entries[0];
    ctx->lru_tail = &ctx->entries[max_entries - 1];

    return HFSSS_OK;
}

void rc_cleanup(struct read_cache_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->lock, 0);
    free(ctx->data_pool);
    free(ctx->entries);
    mutex_unlock(&ctx->lock);

    mutex_cleanup(&ctx->lock);
    memset(ctx, 0, sizeof(*ctx));
}

/* Move entry to head of LRU list */
static void rc_move_to_head(struct read_cache_ctx *ctx, struct rc_entry *entry)
{
    if (entry == ctx->lru_head) {
        return;
    }

    /* Remove from current position */
    if (entry->prev) {
        entry->prev->next = entry->next;
    }
    if (entry->next) {
        entry->next->prev = entry->prev;
    }
    if (entry == ctx->lru_tail) {
        ctx->lru_tail = entry->prev;
    }

    /* Add to head */
    entry->next = ctx->lru_head;
    entry->prev = NULL;
    if (ctx->lru_head) {
        ctx->lru_head->prev = entry;
    }
    ctx->lru_head = entry;
    if (!ctx->lru_tail) {
        ctx->lru_tail = entry;
    }
}

int rc_insert(struct read_cache_ctx *ctx, u64 lba, u32 len, void *data)
{
    struct rc_entry *entry;

    if (!ctx || !data) {
        return HFSSS_ERR_INVAL;
    }

    if (len > RC_ENTRY_SIZE) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    /* Get LRU entry (tail) */
    entry = ctx->lru_tail;
    if (!entry) {
        mutex_unlock(&ctx->lock);
        return HFSSS_ERR_NOMEM;
    }

    /* Update entry */
    entry->lba = lba;
    entry->len = len;
    entry->timestamp = get_time_ns();
    entry->hit_count = 0;
    memcpy(entry->data, data, len);

    /* Move to head */
    rc_move_to_head(ctx, entry);

    if (ctx->used_count < ctx->entry_count) {
        ctx->used_count++;
    }

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int rc_lookup(struct read_cache_ctx *ctx, u64 lba, u32 len, void *data)
{
    struct rc_entry *entry;
    int found = HFSSS_ERR_NOENT;

    if (!ctx || !data) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    entry = ctx->lru_head;
    while (entry) {
        if (entry->lba == lba && entry->len >= len) {
            memcpy(data, entry->data, len);
            entry->hit_count++;
            entry->timestamp = get_time_ns();
            rc_move_to_head(ctx, entry);
            ctx->hit_count++;
            found = HFSSS_OK;
            break;
        }
        entry = entry->next;
    }

    if (found != HFSSS_OK) {
        ctx->miss_count++;
    }

    mutex_unlock(&ctx->lock);

    return found;
}

void rc_invalidate(struct read_cache_ctx *ctx, u64 lba, u32 len)
{
    struct rc_entry *entry;

    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->lock, 0);

    entry = ctx->lru_head;
    while (entry) {
        struct rc_entry *next = entry->next;
        if (entry->lba >= lba && entry->lba < lba + len) {
            entry->lba = 0;
            entry->hit_count = 0;
        }
        entry = next;
    }

    mutex_unlock(&ctx->lock);
}

void rc_clear(struct read_cache_ctx *ctx)
{
    struct rc_entry *entry;

    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->lock, 0);

    entry = ctx->lru_head;
    while (entry) {
        entry->lba = 0;
        entry->hit_count = 0;
        entry = entry->next;
    }

    ctx->used_count = 0;
    ctx->hit_count = 0;
    ctx->miss_count = 0;

    mutex_unlock(&ctx->lock);
}
