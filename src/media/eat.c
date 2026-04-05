#include "media/eat.h"
#include <string.h>

int eat_ctx_init(struct eat_ctx *ctx)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    return HFSSS_OK;
}

void eat_ctx_cleanup(struct eat_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
}

u64 eat_get_for_plane(struct eat_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane)
{
    if (!ctx) {
        return 0;
    }

    if (ch >= MAX_CHANNELS || chip >= MAX_CHIPS_PER_CHANNEL ||
        die >= MAX_DIES_PER_CHIP || plane >= MAX_PLANES_PER_DIE) {
        return 0;
    }

    return ctx->plane_eat[ch][chip][die][plane];
}

u64 eat_get_for_die(struct eat_ctx *ctx, u32 ch, u32 chip, u32 die)
{
    if (!ctx) {
        return 0;
    }

    if (ch >= MAX_CHANNELS || chip >= MAX_CHIPS_PER_CHANNEL ||
        die >= MAX_DIES_PER_CHIP) {
        return 0;
    }

    return ctx->die_eat[ch][chip][die];
}

u64 eat_get_for_chip(struct eat_ctx *ctx, u32 ch, u32 chip)
{
    if (!ctx) {
        return 0;
    }

    if (ch >= MAX_CHANNELS || chip >= MAX_CHIPS_PER_CHANNEL) {
        return 0;
    }

    return ctx->chip_eat[ch][chip];
}

u64 eat_get_for_channel(struct eat_ctx *ctx, u32 ch)
{
    if (!ctx) {
        return 0;
    }

    if (ch >= MAX_CHANNELS) {
        return 0;
    }

    return ctx->channel_eat[ch];
}

u64 eat_get_max(struct eat_ctx *ctx, enum op_type op,
                u32 ch, u32 chip, u32 die, u32 plane)
{
    u64 max_eat = 0;
    u64 eat;

    if (!ctx) {
        return 0;
    }

    switch (op) {
    case OP_READ:
    case OP_PROGRAM:
    case OP_ERASE:
        /* Reads/programs/erases should not serialize an entire channel for the
         * full NAND busy time. The bus transfer component is not modeled
         * separately here, so only serialize the chip/die/plane resources.
         */
        break;
    default:
        eat = eat_get_for_channel(ctx, ch);
        if (eat > max_eat) max_eat = eat;
        break;
    }

    eat = eat_get_for_chip(ctx, ch, chip);
    if (eat > max_eat) max_eat = eat;

    eat = eat_get_for_die(ctx, ch, chip, die);
    if (eat > max_eat) max_eat = eat;

    eat = eat_get_for_plane(ctx, ch, chip, die, plane);
    if (eat > max_eat) max_eat = eat;

    return max_eat;
}

void eat_update(struct eat_ctx *ctx, enum op_type op,
                u32 ch, u32 chip, u32 die, u32 plane, u64 duration)
{
    u64 current_time;
    u64 new_eat;

    if (!ctx) {
        return;
    }

    if (ch >= MAX_CHANNELS || chip >= MAX_CHIPS_PER_CHANNEL ||
        die >= MAX_DIES_PER_CHIP || plane >= MAX_PLANES_PER_DIE) {
        return;
    }

    /* Use current time as base */
    current_time = get_time_ns();

    /* Update all levels - the new EAT is current time + duration */
    new_eat = current_time + duration;

    switch (op) {
    case OP_READ:
    case OP_PROGRAM:
    case OP_ERASE:
        break;
    default:
        if (new_eat > ctx->channel_eat[ch]) {
            ctx->channel_eat[ch] = new_eat;
        }
        break;
    }

    if (new_eat > ctx->chip_eat[ch][chip]) {
        ctx->chip_eat[ch][chip] = new_eat;
    }

    if (new_eat > ctx->die_eat[ch][chip][die]) {
        ctx->die_eat[ch][chip][die] = new_eat;
    }

    if (new_eat > ctx->plane_eat[ch][chip][die][plane]) {
        ctx->plane_eat[ch][chip][die][plane] = new_eat;
    }
}

void eat_reset(struct eat_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
}
