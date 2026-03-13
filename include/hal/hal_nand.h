#ifndef __HFSSS_HAL_NAND_H
#define __HFSSS_HAL_NAND_H

#include "common/common.h"

/* HAL NAND Command Opcode */
enum hal_nand_opcode {
    HAL_NAND_OP_READ = 0,
    HAL_NAND_OP_PROGRAM = 1,
    HAL_NAND_OP_ERASE = 2,
    HAL_NAND_OP_RESET = 3,
    HAL_NAND_OP_STATUS = 4,
};

/* HAL NAND Command */
struct hal_nand_cmd {
    enum hal_nand_opcode opcode;
    u32 ch;
    u32 chip;
    u32 die;
    u32 plane;
    u32 block;
    u32 page;
    void *data;
    void *spare;
    u64 timestamp;
    int (*callback)(void *ctx, int status);
    void *callback_ctx;
};

/* HAL NAND Device */
struct hal_nand_dev {
    u32 channel_count;
    u32 chips_per_channel;
    u32 dies_per_chip;
    u32 planes_per_die;
    u32 blocks_per_plane;
    u32 pages_per_block;
    u32 page_size;
    u32 spare_size;
    void *media_ctx;
};

/* Function Prototypes */
int hal_nand_dev_init(struct hal_nand_dev *dev, u32 channel_count,
                      u32 chips_per_channel, u32 dies_per_chip,
                      u32 planes_per_die, u32 blocks_per_plane,
                      u32 pages_per_block, u32 page_size, u32 spare_size,
                      void *media_ctx);
void hal_nand_dev_cleanup(struct hal_nand_dev *dev);
int hal_nand_read(struct hal_nand_dev *dev, struct hal_nand_cmd *cmd);
int hal_nand_program(struct hal_nand_dev *dev, struct hal_nand_cmd *cmd);
int hal_nand_erase(struct hal_nand_dev *dev, struct hal_nand_cmd *cmd);
int hal_nand_is_bad_block(struct hal_nand_dev *dev, u32 ch, u32 chip,
                           u32 die, u32 plane, u32 block);
int hal_nand_mark_bad_block(struct hal_nand_dev *dev, u32 ch, u32 chip,
                             u32 die, u32 plane, u32 block);
u32 hal_nand_get_erase_count(struct hal_nand_dev *dev, u32 ch, u32 chip,
                              u32 die, u32 plane, u32 block);

#endif /* __HFSSS_HAL_NAND_H */
