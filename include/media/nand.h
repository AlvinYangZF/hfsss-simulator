#ifndef __HFSSS_NAND_H
#define __HFSSS_NAND_H

#include "common/common.h"
#include "common/mutex.h"
#include "media/eat.h"
#include "media/timing.h"

#define MAX_BLOCKS_PER_PLANE 2048
#define MAX_PAGES_PER_BLOCK 512
#define PAGE_SIZE_TLC 16384
#define SPARE_SIZE_TLC 2048

/* NAND Command */
enum nand_cmd {
    NAND_CMD_READ = 0x00,
    NAND_CMD_READ_START = 0x30,
    NAND_CMD_PROG = 0x80,
    NAND_CMD_PROG_START = 0x10,
    NAND_CMD_ERASE = 0x60,
    NAND_CMD_ERASE_START = 0xD0,
    NAND_CMD_RESET = 0xFF,
    NAND_CMD_STATUS = 0x70,
};

/* Page State */
enum page_state {
    PAGE_FREE = 0,
    PAGE_VALID = 1,
    PAGE_INVALID = 2,
};

/* Block State */
enum block_state {
    BLOCK_FREE = 0,
    BLOCK_OPEN = 1,
    BLOCK_CLOSED = 2,
    BLOCK_BAD = 3,
};

/* Page */
struct nand_page {
    enum page_state state;
    u64 program_ts;
    u32 erase_count;
    u32 bit_errors;
    u32 read_count;
    u8 *data;
    u8 *spare;
};

/* Block */
struct nand_block {
    u32 block_id;
    enum block_state state;
    u32 pages_written;
    struct nand_page *pages;
    u32 page_count;
};

/* Plane */
struct nand_plane {
    u32 plane_id;
    struct nand_block *blocks;
    u32 block_count;
    u64 next_available_ts;
};

/* Die */
struct nand_die {
    u32 die_id;
    struct nand_plane planes[MAX_PLANES_PER_DIE];
    u32 plane_count;
    u64 next_available_ts;
};

/* Chip */
struct nand_chip {
    u32 chip_id;
    struct nand_die dies[MAX_DIES_PER_CHIP];
    u32 die_count;
    u64 next_available_ts;
};

/* Channel */
struct nand_channel {
    u32 channel_id;
    struct nand_chip chips[MAX_CHIPS_PER_CHANNEL];
    u32 chip_count;
    u64 current_time;
    struct mutex lock;
};

/* NAND Device */
struct nand_device {
    struct nand_channel channels[MAX_CHANNELS];
    u32 channel_count;
    struct timing_model *timing;
    struct eat_ctx *eat;
};

/* Function Prototypes */
int nand_device_init(struct nand_device *dev, u32 channel_count, u32 chips_per_channel,
                     u32 dies_per_chip, u32 planes_per_die, u32 blocks_per_plane,
                     u32 pages_per_block, u32 page_size, u32 spare_size);
void nand_device_cleanup(struct nand_device *dev);
struct nand_page *nand_get_page(struct nand_device *dev, u32 ch, u32 chip, u32 die,
                                 u32 plane, u32 block, u32 page);
struct nand_block *nand_get_block(struct nand_device *dev, u32 ch, u32 chip, u32 die,
                                   u32 plane, u32 block);
int nand_validate_address(struct nand_device *dev, u32 ch, u32 chip, u32 die,
                          u32 plane, u32 block, u32 page);

#endif /* __HFSSS_NAND_H */
