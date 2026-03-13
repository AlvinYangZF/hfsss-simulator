#include "hal/hal_pci.h"
#include <string.h>

int hal_pci_init(struct hal_pci_ctx *ctx)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->initialized = true;

    return HFSSS_OK;
}

void hal_pci_cleanup(struct hal_pci_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
}
