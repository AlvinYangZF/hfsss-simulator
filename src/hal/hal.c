#include "hal/hal.h"
#include <stdlib.h>
#include <string.h>

int hal_init(struct hal_ctx *ctx, struct hal_nand_dev *nand_dev)
{
    int ret;

    if (!ctx || !nand_dev) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    /* Initialize lock */
    ret = mutex_init(&ctx->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Set NAND device */
    ctx->nand = nand_dev;

    /* Initialize other devices as placeholders */
    ctx->nor = NULL;
    ctx->pci = NULL;
    ctx->power = NULL;

    ctx->initialized = true;
    return HFSSS_OK;
}

void hal_cleanup(struct hal_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->lock, 0);

    ctx->initialized = false;
    ctx->nand = NULL;
    ctx->nor = NULL;
    ctx->pci = NULL;
    ctx->power = NULL;

    mutex_unlock(&ctx->lock);
    mutex_cleanup(&ctx->lock);

    memset(ctx, 0, sizeof(*ctx));
}

int hal_nand_read_sync(struct hal_ctx *ctx, u32 ch, u32 chip, u32 die,
                        u32 plane, u32 block, u32 page, void *data, void *spare)
{
    struct hal_nand_cmd cmd;
    int ret;
    u64 start_time;

    if (!ctx || !ctx->initialized || !data) {
        return HFSSS_ERR_INVAL;
    }

    start_time = get_time_ns();

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = HAL_NAND_OP_READ;
    cmd.ch = ch;
    cmd.chip = chip;
    cmd.die = die;
    cmd.plane = plane;
    cmd.block = block;
    cmd.page = page;
    cmd.data = data;
    cmd.spare = spare;

    ret = hal_nand_read(ctx->nand, &cmd);

    /* Update stats */
    mutex_lock(&ctx->lock, 0);
    if (ret == HFSSS_OK) {
        ctx->stats.nand_read_count++;
        ctx->stats.nand_read_bytes += ctx->nand->page_size;
        ctx->stats.total_read_ns += get_time_ns() - start_time;
    }
    mutex_unlock(&ctx->lock);

    return ret;
}

int hal_nand_program_sync(struct hal_ctx *ctx, u32 ch, u32 chip, u32 die,
                           u32 plane, u32 block, u32 page, const void *data, const void *spare)
{
    struct hal_nand_cmd cmd;
    int ret;
    u64 start_time;

    if (!ctx || !ctx->initialized || !data) {
        return HFSSS_ERR_INVAL;
    }

    start_time = get_time_ns();

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = HAL_NAND_OP_PROGRAM;
    cmd.ch = ch;
    cmd.chip = chip;
    cmd.die = die;
    cmd.plane = plane;
    cmd.block = block;
    cmd.page = page;
    cmd.data = (void *)data;
    cmd.spare = (void *)spare;

    ret = hal_nand_program(ctx->nand, &cmd);

    /* Update stats */
    mutex_lock(&ctx->lock, 0);
    if (ret == HFSSS_OK) {
        ctx->stats.nand_write_count++;
        ctx->stats.nand_write_bytes += ctx->nand->page_size;
        ctx->stats.total_write_ns += get_time_ns() - start_time;
    }
    mutex_unlock(&ctx->lock);

    return ret;
}

int hal_nand_erase_sync(struct hal_ctx *ctx, u32 ch, u32 chip, u32 die,
                         u32 plane, u32 block)
{
    struct hal_nand_cmd cmd;
    int ret;
    u64 start_time;

    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    start_time = get_time_ns();

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = HAL_NAND_OP_ERASE;
    cmd.ch = ch;
    cmd.chip = chip;
    cmd.die = die;
    cmd.plane = plane;
    cmd.block = block;

    ret = hal_nand_erase(ctx->nand, &cmd);

    /* Update stats */
    mutex_lock(&ctx->lock, 0);
    if (ret == HFSSS_OK) {
        ctx->stats.nand_erase_count++;
        ctx->stats.total_erase_ns += get_time_ns() - start_time;
    }
    mutex_unlock(&ctx->lock);

    return ret;
}

int hal_ctx_nand_is_bad_block(struct hal_ctx *ctx, u32 ch, u32 chip,
                               u32 die, u32 plane, u32 block)
{
    if (!ctx || !ctx->initialized) {
        return -1;
    }

    return hal_nand_is_bad_block(ctx->nand, ch, chip, die, plane, block);
}

int hal_ctx_nand_mark_bad_block(struct hal_ctx *ctx, u32 ch, u32 chip,
                                 u32 die, u32 plane, u32 block)
{
    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    return hal_nand_mark_bad_block(ctx->nand, ch, chip, die, plane, block);
}

u32 hal_ctx_nand_get_erase_count(struct hal_ctx *ctx, u32 ch, u32 chip,
                                   u32 die, u32 plane, u32 block)
{
    if (!ctx || !ctx->initialized) {
        return 0;
    }

    return hal_nand_get_erase_count(ctx->nand, ch, chip, die, plane, block);
}

void hal_get_stats(struct hal_ctx *ctx, struct hal_stats *stats)
{
    if (!ctx || !stats) {
        return;
    }

    mutex_lock(&ctx->lock, 0);
    memcpy(stats, &ctx->stats, sizeof(*stats));
    mutex_unlock(&ctx->lock);
}

void hal_reset_stats(struct hal_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->lock, 0);
    memset(&ctx->stats, 0, sizeof(ctx->stats));
    mutex_unlock(&ctx->lock);
}
