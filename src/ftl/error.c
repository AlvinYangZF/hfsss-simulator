#include "ftl/error.h"
#include <string.h>

int error_init(struct error_ctx *ctx)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->read_error_count = 0;
    ctx->write_error_count = 0;
    ctx->erase_error_count = 0;
    ctx->uncorrectable_count = 0;

    return HFSSS_OK;
}

void error_cleanup(struct error_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
}
