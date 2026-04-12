#ifndef __HFSSS_NAND_CMD_STATE_H
#define __HFSSS_NAND_CMD_STATE_H

#include <stdatomic.h>

#include "common/common.h"
#include "common/mutex.h"

enum nand_die_state {
    DIE_IDLE = 0,
    DIE_READ_SETUP,
    DIE_READ_ARRAY_BUSY,
    DIE_READ_DATA_XFER,
    DIE_PROG_SETUP,
    DIE_PROG_ARRAY_BUSY,
    DIE_ERASE_SETUP,
    DIE_ERASE_ARRAY_BUSY,
    DIE_SUSPENDED_PROG,
    DIE_SUSPENDED_ERASE,
    DIE_RESETTING,
    DIE_STATE_COUNT
};

enum nand_cmd_phase {
    CMD_PHASE_NONE = 0,
    CMD_PHASE_SETUP,
    CMD_PHASE_ARRAY_BUSY,
    CMD_PHASE_DATA_XFER,
    CMD_PHASE_COMPLETE,
    CMD_PHASE_COUNT
};

/*
 * Engine-level opcode enum. Distinct from the hardware-facing enum nand_cmd
 * in nand.h which encodes raw NAND command bytes.
 */
enum nand_cmd_opcode {
    NAND_OP_READ = 0,
    NAND_OP_PROG,
    NAND_OP_ERASE,
    NAND_OP_RESET,
    NAND_OP_READ_STATUS,
    NAND_OP_READ_STATUS_ENHANCED,
    NAND_OP_READ_ID,
    NAND_OP_READ_PARAM_PAGE,
    NAND_OP_PROG_SUSPEND,
    NAND_OP_PROG_RESUME,
    NAND_OP_ERASE_SUSPEND,
    NAND_OP_ERASE_RESUME,
    NAND_OP_COUNT
};

struct nand_cmd_target {
    u32 ch;
    u32 chip;
    u32 die;
    u32 plane_mask;
    u32 block;
    u32 page;
};

struct nand_die_cmd_state {
    enum nand_die_state state;
    enum nand_cmd_phase phase;
    enum nand_cmd_opcode opcode;
    struct nand_cmd_target target;
    u64 start_ts_ns;
    u64 phase_start_ts_ns;
    u64 total_budget_ns;
    u64 remaining_ns;
    u32 suspend_count;
    u64 last_suspend_ts_ns;
    int result_status;
    bool in_flight;
    /*
     * Sticky FAIL latch per ONFI 4.2 Status Register semantics: set when the
     * most recent PROG/ERASE completes with an error; cleared only by the next
     * successful PROG/ERASE completion or by RESET. Read-class and identity
     * operations must not disturb it.
     */
    bool latched_fail;
    /*
     * Suspend/resume bookkeeping (Phase 3). array_started_ns anchors the
     * current array-busy segment so the engine can compute how much of
     * remaining_ns has drained when a suspend preempts it. suspended_target
     * snapshots the target of the op that is under suspension so a
     * concurrent read can be matched against it for conflict gating;
     * relying on target here is unsafe because a non-conflicting read
     * legitimately rewrites it. suspend_request is the atomic interrupt
     * flag polled by the array-busy spin loop in engine_submit. abort_request
     * is set by reset-during-suspend to wake the spin, have it skip the
     * commit hook, and let it drive the die to IDLE cleanly.
     */
    /*
     * array_started_ns, array_budget_ns, and remaining_ns are read by the
     * tight spin in engine_run_array_busy without holding die_lock. On
     * x86-64 and AArch64 (the project's target platforms) aligned u64
     * loads are naturally atomic. If the simulator is ever ported to a
     * 32-bit target these should become _Atomic u64.
     */
    u64 array_started_ns;
    u64 array_budget_ns;
    struct nand_cmd_target suspended_target;
    _Atomic int suspend_request;
    /*
     * abort_epoch: incremented by RESET to signal abort. Workers snapshot
     * the epoch at submit time; if the current epoch differs from the
     * snapshot the worker knows a reset occurred. This avoids the race
     * where a new command's begin() clears a boolean abort_request
     * before the old worker observes it.
     */
    _Atomic u32 abort_epoch;
};

void nand_cmd_state_init(struct nand_die_cmd_state *s);
void nand_cmd_state_reset(struct nand_die_cmd_state *s);
void nand_cmd_state_begin(struct nand_die_cmd_state *s, enum nand_cmd_opcode op, const struct nand_cmd_target *target,
                          u64 total_budget_ns);
void nand_cmd_state_advance_phase(struct nand_die_cmd_state *s, enum nand_cmd_phase next_phase,
                                  enum nand_die_state next_state);
void nand_cmd_state_snapshot(const struct nand_die_cmd_state *s, struct nand_die_cmd_state *out);
void nand_cmd_state_abort(struct nand_die_cmd_state *s, int result_status);

#endif /* __HFSSS_NAND_CMD_STATE_H */
