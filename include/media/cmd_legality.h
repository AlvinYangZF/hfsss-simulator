#ifndef __HFSSS_NAND_CMD_LEGALITY_H
#define __HFSSS_NAND_CMD_LEGALITY_H

#include "common/common.h"
#include "media/cmd_state.h"

bool nand_cmd_is_legal_in_state(enum nand_die_state state, enum nand_cmd_opcode op);

enum nand_die_state nand_cmd_next_state_on_accept(enum nand_die_state state, enum nand_cmd_opcode op);

enum nand_die_state nand_cmd_next_state_on_complete(enum nand_die_state state, enum nand_cmd_opcode op);

enum nand_die_state nand_cmd_next_state_on_abort(enum nand_die_state state);

#endif /* __HFSSS_NAND_CMD_LEGALITY_H */
