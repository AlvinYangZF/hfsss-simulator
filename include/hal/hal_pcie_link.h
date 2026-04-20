#ifndef HFSSS_HAL_PCIE_LINK_H
#define HFSSS_HAL_PCIE_LINK_H

/*
 * PCIe link state management (REQ-064, LLD_13 §5.2).
 *
 * Legal transitions (LLD_13 §6.2):
 *   L0  <-> L0s   (exit 10..200us)
 *   L0  <-> L1    (exit 1..65ms)
 *   L1  ->  L2    (power-off entry)
 *   L2  ->  L0    (recovery)
 *   any ->  RESET -> L0
 *   L0  ->  FLR   -> L0
 *
 * Illegal transitions (tested: HA-010): L0s->L2, L0s->L1,
 *                                        L1->L0s, * ->FLR except L0->FLR.
 */

#include "common/common.h"
#include <pthread.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum pcie_link_state {
    PCIE_LINK_L0    = 0,   /* Active */
    PCIE_LINK_L0S   = 1,   /* Standby */
    PCIE_LINK_L1    = 2,   /* Low power */
    PCIE_LINK_L2    = 3,   /* Power off */
    PCIE_LINK_RESET = 4,   /* Hot reset in progress */
    PCIE_LINK_FLR   = 5,   /* Function-Level Reset in progress */
    PCIE_LINK_STATE_COUNT
};

enum pcie_aspm_policy {
    PCIE_ASPM_DISABLED = 0,
    PCIE_ASPM_L0S      = 1,
    PCIE_ASPM_L1       = 2,
    PCIE_ASPM_L0S_L1   = 3,
};

struct pcie_link_ctx {
    enum pcie_link_state  state;
    enum pcie_link_state  prev_state;
    enum pcie_aspm_policy aspm_policy;
    u64  state_enter_ns;       /* get_time_ns() when `state` was entered */
    u32  l0s_exit_latency_us;  /* 10..200 us per NVMe/PCIe spec */
    u32  l1_exit_latency_us;   /* 1000..65000 us */
    u32  l2_recovery_ms;       /* tens of ms */
    u32  transition_count;     /* total legal transitions, for observability */
    u32  rejected_transitions; /* total illegal transition attempts */
    bool flr_in_progress;
    bool initialized;
    pthread_mutex_t lock;
};

/* Lifecycle. Starts in L0 with ASPM_L0S_L1. */
int  pcie_link_init(struct pcie_link_ctx *ctx);
void pcie_link_cleanup(struct pcie_link_ctx *ctx);

/* Drive a state transition. Returns HFSSS_ERR_INVAL when the
 * requested edge is not in the LLD_13 §6.2 graph. */
int pcie_link_transition(struct pcie_link_ctx *ctx,
                         enum pcie_link_state new_state);

/* Convenience wrappers around common edges. Each returns HFSSS_OK
 * on success or HFSSS_ERR_INVAL on illegal current-state. */
int pcie_link_enter_l0s(struct pcie_link_ctx *ctx);
int pcie_link_exit_l0s (struct pcie_link_ctx *ctx);
int pcie_link_enter_l1 (struct pcie_link_ctx *ctx);
int pcie_link_exit_l1  (struct pcie_link_ctx *ctx);

/* Hot Reset drives the link through RESET -> L0. Always legal. */
int pcie_hot_reset(struct pcie_link_ctx *ctx);

/* Function-Level Reset is only legal from L0. Drives L0 -> FLR -> L0. */
int pcie_flr(struct pcie_link_ctx *ctx);

/* ASPM policy controls which L* states hardware may auto-enter. */
int pcie_link_set_aspm_policy(struct pcie_link_ctx *ctx,
                              enum pcie_aspm_policy policy);

/* Observability */
enum pcie_link_state pcie_link_get_state(struct pcie_link_ctx *ctx);
u64  pcie_link_time_in_state_ns(struct pcie_link_ctx *ctx);
u32  pcie_link_transition_count(struct pcie_link_ctx *ctx);
u32  pcie_link_rejected_count(struct pcie_link_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* HFSSS_HAL_PCIE_LINK_H */
