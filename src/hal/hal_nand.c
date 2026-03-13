#include "hal/hal_nand.h"
#include "media/media.h"
#include <string.h>

int hal_nand_dev_init(struct hal_nand_dev *dev, u32 channel_count,
                      u32 chips_per_channel, u32 dies_per_chip,
                      u32 planes_per_die, u32 blocks_per_plane,
                      u32 pages_per_block, u32 page_size, u32 spare_size,
                      void *media_ctx)
{
    if (!dev) {
        return HFSSS_ERR_INVAL;
    }

    memset(dev, 0, sizeof(*dev));

    dev->channel_count = channel_count;
    dev->chips_per_channel = chips_per_channel;
    dev->dies_per_chip = dies_per_chip;
    dev->planes_per_die = planes_per_die;
    dev->blocks_per_plane = blocks_per_plane;
    dev->pages_per_block = pages_per_block;
    dev->page_size = page_size;
    dev->spare_size = spare_size;
    dev->media_ctx = media_ctx;

    return HFSSS_OK;
}

void hal_nand_dev_cleanup(struct hal_nand_dev *dev)
{
    if (!dev) {
        return;
    }

    memset(dev, 0, sizeof(*dev));
}

int hal_nand_read(struct hal_nand_dev *dev, struct hal_nand_cmd *cmd)
{
    struct media_ctx *media;

    if (!dev || !cmd) {
        return HFSSS_ERR_INVAL;
    }

    if (cmd->opcode != HAL_NAND_OP_READ) {
        return HFSSS_ERR_INVAL;
    }

    media = (struct media_ctx *)dev->media_ctx;
    if (!media) {
        return HFSSS_ERR_INVAL;
    }

    return media_nand_read(media, cmd->ch, cmd->chip, cmd->die,
                           cmd->plane, cmd->block, cmd->page,
                           cmd->data, cmd->spare);
}

int hal_nand_program(struct hal_nand_dev *dev, struct hal_nand_cmd *cmd)
{
    struct media_ctx *media;

    if (!dev || !cmd) {
        return HFSSS_ERR_INVAL;
    }

    if (cmd->opcode != HAL_NAND_OP_PROGRAM) {
        return HFSSS_ERR_INVAL;
    }

    media = (struct media_ctx *)dev->media_ctx;
    if (!media) {
        return HFSSS_ERR_INVAL;
    }

    return media_nand_program(media, cmd->ch, cmd->chip, cmd->die,
                              cmd->plane, cmd->block, cmd->page,
                              cmd->data, cmd->spare);
}

int hal_nand_erase(struct hal_nand_dev *dev, struct hal_nand_cmd *cmd)
{
    struct media_ctx *media;

    if (!dev || !cmd) {
        return HFSSS_ERR_INVAL;
    }

    if (cmd->opcode != HAL_NAND_OP_ERASE) {
        return HFSSS_ERR_INVAL;
    }

    media = (struct media_ctx *)dev->media_ctx;
    if (!media) {
        return HFSSS_ERR_INVAL;
    }

    return media_nand_erase(media, cmd->ch, cmd->chip, cmd->die,
                            cmd->plane, cmd->block);
}

int hal_nand_is_bad_block(struct hal_nand_dev *dev, u32 ch, u32 chip,
                           u32 die, u32 plane, u32 block)
{
    struct media_ctx *media;

    if (!dev) {
        return -1;
    }

    media = (struct media_ctx *)dev->media_ctx;
    if (!media) {
        return -1;
    }

    return media_nand_is_bad_block(media, ch, chip, die, plane, block);
}

int hal_nand_mark_bad_block(struct hal_nand_dev *dev, u32 ch, u32 chip,
                             u32 die, u32 plane, u32 block)
{
    struct media_ctx *media;

    if (!dev) {
        return HFSSS_ERR_INVAL;
    }

    media = (struct media_ctx *)dev->media_ctx;
    if (!media) {
        return HFSSS_ERR_INVAL;
    }

    return media_nand_mark_bad_block(media, ch, chip, die, plane, block);
}

u32 hal_nand_get_erase_count(struct hal_nand_dev *dev, u32 ch, u32 chip,
                              u32 die, u32 plane, u32 block)
{
    struct media_ctx *media;

    if (!dev) {
        return 0;
    }

    media = (struct media_ctx *)dev->media_ctx;
    if (!media) {
        return 0;
    }

    return media_nand_get_erase_count(media, ch, chip, die, plane, block);
}
