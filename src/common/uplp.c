#include "common/uplp.h"
#include "common/log.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ------------------------------------------------------------------
 * Default energy costs for emergency flush simulation
 * ------------------------------------------------------------------ */
#define DEFAULT_NAND_PROGRAM_J   0.001   /* 1 mJ per NAND program */
#define DEFAULT_NOR_WRITE_J      0.0005  /* 0.5 mJ per NOR write */
#define DEFAULT_DRAM_HOLD_J_MS   0.002   /* 2 mJ per ms DRAM retention */

/* Energy cost per flush step (joules) */
static const double flush_step_energy[] = {
    0.010,  /* FLUSH_STEP_INFLIGHT_NAND: wait for in-flight programs */
    0.008,  /* FLUSH_STEP_L2P_JOURNAL:   flush L2P journal */
    0.005,  /* FLUSH_STEP_BBT:           update bad block table */
    0.003,  /* FLUSH_STEP_SMART:         persist SMART counters */
    0.004,  /* FLUSH_STEP_WAL_COMMIT:    WAL commit marker */
    0.002,  /* FLUSH_STEP_SYSINFO:       SysInfo clean shutdown marker */
};

#define FLUSH_STEP_COUNT 6

/* ------------------------------------------------------------------
 * Supercapacitor model implementation (REQ-139)
 * ------------------------------------------------------------------ */

void supercap_init(struct supercap_model *cap, double cap_f,
                   double esr, double v_charged, double v_cutoff,
                   double r_load)
{
    if (!cap) return;

    memset(cap, 0, sizeof(*cap));
    cap->capacitance_f = cap_f;
    cap->esr_ohm = esr;
    cap->voltage_v = v_charged;
    cap->v_charged = v_charged;
    cap->v_cutoff = v_cutoff;
    cap->r_load = r_load;
    cap->energy_j = 0.5 * cap_f * (v_charged * v_charged -
                                     v_cutoff * v_cutoff);
    cap->discharging = false;
    cap->discharge_start_ns = 0;
}

void supercap_start_discharge(struct supercap_model *cap, u64 now_ns)
{
    if (!cap) return;

    cap->discharging = true;
    cap->discharge_start_ns = now_ns;

    HFSSS_LOG_INFO("UPLP", "Supercap discharge started at %llu ns, "
                   "V=%.2fV, E=%.4fJ",
                   (unsigned long long)now_ns,
                   cap->voltage_v, cap->energy_j);
}

void supercap_update(struct supercap_model *cap, u64 now_ns)
{
    if (!cap || !cap->discharging) return;

    double t_s = (double)(now_ns - cap->discharge_start_ns) / 1e9;
    double tau = cap->r_load * cap->capacitance_f;

    cap->voltage_v = cap->v_charged * exp(-t_s / tau);
    if (cap->voltage_v < cap->v_cutoff) {
        cap->voltage_v = cap->v_cutoff;
    }

    cap->energy_j = 0.5 * cap->capacitance_f *
                    (cap->voltage_v * cap->voltage_v -
                     cap->v_cutoff * cap->v_cutoff);
    if (cap->energy_j < 0.0) {
        cap->energy_j = 0.0;
    }
}

bool supercap_has_energy(const struct supercap_model *cap)
{
    if (!cap) return false;
    return (cap->voltage_v > cap->v_cutoff) && (cap->energy_j > 0.0);
}

double supercap_drain_time_ms(const struct supercap_model *cap)
{
    if (!cap || cap->v_cutoff <= 0.0 || cap->v_charged <= 0.0) return 0.0;

    double tau = cap->r_load * cap->capacitance_f;
    return -tau * log(cap->v_cutoff / cap->v_charged) * 1000.0;
}

double supercap_get_voltage(const struct supercap_model *cap)
{
    if (!cap) return 0.0;
    return cap->voltage_v;
}

double supercap_get_energy(const struct supercap_model *cap)
{
    if (!cap) return 0.0;
    return cap->energy_j;
}

/* ------------------------------------------------------------------
 * UPLP state machine implementation (REQ-140)
 * ------------------------------------------------------------------ */

static void uplp_set_state(struct uplp_ctx *ctx, enum uplp_state new_state)
{
    ctx->state = new_state;
    ctx->state_enter_ns = get_time_ns();
}

int uplp_init(struct uplp_ctx *ctx, double cap_f, double esr,
              double v_charged, double v_cutoff, double r_load)
{
    if (!ctx) return HFSSS_ERR_INVAL;

    memset(ctx, 0, sizeof(*ctx));

    supercap_init(&ctx->cap, cap_f, esr, v_charged, v_cutoff, r_load);

    /* Initialize energy budget with defaults */
    ctx->budget.total_j = ctx->cap.energy_j;
    ctx->budget.consumed_j = 0.0;
    ctx->budget.nand_program_j = DEFAULT_NAND_PROGRAM_J;
    ctx->budget.nor_write_j = DEFAULT_NOR_WRITE_J;
    ctx->budget.dram_hold_j_per_ms = DEFAULT_DRAM_HOLD_J_MS;

    /* Initialize flush progress */
    ctx->progress.completed_steps = 0;
    ctx->progress.last_wal_seq = 0;
    ctx->progress.crc32 = 0;

    ctx->state = UPLP_NORMAL;
    ctx->state_enter_ns = get_time_ns();
    ctx->power_fail_ns = 0;
    ctx->unsafe_shutdown_count = 0;

    /* Test hook defaults */
    ctx->inject_power_fail = false;
    ctx->inject_delay_ns = 0;
    ctx->override_drain_time_ms = 0.0;
    ctx->inject_at_phase = -1;

    ctx->initialized = true;

    HFSSS_LOG_INFO("UPLP", "UPLP initialized: cap=%.1fF, V=%.1fV, "
                   "Vcut=%.1fV, E=%.3fJ",
                   cap_f, v_charged, v_cutoff, ctx->cap.energy_j);

    return HFSSS_OK;
}

void uplp_cleanup(struct uplp_ctx *ctx)
{
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
}

enum uplp_state uplp_get_state(const struct uplp_ctx *ctx)
{
    if (!ctx) return UPLP_NORMAL;
    return ctx->state;
}

const struct flush_progress *uplp_get_flush_progress(
    const struct uplp_ctx *ctx)
{
    if (!ctx) return NULL;
    return &ctx->progress;
}

/* ------------------------------------------------------------------
 * Power fail signal and state transitions (REQ-140, REQ-141)
 *
 * Transition chain:
 *   NORMAL -> POWER_FAIL -> CAP_DRAINING -> EMERGENCY_FLUSH -> SAFE_STATE
 * ------------------------------------------------------------------ */

int uplp_power_fail_signal(struct uplp_ctx *ctx)
{
    if (!ctx || !ctx->initialized) return HFSSS_ERR_INVAL;

    if (ctx->state != UPLP_NORMAL) {
        HFSSS_LOG_WARN("UPLP", "Power fail signal in non-NORMAL state %d",
                       ctx->state);
        return HFSSS_ERR_BUSY;
    }

    u64 now = get_time_ns();

    /* NORMAL -> POWER_FAIL */
    ctx->power_fail_ns = now;
    uplp_set_state(ctx, UPLP_POWER_FAIL);
    ctx->unsafe_shutdown_count++;
    HFSSS_LOG_INFO("UPLP", "Power fail detected, entering POWER_FAIL state");

    /* POWER_FAIL -> CAP_DRAINING: start supercap discharge */
    supercap_start_discharge(&ctx->cap, now);
    uplp_set_state(ctx, UPLP_CAP_DRAINING);
    HFSSS_LOG_INFO("UPLP", "Supercap discharging, drain time=%.1fms",
                   supercap_drain_time_ms(&ctx->cap));

    /* Update energy budget from current cap state */
    ctx->budget.total_j = ctx->cap.energy_j;
    ctx->budget.consumed_j = 0.0;

    /* CAP_DRAINING -> EMERGENCY_FLUSH */
    uplp_set_state(ctx, UPLP_EMERGENCY_FLUSH);
    HFSSS_LOG_INFO("UPLP", "Entering emergency flush, budget=%.3fJ",
                   ctx->budget.total_j);

    /* Execute emergency flush */
    int ret = uplp_emergency_flush(ctx);

    /* EMERGENCY_FLUSH -> SAFE_STATE */
    uplp_set_state(ctx, UPLP_SAFE_STATE);
    HFSSS_LOG_INFO("UPLP", "Reached SAFE_STATE, flushed steps=0x%x",
                   ctx->progress.completed_steps);

    return ret;
}

/* ------------------------------------------------------------------
 * Emergency flush (REQ-141, REQ-142)
 *
 * Execute flush steps in order, checking energy budget before each.
 * Steps:
 *   0. Wait for in-flight NAND programs
 *   1. Flush L2P journal
 *   2. Update BBT
 *   3. Persist SMART counters
 *   4. WAL commit marker
 *   5. SysInfo clean shutdown marker
 * ------------------------------------------------------------------ */

int uplp_emergency_flush(struct uplp_ctx *ctx)
{
    if (!ctx || !ctx->initialized) return HFSSS_ERR_INVAL;

    static const u32 step_bits[FLUSH_STEP_COUNT] = {
        FLUSH_STEP_INFLIGHT_NAND,
        FLUSH_STEP_L2P_JOURNAL,
        FLUSH_STEP_BBT,
        FLUSH_STEP_SMART,
        FLUSH_STEP_WAL_COMMIT,
        FLUSH_STEP_SYSINFO,
    };

    static const char *step_names[FLUSH_STEP_COUNT] = {
        "inflight NAND",
        "L2P journal",
        "BBT update",
        "SMART persist",
        "WAL commit",
        "SysInfo marker",
    };

    double remaining = ctx->budget.total_j - ctx->budget.consumed_j;

    for (int i = 0; i < FLUSH_STEP_COUNT; i++) {
        /* Check if we have enough energy for this step */
        if (flush_step_energy[i] > remaining) {
            HFSSS_LOG_WARN("UPLP", "Insufficient energy for step %d (%s): "
                           "need=%.4fJ, have=%.4fJ",
                           i, step_names[i],
                           flush_step_energy[i], remaining);
            break;
        }

        /* Execute step (simulated) */
        ctx->budget.consumed_j += flush_step_energy[i];
        remaining -= flush_step_energy[i];
        ctx->progress.completed_steps |= step_bits[i];

        HFSSS_LOG_INFO("UPLP", "Flush step %d (%s) complete, "
                       "consumed=%.4fJ, remaining=%.4fJ",
                       i, step_names[i],
                       ctx->budget.consumed_j, remaining);
    }

    /* Update flush progress CRC */
    ctx->progress.crc32 = hfsss_crc32(&ctx->progress,
                                       offsetof(struct flush_progress, crc32));

    if (ctx->progress.completed_steps == FLUSH_ALL_STEPS) {
        HFSSS_LOG_INFO("UPLP", "Emergency flush complete: all steps done");
        return HFSSS_OK;
    }

    HFSSS_LOG_WARN("UPLP", "Emergency flush partial: steps=0x%x",
                   ctx->progress.completed_steps);
    return HFSSS_ERR_POWER_LOSS;
}

/* ------------------------------------------------------------------
 * Torn write detection (REQ-143)
 * ------------------------------------------------------------------ */

bool uplp_check_torn_write(const struct write_unit_header *hdr,
                           const u8 *data, u32 len)
{
    if (!hdr || !data || len == 0) return true;

    if (hdr->magic != WRITE_UNIT_MAGIC) {
        return true;  /* torn: invalid magic */
    }

    u32 crc = hfsss_crc32(data, len);
    return (crc != hdr->data_crc32);  /* torn if CRC mismatch */
}

/* ------------------------------------------------------------------
 * Test hooks (REQ-144, REQ-145, REQ-146)
 * ------------------------------------------------------------------ */

void uplp_inject_power_fail(struct uplp_ctx *ctx, u64 delay_ns)
{
    if (!ctx) return;
    ctx->inject_power_fail = true;
    ctx->inject_delay_ns = delay_ns;
}

void uplp_set_cap_drain_time(struct uplp_ctx *ctx, double drain_ms)
{
    if (!ctx) return;
    ctx->override_drain_time_ms = drain_ms;

    /*
     * Adjust r_load to achieve the desired drain time.
     * drain_time = -tau * ln(v_cutoff / v_charged) * 1000
     * tau = drain_time / (-ln(v_cutoff / v_charged) * 1000)
     * tau = r_load * C
     * r_load = tau / C
     */
    if (ctx->cap.capacitance_f > 0.0 &&
        ctx->cap.v_cutoff > 0.0 &&
        ctx->cap.v_charged > 0.0) {
        double ln_ratio = log(ctx->cap.v_cutoff / ctx->cap.v_charged);
        if (ln_ratio != 0.0) {
            double tau = (drain_ms / 1000.0) / (-ln_ratio);
            ctx->cap.r_load = tau / ctx->cap.capacitance_f;
        }
    }
}

void uplp_inject_at_phase(struct uplp_ctx *ctx, int phase)
{
    if (!ctx) return;
    ctx->inject_at_phase = phase;
}
