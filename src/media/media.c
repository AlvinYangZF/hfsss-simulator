#include "media/media.h"
#include <stdlib.h>
#include <string.h>

/* Forward declarations */
static void media_wait_until_available(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane);
static void media_inject_bit_errors(struct media_ctx *ctx, struct nand_page *page, void *data, void *spare, u32 page_size, u32 spare_size);

int media_init(struct media_ctx *ctx, struct media_config *config)
{
    int ret;

    if (!ctx || !config) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    /* Copy configuration */
    memcpy(&ctx->config, config, sizeof(*config));

    /* Initialize lock */
    ret = mutex_init(&ctx->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Allocate NAND device */
    ctx->nand = (struct nand_device *)malloc(sizeof(struct nand_device));
    if (!ctx->nand) {
        mutex_cleanup(&ctx->lock);
        return HFSSS_ERR_NOMEM;
    }

    /* Initialize NAND device */
    ret = nand_device_init(ctx->nand, config->channel_count, config->chips_per_channel,
                            config->dies_per_chip, config->planes_per_die,
                            config->blocks_per_plane, config->pages_per_block,
                            config->page_size, config->spare_size);
    if (ret != HFSSS_OK) {
        free(ctx->nand);
        mutex_cleanup(&ctx->lock);
        return ret;
    }

    /* Allocate and initialize timing model */
    ctx->timing = (struct timing_model *)malloc(sizeof(struct timing_model));
    if (!ctx->timing) {
        media_cleanup(ctx);
        return HFSSS_ERR_NOMEM;
    }

    ret = timing_model_init(ctx->timing, config->nand_type);
    if (ret != HFSSS_OK) {
        media_cleanup(ctx);
        return ret;
    }

    /* Associate timing model with NAND device */
    ctx->nand->timing = ctx->timing;

    /* Allocate and initialize EAT context */
    ctx->eat = (struct eat_ctx *)malloc(sizeof(struct eat_ctx));
    if (!ctx->eat) {
        media_cleanup(ctx);
        return HFSSS_ERR_NOMEM;
    }

    ret = eat_ctx_init(ctx->eat);
    if (ret != HFSSS_OK) {
        media_cleanup(ctx);
        return ret;
    }

    /* Associate EAT context with NAND device */
    ctx->nand->eat = ctx->eat;

    /* Allocate and initialize reliability model */
    ctx->reliability = (struct reliability_model *)malloc(sizeof(struct reliability_model));
    if (!ctx->reliability) {
        media_cleanup(ctx);
        return HFSSS_ERR_NOMEM;
    }

    ret = reliability_model_init(ctx->reliability);
    if (ret != HFSSS_OK) {
        media_cleanup(ctx);
        return ret;
    }

    /* Allocate and initialize BBT */
    ctx->bbt = (struct bbt *)malloc(sizeof(struct bbt));
    if (!ctx->bbt) {
        media_cleanup(ctx);
        return HFSSS_ERR_NOMEM;
    }

    ret = bbt_init(ctx->bbt, config->channel_count, config->chips_per_channel,
                   config->dies_per_chip, config->planes_per_die,
                   config->blocks_per_plane);
    if (ret != HFSSS_OK) {
        media_cleanup(ctx);
        return ret;
    }

    ctx->initialized = true;
    return HFSSS_OK;
}

void media_cleanup(struct media_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->lock, 0);

    if (ctx->bbt) {
        bbt_cleanup(ctx->bbt);
        free(ctx->bbt);
    }

    if (ctx->reliability) {
        reliability_model_cleanup(ctx->reliability);
        free(ctx->reliability);
    }

    if (ctx->eat) {
        eat_ctx_cleanup(ctx->eat);
        free(ctx->eat);
    }

    if (ctx->timing) {
        timing_model_cleanup(ctx->timing);
        free(ctx->timing);
    }

    if (ctx->nand) {
        nand_device_cleanup(ctx->nand);
        free(ctx->nand);
    }

    ctx->initialized = false;

    mutex_unlock(&ctx->lock);
    mutex_cleanup(&ctx->lock);

    memset(ctx, 0, sizeof(*ctx));
}

static void media_wait_until_available(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane)
{
    u64 eat;
    u64 now;
    u64 remaining;

    /* Get the earliest available time */
    eat = eat_get_max(ctx->eat, ch, chip, die, plane);
    now = get_time_ns();

    if (eat > now) {
        /* Need to wait - busy wait for now (could use nanosleep) */
        remaining = eat - now;
        if (remaining > 1000000) {
            /* Only log if waiting more than 1ms */
        }
        /* Simple busy wait for accuracy */
        while (get_time_ns() < eat) {
            /* Spin */
        }
    }
}

static void media_inject_bit_errors(struct media_ctx *ctx, struct nand_page *page, void *data, void *spare, u32 page_size, u32 spare_size)
{
    u32 bit_errors;
    u64 retention_ns;
    u32 erase_count;

    /* Don't inject errors if page is free */
    if (page->state == PAGE_FREE) {
        return;
    }

    erase_count = page->erase_count;
    retention_ns = get_time_ns() - page->program_ts;

    /* Calculate expected bit errors */
    bit_errors = reliability_calculate_bit_errors(ctx->reliability,
                                                   ctx->config.nand_type,
                                                   erase_count,
                                                   page->read_count,
                                                   retention_ns);

    /* Store for stats */
    page->bit_errors = bit_errors;

    /* For simplicity, we don't actually flip bits in this implementation
     * but we track the error count for reliability modeling.
     * A more detailed implementation could actually flip random bits here.
     */
    (void)bit_errors;
    (void)spare;
    (void)spare_size;

    /* Just copy the data without errors for now */
    memcpy(data, page->data, page_size);
}

int media_nand_read(struct media_ctx *ctx, u32 ch, u32 chip, u32 die,
                    u32 plane, u32 block, u32 page, void *data, void *spare)
{
    struct nand_page *nand_page;
    u64 latency;
    u64 start_time;

    if (!ctx || !ctx->initialized || !data) {
        return HFSSS_ERR_INVAL;
    }

    /* Validate address */
    if (nand_validate_address(ctx->nand, ch, chip, die, plane, block, page) != HFSSS_OK) {
        return HFSSS_ERR_INVAL;
    }

    /* Check if block is bad */
    if (bbt_is_bad(ctx->bbt, ch, chip, die, plane, block) == 1) {
        return HFSSS_ERR_IO;
    }

    /* Wait for the plane/die/chip/channel to be available */
    media_wait_until_available(ctx, ch, chip, die, plane);

    /* Get the page */
    nand_page = nand_get_page(ctx->nand, ch, chip, die, plane, block, page);
    if (!nand_page) {
        return HFSSS_ERR_INVAL;
    }

    /* Check if page is valid */
    if (nand_page->state != PAGE_VALID) {
        return HFSSS_ERR_NOENT;
    }

    /* Get read latency */
    latency = timing_get_read_latency(ctx->timing, page);
    start_time = get_time_ns();

    /* Update EAT */
    eat_update(ctx->eat, ch, chip, die, plane, latency);

    /* Copy data with potential bit errors */
    media_inject_bit_errors(ctx, nand_page, data, spare, ctx->config.page_size, ctx->config.spare_size);

    /* Increment read count */
    nand_page->read_count++;

    /* Update stats */
    mutex_lock(&ctx->lock, 0);
    ctx->stats.read_count++;
    ctx->stats.total_read_bytes += ctx->config.page_size;
    ctx->stats.total_read_ns += get_time_ns() - start_time;
    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int media_nand_program(struct media_ctx *ctx, u32 ch, u32 chip, u32 die,
                       u32 plane, u32 block, u32 page, const void *data, const void *spare)
{
    struct nand_page *nand_page;
    u64 latency;
    u64 start_time;
    int is_bad;

    if (!ctx || !ctx->initialized || !data) {
        return HFSSS_ERR_INVAL;
    }

    /* Validate address */
    if (nand_validate_address(ctx->nand, ch, chip, die, plane, block, page) != HFSSS_OK) {
        return HFSSS_ERR_INVAL;
    }

    /* Check if block is bad */
    is_bad = bbt_is_bad(ctx->bbt, ch, chip, die, plane, block);
    if (is_bad == 1) {
        return HFSSS_ERR_IO;
    }
    if (is_bad == -1) {
        return HFSSS_ERR_INVAL;
    }

    /* Wait for the plane/die/chip/channel to be available */
    media_wait_until_available(ctx, ch, chip, die, plane);

    /* Get the page */
    nand_page = nand_get_page(ctx->nand, ch, chip, die, plane, block, page);
    if (!nand_page) {
        return HFSSS_ERR_INVAL;
    }

    /* Get program latency */
    latency = timing_get_prog_latency(ctx->timing, page);
    start_time = get_time_ns();

    /* Update EAT */
    eat_update(ctx->eat, ch, chip, die, plane, latency);

    /* Write data to NAND */
    memcpy(nand_page->data, data, ctx->config.page_size);
    if (spare) {
        memcpy(nand_page->spare, spare, ctx->config.spare_size);
    }

    /* Update page state */
    nand_page->state = PAGE_VALID;
    nand_page->program_ts = get_time_ns();
    nand_page->erase_count = bbt_get_erase_count(ctx->bbt, ch, chip, die, plane, block);
    nand_page->read_count = 0;

    /* Update block state */
    struct nand_block *nand_block = nand_get_block(ctx->nand, ch, chip, die, plane, block);
    if (nand_block) {
        nand_block->state = BLOCK_OPEN;
        nand_block->pages_written++;
    }

    /* Update stats */
    mutex_lock(&ctx->lock, 0);
    ctx->stats.write_count++;
    ctx->stats.total_write_bytes += ctx->config.page_size;
    ctx->stats.total_write_ns += get_time_ns() - start_time;
    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int media_nand_erase(struct media_ctx *ctx, u32 ch, u32 chip, u32 die,
                     u32 plane, u32 block)
{
    struct nand_block *nand_block;
    u64 latency;
    u64 start_time;
    int is_bad;

    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    /* Validate address */
    if (ch >= ctx->config.channel_count ||
        chip >= ctx->config.chips_per_channel ||
        die >= ctx->config.dies_per_chip ||
        plane >= ctx->config.planes_per_die ||
        block >= ctx->config.blocks_per_plane) {
        return HFSSS_ERR_INVAL;
    }

    /* Check if block is bad */
    is_bad = bbt_is_bad(ctx->bbt, ch, chip, die, plane, block);
    if (is_bad == 1) {
        return HFSSS_ERR_IO;
    }
    if (is_bad == -1) {
        return HFSSS_ERR_INVAL;
    }

    /* Wait for the plane/die/chip/channel to be available */
    media_wait_until_available(ctx, ch, chip, die, plane);

    /* Get the block */
    nand_block = nand_get_block(ctx->nand, ch, chip, die, plane, block);
    if (!nand_block) {
        return HFSSS_ERR_INVAL;
    }

    /* Get erase latency */
    latency = timing_get_erase_latency(ctx->timing);
    start_time = get_time_ns();

    /* Update EAT */
    eat_update(ctx->eat, ch, chip, die, plane, latency);

    /* Erase all pages in the block */
    for (u32 page = 0; page < nand_block->page_count; page++) {
        struct nand_page *nand_page = &nand_block->pages[page];
        memset(nand_page->data, 0xFF, ctx->config.page_size);
        memset(nand_page->spare, 0xFF, ctx->config.spare_size);
        nand_page->state = PAGE_FREE;
        nand_page->program_ts = 0;
        nand_page->read_count = 0;
        nand_page->bit_errors = 0;
    }

    /* Update block state */
    nand_block->state = BLOCK_FREE;
    nand_block->pages_written = 0;

    /* Increment erase count */
    bbt_increment_erase_count(ctx->bbt, ch, chip, die, plane, block);

    /* Check if block should be marked as bad now */
    u32 erase_count = bbt_get_erase_count(ctx->bbt, ch, chip, die, plane, block);
    if (reliability_is_block_bad(ctx->reliability, ctx->config.nand_type, erase_count)) {
        bbt_mark_bad(ctx->bbt, ch, chip, die, plane, block);
    }

    /* Update stats */
    mutex_lock(&ctx->lock, 0);
    ctx->stats.erase_count++;
    ctx->stats.total_erase_ns += get_time_ns() - start_time;
    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int media_nand_is_bad_block(struct media_ctx *ctx, u32 ch, u32 chip, u32 die,
                            u32 plane, u32 block)
{
    int is_bad;

    if (!ctx || !ctx->initialized) {
        return -1;
    }

    is_bad = bbt_is_bad(ctx->bbt, ch, chip, die, plane, block);
    return is_bad;
}

int media_nand_mark_bad_block(struct media_ctx *ctx, u32 ch, u32 chip, u32 die,
                              u32 plane, u32 block)
{
    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    return bbt_mark_bad(ctx->bbt, ch, chip, die, plane, block);
}

u32 media_nand_get_erase_count(struct media_ctx *ctx, u32 ch, u32 chip, u32 die,
                               u32 plane, u32 block)
{
    if (!ctx || !ctx->initialized) {
        return 0;
    }

    return bbt_get_erase_count(ctx->bbt, ch, chip, die, plane, block);
}

void media_get_stats(struct media_ctx *ctx, struct media_stats *stats)
{
    if (!ctx || !stats) {
        return;
    }

    mutex_lock(&ctx->lock, 0);
    memcpy(stats, &ctx->stats, sizeof(*stats));
    mutex_unlock(&ctx->lock);
}

void media_reset_stats(struct media_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->lock, 0);
    memset(&ctx->stats, 0, sizeof(ctx->stats));
    mutex_unlock(&ctx->lock);
}
