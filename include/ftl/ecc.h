#ifndef __HFSSS_ECC_H
#define __HFSSS_ECC_H

#include "common/common.h"

/* ECC Type */
enum ecc_type {
    ECC_BCH = 0,
    ECC_LDPC = 1,
};

/* ECC Context (Placeholder) */
struct ecc_ctx {
    enum ecc_type type;
    u32 codeword_size;
    u32 data_size;
    u32 parity_size;
    u32 correctable_bits;
    u64 corrected_count;
    u64 uncorrectable_count;
};

/* Function Prototypes (Placeholders) */
int ecc_init(struct ecc_ctx *ctx, enum ecc_type type);
void ecc_cleanup(struct ecc_ctx *ctx);
int ecc_encode(struct ecc_ctx *ctx, const void *data, void *codeword);
int ecc_decode(struct ecc_ctx *ctx, const void *codeword, void *data);

#endif /* __HFSSS_ECC_H */
