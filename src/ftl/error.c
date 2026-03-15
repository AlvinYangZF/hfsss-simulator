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
    ctx->read_retry_count = 0;
    ctx->read_retry_success = 0;
    ctx->write_verify_count = 0;
    ctx->write_verify_failure = 0;

    return HFSSS_OK;
}

void error_cleanup(struct error_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
}

int error_read_retry_attempt(struct error_ctx *ctx)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    ctx->read_retry_count++;
    return HFSSS_OK;
}

int error_read_retry_success(struct error_ctx *ctx)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    ctx->read_retry_success++;
    return HFSSS_OK;
}

int error_write_verify_attempt(struct error_ctx *ctx)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    ctx->write_verify_count++;
    return HFSSS_OK;
}

int error_write_verify_failure(struct error_ctx *ctx)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    ctx->write_verify_failure++;
    return HFSSS_OK;
}
