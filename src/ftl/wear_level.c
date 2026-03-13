#include "ftl/wear_level.h"
#include <string.h>

int wear_level_init(struct wear_level_ctx *ctx)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->enabled = 1;
    ctx->move_count = 0;

    return HFSSS_OK;
}

void wear_level_cleanup(struct wear_level_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
}
