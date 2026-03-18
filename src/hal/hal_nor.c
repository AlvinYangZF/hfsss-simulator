#include "hal/hal_nor.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* NOR file magic and version */
#define HAL_NOR_FILE_MAGIC 0x48464E52  /* "HFNR" */
#define HAL_NOR_FILE_VERSION 1

struct hal_nor_file_header {
    u32 magic;
    u32 version;
    u32 size;
    u32 sector_size;
    u32 block_size;
};

int hal_nor_dev_init(struct hal_nor_dev *dev, u32 size, void *media_ctx)
{
    int ret;

    if (!dev || size == 0) {
        return HFSSS_ERR_INVAL;
    }

    memset(dev, 0, sizeof(*dev));

    /* Allocate data buffer */
    dev->data = (u8 *)malloc(size);
    if (!dev->data) {
        return HFSSS_ERR_NOMEM;
    }

    /* Initialize NOR to all 0xFF (erased state) */
    memset(dev->data, 0xFF, size);

    dev->size = size;
    dev->sector_size = HAL_NOR_SECTOR_SIZE;
    dev->block_size = HAL_NOR_BLOCK_SIZE;
    dev->media_ctx = media_ctx;

    ret = mutex_init(&dev->lock);
    if (ret != HFSSS_OK) {
        free(dev->data);
        memset(dev, 0, sizeof(*dev));
        return ret;
    }

    dev->initialized = true;
    return HFSSS_OK;
}

void hal_nor_dev_cleanup(struct hal_nor_dev *dev)
{
    if (!dev) {
        return;
    }

    mutex_lock(&dev->lock, 0);

    if (dev->data) {
        free(dev->data);
    }

    dev->initialized = false;

    mutex_unlock(&dev->lock);
    mutex_cleanup(&dev->lock);

    memset(dev, 0, sizeof(*dev));
}

int hal_nor_read(struct hal_nor_dev *dev, u32 addr, void *data, u32 len)
{
    if (!dev || !dev->initialized || !data) {
        return HFSSS_ERR_INVAL;
    }

    if (addr + len > dev->size) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&dev->lock, 0);
    memcpy(data, dev->data + addr, len);
    mutex_unlock(&dev->lock);

    return HFSSS_OK;
}

int hal_nor_write(struct hal_nor_dev *dev, u32 addr, const void *data, u32 len)
{
    u32 i;
    const u8 *src;
    u8 *dst;

    if (!dev || !dev->initialized || !data) {
        return HFSSS_ERR_INVAL;
    }

    if (addr + len > dev->size) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&dev->lock, 0);

    src = (const u8 *)data;
    dst = dev->data + addr;

    /* NOR Flash: can only clear bits (0 -> 1 not allowed without erase) */
    for (i = 0; i < len; i++) {
        dst[i] &= src[i];
    }

    mutex_unlock(&dev->lock);

    return HFSSS_OK;
}

int hal_nor_erase_sector(struct hal_nor_dev *dev, u32 addr)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }

    if (addr % dev->sector_size != 0) {
        return HFSSS_ERR_INVAL;
    }

    if (addr + dev->sector_size > dev->size) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&dev->lock, 0);
    memset(dev->data + addr, 0xFF, dev->sector_size);
    mutex_unlock(&dev->lock);

    return HFSSS_OK;
}

int hal_nor_erase_block(struct hal_nor_dev *dev, u32 addr)
{
    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }

    if (addr % dev->block_size != 0) {
        return HFSSS_ERR_INVAL;
    }

    if (addr + dev->block_size > dev->size) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&dev->lock, 0);
    memset(dev->data + addr, 0xFF, dev->block_size);
    mutex_unlock(&dev->lock);

    return HFSSS_OK;
}

/* ------------------------------------------------------------------ */
/* hal_nor_save / hal_nor_load                                         */
/* ------------------------------------------------------------------ */

int hal_nor_save(struct hal_nor_dev *dev, const char *filepath)
{
    FILE *f;
    struct hal_nor_file_header hdr;
    u32 crc;

    if (!dev || !dev->initialized || !filepath) {
        return HFSSS_ERR_INVAL;
    }

    f = fopen(filepath, "wb");
    if (!f) {
        return HFSSS_ERR_IO;
    }

    hdr.magic       = HAL_NOR_FILE_MAGIC;
    hdr.version     = HAL_NOR_FILE_VERSION;
    hdr.size        = dev->size;
    hdr.sector_size = dev->sector_size;
    hdr.block_size  = dev->block_size;

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return HFSSS_ERR_IO;
    }

    mutex_lock(&dev->lock, 0);
    if (fwrite(dev->data, dev->size, 1, f) != 1) {
        mutex_unlock(&dev->lock);
        fclose(f);
        return HFSSS_ERR_IO;
    }
    crc = hfsss_crc32(dev->data, dev->size);
    mutex_unlock(&dev->lock);

    if (fwrite(&crc, sizeof(crc), 1, f) != 1) {
        fclose(f);
        return HFSSS_ERR_IO;
    }

    fclose(f);
    return HFSSS_OK;
}

int hal_nor_load(struct hal_nor_dev *dev, const char *filepath)
{
    FILE *f;
    struct hal_nor_file_header hdr;
    u32 crc;
    u32 stored_crc;

    if (!dev || !dev->initialized || !filepath) {
        return HFSSS_ERR_INVAL;
    }

    f = fopen(filepath, "rb");
    if (!f) {
        return HFSSS_ERR_NOENT;
    }

    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return HFSSS_ERR_IO;
    }

    if (hdr.magic != HAL_NOR_FILE_MAGIC || hdr.version != HAL_NOR_FILE_VERSION) {
        fclose(f);
        return HFSSS_ERR_INVAL;
    }

    if (hdr.size != dev->size) {
        fclose(f);
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&dev->lock, 0);
    if (fread(dev->data, dev->size, 1, f) != 1) {
        mutex_unlock(&dev->lock);
        fclose(f);
        return HFSSS_ERR_IO;
    }
    mutex_unlock(&dev->lock);

    if (fread(&stored_crc, sizeof(stored_crc), 1, f) != 1) {
        fclose(f);
        return HFSSS_ERR_IO;
    }

    crc = hfsss_crc32(dev->data, dev->size);
    if (crc != stored_crc) {
        fclose(f);
        return HFSSS_ERR_IO;
    }

    fclose(f);
    return HFSSS_OK;
}

int hal_nor_erase(struct hal_nor_dev *dev, u32 addr, u32 len)
{
    u32 end_addr;
    u32 cur_addr;
    int ret;

    if (!dev || !dev->initialized) {
        return HFSSS_ERR_INVAL;
    }

    if (len == 0) {
        return HFSSS_OK;
    }

    end_addr = addr + len;
    if (end_addr > dev->size) {
        return HFSSS_ERR_INVAL;
    }

    /* Align to block size if possible, otherwise use sector */
    cur_addr = addr;
    while (cur_addr < end_addr) {
        u32 remaining = end_addr - cur_addr;
        u32 erase_len;

        if (cur_addr % dev->block_size == 0 && remaining >= dev->block_size) {
            erase_len = dev->block_size;
            ret = hal_nor_erase_block(dev, cur_addr);
        } else if (cur_addr % dev->sector_size == 0 && remaining >= dev->sector_size) {
            erase_len = dev->sector_size;
            ret = hal_nor_erase_sector(dev, cur_addr);
        } else {
            /* Unaligned erase - not allowed in real NOR, but for simulation, let's just erase the sector */
            u32 sector_aligned = cur_addr & ~(dev->sector_size - 1);
            erase_len = dev->sector_size;
            ret = hal_nor_erase_sector(dev, sector_aligned);
        }

        if (ret != HFSSS_OK) {
            return ret;
        }

        cur_addr += erase_len;
    }

    return HFSSS_OK;
}
