#ifndef __HFSSS_ERROR_H
#define __HFSSS_ERROR_H

#include "common/common.h"

/* Read Retry Configuration */
#define READ_RETRY_MAX_ATTEMPTS 3
#define READ_RETRY_VOLTAGE_OFFSETS { -200, 0, 200 } /* mV */

/* Error Context */
struct error_ctx {
    u64 read_error_count;
    u64 write_error_count;
    u64 erase_error_count;
    u64 uncorrectable_count;
    u64 read_retry_count;      /* Number of read retry attempts */
    u64 read_retry_success;    /* Number of read retries that succeeded */
    u64 write_verify_count;    /* Number of write verify operations */
    u64 write_verify_failure;  /* Number of write verify failures */
};

/* Function Prototypes */
int error_init(struct error_ctx *ctx);
void error_cleanup(struct error_ctx *ctx);
int error_read_retry_attempt(struct error_ctx *ctx);
int error_read_retry_success(struct error_ctx *ctx);
int error_write_verify_attempt(struct error_ctx *ctx);
int error_write_verify_failure(struct error_ctx *ctx);

#endif /* __HFSSS_ERROR_H */
