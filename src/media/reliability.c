#include "media/reliability.h"
#include <string.h>

#define PAGE_SIZE_TLC 16384

/* Default reliability parameters */
static const struct reliability_params default_slc_params = {
    .max_pe_cycles = 100000,
    .raw_bit_error_rate = 1e-8,
    .read_disturb_rate = 1e-10,
    .data_retention_rate = 1e-8,
};

static const struct reliability_params default_mlc_params = {
    .max_pe_cycles = 30000,
    .raw_bit_error_rate = 1e-7,
    .read_disturb_rate = 1e-9,
    .data_retention_rate = 1e-7,
};

static const struct reliability_params default_tlc_params = {
    .max_pe_cycles = 3000,
    .raw_bit_error_rate = 1e-6,
    .read_disturb_rate = 1e-8,
    .data_retention_rate = 1e-6,
};

static const struct reliability_params default_qlc_params = {
    .max_pe_cycles = 1000,
    .raw_bit_error_rate = 1e-5,
    .read_disturb_rate = 1e-7,
    .data_retention_rate = 1e-5,
};

int reliability_model_init(struct reliability_model *model)
{
    if (!model) {
        return HFSSS_ERR_INVAL;
    }

    memset(model, 0, sizeof(*model));

    /* Initialize with default parameters */
    memcpy(&model->slc, &default_slc_params, sizeof(model->slc));
    memcpy(&model->mlc, &default_mlc_params, sizeof(model->mlc));
    memcpy(&model->tlc, &default_tlc_params, sizeof(model->tlc));
    memcpy(&model->qlc, &default_qlc_params, sizeof(model->qlc));

    return HFSSS_OK;
}

void reliability_model_cleanup(struct reliability_model *model)
{
    if (!model) {
        return;
    }

    memset(model, 0, sizeof(*model));
}

static const struct reliability_params *get_params_for_type(struct reliability_model *model,
                                                              enum nand_type type)
{
    switch (type) {
    case NAND_TYPE_SLC:
        return &model->slc;
    case NAND_TYPE_MLC:
        return &model->mlc;
    case NAND_TYPE_TLC:
        return &model->tlc;
    case NAND_TYPE_QLC:
        return &model->qlc;
    default:
        return &model->tlc;
    }
}

u32 reliability_calculate_bit_errors(struct reliability_model *model,
                                      enum nand_type type,
                                      u32 erase_count,
                                      u32 read_count,
                                      u64 retention_ns)
{
    const struct reliability_params *params;
    double pe_factor;
    double rber;
    double total_errors;
    u32 bit_errors;
    u64 page_size_bits = PAGE_SIZE_TLC * 8;

    if (!model) {
        return 0;
    }

    params = get_params_for_type(model, type);

    /* Calculate P/E cycle factor - RBER increases with wear */
    if (params->max_pe_cycles > 0) {
        pe_factor = 1.0 + ((double)erase_count / params->max_pe_cycles) * 10.0;
    } else {
        pe_factor = 1.0;
    }

    /* Base RBER with P/E wear */
    rber = params->raw_bit_error_rate * pe_factor;

    /* Add read disturb errors */
    rber += params->read_disturb_rate * read_count;

    /* Add data retention errors (convert ns to days approx) */
    double retention_days = (double)retention_ns / (24.0 * 60.0 * 60.0 * 1e9);
    rber += params->data_retention_rate * retention_days;

    /* Calculate expected number of bit errors */
    total_errors = rber * page_size_bits;

    /* Convert to integer - use a simple approximation */
    bit_errors = (u32)total_errors;

    /* Add some randomness for realism (but deterministic for reproducibility) */
    u32 seed = erase_count + read_count + (u32)(retention_ns & 0xFFFFFFFF);
    seed ^= (seed << 13);
    seed ^= (seed >> 17);
    seed ^= (seed << 5);
    if ((seed & 0x7) == 0) {
        bit_errors++;
    }

    /* Cap the maximum bit errors */
    if (bit_errors > page_size_bits / 10) {
        bit_errors = page_size_bits / 10;
    }

    return bit_errors;
}

bool reliability_is_block_bad(struct reliability_model *model,
                               enum nand_type type,
                               u32 erase_count)
{
    const struct reliability_params *params;

    if (!model) {
        return false;
    }

    params = get_params_for_type(model, type);

    /* Block is considered bad when it exceeds max P/E cycles by 20% */
    if (erase_count > params->max_pe_cycles + (params->max_pe_cycles / 5)) {
        return true;
    }

    return false;
}
