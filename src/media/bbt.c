#include "media/bbt.h"
#include <stdlib.h>
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
    bbt->channel_count = channel_count;
    bbt->chips_per_channel = chips_per_channel;
    bbt->dies_per_chip = dies_per_chip;
    bbt->planes_per_die = planes_per_die;
    bbt->blocks_per_plane = blocks_per_plane;

    /* Allocate 5D array dynamically */
    bbt->entries = (struct bbt_entry *****)malloc(channel_count * sizeof(struct bbt_entry ****));
    if (!bbt->entries) {
        return HFSSS_ERR_NOMEM;
    }

    for (ch = 0; ch < channel_count; ch++) {
        bbt->entries[ch] = (struct bbt_entry ****)malloc(chips_per_channel * sizeof(struct bbt_entry ***));
        if (!bbt->entries[ch]) {
            bbt_cleanup(bbt);
            return HFSSS_ERR_NOMEM;
        }

        for (chip = 0; chip < chips_per_channel; chip++) {
            bbt->entries[ch][chip] = (struct bbt_entry ***)malloc(dies_per_chip * sizeof(struct bbt_entry **));
            if (!bbt->entries[ch][chip]) {
                bbt_cleanup(bbt);
                return HFSSS_ERR_NOMEM;
            }

            for (die = 0; die < dies_per_chip; die++) {
                bbt->entries[ch][chip][die] = (struct bbt_entry **)malloc(planes_per_die * sizeof(struct bbt_entry *));
                if (!bbt->entries[ch][chip][die]) {
                    bbt_cleanup(bbt);
                    return HFSSS_ERR_NOMEM;
                }

                for (plane = 0; plane < planes_per_die; plane++) {
                    bbt->entries[ch][chip][die][plane] = (struct bbt_entry *)calloc(blocks_per_plane, sizeof(struct bbt_entry));
                    if (!bbt->entries[ch][chip][die][plane]) {
                        bbt_cleanup(bbt);
                        return HFSSS_ERR_NOMEM;
                    }

                    /* Initialize entries */
                    for (block = 0; block < blocks_per_plane; block++) {
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
    u32 ch, chip, die, plane;

    if (!bbt) {
        return;
    }

    if (bbt->entries) {
        for (ch = 0; ch < bbt->channel_count; ch++) {
            if (!bbt->entries[ch]) continue;

            for (chip = 0; chip < bbt->chips_per_channel; chip++) {
                if (!bbt->entries[ch][chip]) continue;

                for (die = 0; die < bbt->dies_per_chip; die++) {
                    if (!bbt->entries[ch][chip][die]) continue;

                    for (plane = 0; plane < bbt->planes_per_die; plane++) {
                        if (bbt->entries[ch][chip][die][plane]) {
                            free(bbt->entries[ch][chip][die][plane]);
                        }
                    }
                    free(bbt->entries[ch][chip][die]);
                }
                free(bbt->entries[ch][chip]);
            }
            free(bbt->entries[ch]);
        }
        free(bbt->entries);
    }

    memset(bbt, 0, sizeof(*bbt));
}

int bbt_mark_bad(struct bbt *bbt, u32 ch, u32 chip, u32 die, u32 plane, u32 block)
{
    if (!bbt || !bbt->entries) {
        return HFSSS_ERR_INVAL;
    }

    if (ch >= bbt->channel_count || chip >= bbt->chips_per_channel ||
        die >= bbt->dies_per_chip || plane >= bbt->planes_per_die ||
        block >= bbt->blocks_per_plane) {
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
    if (!bbt || !bbt->entries) {
        return -1;
    }

    if (ch >= bbt->channel_count || chip >= bbt->chips_per_channel ||
        die >= bbt->dies_per_chip || plane >= bbt->planes_per_die ||
        block >= bbt->blocks_per_plane) {
        return -1;
    }

    return (bbt->entries[ch][chip][die][plane][block].state == BBT_ENTRY_BAD) ? 1 : 0;
}

u32 bbt_get_erase_count(struct bbt *bbt, u32 ch, u32 chip, u32 die, u32 plane, u32 block)
{
    if (!bbt || !bbt->entries) {
        return 0;
    }

    if (ch >= bbt->channel_count || chip >= bbt->chips_per_channel ||
        die >= bbt->dies_per_chip || plane >= bbt->planes_per_die ||
        block >= bbt->blocks_per_plane) {
        return 0;
    }

    return bbt->entries[ch][chip][die][plane][block].erase_count;
}

int bbt_increment_erase_count(struct bbt *bbt, u32 ch, u32 chip, u32 die, u32 plane, u32 block)
{
    if (!bbt || !bbt->entries) {
        return HFSSS_ERR_INVAL;
    }

    if (ch >= bbt->channel_count || chip >= bbt->chips_per_channel ||
        die >= bbt->dies_per_chip || plane >= bbt->planes_per_die ||
        block >= bbt->blocks_per_plane) {
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
