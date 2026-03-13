#ifndef __HFSSS_HAL_NOR_H
#define __HFSSS_HAL_NOR_H

#include "common/common.h"

/* HAL NOR Device (Placeholder for future implementation) */
struct hal_nor_dev {
    u32 size;
    void *media_ctx;
};

/* Function Prototypes (Placeholders) */
int hal_nor_dev_init(struct hal_nor_dev *dev, u32 size, void *media_ctx);
void hal_nor_dev_cleanup(struct hal_nor_dev *dev);
int hal_nor_read(struct hal_nor_dev *dev, u32 addr, void *data, u32 len);
int hal_nor_write(struct hal_nor_dev *dev, u32 addr, const void *data, u32 len);
int hal_nor_erase(struct hal_nor_dev *dev, u32 addr, u32 len);

#endif /* __HFSSS_HAL_NOR_H */
