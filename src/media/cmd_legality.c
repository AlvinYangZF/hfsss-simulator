#include "media/cmd_legality.h"

/*
 * Legality matrix — rows are current die state, columns are incoming opcode.
 * 'true' means the engine accepts the submission in that state. RESET is
 * always accepted except when the die is already resetting.
 *
 * Status operations (READ_STATUS, READ_STATUS_ENHANCED) are legal in every
 * die state because ONFI 4.2 requires status to be observable mid-flight so
 * the host can detect RDY/ARDY transitions. They take the lightweight
 * die-lock-only snapshot path and never cancel or interfere with an in-flight
 * command.
 *
 * Identity operations (READ_ID, READ_PARAM_PAGE) use the data bus and cannot
 * race an active array operation, so they are legal only in DIE_IDLE and the
 * suspend states (design spec "Suspend/Resume Semantics" allows
 * non-conflicting reads during suspend).
 */
static const bool k_legal[DIE_STATE_COUNT][NAND_OP_COUNT] = {
    [DIE_IDLE] =
        {
            [NAND_OP_READ] = true,
            [NAND_OP_PROG] = true,
            [NAND_OP_ERASE] = true,
            [NAND_OP_RESET] = true,
            [NAND_OP_READ_STATUS] = true,
            [NAND_OP_READ_STATUS_ENHANCED] = true,
            [NAND_OP_READ_ID] = true,
            [NAND_OP_READ_PARAM_PAGE] = true,
        },
    [DIE_READ_SETUP] =
        {
            [NAND_OP_RESET] = true,
            [NAND_OP_READ_STATUS] = true,
            [NAND_OP_READ_STATUS_ENHANCED] = true,
        },
    [DIE_READ_ARRAY_BUSY] =
        {
            [NAND_OP_RESET] = true,
            [NAND_OP_READ_STATUS] = true,
            [NAND_OP_READ_STATUS_ENHANCED] = true,
        },
    [DIE_READ_DATA_XFER] =
        {
            [NAND_OP_RESET] = true,
            [NAND_OP_READ_STATUS] = true,
            [NAND_OP_READ_STATUS_ENHANCED] = true,
        },
    [DIE_PROG_SETUP] =
        {
            [NAND_OP_RESET] = true,
            [NAND_OP_READ_STATUS] = true,
            [NAND_OP_READ_STATUS_ENHANCED] = true,
        },
    [DIE_PROG_ARRAY_BUSY] =
        {
            [NAND_OP_RESET] = true,
            [NAND_OP_READ_STATUS] = true,
            [NAND_OP_READ_STATUS_ENHANCED] = true,
            [NAND_OP_PROG_SUSPEND] = true,
        },
    [DIE_ERASE_SETUP] =
        {
            [NAND_OP_RESET] = true,
            [NAND_OP_READ_STATUS] = true,
            [NAND_OP_READ_STATUS_ENHANCED] = true,
        },
    [DIE_ERASE_ARRAY_BUSY] =
        {
            [NAND_OP_RESET] = true,
            [NAND_OP_READ_STATUS] = true,
            [NAND_OP_READ_STATUS_ENHANCED] = true,
            [NAND_OP_ERASE_SUSPEND] = true,
        },
    [DIE_SUSPENDED_PROG] =
        {
            [NAND_OP_READ] = true,
            [NAND_OP_RESET] = true,
            [NAND_OP_READ_STATUS] = true,
            [NAND_OP_READ_STATUS_ENHANCED] = true,
            [NAND_OP_READ_ID] = true,
            [NAND_OP_READ_PARAM_PAGE] = true,
            [NAND_OP_PROG_RESUME] = true,
        },
    [DIE_SUSPENDED_ERASE] =
        {
            [NAND_OP_READ] = true,
            [NAND_OP_RESET] = true,
            [NAND_OP_READ_STATUS] = true,
            [NAND_OP_READ_STATUS_ENHANCED] = true,
            [NAND_OP_READ_ID] = true,
            [NAND_OP_READ_PARAM_PAGE] = true,
            [NAND_OP_ERASE_RESUME] = true,
        },
    [DIE_RESETTING] =
        {
            [NAND_OP_READ_STATUS] = true,
            [NAND_OP_READ_STATUS_ENHANCED] = true,
        },
};

bool nand_cmd_is_legal_in_state(enum nand_die_state state, enum nand_cmd_opcode op)
{
    if ((unsigned)state >= DIE_STATE_COUNT || (unsigned)op >= NAND_OP_COUNT) {
        return false;
    }
    return k_legal[state][op];
}

enum nand_die_state nand_cmd_next_state_on_accept(enum nand_die_state state, enum nand_cmd_opcode op)
{
    if (!nand_cmd_is_legal_in_state(state, op)) {
        return state;
    }
    switch (op) {
    case NAND_OP_READ:
        /* Read accepted during a suspended program/erase stays in the
         * suspend state — the read runs to completion on top of the
         * preserved suspension context and must not disturb it. The engine
         * drives its own transitions for the read path in that case. */
        if (state == DIE_SUSPENDED_PROG || state == DIE_SUSPENDED_ERASE) {
            return state;
        }
        return DIE_READ_SETUP;
    case NAND_OP_PROG:
        return DIE_PROG_SETUP;
    case NAND_OP_ERASE:
        return DIE_ERASE_SETUP;
    case NAND_OP_RESET:
        return DIE_RESETTING;
    case NAND_OP_PROG_SUSPEND:
        return DIE_SUSPENDED_PROG;
    case NAND_OP_ERASE_SUSPEND:
        return DIE_SUSPENDED_ERASE;
    case NAND_OP_PROG_RESUME:
        return DIE_PROG_ARRAY_BUSY;
    case NAND_OP_ERASE_RESUME:
        return DIE_ERASE_ARRAY_BUSY;
    default:
        return state;
    }
}

enum nand_die_state nand_cmd_next_state_on_complete(enum nand_die_state state, enum nand_cmd_opcode op)
{
    (void)op;
    switch (state) {
    case DIE_READ_SETUP:
        return DIE_READ_ARRAY_BUSY;
    case DIE_READ_ARRAY_BUSY:
        return DIE_READ_DATA_XFER;
    case DIE_READ_DATA_XFER:
        return DIE_IDLE;
    case DIE_PROG_SETUP:
        return DIE_PROG_ARRAY_BUSY;
    case DIE_PROG_ARRAY_BUSY:
        return DIE_IDLE;
    case DIE_ERASE_SETUP:
        return DIE_ERASE_ARRAY_BUSY;
    case DIE_ERASE_ARRAY_BUSY:
        return DIE_IDLE;
    case DIE_RESETTING:
        return DIE_IDLE;
    default:
        return state;
    }
}

enum nand_die_state nand_cmd_next_state_on_abort(enum nand_die_state state)
{
    (void)state;
    return DIE_RESETTING;
}
