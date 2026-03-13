#ifndef __HFSSS_BBT_H
#define __HFSSS_BBT_H

#include "common/common.h"

#define MAX_CHANNELS 32
#define MAX_CHIPS_PER_CHANNEL 8
#define MAX_DIES_PER_CHIP 4
#define MAX_PLANES_PER_DIE 2
#define MAX_BLOCKS_PER_PLANE 2048

#define BBT_ENTRY_FREE 0x00
#define BBT_ENTRY_BAD 0xFF

/* BBT Entry */
struct bbt_entry {
    u8 state;
    u32 erase_count;
};

/* BBT Table */
struct bbt {
    struct bbt_entry entries[MAX_CHANNELS][MAX_CHIPS_PER_CHANNEL][MAX_DIES_PER_CHIP][MAX_PLANES_PER_DIE][MAX_BLOCKS_PER_PLANE];
    u64 bad_block_count;
    u64 total_blocks;
};

/* Function Prototypes */
int bbt_init(struct bbt *bbt, u32 channel_count, u32 chips_per_channel,
             u32 dies_per_chip, u32 planes_per_die, u32 blocks_per_plane);
void bbt_cleanup(struct bbt *bbt);
int bbt_mark_bad(struct bbt *bbt, u32 ch, u32 chip, u32 die, u32 plane, u32 block);
int bbt_is_bad(struct bbt *bbt, u32 ch, u32 chip, u32 die, u32 plane, u32 block);
u32 bbt_get_erase_count(struct bbt *bbt, u32 ch, u32 chip, u32 die, u32 plane, u32 block);
int bbt_increment_erase_count(struct bbt *bbt, u32 ch, u32 chip, u32 die, u32 plane, u32 block);
u64 bbt_get_bad_block_count(struct bbt *bbt);

#endif /* __HFSSS_BBT_H */
