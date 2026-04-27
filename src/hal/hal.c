#include "hal/hal.h"
#include "common/trace.h"
#include "common/io_err_trace.h"
#include "media/media.h"
#ifdef HFSSS_DEBUG_TRACE
#include "ftl/mapping.h"  /* union ppn layout; debug-only cross-layer pull */
#endif
#include <stdlib.h>
#include <string.h>

#ifdef HFSSS_DEBUG_TRACE
static inline uint64_t hal_trace_encode_ppn(const struct hal_nand_cmd *c)
{
    union ppn p;
    p.raw = 0;
    p.bits.channel = c->ch;
    p.bits.chip = c->chip;
    p.bits.die = c->die;
    p.bits.plane = c->plane;
    p.bits.block = c->block;
    p.bits.page = c->page;
    return p.raw;
}
#endif

int hal_init_full(struct hal_ctx *ctx, struct hal_nand_dev *nand_dev,
                  struct hal_nor_dev *nor_dev, struct hal_pci_ctx *pci_ctx,
                  struct hal_power_ctx *power_ctx)
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

    /* Set devices */
    ctx->nand = nand_dev;
    ctx->nor = nor_dev;
    ctx->pci = pci_ctx;
    ctx->power = power_ctx;

    ctx->initialized = true;
    return HFSSS_OK;
}

int hal_init(struct hal_ctx *ctx, struct hal_nand_dev *nand_dev)
{
    /* Backward-compatible init: only NAND device, others NULL */
    return hal_init_full(ctx, nand_dev, NULL, NULL, NULL);
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

#ifdef HFSSS_DEBUG_TRACE
    {
        uint32_t crc = (ret == 0 && data != NULL)
                       ? trace_crc32c(data, ctx->nand->page_size)
                       : 0;
        /* lba not in scope here; analyzer joins on (op, ppn). Pass 0 for lba.
         * Encode the full (ch, chip, die, plane, block, page) into the same
         * layout as union ppn so T4.ppn == T5.ppn for matching ops. */
        uint64_t ppn_raw = hal_trace_encode_ppn(&cmd);
        TRACE_EMIT(TRACE_POINT_T5_POST_HAL, (uint32_t)TRACE_OP_READ,
                   0, ppn_raw, crc, (uint32_t)ret);
    }
#endif

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

#ifdef HFSSS_DEBUG_TRACE
    {
        uint32_t crc = (ret == 0 && data != NULL)
                       ? trace_crc32c(data, ctx->nand->page_size)
                       : 0;
        uint64_t ppn_raw = hal_trace_encode_ppn(&cmd);
        TRACE_EMIT(TRACE_POINT_T5_POST_HAL, (uint32_t)TRACE_OP_WRITE,
                   0, ppn_raw, crc, (uint32_t)ret);
    }
#endif

    if (ret != HFSSS_OK) {
        IO_ERR_TRACE("L=hal_nand_program_sync ch=%u chip=%u die=%u plane=%u block=%u page=%u rc=%d",
                     ch, chip, die, plane, block, page, ret);
    }

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

/* NOR wrapper functions */
int hal_nor_read_sync(struct hal_ctx *ctx, u32 addr, void *data, u32 len)
{
    int ret;

    if (!ctx || !ctx->initialized || !ctx->nor || !data) {
        return HFSSS_ERR_INVAL;
    }

    ret = hal_nor_read(ctx->nor, addr, data, len);

    mutex_lock(&ctx->lock, 0);
    if (ret == HFSSS_OK) {
        ctx->stats.nor_read_count++;
    }
    mutex_unlock(&ctx->lock);

    return ret;
}

int hal_nor_write_sync(struct hal_ctx *ctx, u32 addr, const void *data, u32 len)
{
    int ret;

    if (!ctx || !ctx->initialized || !ctx->nor || !data) {
        return HFSSS_ERR_INVAL;
    }

    ret = hal_nor_write(ctx->nor, addr, data, len);

    mutex_lock(&ctx->lock, 0);
    if (ret == HFSSS_OK) {
        ctx->stats.nor_write_count++;
    }
    mutex_unlock(&ctx->lock);

    return ret;
}

int hal_nor_erase_sync(struct hal_ctx *ctx, u32 addr, u32 len)
{
    int ret;

    if (!ctx || !ctx->initialized || !ctx->nor) {
        return HFSSS_ERR_INVAL;
    }

    ret = hal_nor_erase(ctx->nor, addr, len);

    mutex_lock(&ctx->lock, 0);
    if (ret == HFSSS_OK) {
        ctx->stats.nor_erase_count++;
    }
    mutex_unlock(&ctx->lock);

    return ret;
}

/* PCI wrapper functions */
int hal_pci_submit_completion_sync(struct hal_ctx *ctx, const struct hal_pci_completion *comp)
{
    if (!ctx || !ctx->initialized || !ctx->pci || !comp) {
        return HFSSS_ERR_INVAL;
    }

    return hal_pci_submit_completion(ctx->pci, comp);
}

int hal_pci_poll_completion_sync(struct hal_ctx *ctx, struct hal_pci_completion *comp)
{
    if (!ctx || !ctx->initialized || !ctx->pci || !comp) {
        return HFSSS_ERR_INVAL;
    }

    return hal_pci_poll_completion(ctx->pci, comp);
}

int hal_pci_ns_attach_sync(struct hal_ctx *ctx, u32 nsid, u64 size, u32 lba_size)
{
    if (!ctx || !ctx->initialized || !ctx->pci) {
        return HFSSS_ERR_INVAL;
    }

    return hal_pci_ns_attach(ctx->pci, nsid, size, lba_size);
}

int hal_pci_ns_detach_sync(struct hal_ctx *ctx, u32 nsid)
{
    if (!ctx || !ctx->initialized || !ctx->pci) {
        return HFSSS_ERR_INVAL;
    }

    return hal_pci_ns_detach(ctx->pci, nsid);
}

int hal_pci_ns_get_info_sync(struct hal_ctx *ctx, u32 nsid, struct hal_pci_namespace *info)
{
    if (!ctx || !ctx->initialized || !ctx->pci || !info) {
        return HFSSS_ERR_INVAL;
    }

    return hal_pci_ns_get_info(ctx->pci, nsid, info);
}

/* Power wrapper functions */
int hal_power_set_state_sync(struct hal_ctx *ctx, enum hal_nvme_power_state state)
{
    if (!ctx || !ctx->initialized || !ctx->power) {
        return HFSSS_ERR_INVAL;
    }

    return hal_power_set_state(ctx->power, state);
}

enum hal_nvme_power_state hal_power_get_state_sync(struct hal_ctx *ctx)
{
    if (!ctx || !ctx->initialized || !ctx->power) {
        return HAL_POWER_PS4;
    }

    return hal_power_get_state(ctx->power);
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

const struct nand_profile *hal_get_profile(struct hal_ctx *ctx)
{
    if (!ctx || !ctx->nand) {
        return NULL;
    }
    struct media_ctx *m = (struct media_ctx *)ctx->nand->media_ctx;
    if (!m || !m->initialized) {
        return NULL;
    }
    return m->profile;
}
