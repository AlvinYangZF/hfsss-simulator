#include "sssim.h"
#include "common/boot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* NOR device size for SysInfo storage */
#define SSSIM_NOR_SIZE  (64 * 1024)

static bool file_exists(const char *path)
{
    struct stat st;
    return (path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

void sssim_config_default(struct sssim_config *config)
{
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));

    /* Default SSD capacity: 1TB */
    config->lba_size = 4096;
    config->total_lbas = (1ULL << 40) / config->lba_size;  /* 1TB */

    /* Default NAND configuration - TLC */
    config->nand_type = NAND_TYPE_TLC;
    config->page_size = 16384;           /* 16KB page */
    config->spare_size = 64;               /* 64 bytes spare */
    config->pages_per_block = 256;        /* 4MB per block */
    config->blocks_per_plane = 2048;      /* 8GB per plane */
    config->planes_per_die = 2;            /* 16GB per die */
    config->dies_per_chip = 2;             /* 32GB per chip */
    config->chips_per_channel = 4;         /* 128GB per channel */
    config->channel_count = 8;              /* 1TB total */

    /* FTL configuration */
    config->op_ratio = 20;                  /* 20% over-provisioning */
    config->gc_policy = GC_POLICY_FIFO;
    config->gc_threshold = 10;
    config->gc_hiwater = 20;
    config->gc_lowater = 5;
}

int sssim_init(struct sssim_ctx *ctx, struct sssim_config *config)
{
    struct media_config media_cfg;
    struct ftl_config ftl_cfg;
    int ret;

    if (!ctx || !config) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    memcpy(&ctx->config, config, sizeof(*config));

    /* Initialize media layer */
    memset(&media_cfg, 0, sizeof(media_cfg));
    media_cfg.channel_count = config->channel_count;
    media_cfg.chips_per_channel = config->chips_per_channel;
    media_cfg.dies_per_chip = config->dies_per_chip;
    media_cfg.planes_per_die = config->planes_per_die;
    media_cfg.blocks_per_plane = config->blocks_per_plane;
    media_cfg.pages_per_block = config->pages_per_block;
    media_cfg.page_size = config->page_size;
    media_cfg.spare_size = config->spare_size;
    media_cfg.nand_type = config->nand_type;

    ret = media_init(&ctx->media, &media_cfg);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Load NAND image from file if persistence path is configured */
    if (file_exists(config->nand_image_path)) {
        ret = media_load(&ctx->media, config->nand_image_path);
        if (ret != HFSSS_OK) {
            media_cleanup(&ctx->media);
            return ret;
        }
    }

    /* Initialize NOR device for SysInfo storage */
    ret = hal_nor_dev_init(&ctx->nor_dev, SSSIM_NOR_SIZE, NULL);
    if (ret != HFSSS_OK) {
        media_cleanup(&ctx->media);
        return ret;
    }

    /* Load NOR image from file if available */
    if (file_exists(config->nor_image_path)) {
        ret = hal_nor_load(&ctx->nor_dev, config->nor_image_path);
        if (ret != HFSSS_OK) {
            /* Non-fatal: proceed with fresh NOR */
            (void)ret;
        }
    }

    /* Initialize HAL NAND device */
    ret = hal_nand_dev_init(&ctx->nand_dev,
                            config->channel_count,
                            config->chips_per_channel,
                            config->dies_per_chip,
                            config->planes_per_die,
                            config->blocks_per_plane,
                            config->pages_per_block,
                            config->page_size,
                            config->spare_size,
                            &ctx->media);
    if (ret != HFSSS_OK) {
        hal_nor_dev_cleanup(&ctx->nor_dev);
        media_cleanup(&ctx->media);
        return ret;
    }

    /* Initialize HAL */
    ret = hal_init(&ctx->hal, &ctx->nand_dev);
    if (ret != HFSSS_OK) {
        hal_nor_dev_cleanup(&ctx->nor_dev);
        media_cleanup(&ctx->media);
        return ret;
    }

    /* Initialize FTL */
    memset(&ftl_cfg, 0, sizeof(ftl_cfg));
    ftl_cfg.total_lbas = config->total_lbas;
    ftl_cfg.page_size = config->page_size;
    ftl_cfg.pages_per_block = config->pages_per_block;
    ftl_cfg.blocks_per_plane = config->blocks_per_plane;
    ftl_cfg.planes_per_die = config->planes_per_die;
    ftl_cfg.dies_per_chip = config->dies_per_chip;
    ftl_cfg.chips_per_channel = config->chips_per_channel;
    ftl_cfg.channel_count = config->channel_count;
    ftl_cfg.op_ratio = config->op_ratio;
    ftl_cfg.gc_policy = config->gc_policy;
    ftl_cfg.gc_threshold = config->gc_threshold;
    ftl_cfg.gc_hiwater = config->gc_hiwater;
    ftl_cfg.gc_lowater = config->gc_lowater;

    ret = ftl_init(&ctx->ftl, &ftl_cfg, &ctx->hal);
    if (ret != HFSSS_OK) {
        hal_cleanup(&ctx->hal);
        hal_nor_dev_cleanup(&ctx->nor_dev);
        media_cleanup(&ctx->media);
        return ret;
    }

    /* Initialize UPLP with default supercapacitor parameters */
    uplp_init(&ctx->uplp, 5.0, 0.1, 5.0, 2.7, 10.0);

    ctx->initialized = true;
    return HFSSS_OK;
}

void sssim_cleanup(struct sssim_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    if (!ctx->initialized) {
        return;
    }

    uplp_cleanup(&ctx->uplp);
    ftl_cleanup(&ctx->ftl);
    hal_cleanup(&ctx->hal);
    hal_nor_dev_cleanup(&ctx->nor_dev);
    media_cleanup(&ctx->media);

    memset(ctx, 0, sizeof(*ctx));
}

int sssim_read(struct sssim_ctx *ctx, u64 lba, u32 count, void *data)
{
    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    if (!data && count > 0) {
        return HFSSS_ERR_INVAL;
    }

    if (lba + count > ctx->config.total_lbas) {
        return HFSSS_ERR_INVAL;
    }

    /* FTL expects len in bytes */
    return ftl_read(&ctx->ftl, lba, count * ctx->config.lba_size, data);
}

int sssim_write(struct sssim_ctx *ctx, u64 lba, u32 count, const void *data)
{
    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    if (!data && count > 0) {
        return HFSSS_ERR_INVAL;
    }

    if (lba + count > ctx->config.total_lbas) {
        return HFSSS_ERR_INVAL;
    }

    /* FTL expects len in bytes */
    return ftl_write(&ctx->ftl, lba, count * ctx->config.lba_size, data);
}

int sssim_trim(struct sssim_ctx *ctx, u64 lba, u32 count)
{
    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    if (lba + count > ctx->config.total_lbas) {
        return HFSSS_ERR_INVAL;
    }

    /* FTL expects len in bytes */
    return ftl_trim(&ctx->ftl, lba, count * ctx->config.lba_size);
}

int sssim_flush(struct sssim_ctx *ctx)
{
    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    return ftl_flush(&ctx->ftl);
}

void sssim_get_stats(struct sssim_ctx *ctx, struct ftl_stats *stats)
{
    if (!ctx || !stats) {
        return;
    }

    ftl_get_stats(&ctx->ftl, stats);
}

void sssim_reset_stats(struct sssim_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    ftl_reset_stats(&ctx->ftl);
}

int sssim_shutdown(struct sssim_ctx *ctx)
{
    struct sysinfo_partition sysinfo;
    int ret;

    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    /* Flush FTL: close CWBs, flush journal, write L2P checkpoint */
    ret = ftl_flush(&ctx->ftl);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Save NAND image (includes superblock pages with L2P checkpoint) */
    if (ctx->config.nand_image_path[0]) {
        ret = media_save(&ctx->media, ctx->config.nand_image_path);
        if (ret != HFSSS_OK) {
            return ret;
        }
    }

    /* Write SysInfo clean shutdown marker to NOR */
    if (ctx->config.nor_image_path[0]) {
        memset(&sysinfo, 0, sizeof(sysinfo));
        sysinfo.magic = SYSINFO_MAGIC;
        sysinfo.clean_shutdown_marker_valid = 1;
        sysinfo.clean_shutdown_marker = SYSINFO_CLEAN_MARKER;
        sysinfo.checkpoint_seq_at_shutdown =
            (uint32_t)ctx->ftl.sb.ckpt_sequence;
        sysinfo.crc32 = hfsss_crc32((const u8 *)&sysinfo,
                                     sizeof(sysinfo) - sizeof(uint32_t));

        /* Erase the sector and write SysInfo at offset 0 */
        hal_nor_erase_sector(&ctx->nor_dev, 0);
        hal_nor_write(&ctx->nor_dev, 0, &sysinfo, sizeof(sysinfo));

        ret = hal_nor_save(&ctx->nor_dev, ctx->config.nor_image_path);
        if (ret != HFSSS_OK) {
            return ret;
        }
    }

    return HFSSS_OK;
}
