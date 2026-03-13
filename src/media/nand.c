#include "media/nand.h"
#include <stdlib.h>
#include <string.h>

static int nand_init_hierarchy(struct nand_device *dev, u32 channel_count,
                                 u32 chips_per_channel, u32 dies_per_chip,
                                 u32 planes_per_die, u32 blocks_per_plane,
                                 u32 pages_per_block, u32 page_size, u32 spare_size)
{
    u32 ch, chip, die, plane, block;

    dev->channel_count = channel_count;

    for (ch = 0; ch < channel_count; ch++) {
        struct nand_channel *channel = &dev->channels[ch];

        channel->channel_id = ch;
        channel->chip_count = chips_per_channel;
        channel->current_time = 0;

        int ret = mutex_init(&channel->lock);
        if (ret != HFSSS_OK) {
            return ret;
        }

        for (chip = 0; chip < chips_per_channel; chip++) {
            struct nand_chip *nand_chip = &channel->chips[chip];

            nand_chip->chip_id = chip;
            nand_chip->die_count = dies_per_chip;
            nand_chip->next_available_ts = 0;

            for (die = 0; die < dies_per_chip; die++) {
                struct nand_die *nand_die = &nand_chip->dies[die];

                nand_die->die_id = die;
                nand_die->plane_count = planes_per_die;
                nand_die->next_available_ts = 0;

                for (plane = 0; plane < planes_per_die; plane++) {
                    struct nand_plane *nand_plane = &nand_die->planes[plane];

                    nand_plane->plane_id = plane;
                    nand_plane->block_count = blocks_per_plane;
                    nand_plane->next_available_ts = 0;

                    /* Allocate blocks */
                    nand_plane->blocks = (struct nand_block *)calloc(blocks_per_plane, sizeof(struct nand_block));
                    if (!nand_plane->blocks) {
                        return HFSSS_ERR_NOMEM;
                    }

                    for (block = 0; block < blocks_per_plane; block++) {
                        struct nand_block *nand_block = &nand_plane->blocks[block];

                        nand_block->block_id = block;
                        nand_block->state = BLOCK_FREE;
                        nand_block->pages_written = 0;
                        nand_block->page_count = pages_per_block;

                        /* Allocate pages */
                        nand_block->pages = (struct nand_page *)calloc(pages_per_block, sizeof(struct nand_page));
                        if (!nand_block->pages) {
                            return HFSSS_ERR_NOMEM;
                        }

                        for (u32 page = 0; page < pages_per_block; page++) {
                            struct nand_page *nand_page = &nand_block->pages[page];

                            nand_page->state = PAGE_FREE;
                            nand_page->program_ts = 0;
                            nand_page->erase_count = 0;
                            nand_page->bit_errors = 0;
                            nand_page->read_count = 0;

                            /* Allocate data buffer */
                            nand_page->data = (u8 *)malloc(page_size);
                            if (!nand_page->data) {
                                return HFSSS_ERR_NOMEM;
                            }
                            memset(nand_page->data, 0xFF, page_size);

                            /* Allocate spare buffer */
                            nand_page->spare = (u8 *)malloc(spare_size);
                            if (!nand_page->spare) {
                                return HFSSS_ERR_NOMEM;
                            }
                            memset(nand_page->spare, 0xFF, spare_size);
                        }
                    }
                }
            }
        }
    }

    return HFSSS_OK;
}

int nand_device_init(struct nand_device *dev, u32 channel_count, u32 chips_per_channel,
                     u32 dies_per_chip, u32 planes_per_die, u32 blocks_per_plane,
                     u32 pages_per_block, u32 page_size, u32 spare_size)
{
    if (!dev) {
        return HFSSS_ERR_INVAL;
    }

    if (channel_count > MAX_CHANNELS ||
        chips_per_channel > MAX_CHIPS_PER_CHANNEL ||
        dies_per_chip > MAX_DIES_PER_CHIP ||
        planes_per_die > MAX_PLANES_PER_DIE ||
        blocks_per_plane > MAX_BLOCKS_PER_PLANE ||
        pages_per_block > MAX_PAGES_PER_BLOCK) {
        return HFSSS_ERR_INVAL;
    }

    memset(dev, 0, sizeof(*dev));

    /* Initialize timing model (will be set later) */
    dev->timing = NULL;
    dev->eat = NULL;

    /* Initialize NAND hierarchy */
    return nand_init_hierarchy(dev, channel_count, chips_per_channel,
                                dies_per_chip, planes_per_die, blocks_per_plane,
                                pages_per_block, page_size, spare_size);
}

static void nand_cleanup_hierarchy(struct nand_device *dev)
{
    u32 ch, chip, die, plane, block;

    for (ch = 0; ch < dev->channel_count; ch++) {
        struct nand_channel *channel = &dev->channels[ch];

        mutex_cleanup(&channel->lock);

        for (chip = 0; chip < channel->chip_count; chip++) {
            struct nand_chip *nand_chip = &channel->chips[chip];

            for (die = 0; die < nand_chip->die_count; die++) {
                struct nand_die *nand_die = &nand_chip->dies[die];

                for (plane = 0; plane < nand_die->plane_count; plane++) {
                    struct nand_plane *nand_plane = &nand_die->planes[plane];

                    if (nand_plane->blocks) {
                        for (block = 0; block < nand_plane->block_count; block++) {
                            struct nand_block *nand_block = &nand_plane->blocks[block];

                            if (nand_block->pages) {
                                for (u32 page = 0; page < nand_block->page_count; page++) {
                                    struct nand_page *nand_page = &nand_block->pages[page];
                                    free(nand_page->data);
                                    free(nand_page->spare);
                                }
                                free(nand_block->pages);
                            }
                        }
                        free(nand_plane->blocks);
                    }
                }
            }
        }
    }
}

void nand_device_cleanup(struct nand_device *dev)
{
    if (!dev) {
        return;
    }

    nand_cleanup_hierarchy(dev);
    memset(dev, 0, sizeof(*dev));
}

struct nand_page *nand_get_page(struct nand_device *dev, u32 ch, u32 chip, u32 die,
                                 u32 plane, u32 block, u32 page)
{
    struct nand_block *blk;

    if (!dev) {
        return NULL;
    }

    blk = nand_get_block(dev, ch, chip, die, plane, block);
    if (!blk) {
        return NULL;
    }

    if (page >= blk->page_count) {
        return NULL;
    }

    return &blk->pages[page];
}

struct nand_block *nand_get_block(struct nand_device *dev, u32 ch, u32 chip, u32 die,
                                   u32 plane, u32 block)
{
    struct nand_channel *channel;
    struct nand_chip *nand_chip;
    struct nand_die *nand_die;
    struct nand_plane *nand_plane;

    if (!dev) {
        return NULL;
    }

    if (ch >= dev->channel_count) {
        return NULL;
    }
    channel = &dev->channels[ch];

    if (chip >= channel->chip_count) {
        return NULL;
    }
    nand_chip = &channel->chips[chip];

    if (die >= nand_chip->die_count) {
        return NULL;
    }
    nand_die = &nand_chip->dies[die];

    if (plane >= nand_die->plane_count) {
        return NULL;
    }
    nand_plane = &nand_die->planes[plane];

    if (block >= nand_plane->block_count) {
        return NULL;
    }

    return &nand_plane->blocks[block];
}

int nand_validate_address(struct nand_device *dev, u32 ch, u32 chip, u32 die,
                          u32 plane, u32 block, u32 page)
{
    struct nand_block *blk;

    if (!dev) {
        return HFSSS_ERR_INVAL;
    }

    blk = nand_get_block(dev, ch, chip, die, plane, block);
    if (!blk) {
        return HFSSS_ERR_INVAL;
    }

    if (page >= blk->page_count) {
        return HFSSS_ERR_INVAL;
    }

    return HFSSS_OK;
}
