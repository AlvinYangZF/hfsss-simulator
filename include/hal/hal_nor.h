#ifndef __HFSSS_HAL_NOR_H
#define __HFSSS_HAL_NOR_H

#include "common/common.h"
#include "common/mutex.h"

#define HAL_NOR_SECTOR_SIZE 512
#define HAL_NOR_BLOCK_SIZE 65536  /* 64KB erase block */

/* HAL NOR Device */
struct hal_nor_dev {
    u32 size;
    u32 sector_size;
    u32 block_size;
    u8 *data;
    struct mutex lock;
    bool initialized;
    void *media_ctx;
};

/* Function Prototypes */
int hal_nor_dev_init(struct hal_nor_dev *dev, u32 size, void *media_ctx);
void hal_nor_dev_cleanup(struct hal_nor_dev *dev);
int hal_nor_read(struct hal_nor_dev *dev, u32 addr, void *data, u32 len);
int hal_nor_write(struct hal_nor_dev *dev, u32 addr, const void *data, u32 len);
int hal_nor_erase(struct hal_nor_dev *dev, u32 addr, u32 len);
int hal_nor_erase_sector(struct hal_nor_dev *dev, u32 addr);
int hal_nor_erase_block(struct hal_nor_dev *dev, u32 addr);
int hal_nor_save(struct hal_nor_dev *dev, const char *filepath);
int hal_nor_load(struct hal_nor_dev *dev, const char *filepath);

#endif /* __HFSSS_HAL_NOR_H */
