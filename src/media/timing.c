#include "media/timing.h"
#include <string.h>

/* Default timing parameters for SLC (ns) */
static const struct timing_params default_slc_timing = {
    .tCCS = 100,
    .tR = 25000,
    .tPROG = 200000,
    .tERS = 1500000,
    .tWC = 30,
    .tRC = 30,
    .tADL = 100,
    .tWB = 100,
    .tWHR = 60,
    .tRHW = 60,
    .tSSBSY = 5000,
    .tRSBSY = 5000,
};

/* Default timing parameters for MLC (ns) */
static const struct timing_params default_mlc_timing = {
    .tCCS = 150,
    .tR = 50000,
    .tPROG = 600000,
    .tERS = 3000000,
    .tWC = 40,
    .tRC = 40,
    .tADL = 150,
    .tWB = 150,
    .tWHR = 80,
    .tRHW = 80,
    .tSSBSY = 10000,
    .tRSBSY = 10000,
};

/* Default timing parameters for TLC (ns) */
static const struct tlc_timing default_tlc_timing = {
    .tR_LSB = 60000,
    .tR_CSB = 70000,
    .tR_MSB = 80000,
    .tPROG_LSB = 900000,
    .tPROG_CSB = 1100000,
    .tPROG_MSB = 1300000,
    .tSSBSY = 25000,
    .tRSBSY = 25000,
};

/* Default timing parameters for QLC (ns) */
static const struct timing_params default_qlc_timing = {
    .tCCS = 200,
    .tR = 120000,
    .tPROG = 2000000,
    .tERS = 4500000,
    .tWC = 50,
    .tRC = 50,
    .tADL = 200,
    .tWB = 200,
    .tWHR = 100,
    .tRHW = 100,
    .tSSBSY = 50000,
    .tRSBSY = 50000,
};

int timing_model_init(struct timing_model *model, enum nand_type type)
{
    if (!model) {
        return HFSSS_ERR_INVAL;
    }

    memset(model, 0, sizeof(*model));
    model->type = type;

    /* Initialize with default timings */
    memcpy(&model->slc, &default_slc_timing, sizeof(model->slc));
    memcpy(&model->mlc, &default_mlc_timing, sizeof(model->mlc));
    memcpy(&model->tlc, &default_tlc_timing, sizeof(model->tlc));
    memcpy(&model->qlc, &default_qlc_timing, sizeof(model->qlc));

    return HFSSS_OK;
}

void timing_model_cleanup(struct timing_model *model)
{
    if (!model) {
        return;
    }

    memset(model, 0, sizeof(*model));
}

u64 timing_get_read_latency(struct timing_model *model, u32 page_idx)
{
    if (!model) {
        return 0;
    }

    switch (model->type) {
    case NAND_TYPE_SLC:
        return model->slc.tR;
    case NAND_TYPE_MLC:
        return model->mlc.tR;
    case NAND_TYPE_TLC:
        /* TLC has different read latencies for different page types */
        switch (page_idx % 3) {
        case 0:
            return model->tlc.tR_LSB;
        case 1:
            return model->tlc.tR_CSB;
        case 2:
            return model->tlc.tR_MSB;
        default:
            return model->tlc.tR_LSB;
        }
    case NAND_TYPE_QLC:
        return model->qlc.tR;
    default:
        return model->tlc.tR_LSB;
    }
}

u64 timing_get_prog_latency(struct timing_model *model, u32 page_idx)
{
    if (!model) {
        return 0;
    }

    switch (model->type) {
    case NAND_TYPE_SLC:
        return model->slc.tPROG;
    case NAND_TYPE_MLC:
        return model->mlc.tPROG;
    case NAND_TYPE_TLC:
        /* TLC has different program latencies for different page types */
        switch (page_idx % 3) {
        case 0:
            return model->tlc.tPROG_LSB;
        case 1:
            return model->tlc.tPROG_CSB;
        case 2:
            return model->tlc.tPROG_MSB;
        default:
            return model->tlc.tPROG_LSB;
        }
    case NAND_TYPE_QLC:
        return model->qlc.tPROG;
    default:
        return model->tlc.tPROG_LSB;
    }
}

u64 timing_get_erase_latency(struct timing_model *model)
{
    if (!model) {
        return 0;
    }

    switch (model->type) {
    case NAND_TYPE_SLC:
        return model->slc.tERS;
    case NAND_TYPE_MLC:
        return model->mlc.tERS;
    case NAND_TYPE_TLC:
        /* Use SLC erase as approximation for TLC block erase */
        return model->slc.tERS * 2;
    case NAND_TYPE_QLC:
        return model->qlc.tERS;
    default:
        return model->slc.tERS * 2;
    }
}

u64 timing_get_suspend_overhead_ns(struct timing_model *model)
{
    if (!model) {
        return 0;
    }

    switch (model->type) {
    case NAND_TYPE_SLC:
        return model->slc.tSSBSY;
    case NAND_TYPE_MLC:
        return model->mlc.tSSBSY;
    case NAND_TYPE_TLC:
        return model->tlc.tSSBSY;
    case NAND_TYPE_QLC:
        return model->qlc.tSSBSY;
    default:
        return model->tlc.tSSBSY;
    }
}

u64 timing_get_resume_overhead_ns(struct timing_model *model)
{
    if (!model) {
        return 0;
    }

    switch (model->type) {
    case NAND_TYPE_SLC:
        return model->slc.tRSBSY;
    case NAND_TYPE_MLC:
        return model->mlc.tRSBSY;
    case NAND_TYPE_TLC:
        return model->tlc.tRSBSY;
    case NAND_TYPE_QLC:
        return model->qlc.tRSBSY;
    default:
        return model->tlc.tRSBSY;
    }
}
