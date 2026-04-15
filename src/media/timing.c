#include "media/timing.h"
#include "media/nand_profile.h"
#include <string.h>

int timing_model_init_from_profile(struct timing_model *model, const struct nand_profile *profile)
{
    if (!model || !profile) {
        return HFSSS_ERR_INVAL;
    }

    memset(model, 0, sizeof(*model));

    /*
     * Profile carries timing for every cell variant; copying all four lanes
     * keeps the legacy timing_get_* switches working unchanged. The model's
     * type field is derived from the profile's nand_class so the read/program
     * latency lookups still pick the right lane.
     */
    memcpy(&model->slc, &profile->timing.slc_params, sizeof(model->slc));
    memcpy(&model->mlc, &profile->timing.mlc_params, sizeof(model->mlc));
    memcpy(&model->tlc, &profile->timing.tlc_timing, sizeof(model->tlc));
    memcpy(&model->qlc, &profile->timing.qlc_params, sizeof(model->qlc));

    switch (profile->nand_class) {
    case NAND_CLASS_ENTERPRISE_QLC:
        model->type = NAND_TYPE_QLC;
        break;
    case NAND_CLASS_ENTERPRISE_TLC:
    default:
        model->type = NAND_TYPE_TLC;
        break;
    }

    return HFSSS_OK;
}

int timing_model_init(struct timing_model *model, enum nand_type type)
{
    if (!model) {
        return HFSSS_ERR_INVAL;
    }

    const struct nand_profile *profile = nand_profile_get_default_for_type(type);
    int ret = timing_model_init_from_profile(model, profile);
    if (ret != HFSSS_OK) {
        return ret;
    }
    /* Preserve caller-provided type so SLC/MLC stays observable through
     * timing_get_*_latency lane selection even though the default profile is
     * a TLC one. */
    model->type = type;
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

u64 timing_get_cache_busy_ns(struct timing_model *model, u32 page_idx)
{
    if (!model) {
        return 0;
    }
    (void)page_idx;
    switch (model->type) {
    case NAND_TYPE_SLC:
        return model->slc.tCBSY;
    case NAND_TYPE_MLC:
        return model->mlc.tCBSY;
    case NAND_TYPE_TLC:
        return model->tlc.tCBSY;
    case NAND_TYPE_QLC:
        return model->qlc.tCBSY;
    default:
        return model->tlc.tCBSY;
    }
}

u64 timing_get_data_cache_busy_read_ns(struct timing_model *model, u32 page_idx)
{
    if (!model) {
        return 0;
    }
    (void)page_idx;
    switch (model->type) {
    case NAND_TYPE_SLC:
        return model->slc.tDCBSYR1;
    case NAND_TYPE_MLC:
        return model->mlc.tDCBSYR1;
    case NAND_TYPE_TLC:
        return model->tlc.tDCBSYR1;
    case NAND_TYPE_QLC:
        return model->qlc.tDCBSYR1;
    default:
        return model->tlc.tDCBSYR1;
    }
}
