#ifndef __HFSSS_RELIABILITY_H
#define __HFSSS_RELIABILITY_H

#include "common/common.h"
#include "media/timing.h"

/* Reliability Parameters */
struct reliability_params {
    u32 max_pe_cycles;
    double raw_bit_error_rate;
    double read_disturb_rate;
    double data_retention_rate;
};

/* Reliability Model */
struct reliability_model {
    struct reliability_params slc;
    struct reliability_params mlc;
    struct reliability_params tlc;
    struct reliability_params qlc;
};

/* Function Prototypes */
int reliability_model_init(struct reliability_model *model);
void reliability_model_cleanup(struct reliability_model *model);
u32 reliability_calculate_bit_errors(struct reliability_model *model,
                                      enum nand_type type,
                                      u32 erase_count,
                                      u32 read_count,
                                      u64 retention_ns);
bool reliability_is_block_bad(struct reliability_model *model,
                               enum nand_type type,
                               u32 erase_count);

#endif /* __HFSSS_RELIABILITY_H */
