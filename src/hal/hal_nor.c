#include "hal/hal_nor.h"
#include <string.h>

int hal_nor_dev_init(struct hal_nor_dev *dev, u32 size, void *media_ctx)
{
    if (!dev) {
        return HFSSS_ERR_INVAL;
    }

    memset(dev, 0, sizeof(*dev));
    dev->size = size;
    dev->media_ctx = media_ctx;

    return HFSSS_OK;
}

void hal_nor_dev_cleanup(struct hal_nor_dev *dev)
{
    if (!dev) {
        return;
    }

    memset(dev, 0, sizeof(*dev));
}

int hal_nor_read(struct hal_nor_dev *dev, u32 addr, void *data, u32 len)
{
    (void)dev;
    (void)addr;
    (void)data;
    (void)len;
    /* Placeholder - NOR Flash not implemented yet */
    return HFSSS_ERR_NOTSUPP;
}

int hal_nor_write(struct hal_nor_dev *dev, u32 addr, const void *data, u32 len)
{
    (void)dev;
    (void)addr;
    (void)data;
    (void)len;
    /* Placeholder - NOR Flash not implemented yet */
    return HFSSS_ERR_NOTSUPP;
}

int hal_nor_erase(struct hal_nor_dev *dev, u32 addr, u32 len)
{
    (void)dev;
    (void)addr;
    (void)len;
    /* Placeholder - NOR Flash not implemented yet */
    return HFSSS_ERR_NOTSUPP;
}
