#include "media/media.h"
#include <stdlib.h>
#include <string.h>

/* Forward declarations */
static void media_wait_until_available(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane);
static void media_inject_bit_errors(struct media_ctx *ctx, struct nand_page *page, void *data, void *spare, u32 page_size, u32 spare_size);

/* Page metadata structure for persistence */
struct page_metadata {
    enum page_state state;
    u64 program_ts;
    u32 erase_count;
    u32 bit_errors;
    u32 read_count;
};

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

    if (ctx->bbt) {
        bbt_cleanup(ctx->bbt);
        free(ctx->bbt);
        ctx->bbt = NULL;
    }

    if (ctx->reliability) {
        reliability_model_cleanup(ctx->reliability);
        free(ctx->reliability);
        ctx->reliability = NULL;
    }

    if (ctx->eat) {
        eat_ctx_cleanup(ctx->eat);
        free(ctx->eat);
        ctx->eat = NULL;
    }

    if (ctx->timing) {
        timing_model_cleanup(ctx->timing);
        free(ctx->timing);
        ctx->timing = NULL;
    }

    if (ctx->nand) {
        nand_device_cleanup(ctx->nand);
        free(ctx->nand);
        ctx->nand = NULL;
    }

    ctx->initialized = false;

    mutex_cleanup(&ctx->lock);
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
    nand_page->dirty = true;  /* Mark page as dirty for incremental checkpointing */

    /* Update block state */
    struct nand_block *nand_block = nand_get_block(ctx->nand, ch, chip, die, plane, block);
    if (nand_block) {
        nand_block->state = BLOCK_OPEN;
        nand_block->pages_written++;
        nand_block->dirty = true;  /* Mark block as dirty for incremental checkpointing */
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
        nand_page->dirty = true;  /* Mark page as dirty for incremental checkpointing */
    }

    /* Update block state */
    nand_block->state = BLOCK_FREE;
    nand_block->pages_written = 0;
    nand_block->dirty = true;  /* Mark block as dirty for incremental checkpointing */

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

/* Persistence implementation */

/* Magic number for file format validation */
#define MEDIA_FILE_MAGIC 0x48465353  /* "HFSS" */
#define MEDIA_FILE_VERSION 2  /* Incremented for incremental checkpoint support */

/* File header flags */
#define MEDIA_FILE_FLAG_FULL         0x00000000
#define MEDIA_FILE_FLAG_INCREMENTAL  0x00000001

/* File header structure */
struct media_file_header {
    u32 magic;
    u32 version;
    u32 flags;        /* Full or incremental checkpoint */
    struct media_config config;
    struct media_stats stats;
    u64 nand_data_offset;
    u64 bbt_offset;
};

static int write_file_header(FILE *f, struct media_ctx *ctx, u32 flags)
{
    struct media_file_header hdr;
    memset(&hdr, 0, sizeof(hdr));

    hdr.magic = MEDIA_FILE_MAGIC;
    hdr.version = MEDIA_FILE_VERSION;
    hdr.flags = flags;
    hdr.config = ctx->config;
    hdr.stats = ctx->stats;

    /* Calculate offsets - header comes first */
    hdr.nand_data_offset = sizeof(hdr);

    /* Calculate NAND data size including metadata */
    u32 total_blocks = ctx->config.channel_count *
                       ctx->config.chips_per_channel *
                       ctx->config.dies_per_chip *
                       ctx->config.planes_per_die *
                       ctx->config.blocks_per_plane;
    u32 total_pages = total_blocks * ctx->config.pages_per_block;
    u64 block_metadata_size = total_blocks * (sizeof(enum block_state) + sizeof(u32) + sizeof(bool));
    u64 page_metadata_size = total_pages * (sizeof(struct page_metadata) + sizeof(bool));
    u64 page_data_size = total_pages * (ctx->config.page_size + ctx->config.spare_size);
    u64 nand_data_size = block_metadata_size + page_metadata_size + page_data_size;

    hdr.bbt_offset = hdr.nand_data_offset + nand_data_size;

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) {
        return HFSSS_ERR_IO;
    }

    return HFSSS_OK;
}

/* Version 1 file header (no flags field) */
struct media_file_header_v1 {
    u32 magic;
    u32 version;
    struct media_config config;
    struct media_stats stats;
    u64 nand_data_offset;
    u64 bbt_offset;
};

static int read_file_header(FILE *f, struct media_file_header *hdr)
{
    /* First read just the magic and version */
    if (fread(&hdr->magic, sizeof(hdr->magic), 1, f) != 1) {
        return HFSSS_ERR_IO;
    }
    if (fread(&hdr->version, sizeof(hdr->version), 1, f) != 1) {
        return HFSSS_ERR_IO;
    }

    if (hdr->magic != MEDIA_FILE_MAGIC) {
        return HFSSS_ERR_INVAL;
    }

    /* Seek back to start */
    if (fseek(f, 0, SEEK_SET) != 0) {
        return HFSSS_ERR_IO;
    }

    if (hdr->version == 1) {
        /* Handle version 1 format */
        struct media_file_header_v1 hdr_v1;
        if (fread(&hdr_v1, sizeof(hdr_v1), 1, f) != 1) {
            return HFSSS_ERR_IO;
        }
        hdr->magic = hdr_v1.magic;
        hdr->version = hdr_v1.version;
        hdr->flags = MEDIA_FILE_FLAG_FULL;
        hdr->config = hdr_v1.config;
        hdr->stats = hdr_v1.stats;
        hdr->nand_data_offset = hdr_v1.nand_data_offset;
        hdr->bbt_offset = hdr_v1.bbt_offset;
    } else if (hdr->version == MEDIA_FILE_VERSION) {
        /* Handle current version */
        if (fread(hdr, sizeof(*hdr), 1, f) != 1) {
            return HFSSS_ERR_IO;
        }
    } else {
        return HFSSS_ERR_INVAL;
    }

    return HFSSS_OK;
}

/* Page metadata structure for persistence */
static int write_nand_data(FILE *f, struct media_ctx *ctx, bool incremental)
{
    u32 ch, chip, die, plane, block, page;

    for (ch = 0; ch < ctx->config.channel_count; ch++) {
        for (chip = 0; chip < ctx->config.chips_per_channel; chip++) {
            for (die = 0; die < ctx->config.dies_per_chip; die++) {
                for (plane = 0; plane < ctx->config.planes_per_die; plane++) {
                    for (block = 0; block < ctx->config.blocks_per_plane; block++) {
                        struct nand_block *blk = nand_get_block(ctx->nand, ch, chip, die, plane, block);
                        if (!blk) continue;

                        /* Write block dirty flag, state, and pages_written */
                        bool block_dirty = blk->dirty;
                        if (incremental && !block_dirty) {
                            /* Write just the dirty flag as false and skip the rest */
                            if (fwrite(&block_dirty, sizeof(block_dirty), 1, f) != 1) return HFSSS_ERR_IO;
                            continue;
                        }

                        if (fwrite(&block_dirty, sizeof(block_dirty), 1, f) != 1) return HFSSS_ERR_IO;
                        if (fwrite(&blk->state, sizeof(blk->state), 1, f) != 1) return HFSSS_ERR_IO;
                        if (fwrite(&blk->pages_written, sizeof(blk->pages_written), 1, f) != 1) return HFSSS_ERR_IO;

                        for (page = 0; page < ctx->config.pages_per_block; page++) {
                            struct nand_page *pg = &blk->pages[page];
                            if (!pg) continue;

                            bool page_dirty = pg->dirty;
                            if (incremental && !page_dirty) {
                                /* Write just the dirty flag as false and skip the rest */
                                if (fwrite(&page_dirty, sizeof(page_dirty), 1, f) != 1) return HFSSS_ERR_IO;
                                continue;
                            }

                            if (fwrite(&page_dirty, sizeof(page_dirty), 1, f) != 1) return HFSSS_ERR_IO;

                            /* Write page metadata */
                            struct page_metadata meta;
                            meta.state = pg->state;
                            meta.program_ts = pg->program_ts;
                            meta.erase_count = pg->erase_count;
                            meta.bit_errors = pg->bit_errors;
                            meta.read_count = pg->read_count;
                            if (fwrite(&meta, sizeof(meta), 1, f) != 1) {
                                return HFSSS_ERR_IO;
                            }

                            /* Write page data */
                            if (fwrite(pg->data, ctx->config.page_size, 1, f) != 1) {
                                return HFSSS_ERR_IO;
                            }

                            /* Write page spare */
                            if (fwrite(pg->spare, ctx->config.spare_size, 1, f) != 1) {
                                return HFSSS_ERR_IO;
                            }
                        }
                    }
                }
            }
        }
    }

    return HFSSS_OK;
}

static int read_nand_data(FILE *f, struct media_ctx *ctx, bool incremental)
{
    u32 ch, chip, die, plane, block, page;

    for (ch = 0; ch < ctx->config.channel_count; ch++) {
        for (chip = 0; chip < ctx->config.chips_per_channel; chip++) {
            for (die = 0; die < ctx->config.dies_per_chip; die++) {
                for (plane = 0; plane < ctx->config.planes_per_die; plane++) {
                    for (block = 0; block < ctx->config.blocks_per_plane; block++) {
                        struct nand_block *blk = nand_get_block(ctx->nand, ch, chip, die, plane, block);
                        if (!blk) continue;

                        /* Read block dirty flag */
                        bool block_dirty;
                        if (fread(&block_dirty, sizeof(block_dirty), 1, f) != 1) return HFSSS_ERR_IO;

                        if (incremental && !block_dirty) {
                            /* Skip this block - not dirty */
                            continue;
                        }

                        /* Read block state and pages_written */
                        if (fread(&blk->state, sizeof(blk->state), 1, f) != 1) return HFSSS_ERR_IO;
                        if (fread(&blk->pages_written, sizeof(blk->pages_written), 1, f) != 1) return HFSSS_ERR_IO;
                        blk->dirty = false;  /* Clear dirty flag after loading */

                        for (page = 0; page < ctx->config.pages_per_block; page++) {
                            struct nand_page *pg = &blk->pages[page];
                            if (!pg) continue;

                            /* Read page dirty flag */
                            bool page_dirty;
                            if (fread(&page_dirty, sizeof(page_dirty), 1, f) != 1) return HFSSS_ERR_IO;

                            if (incremental && !page_dirty) {
                                /* Skip this page - not dirty */
                                continue;
                            }

                            /* Read page metadata */
                            struct page_metadata meta;
                            if (fread(&meta, sizeof(meta), 1, f) != 1) {
                                return HFSSS_ERR_IO;
                            }
                            pg->state = meta.state;
                            pg->program_ts = meta.program_ts;
                            pg->erase_count = meta.erase_count;
                            pg->bit_errors = meta.bit_errors;
                            pg->read_count = meta.read_count;
                            pg->dirty = false;  /* Clear dirty flag after loading */

                            /* Read page data */
                            if (fread(pg->data, ctx->config.page_size, 1, f) != 1) {
                                return HFSSS_ERR_IO;
                            }

                            /* Read page spare */
                            if (fread(pg->spare, ctx->config.spare_size, 1, f) != 1) {
                                return HFSSS_ERR_IO;
                            }
                        }
                    }
                }
            }
        }
    }

    return HFSSS_OK;
}

/* Check if there is any dirty data */
int media_has_dirty_data(struct media_ctx *ctx)
{
    if (!ctx || !ctx->initialized) {
        return 0;
    }

    u32 ch, chip, die, plane, block, page;

    for (ch = 0; ch < ctx->config.channel_count; ch++) {
        for (chip = 0; chip < ctx->config.chips_per_channel; chip++) {
            for (die = 0; die < ctx->config.dies_per_chip; die++) {
                for (plane = 0; plane < ctx->config.planes_per_die; plane++) {
                    for (block = 0; block < ctx->config.blocks_per_plane; block++) {
                        struct nand_block *blk = nand_get_block(ctx->nand, ch, chip, die, plane, block);
                        if (!blk) continue;
                        if (blk->dirty) {
                            return 1;
                        }
                        for (page = 0; page < blk->page_count; page++) {
                            if (blk->pages[page].dirty) {
                                return 1;
                            }
                        }
                    }
                }
            }
        }
    }

    return 0;
}

/* Mark all data as clean (after checkpoint) */
void media_mark_all_clean(struct media_ctx *ctx)
{
    if (!ctx || !ctx->initialized) {
        return;
    }

    u32 ch, chip, die, plane, block, page;

    for (ch = 0; ch < ctx->config.channel_count; ch++) {
        for (chip = 0; chip < ctx->config.chips_per_channel; chip++) {
            for (die = 0; die < ctx->config.dies_per_chip; die++) {
                for (plane = 0; plane < ctx->config.planes_per_die; plane++) {
                    for (block = 0; block < ctx->config.blocks_per_plane; block++) {
                        struct nand_block *blk = nand_get_block(ctx->nand, ch, chip, die, plane, block);
                        if (!blk) continue;
                        blk->dirty = false;
                        for (page = 0; page < blk->page_count; page++) {
                            blk->pages[page].dirty = false;
                        }
                    }
                }
            }
        }
    }
}

int media_save(struct media_ctx *ctx, const char *filepath)
{
    if (!ctx || !ctx->initialized || !filepath) {
        return HFSSS_ERR_INVAL;
    }

    FILE *f = fopen(filepath, "wb");
    if (!f) {
        return HFSSS_ERR_IO;
    }

    int ret = HFSSS_OK;

    /* Write header */
    ret = write_file_header(f, ctx, MEDIA_FILE_FLAG_FULL);
    if (ret != HFSSS_OK) {
        fclose(f);
        return ret;
    }

    /* Write NAND data (full) */
    ret = write_nand_data(f, ctx, false);
    if (ret != HFSSS_OK) {
        fclose(f);
        return ret;
    }

    /* Write BBT */
    ret = bbt_save(ctx->bbt, f);
    if (ret != HFSSS_OK) {
        fclose(f);
        return ret;
    }

    fclose(f);
    return HFSSS_OK;
}

int media_save_incremental(struct media_ctx *ctx, const char *filepath)
{
    if (!ctx || !ctx->initialized || !filepath) {
        return HFSSS_ERR_INVAL;
    }

    FILE *f = fopen(filepath, "wb");
    if (!f) {
        return HFSSS_ERR_IO;
    }

    int ret = HFSSS_OK;

    /* Write header */
    ret = write_file_header(f, ctx, MEDIA_FILE_FLAG_INCREMENTAL);
    if (ret != HFSSS_OK) {
        fclose(f);
        return ret;
    }

    /* Write NAND data (incremental) */
    ret = write_nand_data(f, ctx, true);
    if (ret != HFSSS_OK) {
        fclose(f);
        return ret;
    }

    /* Write BBT */
    ret = bbt_save(ctx->bbt, f);
    if (ret != HFSSS_OK) {
        fclose(f);
        return ret;
    }

    fclose(f);
    return HFSSS_OK;
}

int media_load(struct media_ctx *ctx, const char *filepath)
{
    if (!ctx || !filepath) {
        return HFSSS_ERR_INVAL;
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        return HFSSS_ERR_IO;
    }

    struct media_file_header hdr;
    int ret = read_file_header(f, &hdr);
    if (ret != HFSSS_OK) {
        fclose(f);
        return ret;
    }

    /* If ctx is already initialized, check if config matches */
    if (ctx->initialized) {
        if (memcmp(&ctx->config, &hdr.config, sizeof(hdr.config)) != 0) {
            fclose(f);
            return HFSSS_ERR_INVAL;
        }
    } else {
        /* Initialize media with the config from file */
        ret = media_init(ctx, &hdr.config);
        if (ret != HFSSS_OK) {
            fclose(f);
            return ret;
        }
    }

    /* Seek to NAND data offset */
    if (fseek(f, hdr.nand_data_offset, SEEK_SET) != 0) {
        fclose(f);
        return HFSSS_ERR_IO;
    }

    /* Read NAND data - handle version 1 (no flags) as full checkpoint */
    bool incremental = (hdr.version >= 2) && (hdr.flags & MEDIA_FILE_FLAG_INCREMENTAL);
    ret = read_nand_data(f, ctx, incremental);
    if (ret != HFSSS_OK) {
        fclose(f);
        return ret;
    }

    /* Seek to BBT offset */
    if (fseek(f, hdr.bbt_offset, SEEK_SET) != 0) {
        fclose(f);
        return HFSSS_ERR_IO;
    }

    /* Load BBT */
    ret = bbt_load(ctx->bbt, f);
    if (ret != HFSSS_OK) {
        fclose(f);
        return ret;
    }

    /* Restore stats */
    ctx->stats = hdr.stats;

    fclose(f);
    return HFSSS_OK;
}

int media_create_checkpoint(struct media_ctx *ctx, const char *checkpoint_dir)
{
    (void)checkpoint_dir; /* TODO: Implement directory-based checkpointing */
    int ret = media_save(ctx, "checkpoint.bin");
    if (ret == HFSSS_OK) {
        media_mark_all_clean(ctx);
    }
    return ret;
}

int media_create_incremental_checkpoint(struct media_ctx *ctx, const char *checkpoint_dir)
{
    (void)checkpoint_dir; /* TODO: Implement directory-based checkpointing */
    int ret = media_save_incremental(ctx, "checkpoint_inc.bin");
    if (ret == HFSSS_OK) {
        media_mark_all_clean(ctx);
    }
    return ret;
}

int media_restore_checkpoint(struct media_ctx *ctx, const char *checkpoint_dir)
{
    (void)checkpoint_dir; /* TODO: Implement directory-based checkpointing */
    return media_load(ctx, "checkpoint.bin");
}
