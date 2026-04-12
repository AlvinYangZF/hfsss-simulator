#include "media/cmd_state.h"
#include "media/cmd_legality.h"

#include <string.h>

void nand_cmd_state_init(struct nand_die_cmd_state *s)
{
    if (!s) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->state = DIE_IDLE;
    s->phase = CMD_PHASE_NONE;
    s->in_flight = false;
    s->result_status = HFSSS_OK;
}

void nand_cmd_state_reset(struct nand_die_cmd_state *s)
{
    nand_cmd_state_init(s);
}

void nand_cmd_state_begin(struct nand_die_cmd_state *s, enum nand_cmd_opcode op, const struct nand_cmd_target *target,
                          u64 total_budget_ns)
{
    u64 now;

    if (!s || !target) {
        return;
    }

    now = get_time_ns();

    s->opcode = op;
    s->target = *target;
    s->start_ts_ns = now;
    s->phase_start_ts_ns = now;
    s->total_budget_ns = total_budget_ns;
    s->remaining_ns = total_budget_ns;
    s->suspend_count = 0;
    s->last_suspend_ts_ns = 0;
    s->result_status = HFSSS_OK;
    s->in_flight = true;
    s->phase = CMD_PHASE_SETUP;
    s->state = nand_cmd_next_state_on_accept(s->state, op);
    s->array_started_ns = 0;
    s->array_budget_ns = 0;
    memset(&s->suspended_target, 0, sizeof(s->suspended_target));
    atomic_store(&s->suspend_request, 0);
    /* Do NOT touch abort_epoch here — a new command must not clear an
     * abort signal that the previous worker has not yet observed. The
     * worker compares its saved epoch snapshot against the current
     * value to detect resets. */
}

void nand_cmd_state_advance_phase(struct nand_die_cmd_state *s, enum nand_cmd_phase next_phase,
                                  enum nand_die_state next_state)
{
    if (!s) {
        return;
    }
    s->phase = next_phase;
    s->phase_start_ts_ns = get_time_ns();
    s->state = next_state;
}

void nand_cmd_state_snapshot(const struct nand_die_cmd_state *s, struct nand_die_cmd_state *out)
{
    if (!s || !out) {
        return;
    }
    *out = *s;
}

void nand_cmd_state_abort(struct nand_die_cmd_state *s, int result_status)
{
    if (!s) {
        return;
    }
    s->result_status = result_status;
    s->in_flight = false;
    s->state = nand_cmd_next_state_on_abort(s->state);
    s->phase = CMD_PHASE_COMPLETE;
}
