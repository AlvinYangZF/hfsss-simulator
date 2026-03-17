#ifndef __HFSSS_SUPERBLOCK_H
#define __HFSSS_SUPERBLOCK_H

#include "common/common.h"
#include "ftl/mapping.h"
#include "ftl/block.h"
#include "hal/hal.h"

/* Magic numbers */
#define SB_HEADER_MAGIC  0x53425F48U  /* "SB_H" */
#define SB_CKPT_MAGIC    0x434B5054U  /* "CKPT" */
#define SB_JRNL_MAGIC    0x4A524E4CU  /* "JRNL" */
#define SB_MAX_BLOCKS    8

/* Superblock page types */
enum sb_page_type {
    SB_PAGE_HEADER    = 0,
    SB_PAGE_CKPT_DATA = 1,
    SB_PAGE_JRNL_DATA = 2,
    SB_PAGE_CKPT_END  = 3,
};

/* Per-page header (32 bytes, at start of each superblock page) */
struct sb_page_header {
    u32 magic;
    u32 page_type;       /* enum sb_page_type */
    u64 sequence;        /* monotonic sequence number */
    u32 page_index;      /* page offset within this logical unit */
    u32 total_pages;     /* total pages in this checkpoint/journal segment */
    u32 crc32;           /* CRC of the data portion (after this header) */
    u32 reserved;
};

/* L2P checkpoint entry (16 bytes) */
struct ckpt_entry {
    u64 lba;
    u64 ppn_raw;
};

/* Journal operation types */
enum journal_op {
    JRNL_OP_WRITE = 1,
    JRNL_OP_TRIM  = 2,
};

/* Journal entry (32 bytes) */
struct journal_entry {
    u32 op;              /* enum journal_op */
    u32 reserved;
    u64 lba;
    u64 ppn_raw;         /* new PPN for write; 0 for trim */
    u64 sequence;        /* monotonic within journal */
};

/* Physical location of a superblock */
struct sb_block_loc {
    u32 channel;
    u32 chip;
    u32 die;
    u32 plane;
    u32 block_id;
};

/* Superblock manager context */
struct superblock_ctx {
    struct sb_block_loc blocks[SB_MAX_BLOCKS];
    u32 block_count;

    u32 active_block_idx;
    u32 current_page;

    u64 ckpt_sequence;
    u64 journal_sequence;
    u32 journal_entry_count;
    u32 journal_trigger;

    u8 *page_buf;
    u32 buf_used;

    u32 page_size;
    u32 pages_per_block;

    struct hal_ctx *hal;
    bool initialized;
};

/* Forward declaration to avoid circular include with ftl/ftl.h */
struct ftl_config;

/* API */
int sb_init(struct superblock_ctx *sb, struct block_mgr *mgr,
            struct hal_ctx *hal, struct ftl_config *config);
void sb_cleanup(struct superblock_ctx *sb);

int sb_checkpoint_write(struct superblock_ctx *sb, struct mapping_ctx *mapping);
int sb_checkpoint_read(struct superblock_ctx *sb, struct mapping_ctx *mapping);

int sb_journal_append(struct superblock_ctx *sb, enum journal_op op, u64 lba, u64 ppn_raw);
int sb_journal_flush(struct superblock_ctx *sb);
int sb_journal_replay(struct superblock_ctx *sb, struct mapping_ctx *mapping);

int sb_recover(struct superblock_ctx *sb, struct mapping_ctx *mapping, struct block_mgr *mgr);
bool sb_has_valid_checkpoint(struct superblock_ctx *sb);

#endif /* __HFSSS_SUPERBLOCK_H */
