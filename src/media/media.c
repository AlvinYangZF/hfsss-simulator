#include "media/media.h"
#include "media/cmd_engine.h"
#include "media/cmd_state.h"
#include "sssim.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Forward declarations */
static void media_inject_bit_errors(struct media_ctx *ctx, struct nand_page *page, void *data, void *spare,
                                    u32 page_size, u32 spare_size);

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
    ret = nand_device_init(ctx->nand, config->channel_count, config->chips_per_channel, config->dies_per_chip,
                           config->planes_per_die, config->blocks_per_plane, config->pages_per_block, config->page_size,
                           config->spare_size);
    if (ret != HFSSS_OK) {
        free(ctx->nand);
        mutex_cleanup(&ctx->lock);
        return ret;
    }

    /* Resolve the active profile. Explicit profile_id wins; otherwise fall
     * back to the default for nand_type so existing zero-initialized callers
     * keep working unchanged. */
    if (config->profile_explicit) {
        ctx->profile = nand_profile_get(config->profile_id);
    }
    if (!ctx->profile) {
        ctx->profile = nand_profile_get_default_for_type(config->nand_type);
    }
    ctx->nand->profile = ctx->profile;

    /* Allocate and initialize timing model */
    ctx->timing = (struct timing_model *)malloc(sizeof(struct timing_model));
    if (!ctx->timing) {
        media_cleanup(ctx);
        return HFSSS_ERR_NOMEM;
    }

    ret = timing_model_init_from_profile(ctx->timing, ctx->profile);
    if (ret != HFSSS_OK) {
        media_cleanup(ctx);
        return ret;
    }
    /* Preserve the caller-requested nand_type so timing_get_*_latency lane
     * selection still works for SLC/MLC fallbacks where the profile is a
     * TLC default. */
    ctx->timing->type = config->nand_type;

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

    /* Populate device identity and parameter page from the active config
     * after timing/EAT wiring so tR/tPROG/tBERS advertisement reflects the
     * real timing model. */
    nand_identity_build_from_config(ctx->nand, &ctx->config);

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

    ret = bbt_init(ctx->bbt, config->channel_count, config->chips_per_channel, config->dies_per_chip,
                   config->planes_per_die, config->blocks_per_plane);
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

static void media_inject_bit_errors(struct media_ctx *ctx, struct nand_page *page, void *data, void *spare,
                                    u32 page_size, u32 spare_size)
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
    bit_errors = reliability_calculate_bit_errors(ctx->reliability, ctx->config.nand_type, erase_count,
                                                  page->read_count, retention_ns);

    /* Store for stats */
    page->bit_errors = bit_errors;

    /* For simplicity, we don't actually flip bits in this implementation
     * but we track the error count for reliability modeling.
     * A more detailed implementation could actually flip random bits here.
     */
    (void)bit_errors;

    /* Copy data back to caller */
    memcpy(data, page->data, page_size);

    /* Copy spare back to caller when a buffer was supplied (REQ-157).
     * The spare carries T10 PI tuples on the GC/FTL read-then-rewrite
     * path; if this copy is skipped, migrated pages lose their PI and
     * fail pi_verify at the destination. */
    if (spare && page->spare) {
        memcpy(spare, page->spare, spare_size);
    }
}

struct read_cb_ctx {
    struct media_ctx *ctx;
    u32 ch;
    u32 chip;
    u32 die;
    u32 plane;
    u32 block;
    u32 page;
    void *data;
    void *spare;
    struct nand_page *nand_page;
    int out_status;
};

static int read_on_setup_commit(void *raw)
{
    struct read_cb_ctx *c = (struct read_cb_ctx *)raw;
    c->nand_page = nand_get_page(c->ctx->nand, c->ch, c->chip, c->die, c->plane, c->block, c->page);
    if (!c->nand_page) {
        c->out_status = HFSSS_ERR_INVAL;
        return HFSSS_ERR_INVAL;
    }
    if (c->nand_page->state != PAGE_VALID) {
        c->out_status = HFSSS_ERR_NOENT;
        return HFSSS_ERR_NOENT;
    }
    return HFSSS_OK;
}

static int read_on_data_xfer_commit(void *raw)
{
    struct read_cb_ctx *c = (struct read_cb_ctx *)raw;
    if (!c->nand_page) {
        return HFSSS_ERR_INVAL;
    }
    media_inject_bit_errors(c->ctx, c->nand_page, c->data, c->spare, c->ctx->config.page_size,
                            c->ctx->config.spare_size);
    c->nand_page->read_count++;
    return HFSSS_OK;
}

int media_nand_read(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane, u32 block, u32 page, void *data,
                    void *spare)
{
    if (!ctx || !ctx->initialized || !data) {
        return HFSSS_ERR_INVAL;
    }

    if (nand_validate_address(ctx->nand, ch, chip, die, plane, block, page) != HFSSS_OK) {
        return HFSSS_ERR_INVAL;
    }

    if (bbt_is_bad(ctx->bbt, ch, chip, die, plane, block) == 1) {
        return HFSSS_ERR_IO;
    }

    struct read_cb_ctx cbc = {
        .ctx = ctx,
        .ch = ch,
        .chip = chip,
        .die = die,
        .plane = plane,
        .block = block,
        .page = page,
        .data = data,
        .spare = spare,
        .nand_page = NULL,
        .out_status = HFSSS_OK,
    };
    struct nand_cmd_target target = {
        .ch = ch,
        .chip = chip,
        .die = die,
        .plane_mask = 1u << plane,
        .block = block,
        .page = page,
    };
    struct nand_cmd_ops ops = {
        .on_setup_commit = read_on_setup_commit,
        .on_array_commit = NULL,
        .on_data_xfer_commit = read_on_data_xfer_commit,
    };

    u64 start_time = get_time_ns();
    int rc = nand_cmd_engine_submit_read(ctx->nand, &target, &ops, &cbc);
    if (rc != HFSSS_OK) {
        return cbc.out_status != HFSSS_OK ? cbc.out_status : rc;
    }

    mutex_lock(&ctx->lock, 0);
    ctx->stats.read_count++;
    ctx->stats.total_read_bytes += ctx->config.page_size;
    ctx->stats.total_read_ns += get_time_ns() - start_time;
    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

struct prog_cb_ctx {
    struct media_ctx *ctx;
    u32 ch;
    u32 chip;
    u32 die;
    u32 plane;
    u32 block;
    u32 page;
    const void *data;
    const void *spare;
    int out_status;
};

static int prog_on_array_commit(void *raw)
{
    struct prog_cb_ctx *c = (struct prog_cb_ctx *)raw;
    struct nand_page *nand_page = nand_get_page(c->ctx->nand, c->ch, c->chip, c->die, c->plane, c->block, c->page);
    if (!nand_page) {
        c->out_status = HFSSS_ERR_INVAL;
        return HFSSS_ERR_INVAL;
    }

    memcpy(nand_page->data, c->data, c->ctx->config.page_size);
    if (c->spare) {
        memcpy(nand_page->spare, c->spare, c->ctx->config.spare_size);
    }

    nand_page->state = PAGE_VALID;
    nand_page->program_ts = get_time_ns();
    nand_page->erase_count = bbt_get_erase_count(c->ctx->bbt, c->ch, c->chip, c->die, c->plane, c->block);
    nand_page->read_count = 0;
    nand_page->dirty = true;

    struct nand_block *nand_block = nand_get_block(c->ctx->nand, c->ch, c->chip, c->die, c->plane, c->block);
    if (nand_block) {
        nand_block->state = BLOCK_OPEN;
        nand_block->pages_written++;
        nand_block->dirty = true;
    }
    return HFSSS_OK;
}

int media_nand_program(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane, u32 block, u32 page,
                       const void *data, const void *spare)
{
    int is_bad;

    if (!ctx || !ctx->initialized || !data) {
        return HFSSS_ERR_INVAL;
    }

    if (nand_validate_address(ctx->nand, ch, chip, die, plane, block, page) != HFSSS_OK) {
        return HFSSS_ERR_INVAL;
    }

    is_bad = bbt_is_bad(ctx->bbt, ch, chip, die, plane, block);
    if (is_bad == 1) {
        return HFSSS_ERR_IO;
    }
    if (is_bad == -1) {
        return HFSSS_ERR_INVAL;
    }

    struct prog_cb_ctx cbc = {
        .ctx = ctx,
        .ch = ch,
        .chip = chip,
        .die = die,
        .plane = plane,
        .block = block,
        .page = page,
        .data = data,
        .spare = spare,
        .out_status = HFSSS_OK,
    };
    struct nand_cmd_target target = {
        .ch = ch,
        .chip = chip,
        .die = die,
        .plane_mask = 1u << plane,
        .block = block,
        .page = page,
    };
    struct nand_cmd_ops ops = {
        .on_setup_commit = NULL,
        .on_array_commit = prog_on_array_commit,
        .on_data_xfer_commit = NULL,
    };

    u64 start_time = get_time_ns();
    int rc = nand_cmd_engine_submit_program(ctx->nand, &target, &ops, &cbc);
    if (rc != HFSSS_OK) {
        return cbc.out_status != HFSSS_OK ? cbc.out_status : rc;
    }

    mutex_lock(&ctx->lock, 0);
    ctx->stats.write_count++;
    ctx->stats.total_write_bytes += ctx->config.page_size;
    ctx->stats.total_write_ns += get_time_ns() - start_time;
    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

struct erase_cb_ctx {
    struct media_ctx *ctx;
    u32 ch;
    u32 chip;
    u32 die;
    u32 plane;
    u32 block;
    int out_status;
};

static int erase_on_array_commit(void *raw)
{
    struct erase_cb_ctx *c = (struct erase_cb_ctx *)raw;
    struct nand_block *nand_block = nand_get_block(c->ctx->nand, c->ch, c->chip, c->die, c->plane, c->block);
    if (!nand_block) {
        c->out_status = HFSSS_ERR_INVAL;
        return HFSSS_ERR_INVAL;
    }

    for (u32 page = 0; page < nand_block->page_count; page++) {
        struct nand_page *nand_page = &nand_block->pages[page];
        memset(nand_page->data, 0xFF, c->ctx->config.page_size);
        memset(nand_page->spare, 0xFF, c->ctx->config.spare_size);
        nand_page->state = PAGE_FREE;
        nand_page->program_ts = 0;
        nand_page->read_count = 0;
        nand_page->bit_errors = 0;
        nand_page->dirty = true;
    }

    nand_block->state = BLOCK_FREE;
    nand_block->pages_written = 0;
    nand_block->dirty = true;

    bbt_increment_erase_count(c->ctx->bbt, c->ch, c->chip, c->die, c->plane, c->block);

    u32 erase_count = bbt_get_erase_count(c->ctx->bbt, c->ch, c->chip, c->die, c->plane, c->block);
    if (reliability_is_block_bad(c->ctx->reliability, c->ctx->config.nand_type, erase_count)) {
        bbt_mark_bad(c->ctx->bbt, c->ch, c->chip, c->die, c->plane, c->block);
    }
    return HFSSS_OK;
}

int media_nand_erase(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane, u32 block)
{
    int is_bad;

    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    if (ch >= ctx->config.channel_count || chip >= ctx->config.chips_per_channel || die >= ctx->config.dies_per_chip ||
        plane >= ctx->config.planes_per_die || block >= ctx->config.blocks_per_plane) {
        return HFSSS_ERR_INVAL;
    }

    is_bad = bbt_is_bad(ctx->bbt, ch, chip, die, plane, block);
    if (is_bad == 1) {
        return HFSSS_ERR_IO;
    }
    if (is_bad == -1) {
        return HFSSS_ERR_INVAL;
    }

    struct erase_cb_ctx cbc = {
        .ctx = ctx,
        .ch = ch,
        .chip = chip,
        .die = die,
        .plane = plane,
        .block = block,
        .out_status = HFSSS_OK,
    };
    struct nand_cmd_target target = {
        .ch = ch,
        .chip = chip,
        .die = die,
        .plane_mask = 1u << plane,
        .block = block,
        .page = 0,
    };
    struct nand_cmd_ops ops = {
        .on_setup_commit = NULL,
        .on_array_commit = erase_on_array_commit,
        .on_data_xfer_commit = NULL,
    };

    u64 start_time = get_time_ns();
    int rc = nand_cmd_engine_submit_erase(ctx->nand, &target, &ops, &cbc);
    if (rc != HFSSS_OK) {
        return cbc.out_status != HFSSS_OK ? cbc.out_status : rc;
    }

    mutex_lock(&ctx->lock, 0);
    ctx->stats.erase_count++;
    ctx->stats.total_erase_ns += get_time_ns() - start_time;
    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int media_nand_read_status(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, struct nand_die_cmd_state *out)
{
    if (!ctx || !ctx->initialized || !out) {
        return HFSSS_ERR_INVAL;
    }

    struct nand_cmd_target target = {
        .ch = ch,
        .chip = chip,
        .die = die,
        .plane_mask = 0,
        .block = 0,
        .page = 0,
    };

    return nand_cmd_engine_snapshot(ctx->nand, &target, out);
}

int media_nand_read_status_byte(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u8 *out)
{
    if (!ctx || !ctx->initialized || !out) {
        return HFSSS_ERR_INVAL;
    }
    struct nand_cmd_target target = {
        .ch = ch,
        .chip = chip,
        .die = die,
        .plane_mask = 0,
        .block = 0,
        .page = 0,
    };
    return nand_cmd_engine_submit_read_status(ctx->nand, &target, out);
}

int media_nand_read_status_enhanced(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, struct nand_status_enhanced *out)
{
    if (!ctx || !ctx->initialized || !out) {
        return HFSSS_ERR_INVAL;
    }
    struct nand_cmd_target target = {
        .ch = ch,
        .chip = chip,
        .die = die,
        .plane_mask = 0,
        .block = 0,
        .page = 0,
    };
    return nand_cmd_engine_submit_read_status_enhanced(ctx->nand, &target, out);
}

int media_nand_read_id(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, struct nand_id *out)
{
    if (!ctx || !ctx->initialized || !out) {
        return HFSSS_ERR_INVAL;
    }
    struct nand_cmd_target target = {
        .ch = ch,
        .chip = chip,
        .die = die,
        .plane_mask = 0,
        .block = 0,
        .page = 0,
    };
    return nand_cmd_engine_submit_read_id(ctx->nand, &target, out);
}

int media_nand_read_parameter_page(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, struct nand_parameter_page *out)
{
    if (!ctx || !ctx->initialized || !out) {
        return HFSSS_ERR_INVAL;
    }
    struct nand_cmd_target target = {
        .ch = ch,
        .chip = chip,
        .die = die,
        .plane_mask = 0,
        .block = 0,
        .page = 0,
    };
    return nand_cmd_engine_submit_read_param_page(ctx->nand, &target, out);
}

/*
 * Multi-plane callback contexts. Each carries an array of per-plane
 * data/spare pointers indexed by position within the plane_mask (LSB
 * first). The commit hooks iterate all planes, performing the same
 * operation that the single-plane hooks do but once per plane.
 */
struct mp_prog_cb_ctx {
    struct media_ctx *ctx;
    u32 ch;
    u32 chip;
    u32 die;
    u32 plane_mask;
    u32 block;
    u32 page;
    const void **data_array;
    const void **spare_array;
    int out_status;
};

static int mp_prog_on_array_commit(void *raw)
{
    struct mp_prog_cb_ctx *c = (struct mp_prog_cb_ctx *)raw;
    u32 idx = 0;
    u32 remaining = c->plane_mask;
    while (remaining) {
        u32 p = (u32)__builtin_ctz(remaining);
        remaining &= remaining - 1;
        struct nand_page *np = nand_get_page(c->ctx->nand, c->ch, c->chip, c->die, p, c->block, c->page);
        if (!np) {
            c->out_status = HFSSS_ERR_INVAL;
            return HFSSS_ERR_INVAL;
        }
        memcpy(np->data, c->data_array[idx], c->ctx->config.page_size);
        if (c->spare_array && c->spare_array[idx]) {
            memcpy(np->spare, c->spare_array[idx], c->ctx->config.spare_size);
        }
        np->state = PAGE_VALID;
        np->program_ts = get_time_ns();
        np->erase_count = bbt_get_erase_count(c->ctx->bbt, c->ch, c->chip, c->die, p, c->block);
        np->read_count = 0;
        np->dirty = true;
        struct nand_block *nb = nand_get_block(c->ctx->nand, c->ch, c->chip, c->die, p, c->block);
        if (nb) {
            nb->state = BLOCK_OPEN;
            nb->pages_written++;
            nb->dirty = true;
        }
        idx++;
    }
    return HFSSS_OK;
}

struct mp_read_cb_ctx {
    struct media_ctx *ctx;
    u32 ch;
    u32 chip;
    u32 die;
    u32 plane_mask;
    u32 block;
    u32 page;
    void **data_array;
    void **spare_array;
    int out_status;
};

static int mp_read_on_setup_commit(void *raw)
{
    struct mp_read_cb_ctx *c = (struct mp_read_cb_ctx *)raw;
    u32 remaining = c->plane_mask;
    while (remaining) {
        u32 p = (u32)__builtin_ctz(remaining);
        remaining &= remaining - 1;
        struct nand_page *np = nand_get_page(c->ctx->nand, c->ch, c->chip, c->die, p, c->block, c->page);
        if (!np || np->state != PAGE_VALID) {
            c->out_status = HFSSS_ERR_NOENT;
            return HFSSS_ERR_NOENT;
        }
    }
    return HFSSS_OK;
}

static int mp_read_on_data_xfer_commit(void *raw)
{
    struct mp_read_cb_ctx *c = (struct mp_read_cb_ctx *)raw;
    u32 idx = 0;
    u32 remaining = c->plane_mask;
    while (remaining) {
        u32 p = (u32)__builtin_ctz(remaining);
        remaining &= remaining - 1;
        struct nand_page *np = nand_get_page(c->ctx->nand, c->ch, c->chip, c->die, p, c->block, c->page);
        if (!np) {
            return HFSSS_ERR_INVAL;
        }
        media_inject_bit_errors(c->ctx, np, c->data_array[idx], (c->spare_array ? c->spare_array[idx] : NULL),
                                c->ctx->config.page_size, c->ctx->config.spare_size);
        np->read_count++;
        idx++;
    }
    return HFSSS_OK;
}

struct mp_erase_cb_ctx {
    struct media_ctx *ctx;
    u32 ch;
    u32 chip;
    u32 die;
    u32 plane_mask;
    u32 block;
    int out_status;
};

static int mp_erase_on_array_commit(void *raw)
{
    struct mp_erase_cb_ctx *c = (struct mp_erase_cb_ctx *)raw;
    u32 remaining = c->plane_mask;
    while (remaining) {
        u32 p = (u32)__builtin_ctz(remaining);
        remaining &= remaining - 1;
        struct nand_block *nb = nand_get_block(c->ctx->nand, c->ch, c->chip, c->die, p, c->block);
        if (!nb) {
            c->out_status = HFSSS_ERR_INVAL;
            return HFSSS_ERR_INVAL;
        }
        for (u32 pg = 0; pg < nb->page_count; pg++) {
            struct nand_page *np = &nb->pages[pg];
            memset(np->data, 0xFF, c->ctx->config.page_size);
            memset(np->spare, 0xFF, c->ctx->config.spare_size);
            np->state = PAGE_FREE;
            np->program_ts = 0;
            np->read_count = 0;
            np->bit_errors = 0;
            np->dirty = true;
        }
        nb->state = BLOCK_FREE;
        nb->pages_written = 0;
        nb->dirty = true;
        bbt_increment_erase_count(c->ctx->bbt, c->ch, c->chip, c->die, p, c->block);
        u32 ec = bbt_get_erase_count(c->ctx->bbt, c->ch, c->chip, c->die, p, c->block);
        if (reliability_is_block_bad(c->ctx->reliability, c->ctx->config.nand_type, ec)) {
            bbt_mark_bad(c->ctx->bbt, c->ch, c->chip, c->die, p, c->block);
        }
    }
    return HFSSS_OK;
}

static u32 popcount32(u32 x)
{
    return (u32)__builtin_popcount(x);
}

int media_nand_multi_plane_program(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane_mask, u32 block,
                                   u32 page, const void **data_array, const void **spare_array)
{
    if (!ctx || !ctx->initialized || !data_array || plane_mask == 0) {
        return HFSSS_ERR_INVAL;
    }
    if (!ctx->config.enable_multi_plane && popcount32(plane_mask) > 1) {
        return HFSSS_ERR_NOTSUPP;
    }
    u32 remaining = plane_mask;
    u32 idx = 0;
    while (remaining) {
        u32 p = (u32)__builtin_ctz(remaining);
        remaining &= remaining - 1;
        if (nand_validate_address(ctx->nand, ch, chip, die, p, block, page) != HFSSS_OK) {
            return HFSSS_ERR_INVAL;
        }
        int is_bad = bbt_is_bad(ctx->bbt, ch, chip, die, p, block);
        if (is_bad == 1) {
            return HFSSS_ERR_IO;
        }
        if (!data_array[idx]) {
            return HFSSS_ERR_INVAL;
        }
        idx++;
    }

    struct mp_prog_cb_ctx cbc = {
        .ctx = ctx,
        .ch = ch,
        .chip = chip,
        .die = die,
        .plane_mask = plane_mask,
        .block = block,
        .page = page,
        .data_array = data_array,
        .spare_array = spare_array,
        .out_status = HFSSS_OK,
    };
    struct nand_cmd_target target = {
        .ch = ch, .chip = chip, .die = die, .plane_mask = plane_mask, .block = block, .page = page};
    struct nand_cmd_ops ops = {.on_array_commit = mp_prog_on_array_commit};

    u64 start_time = get_time_ns();
    int rc = nand_cmd_engine_submit_program(ctx->nand, &target, &ops, &cbc);
    if (rc != HFSSS_OK) {
        return cbc.out_status != HFSSS_OK ? cbc.out_status : rc;
    }

    u32 n = popcount32(plane_mask);
    mutex_lock(&ctx->lock, 0);
    ctx->stats.write_count += n;
    ctx->stats.total_write_bytes += (u64)n * ctx->config.page_size;
    ctx->stats.total_write_ns += get_time_ns() - start_time;
    mutex_unlock(&ctx->lock);
    return HFSSS_OK;
}

int media_nand_multi_plane_read(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane_mask, u32 block, u32 page,
                                void **data_array, void **spare_array)
{
    if (!ctx || !ctx->initialized || !data_array || plane_mask == 0) {
        return HFSSS_ERR_INVAL;
    }
    if (!ctx->config.enable_multi_plane && popcount32(plane_mask) > 1) {
        return HFSSS_ERR_NOTSUPP;
    }
    u32 remaining = plane_mask;
    u32 idx = 0;
    while (remaining) {
        u32 p = (u32)__builtin_ctz(remaining);
        remaining &= remaining - 1;
        if (nand_validate_address(ctx->nand, ch, chip, die, p, block, page) != HFSSS_OK) {
            return HFSSS_ERR_INVAL;
        }
        int is_bad = bbt_is_bad(ctx->bbt, ch, chip, die, p, block);
        if (is_bad == 1) {
            return HFSSS_ERR_IO;
        }
        if (!data_array[idx]) {
            return HFSSS_ERR_INVAL;
        }
        idx++;
    }

    struct mp_read_cb_ctx cbc = {
        .ctx = ctx,
        .ch = ch,
        .chip = chip,
        .die = die,
        .plane_mask = plane_mask,
        .block = block,
        .page = page,
        .data_array = data_array,
        .spare_array = spare_array,
        .out_status = HFSSS_OK,
    };
    struct nand_cmd_target target = {
        .ch = ch, .chip = chip, .die = die, .plane_mask = plane_mask, .block = block, .page = page};
    struct nand_cmd_ops ops = {.on_setup_commit = mp_read_on_setup_commit,
                               .on_data_xfer_commit = mp_read_on_data_xfer_commit};

    u64 start_time = get_time_ns();
    int rc = nand_cmd_engine_submit_read(ctx->nand, &target, &ops, &cbc);
    if (rc != HFSSS_OK) {
        return cbc.out_status != HFSSS_OK ? cbc.out_status : rc;
    }

    u32 n = popcount32(plane_mask);
    mutex_lock(&ctx->lock, 0);
    ctx->stats.read_count += n;
    ctx->stats.total_read_bytes += (u64)n * ctx->config.page_size;
    ctx->stats.total_read_ns += get_time_ns() - start_time;
    mutex_unlock(&ctx->lock);
    return HFSSS_OK;
}

int media_nand_multi_plane_erase(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane_mask, u32 block)
{
    if (!ctx || !ctx->initialized || plane_mask == 0) {
        return HFSSS_ERR_INVAL;
    }
    if (!ctx->config.enable_multi_plane && popcount32(plane_mask) > 1) {
        return HFSSS_ERR_NOTSUPP;
    }
    u32 remaining = plane_mask;
    while (remaining) {
        u32 p = (u32)__builtin_ctz(remaining);
        remaining &= remaining - 1;
        if (ch >= ctx->config.channel_count || chip >= ctx->config.chips_per_channel ||
            die >= ctx->config.dies_per_chip || p >= ctx->config.planes_per_die ||
            block >= ctx->config.blocks_per_plane) {
            return HFSSS_ERR_INVAL;
        }
        int is_bad = bbt_is_bad(ctx->bbt, ch, chip, die, p, block);
        if (is_bad == 1) {
            return HFSSS_ERR_IO;
        }
    }

    struct mp_erase_cb_ctx cbc = {
        .ctx = ctx,
        .ch = ch,
        .chip = chip,
        .die = die,
        .plane_mask = plane_mask,
        .block = block,
        .out_status = HFSSS_OK,
    };
    struct nand_cmd_target target = {
        .ch = ch, .chip = chip, .die = die, .plane_mask = plane_mask, .block = block, .page = 0};
    struct nand_cmd_ops ops = {.on_array_commit = mp_erase_on_array_commit};

    u64 start_time = get_time_ns();
    int rc = nand_cmd_engine_submit_erase(ctx->nand, &target, &ops, &cbc);
    if (rc != HFSSS_OK) {
        return cbc.out_status != HFSSS_OK ? cbc.out_status : rc;
    }

    u32 n = popcount32(plane_mask);
    mutex_lock(&ctx->lock, 0);
    ctx->stats.erase_count += n;
    ctx->stats.total_erase_ns += get_time_ns() - start_time;
    mutex_unlock(&ctx->lock);
    return HFSSS_OK;
}

int media_nand_cache_read(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane, u32 block, u32 page, void *data,
                          void *spare)
{
    if (!ctx || !ctx->initialized || !data) {
        return HFSSS_ERR_INVAL;
    }
    if (nand_validate_address(ctx->nand, ch, chip, die, plane, block, page) != HFSSS_OK) {
        return HFSSS_ERR_INVAL;
    }

    if (bbt_is_bad(ctx->bbt, ch, chip, die, plane, block) == 1) {
        return HFSSS_ERR_IO;
    }

    struct read_cb_ctx cbc = {
        .ctx = ctx,
        .ch = ch,
        .chip = chip,
        .die = die,
        .plane = plane,
        .block = block,
        .page = page,
        .data = data,
        .spare = spare,
        .nand_page = NULL,
        .out_status = HFSSS_OK,
    };
    struct nand_cmd_target target = {
        .ch = ch, .chip = chip, .die = die, .plane_mask = 1u << plane, .block = block, .page = page};
    struct nand_cmd_ops ops = {.on_setup_commit = read_on_setup_commit,
                               .on_data_xfer_commit = read_on_data_xfer_commit};

    u64 start_time = get_time_ns();
    int rc = nand_cmd_engine_submit_cache_read(ctx->nand, &target, &ops, &cbc);
    if (rc != HFSSS_OK) {
        return cbc.out_status != HFSSS_OK ? cbc.out_status : rc;
    }
    mutex_lock(&ctx->lock, 0);
    ctx->stats.read_count++;
    ctx->stats.total_read_bytes += ctx->config.page_size;
    ctx->stats.total_read_ns += get_time_ns() - start_time;
    mutex_unlock(&ctx->lock);
    return HFSSS_OK;
}

int media_nand_cache_read_end(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane, u32 block, u32 page,
                              void *data, void *spare)
{
    if (!ctx || !ctx->initialized || !data) {
        return HFSSS_ERR_INVAL;
    }
    if (nand_validate_address(ctx->nand, ch, chip, die, plane, block, page) != HFSSS_OK) {
        return HFSSS_ERR_INVAL;
    }

    if (bbt_is_bad(ctx->bbt, ch, chip, die, plane, block) == 1) {
        return HFSSS_ERR_IO;
    }

    struct read_cb_ctx cbc = {
        .ctx = ctx,
        .ch = ch,
        .chip = chip,
        .die = die,
        .plane = plane,
        .block = block,
        .page = page,
        .data = data,
        .spare = spare,
        .nand_page = NULL,
        .out_status = HFSSS_OK,
    };
    struct nand_cmd_target target = {
        .ch = ch, .chip = chip, .die = die, .plane_mask = 1u << plane, .block = block, .page = page};
    struct nand_cmd_ops ops = {.on_setup_commit = read_on_setup_commit,
                               .on_data_xfer_commit = read_on_data_xfer_commit};

    u64 start_time = get_time_ns();
    int rc = nand_cmd_engine_submit_cache_read_end(ctx->nand, &target, &ops, &cbc);
    if (rc != HFSSS_OK) {
        return cbc.out_status != HFSSS_OK ? cbc.out_status : rc;
    }
    mutex_lock(&ctx->lock, 0);
    ctx->stats.read_count++;
    ctx->stats.total_read_bytes += ctx->config.page_size;
    ctx->stats.total_read_ns += get_time_ns() - start_time;
    mutex_unlock(&ctx->lock);
    return HFSSS_OK;
}

int media_nand_cache_program(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane, u32 block, u32 page,
                             const void *data, const void *spare)
{
    if (!ctx || !ctx->initialized || !data) {
        return HFSSS_ERR_INVAL;
    }
    if (nand_validate_address(ctx->nand, ch, chip, die, plane, block, page) != HFSSS_OK) {
        return HFSSS_ERR_INVAL;
    }

    int is_bad = bbt_is_bad(ctx->bbt, ch, chip, die, plane, block);
    if (is_bad == 1) {
        return HFSSS_ERR_IO;
    }
    if (is_bad == -1) {
        return HFSSS_ERR_INVAL;
    }

    struct prog_cb_ctx cbc = {
        .ctx = ctx,
        .ch = ch,
        .chip = chip,
        .die = die,
        .plane = plane,
        .block = block,
        .page = page,
        .data = data,
        .spare = spare,
        .out_status = HFSSS_OK,
    };
    struct nand_cmd_target target = {
        .ch = ch, .chip = chip, .die = die, .plane_mask = 1u << plane, .block = block, .page = page};
    struct nand_cmd_ops ops = {.on_array_commit = prog_on_array_commit};

    u64 start_time = get_time_ns();
    int rc = nand_cmd_engine_submit_cache_program(ctx->nand, &target, &ops, &cbc);
    if (rc != HFSSS_OK) {
        return cbc.out_status != HFSSS_OK ? cbc.out_status : rc;
    }
    mutex_lock(&ctx->lock, 0);
    ctx->stats.write_count++;
    ctx->stats.total_write_bytes += ctx->config.page_size;
    ctx->stats.total_write_ns += get_time_ns() - start_time;
    mutex_unlock(&ctx->lock);
    return HFSSS_OK;
}

static int media_build_die_target(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, struct nand_cmd_target *out)
{
    if (!ctx || !ctx->initialized || !out) {
        return HFSSS_ERR_INVAL;
    }
    if (ch >= ctx->config.channel_count || chip >= ctx->config.chips_per_channel || die >= ctx->config.dies_per_chip) {
        return HFSSS_ERR_INVAL;
    }
    out->ch = ch;
    out->chip = chip;
    out->die = die;
    out->plane_mask = 1u << 0;
    out->block = 0;
    out->page = 0;
    return HFSSS_OK;
}

int media_nand_program_suspend(struct media_ctx *ctx, u32 ch, u32 chip, u32 die)
{
    struct nand_cmd_target target;
    int rc = media_build_die_target(ctx, ch, chip, die, &target);
    if (rc != HFSSS_OK) {
        return rc;
    }
    return nand_cmd_engine_submit_prog_suspend(ctx->nand, &target);
}

int media_nand_program_resume(struct media_ctx *ctx, u32 ch, u32 chip, u32 die)
{
    struct nand_cmd_target target;
    int rc = media_build_die_target(ctx, ch, chip, die, &target);
    if (rc != HFSSS_OK) {
        return rc;
    }
    return nand_cmd_engine_submit_prog_resume(ctx->nand, &target);
}

int media_nand_erase_suspend(struct media_ctx *ctx, u32 ch, u32 chip, u32 die)
{
    struct nand_cmd_target target;
    int rc = media_build_die_target(ctx, ch, chip, die, &target);
    if (rc != HFSSS_OK) {
        return rc;
    }
    return nand_cmd_engine_submit_erase_suspend(ctx->nand, &target);
}

int media_nand_erase_resume(struct media_ctx *ctx, u32 ch, u32 chip, u32 die)
{
    struct nand_cmd_target target;
    int rc = media_build_die_target(ctx, ch, chip, die, &target);
    if (rc != HFSSS_OK) {
        return rc;
    }
    return nand_cmd_engine_submit_erase_resume(ctx->nand, &target);
}

int media_nand_reset(struct media_ctx *ctx, u32 ch, u32 chip, u32 die)
{
    struct nand_cmd_target target;
    int rc = media_build_die_target(ctx, ch, chip, die, &target);
    if (rc != HFSSS_OK) {
        return rc;
    }
    return nand_cmd_engine_submit_reset(ctx->nand, &target);
}

int media_nand_is_bad_block(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane, u32 block)
{
    int is_bad;

    if (!ctx || !ctx->initialized) {
        return -1;
    }

    is_bad = bbt_is_bad(ctx->bbt, ch, chip, die, plane, block);
    return is_bad;
}

int media_nand_mark_bad_block(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane, u32 block)
{
    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    return bbt_mark_bad(ctx->bbt, ch, chip, die, plane, block);
}

u32 media_nand_get_erase_count(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane, u32 block)
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
#define MEDIA_FILE_MAGIC 0x48465353 /* "HFSS" */
#define MEDIA_FILE_VERSION 2        /* Incremented for incremental checkpoint support */

/* File header flags */
#define MEDIA_FILE_FLAG_FULL 0x00000000
#define MEDIA_FILE_FLAG_INCREMENTAL 0x00000001

/* File header structure */
struct media_file_header {
    u32 magic;
    u32 version;
    u32 flags; /* Full or incremental checkpoint */
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
    u32 total_blocks = ctx->config.channel_count * ctx->config.chips_per_channel * ctx->config.dies_per_chip *
                       ctx->config.planes_per_die * ctx->config.blocks_per_plane;
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
                        if (!blk)
                            continue;

                        /* Write block dirty flag, state, and pages_written */
                        bool block_dirty = blk->dirty;
                        if (incremental && !block_dirty) {
                            /* Write just the dirty flag as false and skip the rest */
                            if (fwrite(&block_dirty, sizeof(block_dirty), 1, f) != 1)
                                return HFSSS_ERR_IO;
                            continue;
                        }

                        if (fwrite(&block_dirty, sizeof(block_dirty), 1, f) != 1)
                            return HFSSS_ERR_IO;
                        if (fwrite(&blk->state, sizeof(blk->state), 1, f) != 1)
                            return HFSSS_ERR_IO;
                        if (fwrite(&blk->pages_written, sizeof(blk->pages_written), 1, f) != 1)
                            return HFSSS_ERR_IO;

                        for (page = 0; page < ctx->config.pages_per_block; page++) {
                            struct nand_page *pg = &blk->pages[page];
                            if (!pg)
                                continue;

                            bool page_dirty = pg->dirty;
                            if (incremental && !page_dirty) {
                                /* Write just the dirty flag as false and skip the rest */
                                if (fwrite(&page_dirty, sizeof(page_dirty), 1, f) != 1)
                                    return HFSSS_ERR_IO;
                                continue;
                            }

                            if (fwrite(&page_dirty, sizeof(page_dirty), 1, f) != 1)
                                return HFSSS_ERR_IO;

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
                        if (!blk)
                            continue;

                        /* Read block dirty flag */
                        bool block_dirty;
                        if (fread(&block_dirty, sizeof(block_dirty), 1, f) != 1)
                            return HFSSS_ERR_IO;

                        if (incremental && !block_dirty) {
                            /* Skip this block - not dirty */
                            continue;
                        }

                        /* Read block state and pages_written */
                        if (fread(&blk->state, sizeof(blk->state), 1, f) != 1)
                            return HFSSS_ERR_IO;
                        if (fread(&blk->pages_written, sizeof(blk->pages_written), 1, f) != 1)
                            return HFSSS_ERR_IO;
                        blk->dirty = false; /* Clear dirty flag after loading */

                        for (page = 0; page < ctx->config.pages_per_block; page++) {
                            struct nand_page *pg = &blk->pages[page];
                            if (!pg)
                                continue;

                            /* Read page dirty flag */
                            bool page_dirty;
                            if (fread(&page_dirty, sizeof(page_dirty), 1, f) != 1)
                                return HFSSS_ERR_IO;

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
                            pg->dirty = false; /* Clear dirty flag after loading */

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
                        if (!blk)
                            continue;
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
                        if (!blk)
                            continue;
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

/* Join a directory and filename into out buffer. Caller must ensure out_sz
 * is large enough; returns HFSSS_ERR_INVAL on truncation. */
static int build_ckpt_path(char *out, size_t out_sz, const char *dir, const char *filename)
{
    if (!out || !dir || !filename || out_sz == 0) {
        return HFSSS_ERR_INVAL;
    }
    int n = snprintf(out, out_sz, "%s/%s", dir, filename);
    if (n < 0 || (size_t)n >= out_sz) {
        return HFSSS_ERR_INVAL;
    }
    return HFSSS_OK;
}

int media_create_checkpoint(struct media_ctx *ctx, const char *checkpoint_dir)
{
    if (!ctx || !checkpoint_dir) {
        return HFSSS_ERR_INVAL;
    }
    /* Ensure directory exists (idempotent) */
    mkdir(checkpoint_dir, 0755);

    char path[SSSIM_PATH_LEN];
    int rc = build_ckpt_path(path, sizeof(path), checkpoint_dir, "checkpoint.bin");
    if (rc != HFSSS_OK)
        return rc;

    rc = media_save(ctx, path);
    if (rc == HFSSS_OK) {
        media_mark_all_clean(ctx);
    }
    return rc;
}

int media_create_incremental_checkpoint(struct media_ctx *ctx, const char *checkpoint_dir)
{
    if (!ctx || !checkpoint_dir) {
        return HFSSS_ERR_INVAL;
    }
    mkdir(checkpoint_dir, 0755);

    char path[SSSIM_PATH_LEN];
    int rc = build_ckpt_path(path, sizeof(path), checkpoint_dir, "checkpoint_inc.bin");
    if (rc != HFSSS_OK)
        return rc;

    rc = media_save_incremental(ctx, path);
    if (rc == HFSSS_OK) {
        media_mark_all_clean(ctx);
    }
    return rc;
}

int media_restore_checkpoint(struct media_ctx *ctx, const char *checkpoint_dir)
{
    if (!ctx || !checkpoint_dir) {
        return HFSSS_ERR_INVAL;
    }
    char path[SSSIM_PATH_LEN];
    int rc = build_ckpt_path(path, sizeof(path), checkpoint_dir, "checkpoint.bin");
    if (rc != HFSSS_OK)
        return rc;

    return media_load(ctx, path);
}
