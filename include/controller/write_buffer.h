#ifndef __HFSSS_WRITE_BUFFER_H
#define __HFSSS_WRITE_BUFFER_H

#include "common/common.h"
#include "common/mutex.h"

#define WB_MAX_ENTRIES 65536
#define WB_ENTRY_SIZE 4096

/* Write Buffer Entry State */
enum wb_entry_state {
    WB_FREE = 0,
    WB_ALLOCATED = 1,
    WB_DIRTY = 2,
    WB_FLUSHING = 3,
    WB_FLUSHED = 4,
};

/* Write Buffer Entry */
struct wb_entry {
    u64 lba;
    u32 len;
    enum wb_entry_state state;
    u64 timestamp;
    u32 refcount;
    u32 hit_count;
    void *data;
    struct wb_entry *next;
    struct wb_entry *prev;
};

/* Write Buffer Context */
struct write_buffer_ctx {
    struct wb_entry *entries;
    u8 *data_pool;
    u32 entry_count;
    u32 free_count;
    u32 dirty_count;
    struct wb_entry *free_list;
    struct wb_entry *dirty_list;
    u64 flush_threshold;
    u64 flush_interval_ns;
    u64 last_flush_ts;
    struct mutex lock;
};

/* Function Prototypes */
int wb_init(struct write_buffer_ctx *ctx, u32 max_entries);
void wb_cleanup(struct write_buffer_ctx *ctx);
struct wb_entry *wb_alloc(struct write_buffer_ctx *ctx, u64 lba, u32 len);
void wb_free(struct write_buffer_ctx *ctx, struct wb_entry *entry);
int wb_write(struct write_buffer_ctx *ctx, u64 lba, u32 len, void *data);
int wb_read(struct write_buffer_ctx *ctx, u64 lba, u32 len, void *data);
int wb_flush(struct write_buffer_ctx *ctx);
bool wb_lookup(struct write_buffer_ctx *ctx, u64 lba);

#endif /* __HFSSS_WRITE_BUFFER_H */
