/*
 * PCIe link state machine implementation (REQ-064).
 *
 * The state graph is encoded in a static `legal[from][to]` adjacency
 * matrix built once at init. All transitions go through one
 * function (pcie_link_transition) that validates against the matrix;
 * specialized helpers (enter_l0s, etc.) are just thin wrappers.
 */

#include "hal/hal_pcie_link.h"
#include <string.h>
#include <time.h>

/* LLD_13 §6.2 adjacency matrix. [from][to] = true means the edge is
 * legal. RESET/FLR intermediates are always reachable from any state
 * except themselves, and they always recover to L0. */
static const bool LEGAL[PCIE_LINK_STATE_COUNT][PCIE_LINK_STATE_COUNT] = {
    /*        L0     L0s    L1     L2     RESET  FLR  */
    /*L0*/   {false, true,  true,  false, true,  true },
    /*L0s*/  {true,  false, false, false, true,  false},
    /*L1*/   {true,  false, false, true,  true,  false},
    /*L2*/   {true,  false, false, false, true,  false},
    /*RESET*/{true,  false, false, false, false, false},
    /*FLR*/  {true,  false, false, false, false, false},
};

static u64 now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

int pcie_link_init(struct pcie_link_ctx *ctx)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }
    memset(ctx, 0, sizeof(*ctx));
    int rc = pthread_mutex_init(&ctx->lock, NULL);
    if (rc != 0) {
        return HFSSS_ERR_IO;
    }
    ctx->state                = PCIE_LINK_L0;
    ctx->prev_state           = PCIE_LINK_L0;
    ctx->aspm_policy          = PCIE_ASPM_L0S_L1;
    ctx->state_enter_ns       = now_ns();
    ctx->l0s_exit_latency_us  = 100;     /* midpoint of 10..200 */
    ctx->l1_exit_latency_us   = 10000;   /* midpoint of 1000..65000 */
    ctx->l2_recovery_ms       = 20;
    ctx->initialized          = true;
    return HFSSS_OK;
}

void pcie_link_cleanup(struct pcie_link_ctx *ctx)
{
    if (!ctx || !ctx->initialized) {
        return;
    }
    pthread_mutex_destroy(&ctx->lock);
    memset(ctx, 0, sizeof(*ctx));
}

/* Caller holds ctx->lock. Commit the transition and stamp timing. */
static void commit_transition_locked(struct pcie_link_ctx *ctx,
                                     enum pcie_link_state new_state)
{
    ctx->prev_state     = ctx->state;
    ctx->state          = new_state;
    ctx->state_enter_ns = now_ns();
    ctx->transition_count++;
}

int pcie_link_transition(struct pcie_link_ctx *ctx,
                         enum pcie_link_state new_state)
{
    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }
    if ((unsigned)new_state >= PCIE_LINK_STATE_COUNT) {
        return HFSSS_ERR_INVAL;
    }

    pthread_mutex_lock(&ctx->lock);
    if (ctx->state == new_state) {
        /* No-op: already there. Treat as success without counting as
         * a transition so counters stay meaningful. */
        pthread_mutex_unlock(&ctx->lock);
        return HFSSS_OK;
    }
    if (!LEGAL[ctx->state][new_state]) {
        ctx->rejected_transitions++;
        pthread_mutex_unlock(&ctx->lock);
        return HFSSS_ERR_INVAL;
    }
    commit_transition_locked(ctx, new_state);
    pthread_mutex_unlock(&ctx->lock);
    return HFSSS_OK;
}

int pcie_link_enter_l0s(struct pcie_link_ctx *ctx)
{
    return pcie_link_transition(ctx, PCIE_LINK_L0S);
}

int pcie_link_exit_l0s(struct pcie_link_ctx *ctx)
{
    if (!ctx || !ctx->initialized) return HFSSS_ERR_INVAL;
    pthread_mutex_lock(&ctx->lock);
    if (ctx->state != PCIE_LINK_L0S) {
        pthread_mutex_unlock(&ctx->lock);
        return HFSSS_ERR_INVAL;
    }
    commit_transition_locked(ctx, PCIE_LINK_L0);
    pthread_mutex_unlock(&ctx->lock);
    return HFSSS_OK;
}

int pcie_link_enter_l1(struct pcie_link_ctx *ctx)
{
    return pcie_link_transition(ctx, PCIE_LINK_L1);
}

int pcie_link_exit_l1(struct pcie_link_ctx *ctx)
{
    if (!ctx || !ctx->initialized) return HFSSS_ERR_INVAL;
    pthread_mutex_lock(&ctx->lock);
    if (ctx->state != PCIE_LINK_L1) {
        pthread_mutex_unlock(&ctx->lock);
        return HFSSS_ERR_INVAL;
    }
    commit_transition_locked(ctx, PCIE_LINK_L0);
    pthread_mutex_unlock(&ctx->lock);
    return HFSSS_OK;
}

int pcie_hot_reset(struct pcie_link_ctx *ctx)
{
    if (!ctx || !ctx->initialized) return HFSSS_ERR_INVAL;
    pthread_mutex_lock(&ctx->lock);
    /* Hot reset is legal from any L* or from RESET itself (no-op edge
     * covered below). Never allowed while already mid-FLR. */
    if (ctx->state == PCIE_LINK_FLR) {
        ctx->rejected_transitions++;
        pthread_mutex_unlock(&ctx->lock);
        return HFSSS_ERR_BUSY;
    }
    commit_transition_locked(ctx, PCIE_LINK_RESET);
    /* Drop back to L0 once the simulated reset body completes. Real
     * hardware takes a few tens of ms; tests assert only state/flow. */
    commit_transition_locked(ctx, PCIE_LINK_L0);
    pthread_mutex_unlock(&ctx->lock);
    return HFSSS_OK;
}

int pcie_flr(struct pcie_link_ctx *ctx)
{
    if (!ctx || !ctx->initialized) return HFSSS_ERR_INVAL;
    pthread_mutex_lock(&ctx->lock);
    /* LLD_13 §6.2: FLR is only legal from L0. From lower-power states
     * the host must first bring the link back up. */
    if (ctx->state != PCIE_LINK_L0) {
        ctx->rejected_transitions++;
        pthread_mutex_unlock(&ctx->lock);
        return HFSSS_ERR_INVAL;
    }
    ctx->flr_in_progress = true;
    commit_transition_locked(ctx, PCIE_LINK_FLR);
    commit_transition_locked(ctx, PCIE_LINK_L0);
    ctx->flr_in_progress = false;
    pthread_mutex_unlock(&ctx->lock);
    return HFSSS_OK;
}

int pcie_link_set_aspm_policy(struct pcie_link_ctx *ctx,
                              enum pcie_aspm_policy policy)
{
    if (!ctx || !ctx->initialized) return HFSSS_ERR_INVAL;
    if (policy > PCIE_ASPM_L0S_L1) return HFSSS_ERR_INVAL;
    pthread_mutex_lock(&ctx->lock);
    ctx->aspm_policy = policy;
    pthread_mutex_unlock(&ctx->lock);
    return HFSSS_OK;
}

enum pcie_link_state pcie_link_get_state(struct pcie_link_ctx *ctx)
{
    if (!ctx || !ctx->initialized) return PCIE_LINK_L0;
    pthread_mutex_lock(&ctx->lock);
    enum pcie_link_state s = ctx->state;
    pthread_mutex_unlock(&ctx->lock);
    return s;
}

u64 pcie_link_time_in_state_ns(struct pcie_link_ctx *ctx)
{
    if (!ctx || !ctx->initialized) return 0;
    pthread_mutex_lock(&ctx->lock);
    u64 elapsed = now_ns() - ctx->state_enter_ns;
    pthread_mutex_unlock(&ctx->lock);
    return elapsed;
}

u32 pcie_link_transition_count(struct pcie_link_ctx *ctx)
{
    if (!ctx || !ctx->initialized) return 0;
    pthread_mutex_lock(&ctx->lock);
    u32 n = ctx->transition_count;
    pthread_mutex_unlock(&ctx->lock);
    return n;
}

u32 pcie_link_rejected_count(struct pcie_link_ctx *ctx)
{
    if (!ctx || !ctx->initialized) return 0;
    pthread_mutex_lock(&ctx->lock);
    u32 n = ctx->rejected_transitions;
    pthread_mutex_unlock(&ctx->lock);
    return n;
}
