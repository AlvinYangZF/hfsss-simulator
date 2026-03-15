#include "ftl/ftl.h"
#include <stdlib.h>
#include <string.h>

/* Internal helper functions */
static struct cwb *ftl_get_cwb(struct ftl_ctx *ctx, u32 channel, u32 plane);
static int ftl_allocate_cwb(struct ftl_ctx *ctx, u32 channel, u32 plane);
static int ftl_write_page(struct ftl_ctx *ctx, u64 lba, const void *data);
static int ftl_read_page(struct ftl_ctx *ctx, u64 lba, void *data);
static union ppn ftl_encode_ppn(u32 channel, u32 chip, u32 die, u32 plane, u32 block, u32 page);
static void ftl_decode_ppn(union ppn ppn, u32 *channel, u32 *chip, u32 *die, u32 *plane, u32 *block, u32 *page);

static union ppn ftl_encode_ppn(u32 channel, u32 chip, u32 die, u32 plane, u32 block, u32 page)
{
    union ppn ppn;
    ppn.raw = 0;
    ppn.bits.channel = channel;
    ppn.bits.chip = chip;
    ppn.bits.die = die;
    ppn.bits.plane = plane;
    ppn.bits.block = block;
    ppn.bits.page = page;
    return ppn;
}

static void ftl_decode_ppn(union ppn ppn, u32 *channel, u32 *chip, u32 *die, u32 *plane, u32 *block, u32 *page)
{
    if (channel) *channel = ppn.bits.channel;
    if (chip) *chip = ppn.bits.chip;
    if (die) *die = ppn.bits.die;
    if (plane) *plane = ppn.bits.plane;
    if (block) *block = ppn.bits.block;
    if (page) *page = ppn.bits.page;
}

static struct cwb *ftl_get_cwb(struct ftl_ctx *ctx, u32 channel, u32 plane)
{
    u32 idx = channel * ctx->config.planes_per_die + plane;

    if (idx >= ctx->cwb_count) {
        return NULL;
    }

    return &ctx->cwbs[idx];
}

static int ftl_allocate_cwb(struct ftl_ctx *ctx, u32 channel, u32 plane)
{
    struct cwb *cwb;
    struct block_desc *block;

    cwb = ftl_get_cwb(ctx, channel, plane);
    if (!cwb) {
        return HFSSS_ERR_INVAL;
    }

    /* If CWB already has an open block and it's not full, use it */
    if (cwb->block && cwb->block->state == FTL_BLOCK_OPEN &&
        cwb->current_page < ctx->config.pages_per_block) {
        return HFSSS_OK;
    }

    /* If CWB has a block, close it */
    if (cwb->block) {
        block_mark_closed(&ctx->block_mgr, cwb->block);
    }

    /* Allocate a new block */
    block = block_alloc(&ctx->block_mgr);
    if (!block) {
        return HFSSS_ERR_NOSPC;
    }

    /* Initialize CWB */
    cwb->block = block;
    cwb->current_page = 0;
    cwb->last_write_ts = get_time_ns();

    return HFSSS_OK;
}

static int ftl_write_page(struct ftl_ctx *ctx, u64 lba, const void *data)
{
    struct cwb *cwb;
    union ppn ppn;
    union ppn old_ppn;
    u32 ch, plane;
    int ret;
    int write_retry;
    const int max_write_retries = 3;
    u8 *verify_buf = NULL;

    /* Select channel and plane (simple round-robin) */
    ch = lba % ctx->config.channel_count;
    plane = (lba / ctx->config.channel_count) % ctx->config.planes_per_die;

    /* Get CWB */
    cwb = ftl_get_cwb(ctx, ch, plane);
    if (!cwb) {
        return HFSSS_ERR_INVAL;
    }

    /* Ensure CWB has a block */
    ret = ftl_allocate_cwb(ctx, ch, plane);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Encode PPN */
    ppn = ftl_encode_ppn(ch, 0, 0, plane, cwb->block->block_id, cwb->current_page);

    /* Write with retry logic */
    for (write_retry = 0; write_retry < max_write_retries; write_retry++) {
        /* Write through HAL */
        ret = hal_nand_program_sync(ctx->hal, ch, 0, 0, plane,
                                     cwb->block->block_id, cwb->current_page, data, NULL);

        if (ret != HFSSS_OK) {
            ctx->error.write_error_count++;
            continue;
        }

        /* Write Verify - read back and verify */
        error_write_verify_attempt(&ctx->error);
        verify_buf = (u8 *)malloc(ctx->config.page_size);
        if (verify_buf) {
            ret = hal_nand_read_sync(ctx->hal, ch, 0, 0, plane,
                                      cwb->block->block_id, cwb->current_page, verify_buf, NULL);
            if (ret == HFSSS_OK && memcmp(data, verify_buf, ctx->config.page_size) == 0) {
                free(verify_buf);
                verify_buf = NULL;
                break; /* Verify passed */
            }

            /* Verify failed */
            error_write_verify_failure(&ctx->error);
            free(verify_buf);
            verify_buf = NULL;
        }
    }

    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Update L2P mapping */
    mapping_update(&ctx->mapping, lba, ppn, &old_ppn);

    /* If there was an old mapping, mark that page as invalid */
    if (old_ppn.raw != 0) {
        u32 old_ch, old_chip, old_die, old_plane, old_block, old_page;
        ftl_decode_ppn(old_ppn, &old_ch, &old_chip, &old_die, &old_plane, &old_block, &old_page);

        /* For now, we don't track per-page validity, just increment invalid count on block */
        /* In a full implementation, we'd need a better way to track this */
    }

    /* Update CWB */
    cwb->current_page++;
    cwb->last_write_ts = get_time_ns();
    cwb->block->valid_page_count++;
    cwb->block->last_write_ts = get_time_ns();

    /* Track host write pages for WAF */
    ctx->stats.host_write_pages++;

    /* Check if block is full */
    if (cwb->current_page >= ctx->config.pages_per_block) {
        block_mark_closed(&ctx->block_mgr, cwb->block);
        cwb->block = NULL;
        cwb->current_page = 0;
    }

    /* Check if GC should be triggered */
    u64 free_blocks = block_get_free_count(&ctx->block_mgr);
    if (gc_should_trigger(&ctx->gc, free_blocks)) {
        ftl_gc_trigger(ctx);
    }

    return HFSSS_OK;
}

static int ftl_read_page(struct ftl_ctx *ctx, u64 lba, void *data)
{
    union ppn ppn;
    u32 ch, chip, die, plane, block, page;
    int ret;
    int retry_count;
    const int voltage_offsets[] = READ_RETRY_VOLTAGE_OFFSETS;
    const int max_retries = READ_RETRY_MAX_ATTEMPTS;

    /* Look up L2P mapping */
    ret = mapping_l2p(&ctx->mapping, lba, &ppn);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Decode PPN */
    ftl_decode_ppn(ppn, &ch, &chip, &die, &plane, &block, &page);

    /* Try reading with different voltage offsets */
    for (retry_count = 0; retry_count < max_retries; retry_count++) {
        /* Read through HAL - for retry, use voltage offset */
        ret = hal_nand_read_sync(ctx->hal, ch, chip, die, plane, block, page, data, NULL);

        if (ret == HFSSS_OK) {
            /* If it's a retry that succeeded, track statistics */
            if (retry_count > 0) {
                error_read_retry_success(&ctx->error);
            }
            return HFSSS_OK;
        }

        /* Track retry attempt */
        if (retry_count > 0) {
            error_read_retry_attempt(&ctx->error);
        }
    }

    /* All retry attempts failed */
    ctx->error.uncorrectable_count++;
    return ret;
}

int ftl_init(struct ftl_ctx *ctx, struct ftl_config *config, struct hal_ctx *hal)
{
    u32 cwb_count;
    u64 l2p_size;
    u64 p2l_size;
    u64 total_blocks;
    u32 op_blocks;
    u32 user_blocks;
    int ret;

    if (!ctx || !config || !hal) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    /* Copy config */
    memcpy(&ctx->config, config, sizeof(*config));
    ctx->hal = hal;

    /* Initialize lock */
    ret = mutex_init(&ctx->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Calculate CWB count */
    cwb_count = config->channel_count * config->planes_per_die;
    ctx->cwb_count = cwb_count;

    /* Allocate CWB array */
    ctx->cwbs = (struct cwb *)calloc(cwb_count, sizeof(struct cwb));
    if (!ctx->cwbs) {
        mutex_cleanup(&ctx->lock);
        return HFSSS_ERR_NOMEM;
    }

    /* Calculate L2P/P2L sizes */
    l2p_size = config->total_lbas;
    p2l_size = (u64)config->channel_count * config->chips_per_channel *
                config->dies_per_chip * config->planes_per_die *
                config->blocks_per_plane * config->pages_per_block;

    /* Initialize mapping */
    ret = mapping_init(&ctx->mapping, l2p_size, p2l_size);
    if (ret != HFSSS_OK) {
        free(ctx->cwbs);
        mutex_cleanup(&ctx->lock);
        return ret;
    }

    /* Calculate over-provisioned blocks */
    total_blocks = (u64)config->channel_count * config->chips_per_channel *
                   config->dies_per_chip * config->planes_per_die *
                   config->blocks_per_plane;
    user_blocks = total_blocks * (100 - config->op_ratio) / 100;
    op_blocks = total_blocks - user_blocks;
    (void)user_blocks;
    (void)op_blocks;

    /* Initialize block manager */
    ret = block_mgr_init(&ctx->block_mgr, config->channel_count, config->chips_per_channel,
                         config->dies_per_chip, config->planes_per_die, config->blocks_per_plane);
    if (ret != HFSSS_OK) {
        mapping_cleanup(&ctx->mapping);
        free(ctx->cwbs);
        mutex_cleanup(&ctx->lock);
        return ret;
    }

    /* Initialize GC */
    ret = gc_init(&ctx->gc, config->gc_policy, config->gc_threshold,
                  config->gc_hiwater, config->gc_lowater);
    if (ret != HFSSS_OK) {
        block_mgr_cleanup(&ctx->block_mgr);
        mapping_cleanup(&ctx->mapping);
        free(ctx->cwbs);
        mutex_cleanup(&ctx->lock);
        return ret;
    }

    /* Initialize other components */
    ret = wear_level_init(&ctx->wl);
    if (ret != HFSSS_OK) {
        gc_cleanup(&ctx->gc);
        block_mgr_cleanup(&ctx->block_mgr);
        mapping_cleanup(&ctx->mapping);
        free(ctx->cwbs);
        mutex_cleanup(&ctx->lock);
        return ret;
    }

    ret = ecc_init(&ctx->ecc, ECC_BCH);
    if (ret != HFSSS_OK) {
        wear_level_cleanup(&ctx->wl);
        gc_cleanup(&ctx->gc);
        block_mgr_cleanup(&ctx->block_mgr);
        mapping_cleanup(&ctx->mapping);
        free(ctx->cwbs);
        mutex_cleanup(&ctx->lock);
        return ret;
    }

    ret = error_init(&ctx->error);
    if (ret != HFSSS_OK) {
        ecc_cleanup(&ctx->ecc);
        wear_level_cleanup(&ctx->wl);
        gc_cleanup(&ctx->gc);
        block_mgr_cleanup(&ctx->block_mgr);
        mapping_cleanup(&ctx->mapping);
        free(ctx->cwbs);
        mutex_cleanup(&ctx->lock);
        return ret;
    }

    ctx->initialized = true;
    return HFSSS_OK;
}

void ftl_cleanup(struct ftl_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->lock, 0);

    error_cleanup(&ctx->error);
    ecc_cleanup(&ctx->ecc);
    wear_level_cleanup(&ctx->wl);
    gc_cleanup(&ctx->gc);
    block_mgr_cleanup(&ctx->block_mgr);
    mapping_cleanup(&ctx->mapping);
    free(ctx->cwbs);

    ctx->initialized = false;

    mutex_unlock(&ctx->lock);
    mutex_cleanup(&ctx->lock);

    memset(ctx, 0, sizeof(*ctx));
}

int ftl_read(struct ftl_ctx *ctx, u64 lba, u32 len, void *data)
{
    u64 pages;
    u64 i;
    u8 *ptr;
    int ret;

    if (!ctx || !ctx->initialized || !data) {
        return HFSSS_ERR_INVAL;
    }

    /* Calculate number of pages */
    pages = (len + ctx->config.page_size - 1) / ctx->config.page_size;

    mutex_lock(&ctx->lock, 0);

    /* Read each page */
    ptr = (u8 *)data;
    for (i = 0; i < pages; i++) {
        ret = ftl_read_page(ctx, lba + i, ptr);
        if (ret != HFSSS_OK) {
            mutex_unlock(&ctx->lock);
            return ret;
        }
        ptr += ctx->config.page_size;
    }

    /* Update stats */
    ctx->stats.read_count++;
    ctx->stats.read_bytes += len;

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int ftl_write(struct ftl_ctx *ctx, u64 lba, u32 len, const void *data)
{
    u64 pages;
    u64 i;
    const u8 *ptr;
    int ret;

    if (!ctx || !ctx->initialized || !data) {
        return HFSSS_ERR_INVAL;
    }

    /* Calculate number of pages */
    pages = (len + ctx->config.page_size - 1) / ctx->config.page_size;

    mutex_lock(&ctx->lock, 0);

    /* Write each page */
    ptr = (const u8 *)data;
    for (i = 0; i < pages; i++) {
        ret = ftl_write_page(ctx, lba + i, ptr);
        if (ret != HFSSS_OK) {
            mutex_unlock(&ctx->lock);
            return ret;
        }
        ptr += ctx->config.page_size;
    }

    /* Update stats */
    ctx->stats.write_count++;
    ctx->stats.write_bytes += len;

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int ftl_trim(struct ftl_ctx *ctx, u64 lba, u32 len)
{
    u64 pages;
    u64 i;
    int ret;

    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    /* Calculate number of pages */
    pages = (len + ctx->config.page_size - 1) / ctx->config.page_size;

    mutex_lock(&ctx->lock, 0);

    /* Unmap each page */
    for (i = 0; i < pages; i++) {
        ret = mapping_remove(&ctx->mapping, lba + i);
        /* Ignore error - page may not be mapped */
        (void)ret;
    }

    /* Update stats */
    ctx->stats.trim_count++;

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int ftl_flush(struct ftl_ctx *ctx)
{
    u32 i;

    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    /* Close all open CWB blocks */
    for (i = 0; i < ctx->cwb_count; i++) {
        struct cwb *cwb = &ctx->cwbs[i];
        if (cwb->block && cwb->block->state == FTL_BLOCK_OPEN) {
            block_mark_closed(&ctx->block_mgr, cwb->block);
            cwb->block = NULL;
            cwb->current_page = 0;
        }
    }

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

void ftl_get_stats(struct ftl_ctx *ctx, struct ftl_stats *stats)
{
    if (!ctx || !stats) {
        return;
    }

    mutex_lock(&ctx->lock, 0);

    memcpy(stats, &ctx->stats, sizeof(*stats));

    /* Add GC stats */
    u64 gc_count, moved_pages, reclaimed_blocks, gc_write_pages;
    gc_get_stats(&ctx->gc, &gc_count, &moved_pages, &reclaimed_blocks, &gc_write_pages);
    stats->gc_count = gc_count;
    stats->moved_pages = moved_pages;
    stats->reclaimed_blocks = reclaimed_blocks;
    stats->gc_write_pages = gc_write_pages;

    /* Calculate Write Amplification Factor (WAF) */
    if (stats->host_write_pages > 0) {
        stats->waf = (double)(stats->host_write_pages + stats->gc_write_pages) / stats->host_write_pages;
    } else {
        stats->waf = 0.0;
    }

    mutex_unlock(&ctx->lock);
}

void ftl_reset_stats(struct ftl_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->lock, 0);

    memset(&ctx->stats, 0, sizeof(ctx->stats));

    mutex_unlock(&ctx->lock);
}

int ftl_map_l2p(struct ftl_ctx *ctx, u64 lba, union ppn *ppn)
{
    if (!ctx || !ppn) {
        return HFSSS_ERR_INVAL;
    }

    return mapping_l2p(&ctx->mapping, lba, ppn);
}

int ftl_unmap_lba(struct ftl_ctx *ctx, u64 lba)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    return mapping_remove(&ctx->mapping, lba);
}

int ftl_gc_trigger(struct ftl_ctx *ctx)
{
    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    return gc_run(&ctx->gc, &ctx->block_mgr, &ctx->mapping, ctx->hal);
}
