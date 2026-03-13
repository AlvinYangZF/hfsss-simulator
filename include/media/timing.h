#ifndef __HFSSS_TIMING_H
#define __HFSSS_TIMING_H

#include "common/common.h"

/* NAND Type */
enum nand_type {
    NAND_TYPE_SLC = 0,
    NAND_TYPE_MLC = 1,
    NAND_TYPE_TLC = 2,
    NAND_TYPE_QLC = 3,
};

/* Timing Parameters (ns) */
struct timing_params {
    u64 tCCS;    /* Change Column Setup */
    u64 tR;      /* Read */
    u64 tPROG;   /* Program */
    u64 tERS;    /* Erase */
    u64 tWC;     /* Write Cycle */
    u64 tRC;     /* Read Cycle */
    u64 tADL;    /* Address Load */
    u64 tWB;     /* Write Busy */
    u64 tWHR;    /* Write Hold */
    u64 tRHW;    /* Read Hold */
};

/* TLC Timing Model */
struct tlc_timing {
    u64 tR_LSB;
    u64 tR_CSB;
    u64 tR_MSB;
    u64 tPROG_LSB;
    u64 tPROG_CSB;
    u64 tPROG_MSB;
};

/* Timing Model */
struct timing_model {
    enum nand_type type;
    struct timing_params slc;
    struct timing_params mlc;
    struct tlc_timing tlc;
    struct timing_params qlc;
};

/* Function Prototypes */
int timing_model_init(struct timing_model *model, enum nand_type type);
void timing_model_cleanup(struct timing_model *model);
u64 timing_get_read_latency(struct timing_model *model, u32 page_idx);
u64 timing_get_prog_latency(struct timing_model *model, u32 page_idx);
u64 timing_get_erase_latency(struct timing_model *model);

#endif /* __HFSSS_TIMING_H */
