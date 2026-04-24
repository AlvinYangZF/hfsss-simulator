#include "media/timing.h"
#include "media/nand_profile.h"
#include <stdatomic.h>
#include <string.h>

/*
 * REQ-121 jitter helpers.
 *
 * advance_lcg runs a Knuth LCG one step, atomically, so concurrent
 * NAND worker threads cannot tear the state. memory_order_relaxed is
 * sufficient because jitter quality does not need a happens-before
 * edge with surrounding memory operations.
 *
 * apply_jitter maps the LCG output to a ±basis_points/10000 multiplier
 * on the baseline latency, and clamps the result at base/2 so a large
 * negative draw can never collapse the latency to zero (which would
 * break downstream timing-accounting arithmetic that sometimes divides
 * by the returned value).
 */
static u64 advance_lcg(struct timing_model *model)
{
    u64 prev;
    u64 next;
    do {
        prev = atomic_load_explicit(&model->jitter_state,
                                    memory_order_relaxed);
        next = prev * 6364136223846793005ULL + 1442695040888963407ULL;
    } while (!atomic_compare_exchange_weak_explicit(
                 &model->jitter_state, &prev, next,
                 memory_order_relaxed, memory_order_relaxed));
    return next;
}

static u64 apply_jitter(struct timing_model *model, u64 base)
{
    /* Acquire load pairs with the release store in
     * timing_model_enable_jitter. A non-zero value implies happens-before
     * visibility into the seed store that precedes it. */
    u32 bp = atomic_load_explicit(&model->jitter_basis_points,
                                  memory_order_acquire);
    if (bp == 0 || base == 0) {
        return base;
    }

    /* Top 32 bits of the LCG output are the high-entropy half. */
    u64 rnd   = advance_lcg(model) >> 32;
    u32 range = 2u * bp + 1u;   /* inclusive span */
    s32 delta = (s32)(rnd % range) - (s32)bp;

    /* Signed 128-bit-ish math in u64 domain: treat delta as ppm/10000. */
    s64 scaled = (s64)base * (s64)delta;
    s64 out    = (s64)base + scaled / 10000;

    s64 floor = (s64)(base / 2);
    if (out < floor) {
        out = floor;
    }
    return (u64)out;
}

int timing_model_init_from_profile(struct timing_model *model, const struct nand_profile *profile)
{
    if (!model || !profile) {
        return HFSSS_ERR_INVAL;
    }

    memset(model, 0, sizeof(*model));
    /* C11 reserves atomic_init for initializing implementation-private
     * atomic state. memset works on every mainstream target, but
     * atomic_init is the portable guarantee — do both. */
    atomic_init(&model->jitter_state, 0ULL);
    atomic_init(&model->jitter_basis_points, 0u);

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

    /* Disarm + zero the atomics via the proper store API before the
     * bulk memset, so we never rely on the memset-over-atomic behavior
     * for the live path. The memset itself is retained for the
     * non-atomic timing_params sub-structs. */
    atomic_store_explicit(&model->jitter_basis_points, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&model->jitter_state, 0ULL,
                          memory_order_relaxed);
    memset(model, 0, sizeof(*model));
}

u64 timing_get_read_latency(struct timing_model *model, u32 page_idx)
{
    if (!model) {
        return 0;
    }

    u64 base;
    switch (model->type) {
    case NAND_TYPE_SLC:
        base = model->slc.tR;
        break;
    case NAND_TYPE_MLC:
        base = model->mlc.tR;
        break;
    case NAND_TYPE_TLC:
        /* TLC has different read latencies for different page types */
        switch (page_idx % 3) {
        case 0:  base = model->tlc.tR_LSB; break;
        case 1:  base = model->tlc.tR_CSB; break;
        case 2:  base = model->tlc.tR_MSB; break;
        default: base = model->tlc.tR_LSB; break;
        }
        break;
    case NAND_TYPE_QLC:
        base = model->qlc.tR;
        break;
    default:
        base = model->tlc.tR_LSB;
        break;
    }
    return apply_jitter(model, base);
}

u64 timing_get_prog_latency(struct timing_model *model, u32 page_idx)
{
    if (!model) {
        return 0;
    }

    u64 base;
    switch (model->type) {
    case NAND_TYPE_SLC:
        base = model->slc.tPROG;
        break;
    case NAND_TYPE_MLC:
        base = model->mlc.tPROG;
        break;
    case NAND_TYPE_TLC:
        /* TLC has different program latencies for different page types */
        switch (page_idx % 3) {
        case 0:  base = model->tlc.tPROG_LSB; break;
        case 1:  base = model->tlc.tPROG_CSB; break;
        case 2:  base = model->tlc.tPROG_MSB; break;
        default: base = model->tlc.tPROG_LSB; break;
        }
        break;
    case NAND_TYPE_QLC:
        base = model->qlc.tPROG;
        break;
    default:
        base = model->tlc.tPROG_LSB;
        break;
    }
    return apply_jitter(model, base);
}

u64 timing_get_erase_latency(struct timing_model *model)
{
    if (!model) {
        return 0;
    }

    u64 base;
    switch (model->type) {
    case NAND_TYPE_SLC:
        base = model->slc.tERS;
        break;
    case NAND_TYPE_MLC:
        base = model->mlc.tERS;
        break;
    case NAND_TYPE_TLC:
        /* Use SLC erase as approximation for TLC block erase */
        base = model->slc.tERS * 2;
        break;
    case NAND_TYPE_QLC:
        base = model->qlc.tERS;
        break;
    default:
        base = model->slc.tERS * 2;
        break;
    }
    return apply_jitter(model, base);
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

void timing_model_enable_jitter(struct timing_model *model,
                                u32 basis_points, u64 seed)
{
    if (!model) {
        return;
    }
    if (basis_points == 0) {
        timing_model_disable_jitter(model);
        return;
    }
    if (basis_points > TIMING_JITTER_MAX_BP) {
        basis_points = TIMING_JITTER_MAX_BP;
    }
    /* Seed is published with a relaxed store; the release on
     * basis_points below carries the seed into acquire-visibility for
     * any reader that observes bp != 0. basis_points is the arming
     * bit, so its release store is the synchronization edge. */
    atomic_store_explicit(&model->jitter_state, seed,
                          memory_order_relaxed);
    atomic_store_explicit(&model->jitter_basis_points, basis_points,
                          memory_order_release);
}

void timing_model_disable_jitter(struct timing_model *model)
{
    if (!model) {
        return;
    }
    /* Disarm first (release): once a reader observes bp == 0 it takes
     * the early-return branch and never reads the state. Then zero the
     * state so a subsequent enable starts from a clean slate. */
    atomic_store_explicit(&model->jitter_basis_points, 0u,
                          memory_order_release);
    atomic_store_explicit(&model->jitter_state, 0ULL,
                          memory_order_relaxed);
}
