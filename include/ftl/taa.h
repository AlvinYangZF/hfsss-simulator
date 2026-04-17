#ifndef __HFSSS_TAA_H
#define __HFSSS_TAA_H

#include "common/common.h"
#include "common/mutex.h"
#include "ftl/mapping.h"

#define TAA_DEFAULT_SHARDS 256

/* TAA Shard — owns a range of LBAs with its own lock */
struct taa_shard {
    struct l2p_entry *l2p;
    struct p2l_entry *p2l;
    struct mutex      lock;
    u64               base_lba;
    u64               lba_count;
    u64               p2l_base;
    u64               p2l_count;
    u64               valid_count;
    u64               lookup_count;
    u64               conflict_count;
};

/* TAA Context — replaces single mapping_ctx for multi-threaded access */
struct taa_ctx {
    u32               num_shards;
    u64               total_lbas;
    u64               total_pages;
    u64               lbas_per_shard;
    struct taa_shard *shards;
    bool              initialized;
};

/* Lifecycle */
int  taa_init(struct taa_ctx *ctx, u64 total_lbas, u64 total_pages,
              u32 num_shards);
void taa_cleanup(struct taa_ctx *ctx);

/* L2P operations — each locks only the relevant shard */
int  taa_lookup(struct taa_ctx *ctx, u64 lba, union ppn *ppn_out);
int  taa_insert(struct taa_ctx *ctx, u64 lba, union ppn ppn);
int  taa_remove(struct taa_ctx *ctx, u64 lba);
int  taa_update(struct taa_ctx *ctx, u64 lba, union ppn new_ppn,
                union ppn *old_ppn);

/*
 * Conditional update: only swap L2P[lba] from expected_old to new_ppn when
 * the current mapping equals expected_old.raw. The comparison and swap
 * happen under a single shard-lock hold so no concurrent writer can slip
 * in between the read and the write.
 *
 * Use this from GC relocation paths to avoid overwriting a host write that
 * landed after GC read the victim source PPN but before GC finished copying
 * the data to the destination block.
 *
 * Returns HFSSS_OK on both outcomes; *updated_out tells the caller whether
 * the swap actually happened. *updated_out is optional.
 */
int  taa_update_if_equal(struct taa_ctx *ctx, u64 lba, union ppn expected_old,
                         union ppn new_ppn, bool *updated_out);

/* P2L reverse lookup */
int  taa_reverse_lookup(struct taa_ctx *ctx, union ppn ppn, u64 *lba_out);

/* Bulk operations for recovery */
int  taa_direct_set(struct taa_ctx *ctx, u64 lba, union ppn ppn);
int  taa_direct_clear(struct taa_ctx *ctx, u64 lba);
int  taa_rebuild_p2l(struct taa_ctx *ctx);

/* Statistics */
u64  taa_get_valid_count(struct taa_ctx *ctx);
void taa_get_stats(struct taa_ctx *ctx, u64 *total_lookups,
                   u64 *total_conflicts);

#endif /* __HFSSS_TAA_H */
