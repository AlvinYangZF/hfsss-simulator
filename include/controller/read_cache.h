#ifndef __HFSSS_READ_CACHE_H
#define __HFSSS_READ_CACHE_H

#include "common/common.h"
#include "common/mutex.h"

#define RC_MAX_ENTRIES 131072
#define RC_ENTRY_SIZE 4096

/* Read Cache Entry */
struct rc_entry {
    u64 lba;
    u32 len;
    u64 timestamp;
    u32 hit_count;
    void *data;
    struct rc_entry *next;
    struct rc_entry *prev;
};

/* Read Cache Context (LRU) */
struct read_cache_ctx {
    struct rc_entry *entries;
    u8 *data_pool;
    u32 entry_count;
    u32 used_count;
    struct rc_entry *lru_head;
    struct rc_entry *lru_tail;
    u64 hit_count;
    u64 miss_count;
    struct mutex lock;
};

/* Function Prototypes */
int rc_init(struct read_cache_ctx *ctx, u32 max_entries);
void rc_cleanup(struct read_cache_ctx *ctx);
int rc_insert(struct read_cache_ctx *ctx, u64 lba, u32 len, void *data);
int rc_lookup(struct read_cache_ctx *ctx, u64 lba, u32 len, void *data);
void rc_invalidate(struct read_cache_ctx *ctx, u64 lba, u32 len);
void rc_clear(struct read_cache_ctx *ctx);

#endif /* __HFSSS_READ_CACHE_H */
