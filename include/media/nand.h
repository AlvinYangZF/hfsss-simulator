#ifndef __HFSSS_NAND_H
#define __HFSSS_NAND_H

#include "common/common.h"
#include "common/mutex.h"
#include "media/cmd_state.h"
#include "media/eat.h"
#include "media/nand_identity.h"
#include "media/timing.h"

#define MAX_BLOCKS_PER_PLANE 4096
#define MAX_PAGES_PER_BLOCK 1024
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
    bool dirty; /* Dirty flag for incremental checkpointing */
    u8 *data;
    u8 *spare;
};

/* Block */
struct nand_block {
    u32 block_id;
    enum block_state state;
    u32 pages_written;
    bool dirty; /* Dirty flag for incremental checkpointing */
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
    struct nand_die_cmd_state cmd_state;
    struct mutex die_lock;
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

struct nand_profile;

/* NAND Device */
struct nand_device {
    struct nand_channel channels[MAX_CHANNELS];
    u32 channel_count;
    struct timing_model *timing;
    struct eat_ctx *eat;
    const struct nand_profile *profile;
    struct nand_id nand_id;
    struct nand_parameter_page param_page;
    /*
     * Page/spare sizes cached at init so the command engine can stamp
     * pages without reaching back up into media_ctx. Required by the
     * reset-abort path which fills aborted target pages with the
     * abort pattern below.
     */
    u32 page_size;
    u32 spare_size;
};

/*
 * Reset-abort pattern. When reset interrupts an in-flight PROG / CACHE_PROG
 * / ERASE, the engine stamps the target page(s) with this two-byte
 * pattern repeated over the page, and marks the page PAGE_INVALID. This
 * replaces whatever partial state the aborted command left behind with a
 * visually-distinct signature so debug tools can identify pages that
 * witnessed an aborted operation (they are neither cleanly erased nor
 * validly programmed).
 */
#define NAND_ABORT_PATTERN_BYTE_HI 0xDEu
#define NAND_ABORT_PATTERN_BYTE_LO 0xADu

/* Function Prototypes */
int nand_device_init(struct nand_device *dev, u32 channel_count, u32 chips_per_channel, u32 dies_per_chip,
                     u32 planes_per_die, u32 blocks_per_plane, u32 pages_per_block, u32 page_size, u32 spare_size);
void nand_device_cleanup(struct nand_device *dev);
struct nand_page *nand_get_page(struct nand_device *dev, u32 ch, u32 chip, u32 die, u32 plane, u32 block, u32 page);
struct nand_block *nand_get_block(struct nand_device *dev, u32 ch, u32 chip, u32 die, u32 plane, u32 block);
int nand_validate_address(struct nand_device *dev, u32 ch, u32 chip, u32 die, u32 plane, u32 block, u32 page);

/*
 * Stamp page data with the abort pattern (NAND_ABORT_PATTERN_BYTE_HI/LO
 * alternating across page_size bytes) and mark state = PAGE_INVALID. Safe
 * to call on a NULL page — the function is a no-op in that case so the
 * engine reset path can invoke it over an iterator without extra NULL
 * checks.
 */
void nand_stamp_abort_pattern(struct nand_page *page, u32 page_size);

#endif /* __HFSSS_NAND_H */
