#ifndef __HFSSS_TIMING_H
#define __HFSSS_TIMING_H

#include <stdatomic.h>

#include "common/common.h"

struct nand_profile;

/* NAND Type */
enum nand_type {
    NAND_TYPE_SLC = 0,
    NAND_TYPE_MLC = 1,
    NAND_TYPE_TLC = 2,
    NAND_TYPE_QLC = 3,
};

/* Timing Parameters (ns) */
struct timing_params {
    u64 tCCS;     /* Change Column Setup */
    u64 tR;       /* Read */
    u64 tPROG;    /* Program */
    u64 tERS;     /* Erase */
    u64 tWC;      /* Write Cycle */
    u64 tRC;      /* Read Cycle */
    u64 tADL;     /* Address Load */
    u64 tWB;      /* Write Busy */
    u64 tWHR;     /* Write Hold */
    u64 tRHW;     /* Read Hold */
    u64 tSSBSY;   /* Suspend Setup Busy — cost of entering a suspended state */
    u64 tRSBSY;   /* Resume Setup Busy — cost of returning to array busy */
    u64 tCBSY;    /* Cache Busy — overlap cost during cache program */
    u64 tDCBSYR1; /* Data Cache Busy Read — overlap cost during cache read */
};

/* TLC Timing Model */
struct tlc_timing {
    u64 tR_LSB;
    u64 tR_CSB;
    u64 tR_MSB;
    u64 tPROG_LSB;
    u64 tPROG_CSB;
    u64 tPROG_MSB;
    u64 tSSBSY;
    u64 tRSBSY;
    u64 tCBSY;
    u64 tDCBSYR1;
};

/* Timing Model */
struct timing_model {
    enum nand_type type;
    struct timing_params slc;
    struct timing_params mlc;
    struct tlc_timing tlc;
    struct timing_params qlc;

    /*
     * REQ-121: optional NAND latency error injection. basis_points gives
     * the ±variance in 1/10000 (500 = ±5%); 0 disables jitter so the
     * timing lookups return the deterministic baseline. jitter_state is
     * an LCG, advanced atomically so concurrent NAND workers do not tear
     * it; single-threaded callers get a reproducible sequence from a
     * given seed. See timing_model_enable_jitter for the activation path.
     */
    _Atomic u64 jitter_state;
    u32         jitter_basis_points;
};

#define TIMING_JITTER_MAX_BP 2000u  /* ±20% ceiling */

/* Function Prototypes */
int timing_model_init(struct timing_model *model, enum nand_type type);
int timing_model_init_from_profile(struct timing_model *model, const struct nand_profile *profile);
void timing_model_cleanup(struct timing_model *model);
u64 timing_get_read_latency(struct timing_model *model, u32 page_idx);
u64 timing_get_prog_latency(struct timing_model *model, u32 page_idx);
u64 timing_get_erase_latency(struct timing_model *model);
u64 timing_get_suspend_overhead_ns(struct timing_model *model);
u64 timing_get_resume_overhead_ns(struct timing_model *model);
u64 timing_get_cache_busy_ns(struct timing_model *model, u32 page_idx);
u64 timing_get_data_cache_busy_read_ns(struct timing_model *model, u32 page_idx);

/*
 * REQ-121: enable / disable latency jitter.
 *
 * enable: basis_points in 1..TIMING_JITTER_MAX_BP. Values above the
 *         ceiling are clamped. Passing 0 is equivalent to disable.
 *         seed seeds the LCG; a non-zero seed is recommended for
 *         reproducibility across runs.
 * disable: zeroes both the factor and the state. timing_model_init
 *          leaves jitter disabled by default — production paths see
 *          deterministic timing unless the caller opts in.
 *
 * Jitter is applied only to read / program / erase latencies. Cache
 * busy, suspend, resume, and address-setup timings are out of scope
 * for REQ-121 (the requirement targets NAND array-operation timing).
 */
void timing_model_enable_jitter(struct timing_model *model,
                                u32 basis_points, u64 seed);
void timing_model_disable_jitter(struct timing_model *model);

#endif /* __HFSSS_TIMING_H */
