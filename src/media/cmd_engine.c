#include "media/cmd_engine.h"

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

static int plane_index_from_mask(u32 plane_mask, u32 *out)
{
    if (plane_mask == 0) {
        return HFSSS_ERR_INVAL;
    }
    if ((plane_mask & (plane_mask - 1)) != 0) {
        /* The engine currently supports a single plane per command. */
        return HFSSS_ERR_NOTSUPP;
    }
    *out = (u32)__builtin_ctz(plane_mask);
    return HFSSS_OK;
}

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

static void engine_wait_until_available(struct eat_ctx *eat, enum op_type op, u32 ch, u32 chip, u32 die, u32 plane)
{
    u64 deadline = eat_get_max(eat, op, ch, chip, die, plane);
    while (get_time_ns() < deadline) {
        /* Busy spin preserves wait semantics observed by timing-sensitive tests. */
    }
}

static int engine_submit(struct nand_device *dev, enum nand_cmd_opcode op, const struct nand_cmd_target *target,
                         const struct nand_cmd_ops *ops, void *cb_ctx)
{
    if (!dev || !dev->eat || !dev->timing || !target) {
        return HFSSS_ERR_INVAL;
    }

    u32 plane = 0;
    int rc = plane_index_from_mask(target->plane_mask, &plane);
    if (rc != HFSSS_OK) {
        return rc;
    }

    struct nand_channel *channel = engine_get_channel(dev, target->ch);
    if (!channel) {
        return HFSSS_ERR_INVAL;
    }
    struct nand_die *die = engine_get_die(dev, target->ch, target->chip, target->die);
    if (!die) {
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
     */
    mutex_lock(&channel->lock, 0);
    mutex_lock(&die->die_lock, 0);

    if (!nand_cmd_is_legal_in_state(die->cmd_state.state, op)) {
        mutex_unlock(&die->die_lock);
        mutex_unlock(&channel->lock);
        return HFSSS_ERR_BUSY;
    }

    u64 total_ns = engine_total_budget(dev->timing, op, target->page);
    nand_cmd_state_begin(&die->cmd_state, op, target, total_ns);
    mutex_unlock(&die->die_lock);

    enum op_type eat_op = op_to_eat_op(op);
    engine_wait_until_available(dev->eat, eat_op, target->ch, target->chip, target->die, plane);

    u64 setup_ns = 0;
    u64 array_ns = 0;
    u64 xfer_ns = 0;
    nand_cmd_stage_budget(op, total_ns, &setup_ns, &array_ns, &xfer_ns);

    int stage_rc = HFSSS_OK;

    /* Setup stage runs the setup hook first so address/target validation can
     * reject the command without burning any array/xfer time, matching the
     * legacy short-circuit where a failed read of an unwritten page consumed
     * no EAT. */
    if (ops && ops->on_setup_commit) {
        stage_rc = ops->on_setup_commit(cb_ctx);
    }
    if (stage_rc == HFSSS_OK) {
        eat_update_stage(dev->eat, eat_op, target->ch, target->chip, target->die, plane, setup_ns);
    }

    /* Array-busy stage: EAT is committed before the commit hook so the
     * caller-visible completion boundary is reached at the start of the
     * array mutation, matching the legacy eat-before-memcpy ordering. */
    if (stage_rc == HFSSS_OK) {
        mutex_lock(&die->die_lock, 0);
        enum nand_die_state next_array_state = nand_cmd_next_state_on_complete(die->cmd_state.state, op);
        nand_cmd_state_advance_phase(&die->cmd_state, CMD_PHASE_ARRAY_BUSY, next_array_state);
        mutex_unlock(&die->die_lock);
        eat_update_stage(dev->eat, eat_op, target->ch, target->chip, target->die, plane, array_ns);
        if (ops && ops->on_array_commit) {
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
        eat_update_stage(dev->eat, eat_op, target->ch, target->chip, target->die, plane, xfer_ns);
        if (ops && ops->on_data_xfer_commit) {
            stage_rc = ops->on_data_xfer_commit(cb_ctx);
        }
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

    struct nand_channel *channel = engine_get_channel(dev, target->ch);
    if (!channel) {
        return HFSSS_ERR_INVAL;
    }
    struct nand_die *die = engine_get_die(dev, target->ch, target->chip, target->die);
    if (!die) {
        return HFSSS_ERR_INVAL;
    }

    /*
     * The synchronous submission model means any concurrent reset caller is
     * serialized behind the active command via the channel and die locks, so
     * reset always observes an idle die. True mid-flight abort is deferred to
     * the async execution model in a later phase.
     */
    mutex_lock(&channel->lock, 0);
    mutex_lock(&die->die_lock, 0);

    if (!nand_cmd_is_legal_in_state(die->cmd_state.state, NAND_OP_RESET)) {
        mutex_unlock(&die->die_lock);
        mutex_unlock(&channel->lock);
        return HFSSS_ERR_BUSY;
    }

    nand_cmd_state_begin(&die->cmd_state, NAND_OP_RESET, target, 0);
    die->cmd_state.state = DIE_IDLE;
    die->cmd_state.phase = CMD_PHASE_COMPLETE;
    die->cmd_state.in_flight = false;
    die->cmd_state.remaining_ns = 0;
    die->cmd_state.result_status = HFSSS_OK;
    /* ONFI 4.2: RESET clears the sticky FAIL latch. */
    die->cmd_state.latched_fail = false;

    mutex_unlock(&die->die_lock);
    mutex_unlock(&channel->lock);
    return HFSSS_OK;
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
