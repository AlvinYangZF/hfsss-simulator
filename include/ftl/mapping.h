#ifndef __HFSSS_MAPPING_H
#define __HFSSS_MAPPING_H

#include "common/common.h"
#include "common/mutex.h"

/* Use a smaller table for testing */
#define L2P_TABLE_SIZE (1ULL << 20)    /* 1M entries for testing */
#define P2L_TABLE_SIZE (1ULL << 24)    /* 16M entries for testing */

/* PPN Encoding */
union ppn {
    u64 raw;
    struct {
        u64 channel : 6;
        u64 chip : 4;
        u64 die : 3;
        u64 plane : 2;
        u64 block : 12;
        u64 page : 10;
        u64 reserved : 27;
    } bits;
};

/* L2P Table Entry */
struct l2p_entry {
    union ppn ppn;
    bool valid;
};

/* P2L Table Entry */
struct p2l_entry {
    u64 lba;
    bool valid;
};

/* Mapping Context */
struct mapping_ctx {
    struct l2p_entry *l2p_table;
    struct p2l_entry *p2l_table;
    u64 l2p_size;
    u64 p2l_size;
    u64 valid_count;
    struct mutex lock;
};

/* Function Prototypes */
int mapping_init(struct mapping_ctx *ctx, u64 l2p_size, u64 p2l_size);
void mapping_cleanup(struct mapping_ctx *ctx);
int mapping_l2p(struct mapping_ctx *ctx, u64 lba, union ppn *ppn);
int mapping_p2l(struct mapping_ctx *ctx, union ppn ppn, u64 *lba);
int mapping_insert(struct mapping_ctx *ctx, u64 lba, union ppn ppn);
int mapping_remove(struct mapping_ctx *ctx, u64 lba);
int mapping_update(struct mapping_ctx *ctx, u64 lba, union ppn new_ppn, union ppn *old_ppn);
u64 mapping_get_valid_count(struct mapping_ctx *ctx);

/* Recovery helpers — used during superblock checkpoint load */
int mapping_direct_set(struct mapping_ctx *ctx, u64 lba, union ppn ppn);
int mapping_direct_clear(struct mapping_ctx *ctx, u64 lba);
int mapping_rebuild_p2l(struct mapping_ctx *ctx);

#endif /* __HFSSS_MAPPING_H */
