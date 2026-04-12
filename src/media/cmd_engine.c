#include "media/cmd_engine.h"

#include <sched.h>
#include <stdatomic.h>

#include "common/mutex.h"
#include "media/cmd_legality.h"
#include "media/cmd_state.h"
#include "media/eat.h"
#include "media/nand.h"
#include "media/timing.h"

static struct nand_die *engine_get_die(struct nand_device *dev, u32 ch, u32 chip, u32 die)
{
    if (!dev) {
        return NULL;
    }
    if (ch >= dev->channel_count) {
        return NULL;
    }
    struct nand_channel *channel = &dev->channels[ch];
    if (chip >= channel->chip_count) {
        return NULL;
    }
    struct nand_chip *nand_chip = &channel->chips[chip];
    if (die >= nand_chip->die_count) {
        return NULL;
    }
    return &nand_chip->dies[die];
}

static struct nand_channel *engine_get_channel(struct nand_device *dev, u32 ch)
{
    if (!dev || ch >= dev->channel_count) {
        return NULL;
    }
    return &dev->channels[ch];
}

/*
 * Validate that every set bit in plane_mask refers to a plane that
 * exists in the die geometry. Returns HFSSS_OK and writes the popcount
 * to *out_count. For single-bit masks the first (and only) plane index
 * is written to *out_first for backward compat; for multi-bit masks
 * *out_first receives the lowest set bit.
 */
static int engine_validate_planes(u32 plane_mask, u32 plane_count, u32 *out_count, u32 *out_first)
{
    if (plane_mask == 0) {
        return HFSSS_ERR_INVAL;
    }
    u32 remaining = plane_mask;
    u32 count = 0;
    u32 first = (u32)__builtin_ctz(remaining);
    while (remaining) {
        u32 idx = (u32)__builtin_ctz(remaining);
        if (idx >= plane_count) {
            return HFSSS_ERR_INVAL;
        }
        count++;
        remaining &= remaining - 1;
    }
    if (out_count) {
        *out_count = count;
    }
    if (out_first) {
        *out_first = first;
    }
    return HFSSS_OK;
}

/*
 * Helper to iterate set bits in a plane mask. Usage:
 *   u32 mask = target->plane_mask;
 *   u32 p;
 *   FOR_EACH_PLANE(p, mask) { ... use p ... }
 */
#define FOR_EACH_PLANE(var, mask_copy)                                                                                 \
    for (u32 _m = (mask_copy); _m && ((var) = (u32)__builtin_ctz(_m), 1); _m &= _m - 1)

static enum op_type op_to_eat_op(enum nand_cmd_opcode op)
{
    switch (op) {
    case NAND_OP_READ:
        return OP_READ;
    case NAND_OP_PROG:
        return OP_PROGRAM;
    case NAND_OP_ERASE:
        return OP_ERASE;
    default:
        return OP_READ;
    }
}

static u64 engine_total_budget(struct timing_model *timing, enum nand_cmd_opcode op, u32 page)
{
    switch (op) {
    case NAND_OP_READ:
        return timing_get_read_latency(timing, page);
    case NAND_OP_PROG:
        return timing_get_prog_latency(timing, page);
    case NAND_OP_ERASE:
        return timing_get_erase_latency(timing);
    case NAND_OP_RESET:
    default:
        return 0;
    }
}

static void engine_wait_until_available_mask(struct eat_ctx *eat, enum op_type op, u32 ch, u32 chip, u32 die,
                                             u32 plane_mask)
{
    u64 deadline = 0;
    u32 p;
    FOR_EACH_PLANE(p, plane_mask)
    {
        u64 d = eat_get_max(eat, op, ch, chip, die, p);
        if (d > deadline) {
            deadline = d;
        }
    }
    while (get_time_ns() < deadline) {
        sched_yield();
    }
}

static void eat_update_stage_mask(struct eat_ctx *eat, enum op_type op, u32 ch, u32 chip, u32 die, u32 plane_mask,
                                  u64 stage_ns)
{
    u32 p;
    FOR_EACH_PLANE(p, plane_mask)
    {
        eat_update_stage(eat, op, ch, chip, die, p, stage_ns);
    }
}

/*
 * Phase 3 array-busy wait. Returns a completion code describing why the wait
 * ended:
 *   ENGINE_WAIT_DONE     — remaining_ns drained normally, commit can run
 *   ENGINE_WAIT_SUSPEND  — suspend_request observed; remaining_ns has been
 *                          updated to reflect the unconsumed portion, the die
 *                          state has been flipped to DIE_SUSPENDED_*, and the
 *                          loop has already drained the subsequent suspended
 *                          window by waiting for resume (or abort). On return
 *                          the die is back to *_ARRAY_BUSY with a refreshed
 *                          array_started_ns. Caller resumes counting down.
 *   ENGINE_WAIT_ABORT    — abort_epoch changed (reset-during-suspend, or
 *                          reset-while-spinning). Caller MUST skip the
 *                          commit hook and drive the die to IDLE. remaining_ns
 *                          is zeroed.
 *
 * The loop runs with no locks held except for the brief die_lock windows it
 * opens to inspect/mutate cmd_state. That matches the Phase 2 lock discipline.
 */
enum engine_wait_result {
    ENGINE_WAIT_DONE = 0,
    ENGINE_WAIT_SUSPEND,
    ENGINE_WAIT_ABORT,
};

static enum engine_wait_result engine_run_array_busy(struct nand_device *dev, struct nand_die *die,
                                                     enum nand_cmd_opcode op, const struct nand_cmd_target *target,
                                                     u32 epoch_at_submit)
{
    for (;;) {
        if (atomic_load(&die->cmd_state.abort_epoch) != epoch_at_submit) {
            /* Reset already forced state to DIE_IDLE + in_flight=false
             * under die_lock. We return immediately without touching
             * cmd_state — any mutation here would race with whatever op
             * (if any) the die owner starts next. The caller skips its
             * final cleanup block entirely. */
            return ENGINE_WAIT_ABORT;
        }

        if (atomic_load(&die->cmd_state.suspend_request)) {
            /* Account the drained portion of this array-busy segment and
             * flip to the suspended state under die_lock. Recheck abort
             * inside the lock window to avoid overwriting a concurrent
             * reset's clean state with DIE_SUSPENDED_*. */
            mutex_lock(&die->die_lock, 0);
            if (atomic_load(&die->cmd_state.abort_epoch) != epoch_at_submit) {
                mutex_unlock(&die->die_lock);
                return ENGINE_WAIT_ABORT;
            }
            u64 now = get_time_ns();
            u64 elapsed = now - die->cmd_state.array_started_ns;
            if (elapsed > die->cmd_state.remaining_ns) {
                elapsed = die->cmd_state.remaining_ns;
            }
            die->cmd_state.remaining_ns -= elapsed;
            if (op == NAND_OP_PROG) {
                die->cmd_state.state = DIE_SUSPENDED_PROG;
            } else {
                die->cmd_state.state = DIE_SUSPENDED_ERASE;
            }
            die->cmd_state.last_suspend_ts_ns = now;
            die->cmd_state.suspend_count++;
            atomic_store(&die->cmd_state.suspend_request, 0);
            mutex_unlock(&die->die_lock);

            /* Charge tSSBSY to EAT and drain the matching wall-clock so
             * the overhead is reflected in both statistics and observable
             * latency. */
            u64 sus_ov = timing_get_suspend_overhead_ns(dev->timing);
            enum op_type sus_eat_op = (op == NAND_OP_PROG) ? OP_PROGRAM : OP_ERASE;
            if (sus_ov > 0) {
                eat_update_stage_mask(dev->eat, sus_eat_op, target->ch, target->chip, target->die, target->plane_mask,
                                      sus_ov);
            }
            u64 sus_deadline = get_time_ns() + sus_ov;
            while (get_time_ns() < sus_deadline) {
                sched_yield();
            }

            /* Wait for resume or abort. While suspended the die state is
             * DIE_SUSPENDED_*; resume flips it back to *_ARRAY_BUSY. The
             * spin-and-yield pattern bounds CPU cost during test-driven
             * suspend windows. */
            for (;;) {
                if (atomic_load(&die->cmd_state.abort_epoch) != epoch_at_submit) {
                    return ENGINE_WAIT_ABORT;
                }
                enum nand_die_state s;
                mutex_lock(&die->die_lock, 0);
                s = die->cmd_state.state;
                mutex_unlock(&die->die_lock);
                if (s == DIE_PROG_ARRAY_BUSY || s == DIE_ERASE_ARRAY_BUSY) {
                    break;
                }
                sched_yield();
            }

            /* Resumed: reset the segment anchor so the next iteration
             * measures elapsed time from the resume point, not the original
             * submit. Resume overhead was already accounted for by the
             * resume submit path. */
            mutex_lock(&die->die_lock, 0);
            die->cmd_state.array_started_ns = get_time_ns();
            mutex_unlock(&die->die_lock);
            continue;
        }

        u64 now = get_time_ns();
        u64 elapsed = now - die->cmd_state.array_started_ns;
        if (elapsed >= die->cmd_state.remaining_ns) {
            mutex_lock(&die->die_lock, 0);
            die->cmd_state.remaining_ns = 0;
            mutex_unlock(&die->die_lock);
            return ENGINE_WAIT_DONE;
        }
        sched_yield();
    }
}

static int engine_submit(struct nand_device *dev, enum nand_cmd_opcode op, const struct nand_cmd_target *target,
                         const struct nand_cmd_ops *ops, void *cb_ctx)
{
    if (!dev || !dev->eat || !dev->timing || !target) {
        return HFSSS_ERR_INVAL;
    }

    u32 plane_count = 0;
    u32 plane_first = 0;
    struct nand_die *die = engine_get_die(dev, target->ch, target->chip, target->die);
    if (!die) {
        return HFSSS_ERR_INVAL;
    }
    int rc = engine_validate_planes(target->plane_mask, die->plane_count, &plane_count, &plane_first);
    if (rc != HFSSS_OK) {
        return rc;
    }

    struct nand_channel *channel = engine_get_channel(dev, target->ch);
    if (!channel) {
        return HFSSS_ERR_INVAL;
    }

    /*
     * Channel-level serialization is the first-order command submission
     * boundary in early phases; per-die work is gated underneath it. The
     * die_lock is held only across the short cmd_state transactional
     * updates (begin, each advance_phase, completion) so a concurrent
     * status reader can take a coherent snapshot while the command is
     * busy-waiting or running its commit hook — satisfying the
     * "status observable concurrently with in-flight command" contract
     * in the design spec concurrency section.
     *
     * Phase 3: for PROG/ERASE submissions the channel lock is released
     * across the array-busy wait so a suspend submitted by another thread
     * can acquire it and set the interrupt flag. The die state machine
     * still prevents a second PROG/ERASE from sneaking in because the
     * legality matrix rejects non-suspend ops in DIE_*_ARRAY_BUSY.
     */
    mutex_lock(&channel->lock, 0);
    mutex_lock(&die->die_lock, 0);

    if (!nand_cmd_is_legal_in_state(die->cmd_state.state, op)) {
        mutex_unlock(&die->die_lock);
        mutex_unlock(&channel->lock);
        return HFSSS_ERR_BUSY;
    }

    /*
     * Read-during-suspend conflict gate. A read against the same physical
     * page as the suspended program (or the same block as the suspended
     * erase) returns BUSY without touching cmd_state. Non-conflicting reads
     * fall through to the normal begin() path below and preserve the
     * suspension context because non-conflict read state transitions are
     * engine-managed around the existing fields — see the DIE_SUSPENDED_*
     * arm in nand_cmd_next_state_on_accept.
     */
    if (op == NAND_OP_READ &&
        (die->cmd_state.state == DIE_SUSPENDED_PROG || die->cmd_state.state == DIE_SUSPENDED_ERASE)) {
        const struct nand_cmd_target *s = &die->cmd_state.suspended_target;
        bool planes_overlap = (target->plane_mask & s->plane_mask) != 0;
        bool conflict = false;
        if (planes_overlap) {
            if (die->cmd_state.state == DIE_SUSPENDED_PROG) {
                conflict = (target->block == s->block && target->page == s->page);
            } else {
                conflict = (target->block == s->block);
            }
        }
        if (conflict) {
            mutex_unlock(&die->die_lock);
            mutex_unlock(&channel->lock);
            return HFSSS_ERR_BUSY;
        }
    }

    /*
     * Non-conflicting read during a suspended PROG/ERASE takes a fast path
     * that runs the read entirely on the caller's stack: the suspension
     * context (state/phase/opcode/target/remaining_ns/suspend_count/
     * in_flight) must not be disturbed, and the lightweight path matches
     * the Phase 2 identity/status discipline for mid-flight observability.
     */
    const bool is_read_during_suspend = (op == NAND_OP_READ) && (die->cmd_state.state == DIE_SUSPENDED_PROG ||
                                                                 die->cmd_state.state == DIE_SUSPENDED_ERASE);

    u64 total_ns = engine_total_budget(dev->timing, op, target->page);
    enum op_type eat_op = op_to_eat_op(op);
    u64 setup_ns = 0;
    u64 array_ns = 0;
    u64 xfer_ns = 0;
    nand_cmd_stage_budget(op, total_ns, &setup_ns, &array_ns, &xfer_ns);
    int stage_rc = HFSSS_OK;

    if (is_read_during_suspend) {
        mutex_unlock(&die->die_lock);

        /* Skip the EAT wait: the suspended PROG/ERASE already booked its
         * full array_ns into EAT, so eat_get_max would return a deadline
         * in the future that blocks until the PROG/ERASE would have
         * completed. The ONFI contract says a legal read during suspend
         * runs in the suspended window, not after the suspended op. The
         * read's own EAT stages are still committed below so subsequent
         * operations see the correct timeline. */

        if (ops && ops->on_setup_commit) {
            stage_rc = ops->on_setup_commit(cb_ctx);
        }
        if (stage_rc == HFSSS_OK) {
            eat_update_stage_mask(dev->eat, eat_op, target->ch, target->chip, target->die, target->plane_mask,
                                  setup_ns);
            eat_update_stage_mask(dev->eat, eat_op, target->ch, target->chip, target->die, target->plane_mask,
                                  array_ns);
            if (ops && ops->on_array_commit) {
                stage_rc = ops->on_array_commit(cb_ctx);
            }
        }
        if (stage_rc == HFSSS_OK) {
            eat_update_stage_mask(dev->eat, eat_op, target->ch, target->chip, target->die, target->plane_mask, xfer_ns);
            if (ops && ops->on_data_xfer_commit) {
                stage_rc = ops->on_data_xfer_commit(cb_ctx);
            }
        }

        mutex_unlock(&channel->lock);
        return stage_rc;
    }

    nand_cmd_state_begin(&die->cmd_state, op, target, total_ns);
    u32 epoch_at_submit = atomic_load(&die->cmd_state.abort_epoch);
    mutex_unlock(&die->die_lock);

    engine_wait_until_available_mask(dev->eat, eat_op, target->ch, target->chip, target->die, target->plane_mask);

    enum engine_wait_result wait_rc = ENGINE_WAIT_DONE;

    /* Check for abort before each stage. The epoch comparison detects
     * resets even if a new command was submitted after the reset (which
     * would have left abort_epoch at the incremented value). */
    if (atomic_load(&die->cmd_state.abort_epoch) != epoch_at_submit) {
        wait_rc = ENGINE_WAIT_ABORT;
    }

    /* Setup stage runs the setup hook first so address/target validation can
     * reject the command without burning any array/xfer time, matching the
     * legacy short-circuit where a failed read of an unwritten page consumed
     * no EAT. */
    if (wait_rc != ENGINE_WAIT_ABORT && ops && ops->on_setup_commit) {
        stage_rc = ops->on_setup_commit(cb_ctx);
    }
    if (wait_rc != ENGINE_WAIT_ABORT && stage_rc == HFSSS_OK) {
        eat_update_stage_mask(dev->eat, eat_op, target->ch, target->chip, target->die, target->plane_mask, setup_ns);
    }

    /* Array-busy stage: EAT is committed before the commit hook so the
     * caller-visible completion boundary is reached at the start of the
     * array mutation, matching the legacy eat-before-memcpy ordering. */
    if (wait_rc != ENGINE_WAIT_ABORT && stage_rc == HFSSS_OK) {
        mutex_lock(&die->die_lock, 0);
        enum nand_die_state next_array_state = nand_cmd_next_state_on_complete(die->cmd_state.state, op);
        nand_cmd_state_advance_phase(&die->cmd_state, CMD_PHASE_ARRAY_BUSY, next_array_state);
        die->cmd_state.array_budget_ns = array_ns;
        die->cmd_state.remaining_ns = array_ns;
        die->cmd_state.array_started_ns = get_time_ns();
        mutex_unlock(&die->die_lock);

        eat_update_stage_mask(dev->eat, eat_op, target->ch, target->chip, target->die, target->plane_mask, array_ns);

        if (op == NAND_OP_PROG || op == NAND_OP_ERASE) {
            /* Drop the channel lock so a concurrent suspend/reset submit
             * can reach the die. The die state machine guards against any
             * other submit type sneaking in during the window. */
            mutex_unlock(&channel->lock);
            wait_rc = engine_run_array_busy(dev, die, op, target, epoch_at_submit);
            mutex_lock(&channel->lock, 0);
        }

        /* Recheck abort after the spin returns DONE: a reset could land
         * between the spin exit and this point (the channel lock was just
         * re-acquired, but die_lock was not held across the gap). */
        if (wait_rc == ENGINE_WAIT_DONE && atomic_load(&die->cmd_state.abort_epoch) != epoch_at_submit) {
            wait_rc = ENGINE_WAIT_ABORT;
        }
        if (wait_rc == ENGINE_WAIT_ABORT) {
            stage_rc = HFSSS_ERR_BUSY;
        } else if (ops && ops->on_array_commit) {
            stage_rc = ops->on_array_commit(cb_ctx);
        }
    }

    /* Data transfer stage is only exercised by the read path today, but the
     * transition is wired so future opcodes can plug in without engine
     * changes. */
    if (stage_rc == HFSSS_OK && op == NAND_OP_READ) {
        mutex_lock(&die->die_lock, 0);
        enum nand_die_state next_xfer_state = nand_cmd_next_state_on_complete(die->cmd_state.state, op);
        nand_cmd_state_advance_phase(&die->cmd_state, CMD_PHASE_DATA_XFER, next_xfer_state);
        mutex_unlock(&die->die_lock);
        eat_update_stage_mask(dev->eat, eat_op, target->ch, target->chip, target->die, target->plane_mask, xfer_ns);
        if (ops && ops->on_data_xfer_commit) {
            stage_rc = ops->on_data_xfer_commit(cb_ctx);
        }
    }

    /* On abort the reset path already force-cleaned cmd_state. Touching
     * it again would race with any new op the die owner starts after the
     * reset. Skip the final cleanup entirely — just release the channel
     * lock (re-acquired above) and return the abort error code. */
    if (wait_rc == ENGINE_WAIT_ABORT) {
        mutex_unlock(&channel->lock);
        return HFSSS_ERR_BUSY;
    }

    /* Drive the die back to IDLE regardless of commit status so a failed
     * hook does not strand the die. PROG/ERASE completions update the
     * ONFI sticky FAIL latch: a successful completion clears the latch,
     * a failed completion sets it; any intervening READ or identity op
     * must not touch it (spec "Suspend/Resume Semantics" status layer). */
    mutex_lock(&die->die_lock, 0);
    die->cmd_state.result_status = stage_rc;
    die->cmd_state.in_flight = false;
    die->cmd_state.phase = CMD_PHASE_COMPLETE;
    die->cmd_state.state = DIE_IDLE;
    die->cmd_state.remaining_ns = 0;
    die->cmd_state.array_budget_ns = 0;
    die->cmd_state.array_started_ns = 0;
    atomic_store(&die->cmd_state.suspend_request, 0);
    /* abort_epoch is not cleared here — it is a monotonic counter.
     * The next command will snapshot whatever value it has. */
    if (op == NAND_OP_PROG || op == NAND_OP_ERASE) {
        die->cmd_state.latched_fail = (stage_rc != HFSSS_OK);
    }
    mutex_unlock(&die->die_lock);
    mutex_unlock(&channel->lock);
    return stage_rc;
}

int nand_cmd_engine_init(struct nand_device *dev, struct eat_ctx *eat, struct timing_model *timing)
{
    if (!dev) {
        return HFSSS_ERR_INVAL;
    }
    if (eat) {
        dev->eat = eat;
    }
    if (timing) {
        dev->timing = timing;
    }
    return HFSSS_OK;
}

void nand_cmd_engine_cleanup(struct nand_device *dev)
{
    (void)dev;
}

int nand_cmd_engine_submit_read(struct nand_device *dev, const struct nand_cmd_target *target,
                                const struct nand_cmd_ops *ops, void *cb_ctx)
{
    return engine_submit(dev, NAND_OP_READ, target, ops, cb_ctx);
}

int nand_cmd_engine_submit_program(struct nand_device *dev, const struct nand_cmd_target *target,
                                   const struct nand_cmd_ops *ops, void *cb_ctx)
{
    return engine_submit(dev, NAND_OP_PROG, target, ops, cb_ctx);
}

int nand_cmd_engine_submit_erase(struct nand_device *dev, const struct nand_cmd_target *target,
                                 const struct nand_cmd_ops *ops, void *cb_ctx)
{
    return engine_submit(dev, NAND_OP_ERASE, target, ops, cb_ctx);
}

int nand_cmd_engine_submit_reset(struct nand_device *dev, const struct nand_cmd_target *target)
{
    if (!dev || !target) {
        return HFSSS_ERR_INVAL;
    }

    struct nand_die *die = engine_get_die(dev, target->ch, target->chip, target->die);
    if (!die) {
        return HFSSS_ERR_INVAL;
    }

    /*
     * Reset takes only the die lock so it can abort an in-flight PROG/ERASE
     * whose worker is spinning in engine_run_array_busy with the channel
     * lock released. The force-reset writes a clean state and sets
     * abort_epoch so the worker — if one exists — will observe the change
     * on its next iteration and return ENGINE_WAIT_ABORT. The worker's
     * engine_submit final block is skipped on abort so it cannot overwrite
     * the clean state written here.
     */
    mutex_lock(&die->die_lock, 0);

    if (!nand_cmd_is_legal_in_state(die->cmd_state.state, NAND_OP_RESET)) {
        mutex_unlock(&die->die_lock);
        return HFSSS_ERR_BUSY;
    }

    /* Increment the abort epoch so any running worker (past or present)
     * detects the reset via its saved epoch snapshot. Save the new epoch
     * before zeroing the rest of cmd_state so the memset in
     * nand_cmd_state_init does not revert it to zero. */
    u32 new_epoch = atomic_load(&die->cmd_state.abort_epoch) + 1;

    /* Full state reset: zero every field to canonical post-reset baseline
     * so enhanced status never leaks stale opcode, target, timestamps,
     * or suspend counters from the prior operation. */
    nand_cmd_state_init(&die->cmd_state);
    die->cmd_state.phase = CMD_PHASE_COMPLETE;
    atomic_store(&die->cmd_state.abort_epoch, new_epoch);

    mutex_unlock(&die->die_lock);
    return HFSSS_OK;
}

/*
 * Suspend/resume submission path. Suspend sets an atomic interrupt flag
 * that the array-busy spin observes; it then waits (briefly) for the spin
 * to acknowledge the transition by flipping state to DIE_SUSPENDED_*, so
 * the caller can immediately issue a conflict-checked read afterward with
 * deterministic ordering. Resume does the opposite: it flips state back to
 * DIE_*_ARRAY_BUSY and charges tRSBSY against EAT so timing stats reflect
 * the overhead.
 */
static int engine_submit_suspend(struct nand_device *dev, enum nand_cmd_opcode op, const struct nand_cmd_target *target)
{
    if (!dev || !target) {
        return HFSSS_ERR_INVAL;
    }
    if (op != NAND_OP_PROG_SUSPEND && op != NAND_OP_ERASE_SUSPEND) {
        return HFSSS_ERR_INVAL;
    }

    struct nand_die *die = engine_get_die(dev, target->ch, target->chip, target->die);
    if (!die) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&die->die_lock, 0);
    if (!nand_cmd_is_legal_in_state(die->cmd_state.state, op)) {
        mutex_unlock(&die->die_lock);
        return HFSSS_ERR_BUSY;
    }
    /* Snapshot the running op's target before the spin can rewrite
     * cmd_state.target. The conflict check in engine_submit reads this
     * field under die_lock. */
    die->cmd_state.suspended_target = die->cmd_state.target;
    atomic_store(&die->cmd_state.suspend_request, 1);
    mutex_unlock(&die->die_lock);

    /* Spin briefly waiting for the running worker to acknowledge the
     * suspend by flipping state. Bounded by the spin interval inside
     * engine_run_array_busy plus tSSBSY. */
    enum nand_die_state want = (op == NAND_OP_PROG_SUSPEND) ? DIE_SUSPENDED_PROG : DIE_SUSPENDED_ERASE;
    for (;;) {
        enum nand_die_state s;
        mutex_lock(&die->die_lock, 0);
        s = die->cmd_state.state;
        mutex_unlock(&die->die_lock);
        if (s == want) {
            return HFSSS_OK;
        }
        if (s == DIE_IDLE) {
            /* The running op raced us to completion. The suspend submit
             * lost the race but the caller-visible contract is
             * unchanged: the die is idle, no op is suspended. Report
             * BUSY so the caller knows the suspend was not honored. */
            return HFSSS_ERR_BUSY;
        }
        sched_yield();
    }
}

static int engine_submit_resume(struct nand_device *dev, enum nand_cmd_opcode op, const struct nand_cmd_target *target)
{
    if (!dev || !target) {
        return HFSSS_ERR_INVAL;
    }
    if (op != NAND_OP_PROG_RESUME && op != NAND_OP_ERASE_RESUME) {
        return HFSSS_ERR_INVAL;
    }

    struct nand_die *die = engine_get_die(dev, target->ch, target->chip, target->die);
    if (!die) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&die->die_lock, 0);
    if (!nand_cmd_is_legal_in_state(die->cmd_state.state, op)) {
        mutex_unlock(&die->die_lock);
        return HFSSS_ERR_BUSY;
    }
    u32 suspended_mask = die->cmd_state.suspended_target.plane_mask;
    die->cmd_state.state = (op == NAND_OP_PROG_RESUME) ? DIE_PROG_ARRAY_BUSY : DIE_ERASE_ARRAY_BUSY;
    mutex_unlock(&die->die_lock);

    enum op_type eat_op = (op == NAND_OP_PROG_RESUME) ? OP_PROGRAM : OP_ERASE;
    u64 resume_ov = timing_get_resume_overhead_ns(dev->timing);
    if (resume_ov > 0) {
        eat_update_stage_mask(dev->eat, eat_op, target->ch, target->chip, target->die, suspended_mask, resume_ov);
        /* Drain wall-clock for tRSBSY, symmetric with the tSSBSY
         * wall-clock spin in the suspend arm of engine_run_array_busy. */
        u64 res_deadline = get_time_ns() + resume_ov;
        while (get_time_ns() < res_deadline) {
            /* busy spin */
        }
    }
    return HFSSS_OK;
}

int nand_cmd_engine_submit_prog_suspend(struct nand_device *dev, const struct nand_cmd_target *target)
{
    return engine_submit_suspend(dev, NAND_OP_PROG_SUSPEND, target);
}

int nand_cmd_engine_submit_prog_resume(struct nand_device *dev, const struct nand_cmd_target *target)
{
    return engine_submit_resume(dev, NAND_OP_PROG_RESUME, target);
}

int nand_cmd_engine_submit_erase_suspend(struct nand_device *dev, const struct nand_cmd_target *target)
{
    return engine_submit_suspend(dev, NAND_OP_ERASE_SUSPEND, target);
}

int nand_cmd_engine_submit_erase_resume(struct nand_device *dev, const struct nand_cmd_target *target)
{
    return engine_submit_resume(dev, NAND_OP_ERASE_RESUME, target);
}

int nand_cmd_engine_snapshot(struct nand_device *dev, const struct nand_cmd_target *target,
                             struct nand_die_cmd_state *out)
{
    if (!dev || !target || !out) {
        return HFSSS_ERR_INVAL;
    }

    struct nand_die *die = engine_get_die(dev, target->ch, target->chip, target->die);
    if (!die) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&die->die_lock, 0);
    nand_cmd_state_snapshot(&die->cmd_state, out);
    mutex_unlock(&die->die_lock);
    return HFSSS_OK;
}

/*
 * Lightweight status path. LOCKING: die_lock only — the channel lock is
 * deliberately skipped so a status read never blocks on an in-flight command.
 * A snapshot is taken under the die lock, then decoded on the caller's stack.
 */
static int engine_submit_status_byte(struct nand_device *dev, const struct nand_cmd_target *target, u8 *out_byte)
{
    if (!dev || !target || !out_byte) {
        return HFSSS_ERR_INVAL;
    }
    struct nand_die *die = engine_get_die(dev, target->ch, target->chip, target->die);
    if (!die) {
        return HFSSS_ERR_INVAL;
    }

    struct nand_die_cmd_state snap;
    mutex_lock(&die->die_lock, 0);
    if (!nand_cmd_is_legal_in_state(die->cmd_state.state, NAND_OP_READ_STATUS)) {
        mutex_unlock(&die->die_lock);
        return HFSSS_ERR_BUSY;
    }
    nand_cmd_state_snapshot(&die->cmd_state, &snap);
    mutex_unlock(&die->die_lock);

    nand_status_byte_from_cmd_state(&snap, out_byte);
    return HFSSS_OK;
}

static int engine_submit_status_enhanced(struct nand_device *dev, const struct nand_cmd_target *target,
                                         struct nand_status_enhanced *out)
{
    if (!dev || !target || !out) {
        return HFSSS_ERR_INVAL;
    }
    struct nand_die *die = engine_get_die(dev, target->ch, target->chip, target->die);
    if (!die) {
        return HFSSS_ERR_INVAL;
    }

    struct nand_die_cmd_state snap;
    mutex_lock(&die->die_lock, 0);
    if (!nand_cmd_is_legal_in_state(die->cmd_state.state, NAND_OP_READ_STATUS_ENHANCED)) {
        mutex_unlock(&die->die_lock);
        return HFSSS_ERR_BUSY;
    }
    nand_cmd_state_snapshot(&die->cmd_state, &snap);
    mutex_unlock(&die->die_lock);

    nand_status_enhanced_from_cmd_state(&snap, out);
    return HFSSS_OK;
}

/*
 * Identity path for READ_ID / READ_PARAM_PAGE. Takes the channel→die lock
 * order so the data bus is serialized against concurrent submits. The helper
 * performs a pure payload copy and does NOT disturb the in-flight command
 * bookkeeping: it must not touch cmd_state.opcode/target/state/phase/
 * result_status/in_flight/remaining_ns/latched_fail, because identity reads
 * are legal while a program or erase is suspended (DIE_SUSPENDED_*) and
 * clobbering those fields would destroy the suspension context for the
 * later resume path, and would also clear the ONFI sticky FAIL latch on
 * every identity read. EAT is intentionally not advanced: these are
 * bus-bound ops and the current timing model does not expose
 * tWHR / tRR / tDCBSYR1 fidelity — that is a later-phase refinement.
 */
static int engine_submit_identity(struct nand_device *dev, enum nand_cmd_opcode op,
                                  const struct nand_cmd_target *target, void *out)
{
    if (!dev || !target || !out) {
        return HFSSS_ERR_INVAL;
    }
    if (op != NAND_OP_READ_ID && op != NAND_OP_READ_PARAM_PAGE) {
        return HFSSS_ERR_INVAL;
    }

    struct nand_channel *channel = engine_get_channel(dev, target->ch);
    if (!channel) {
        return HFSSS_ERR_INVAL;
    }
    struct nand_die *die = engine_get_die(dev, target->ch, target->chip, target->die);
    if (!die) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&channel->lock, 0);
    mutex_lock(&die->die_lock, 0);

    if (!nand_cmd_is_legal_in_state(die->cmd_state.state, op)) {
        mutex_unlock(&die->die_lock);
        mutex_unlock(&channel->lock);
        return HFSSS_ERR_BUSY;
    }

    if (op == NAND_OP_READ_ID) {
        struct nand_id *dst = (struct nand_id *)out;
        *dst = dev->nand_id;
    } else {
        struct nand_parameter_page *dst = (struct nand_parameter_page *)out;
        *dst = dev->param_page;
    }

    mutex_unlock(&die->die_lock);
    mutex_unlock(&channel->lock);
    return HFSSS_OK;
}

int nand_cmd_engine_submit_read_status(struct nand_device *dev, const struct nand_cmd_target *target,
                                       u8 *out_status_byte)
{
    return engine_submit_status_byte(dev, target, out_status_byte);
}

int nand_cmd_engine_submit_read_status_enhanced(struct nand_device *dev, const struct nand_cmd_target *target,
                                                struct nand_status_enhanced *out)
{
    return engine_submit_status_enhanced(dev, target, out);
}

int nand_cmd_engine_submit_read_id(struct nand_device *dev, const struct nand_cmd_target *target, struct nand_id *out)
{
    return engine_submit_identity(dev, NAND_OP_READ_ID, target, out);
}

int nand_cmd_engine_submit_read_param_page(struct nand_device *dev, const struct nand_cmd_target *target,
                                           struct nand_parameter_page *out)
{
    return engine_submit_identity(dev, NAND_OP_READ_PARAM_PAGE, target, out);
}
