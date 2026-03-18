#ifndef __HFSSS_SSSIM_H
#define __HFSSS_SSSIM_H

#include "common/common.h"
#include "media/media.h"
#include "hal/hal.h"
#include "hal/hal_nor.h"
#include "ftl/ftl.h"

#define SSSIM_PATH_LEN 512

/* SSD Simulator Configuration */
struct sssim_config {
    /* SSD Capacity */
    u64 total_lbas;
    u32 lba_size;           /* typically 4096 */

    /* NAND Configuration */
    u32 page_size;          /* NAND page size */
    u32 spare_size;         /* Spare area size */
    u32 pages_per_block;
    u32 blocks_per_plane;
    u32 planes_per_die;
    u32 dies_per_chip;
    u32 chips_per_channel;
    u32 channel_count;

    /* FTL Configuration */
    u32 op_ratio;           /* Over-provisioning percentage */
    enum gc_policy gc_policy;
    u32 gc_threshold;
    u32 gc_hiwater;
    u32 gc_lowater;

    /* Media Configuration */
    enum nand_type nand_type;

    /* Persistence: empty string = no persistence */
    char nand_image_path[SSSIM_PATH_LEN];
    char nor_image_path[SSSIM_PATH_LEN];
};

/* SSD Simulator Context */
struct sssim_ctx {
    struct sssim_config config;
    struct media_ctx media;
    struct hal_nand_dev nand_dev;
    struct hal_nor_dev nor_dev;
    struct hal_ctx hal;
    struct ftl_ctx ftl;
    bool initialized;
};

/* Function Prototypes */
int sssim_init(struct sssim_ctx *ctx, struct sssim_config *config);
int sssim_shutdown(struct sssim_ctx *ctx);
void sssim_cleanup(struct sssim_ctx *ctx);
int sssim_read(struct sssim_ctx *ctx, u64 lba, u32 count, void *data);
int sssim_write(struct sssim_ctx *ctx, u64 lba, u32 count, const void *data);
int sssim_trim(struct sssim_ctx *ctx, u64 lba, u32 count);
int sssim_flush(struct sssim_ctx *ctx);
void sssim_get_stats(struct sssim_ctx *ctx, struct ftl_stats *stats);
void sssim_reset_stats(struct sssim_ctx *ctx);

/* Default configuration helper */
void sssim_config_default(struct sssim_config *config);

#endif /* __HFSSS_SSSIM_H */
