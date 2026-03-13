#ifndef __HFSSS_ERROR_H
#define __HFSSS_ERROR_H

#include "common/common.h"

/* Error Context (Placeholder) */
struct error_ctx {
    u64 read_error_count;
    u64 write_error_count;
    u64 erase_error_count;
    u64 uncorrectable_count;
};

/* Function Prototypes (Placeholders) */
int error_init(struct error_ctx *ctx);
void error_cleanup(struct error_ctx *ctx);

#endif /* __HFSSS_ERROR_H */
