#include "media/bbt.h"
#include <string.h>

int bbt_init(struct bbt *bbt, u32 channel_count, u32 chips_per_channel,
             u32 dies_per_chip, u32 planes_per_die, u32 blocks_per_plane)
{
    u32 ch, chip, die, plane, block;

    if (!bbt) {
        return HFSSS_ERR_INVAL;
    }

    if (channel_count > MAX_CHANNELS ||
        chips_per_channel > MAX_CHIPS_PER_CHANNEL ||
        dies_per_chip > MAX_DIES_PER_CHIP ||
        planes_per_die > MAX_PLANES_PER_DIE ||
        blocks_per_plane > MAX_BLOCKS_PER_PLANE) {
        return HFSSS_ERR_INVAL;
    }

    memset(bbt, 0, sizeof(*bbt));

    /* Initialize all entries as free */
    for (ch = 0; ch < MAX_CHANNELS; ch++) {
        for (chip = 0; chip < MAX_CHIPS_PER_CHANNEL; chip++) {
            for (die = 0; die < MAX_DIES_PER_CHIP; die++) {
                for (plane = 0; plane < MAX_PLANES_PER_DIE; plane++) {
                    for (block = 0; block < MAX_BLOCKS_PER_PLANE; block++) {
                        bbt->entries[ch][chip][die][plane][block].state = BBT_ENTRY_FREE;
                        bbt->entries[ch][chip][die][plane][block].erase_count = 0;
                        bbt->total_blocks++;
                    }
                }
            }
        }
    }

    /* Mark some initial bad blocks for realism (1% of blocks) */
    for (ch = 0; ch < channel_count; ch++) {
        for (chip = 0; chip < chips_per_channel; chip++) {
            for (die = 0; die < dies_per_chip; die++) {
                for (plane = 0; plane < planes_per_die; plane++) {
                    for (block = 0; block < blocks_per_plane; block++) {
                        /* Use a simple deterministic pattern to mark bad blocks */
                        u32 idx = (ch * 1000003) + (chip * 100003) + (die * 10007) + (plane * 1009) + block;
                        if ((idx % 100) == 0 && block > 10 && block < blocks_per_plane - 10) {
                            bbt->entries[ch][chip][die][plane][block].state = BBT_ENTRY_BAD;
                            bbt->bad_block_count++;
                        }
                    }
                }
            }
        }
    }

    return HFSSS_OK;
}

void bbt_cleanup(struct bbt *bbt)
{
    if (!bbt) {
        return;
    }

    memset(bbt, 0, sizeof(*bbt));
}

int bbt_mark_bad(struct bbt *bbt, u32 ch, u32 chip, u32 die, u32 plane, u32 block)
{
    if (!bbt) {
        return HFSSS_ERR_INVAL;
    }

    if (ch >= MAX_CHANNELS || chip >= MAX_CHIPS_PER_CHANNEL ||
        die >= MAX_DIES_PER_CHIP || plane >= MAX_PLANES_PER_DIE ||
        block >= MAX_BLOCKS_PER_PLANE) {
        return HFSSS_ERR_INVAL;
    }

    if (bbt->entries[ch][chip][die][plane][block].state != BBT_ENTRY_BAD) {
        bbt->entries[ch][chip][die][plane][block].state = BBT_ENTRY_BAD;
        bbt->bad_block_count++;
    }

    return HFSSS_OK;
}

int bbt_is_bad(struct bbt *bbt, u32 ch, u32 chip, u32 die, u32 plane, u32 block)
{
    if (!bbt) {
        return -1;
    }

    if (ch >= MAX_CHANNELS || chip >= MAX_CHIPS_PER_CHANNEL ||
        die >= MAX_DIES_PER_CHIP || plane >= MAX_PLANES_PER_DIE ||
        block >= MAX_BLOCKS_PER_PLANE) {
        return -1;
    }

    return (bbt->entries[ch][chip][die][plane][block].state == BBT_ENTRY_BAD) ? 1 : 0;
}

u32 bbt_get_erase_count(struct bbt *bbt, u32 ch, u32 chip, u32 die, u32 plane, u32 block)
{
    if (!bbt) {
        return 0;
    }

    if (ch >= MAX_CHANNELS || chip >= MAX_CHIPS_PER_CHANNEL ||
        die >= MAX_DIES_PER_CHIP || plane >= MAX_PLANES_PER_DIE ||
        block >= MAX_BLOCKS_PER_PLANE) {
        return 0;
    }

    return bbt->entries[ch][chip][die][plane][block].erase_count;
}

int bbt_increment_erase_count(struct bbt *bbt, u32 ch, u32 chip, u32 die, u32 plane, u32 block)
{
    if (!bbt) {
        return HFSSS_ERR_INVAL;
    }

    if (ch >= MAX_CHANNELS || chip >= MAX_CHIPS_PER_CHANNEL ||
        die >= MAX_DIES_PER_CHIP || plane >= MAX_PLANES_PER_DIE ||
        block >= MAX_BLOCKS_PER_PLANE) {
        return HFSSS_ERR_INVAL;
    }

    bbt->entries[ch][chip][die][plane][block].erase_count++;

    return HFSSS_OK;
}

u64 bbt_get_bad_block_count(struct bbt *bbt)
{
    if (!bbt) {
        return 0;
    }

    return bbt->bad_block_count;
}
