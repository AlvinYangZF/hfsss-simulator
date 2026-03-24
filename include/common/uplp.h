#ifndef __HFSSS_UPLP_H
#define __HFSSS_UPLP_H

#include "common/common.h"
#include "ftl/wal.h"
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * Supercapacitor model (REQ-139)
 * ------------------------------------------------------------------ */
struct supercap_model {
    double capacitance_f;       /* Farads (1-10F configurable) */
    double esr_ohm;             /* Equivalent Series Resistance */
    double voltage_v;           /* Current voltage */
    double v_charged;           /* Fully charged voltage */
    double v_cutoff;            /* Minimum usable voltage */
    double r_load;              /* Load resistance */
    double energy_j;            /* Current energy in joules */
    u64    discharge_start_ns;
    bool   discharging;
};

/* ------------------------------------------------------------------
 * Energy budget for emergency flush (REQ-141)
 * ------------------------------------------------------------------ */
struct energy_budget {
    double total_j;
    double consumed_j;
    double nand_program_j;      /* Energy per NAND program */
    double nor_write_j;         /* Energy per NOR write */
    double dram_hold_j_per_ms;  /* DRAM retention cost per ms */
};

/* ------------------------------------------------------------------
 * UPLP states (REQ-140)
 * ------------------------------------------------------------------ */
enum uplp_state {
    UPLP_NORMAL           = 0,
    UPLP_POWER_FAIL       = 1,
    UPLP_CAP_DRAINING     = 2,
    UPLP_EMERGENCY_FLUSH  = 3,
    UPLP_SAFE_STATE       = 4,
    UPLP_RECOVERY         = 5,
};

/* ------------------------------------------------------------------
 * Emergency flush progress bitmask (REQ-141, REQ-142)
 * ------------------------------------------------------------------ */
#define FLUSH_STEP_INFLIGHT_NAND  (1u << 0)
#define FLUSH_STEP_L2P_JOURNAL    (1u << 1)
#define FLUSH_STEP_BBT            (1u << 2)
#define FLUSH_STEP_SMART          (1u << 3)
#define FLUSH_STEP_WAL_COMMIT     (1u << 4)
#define FLUSH_STEP_SYSINFO        (1u << 5)
#define FLUSH_ALL_STEPS           (0x3Fu)

/* Flush progress record (persisted to NOR for recovery) */
struct flush_progress {
    u32 completed_steps;        /* bitmask of FLUSH_STEP_* */
    u64 last_wal_seq;
    u32 crc32;
};

/* ------------------------------------------------------------------
 * Write unit header for torn write detection (REQ-143)
 * ------------------------------------------------------------------ */
struct write_unit_header {
    u32 magic;                  /* 0x57524954 "WRIT" */
    u32 sequence;
    u32 data_crc32;
    u32 flags;
};

#define WRITE_UNIT_MAGIC 0x57524954U

/* ------------------------------------------------------------------
 * UPLP context (REQ-139 through REQ-146)
 * ------------------------------------------------------------------ */
struct uplp_ctx {
    enum uplp_state state;
    struct supercap_model cap;
    struct energy_budget budget;
    struct flush_progress progress;
    u64 power_fail_ns;          /* timestamp of power failure */
    u64 state_enter_ns;         /* timestamp of current state entry */
    u32 unsafe_shutdown_count;
    bool initialized;
    /* Test hook fields */
    bool inject_power_fail;
    u64  inject_delay_ns;
    double override_drain_time_ms;
    int  inject_at_phase;       /* -1 = disabled */
};

/* ------------------------------------------------------------------
 * Supercapacitor API (REQ-139)
 * ------------------------------------------------------------------ */
void   supercap_init(struct supercap_model *cap, double cap_f,
                     double esr, double v_charged, double v_cutoff,
                     double r_load);
void   supercap_start_discharge(struct supercap_model *cap, u64 now_ns);
void   supercap_update(struct supercap_model *cap, u64 now_ns);
bool   supercap_has_energy(const struct supercap_model *cap);
double supercap_drain_time_ms(const struct supercap_model *cap);
double supercap_get_voltage(const struct supercap_model *cap);
double supercap_get_energy(const struct supercap_model *cap);

/* ------------------------------------------------------------------
 * UPLP API (REQ-140 through REQ-146)
 * ------------------------------------------------------------------ */
int              uplp_init(struct uplp_ctx *ctx, double cap_f,
                           double esr, double v_charged, double v_cutoff,
                           double r_load);
void             uplp_cleanup(struct uplp_ctx *ctx);
int              uplp_power_fail_signal(struct uplp_ctx *ctx);
enum uplp_state  uplp_get_state(const struct uplp_ctx *ctx);
int              uplp_emergency_flush(struct uplp_ctx *ctx);
bool             uplp_check_torn_write(const struct write_unit_header *hdr,
                                       const u8 *data, u32 len);
const struct flush_progress *uplp_get_flush_progress(
                                       const struct uplp_ctx *ctx);

/* ------------------------------------------------------------------
 * Test hooks (REQ-144, REQ-145, REQ-146)
 * ------------------------------------------------------------------ */
void uplp_inject_power_fail(struct uplp_ctx *ctx, u64 delay_ns);
void uplp_set_cap_drain_time(struct uplp_ctx *ctx, double drain_ms);
void uplp_inject_at_phase(struct uplp_ctx *ctx, int phase);

#ifdef __cplusplus
}
#endif

#endif /* __HFSSS_UPLP_H */
