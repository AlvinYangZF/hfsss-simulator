#include "controller/write_buffer.h"
#include <stdlib.h>
#include <string.h>

int wb_init(struct write_buffer_ctx *ctx, u32 max_entries)
{
    u32 i;
    int ret;

    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    if (max_entries > WB_MAX_ENTRIES) {
        max_entries = WB_MAX_ENTRIES;
    }

    memset(ctx, 0, sizeof(*ctx));

    ret = mutex_init(&ctx->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    ctx->entries = (struct wb_entry *)calloc(max_entries, sizeof(struct wb_entry));
    if (!ctx->entries) {
        mutex_cleanup(&ctx->lock);
        return HFSSS_ERR_NOMEM;
    }

    ctx->data_pool = (u8 *)calloc(max_entries, WB_ENTRY_SIZE);
    if (!ctx->data_pool) {
        free(ctx->entries);
        mutex_cleanup(&ctx->lock);
        return HFSSS_ERR_NOMEM;
    }

    for (i = 0; i < max_entries; i++) {
        ctx->entries[i].state = WB_FREE;
        ctx->entries[i].data = &ctx->data_pool[i * WB_ENTRY_SIZE];
        if (i < max_entries - 1) {
            ctx->entries[i].next = &ctx->entries[i + 1];
        }
    }

    ctx->free_list = ctx->entries;
    ctx->entry_count = max_entries;
    ctx->free_count = max_entries;
    ctx->flush_threshold = (max_entries * 3) / 4;  /* 75% threshold */
    ctx->flush_interval_ns = 100000000;  /* 100ms */

    return HFSSS_OK;
}

void wb_cleanup(struct write_buffer_ctx *ctx)
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

struct wb_entry *wb_alloc(struct write_buffer_ctx *ctx, u64 lba, u32 len)
{
    struct wb_entry *entry = NULL;

    if (!ctx) {
        return NULL;
    }

    if (len > WB_ENTRY_SIZE) {
        return NULL;
    }

    mutex_lock(&ctx->lock, 0);

    if (ctx->free_list) {
        entry = ctx->free_list;
        ctx->free_list = entry->next;
        entry->next = NULL;
        entry->prev = NULL;
        entry->lba = lba;
        entry->len = len;
        entry->state = WB_ALLOCATED;
        entry->timestamp = get_time_ns();
        entry->refcount = 1;
        ctx->free_count--;
    }

    mutex_unlock(&ctx->lock);

    return entry;
}

void wb_free(struct write_buffer_ctx *ctx, struct wb_entry *entry)
{
    if (!ctx || !entry) {
        return;
    }

    mutex_lock(&ctx->lock, 0);

    if (entry->state == WB_DIRTY) {
        ctx->dirty_count--;
    }

    if (entry->prev || entry->next == ctx->dirty_list) {
        if (entry->prev) {
            entry->prev->next = entry->next;
        } else {
            ctx->dirty_list = entry->next;
        }
        if (entry->next) {
            entry->next->prev = entry->prev;
        }
    }

    entry->state = WB_FREE;
    entry->next = ctx->free_list;
    entry->prev = NULL;
    ctx->free_list = entry;
    ctx->free_count++;

    mutex_unlock(&ctx->lock);
}

int wb_write(struct write_buffer_ctx *ctx, u64 lba, u32 len, void *data)
{
    struct wb_entry *entry;

    if (!ctx || !data) {
        return HFSSS_ERR_INVAL;
    }

    if (len > WB_ENTRY_SIZE) {
        return HFSSS_ERR_INVAL;
    }

    entry = wb_alloc(ctx, lba, len);
    if (!entry) {
        return HFSSS_ERR_NOMEM;
    }

    memcpy(entry->data, data, len);

    mutex_lock(&ctx->lock, 0);
    entry->state = WB_DIRTY;

    /* Add to dirty list */
    entry->next = ctx->dirty_list;
    if (ctx->dirty_list) {
        ctx->dirty_list->prev = entry;
    }
    ctx->dirty_list = entry;
    ctx->dirty_count++;
    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int wb_read(struct write_buffer_ctx *ctx, u64 lba, u32 len, void *data)
{
    struct wb_entry *entry;
    int found = HFSSS_ERR_NOENT;

    if (!ctx || !data) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    entry = ctx->dirty_list;
    while (entry) {
        if (entry->lba == lba && entry->state == WB_DIRTY) {
            if (len <= entry->len) {
                memcpy(data, entry->data, len);
                entry->hit_count++;
                entry->timestamp = get_time_ns();
                found = HFSSS_OK;
            }
            break;
        }
        entry = entry->next;
    }

    mutex_unlock(&ctx->lock);

    return found;
}

int wb_flush(struct write_buffer_ctx *ctx)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    /* For now, just mark all dirty entries as flushed */
    mutex_lock(&ctx->lock, 0);

    struct wb_entry *entry = ctx->dirty_list;
    while (entry) {
        struct wb_entry *next = entry->next;
        entry->state = WB_FLUSHED;
        entry = next;
    }

    ctx->dirty_count = 0;
    ctx->dirty_list = NULL;
    ctx->last_flush_ts = get_time_ns();

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

bool wb_lookup(struct write_buffer_ctx *ctx, u64 lba)
{
    struct wb_entry *entry;
    bool found = false;

    if (!ctx) {
        return false;
    }

    mutex_lock(&ctx->lock, 0);

    entry = ctx->dirty_list;
    while (entry) {
        if (entry->lba == lba && entry->state == WB_DIRTY) {
            found = true;
            break;
        }
        entry = entry->next;
    }

    mutex_unlock(&ctx->lock);

    return found;
}
