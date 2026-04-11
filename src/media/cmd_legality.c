#include "media/cmd_legality.h"

/*
 * Legality matrix — rows are current die state, columns are incoming opcode.
 * 'true' means the engine accepts the submission in that state. RESET is
 * always accepted except when the die is already resetting.
 */
static const bool k_legal[DIE_STATE_COUNT][NAND_OP_COUNT] = {
    [DIE_IDLE] =
        {
            [NAND_OP_READ] = true,
            [NAND_OP_PROG] = true,
            [NAND_OP_ERASE] = true,
            [NAND_OP_RESET] = true,
        },
    [DIE_READ_SETUP] = {[NAND_OP_RESET] = true},
    [DIE_READ_ARRAY_BUSY] = {[NAND_OP_RESET] = true},
    [DIE_READ_DATA_XFER] = {[NAND_OP_RESET] = true},
    [DIE_PROG_SETUP] = {[NAND_OP_RESET] = true},
    [DIE_PROG_ARRAY_BUSY] = {[NAND_OP_RESET] = true},
    [DIE_ERASE_SETUP] = {[NAND_OP_RESET] = true},
    [DIE_ERASE_ARRAY_BUSY] = {[NAND_OP_RESET] = true},
    [DIE_SUSPENDED_PROG] = {[NAND_OP_RESET] = true},
    [DIE_SUSPENDED_ERASE] = {[NAND_OP_RESET] = true},
    [DIE_RESETTING] = {0},
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
        return DIE_READ_SETUP;
    case NAND_OP_PROG:
        return DIE_PROG_SETUP;
    case NAND_OP_ERASE:
        return DIE_ERASE_SETUP;
    case NAND_OP_RESET:
        return DIE_RESETTING;
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
