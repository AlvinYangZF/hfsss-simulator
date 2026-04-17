#ifndef __HFSSS_NAND_CMD_LEGALITY_H
#define __HFSSS_NAND_CMD_LEGALITY_H

#include "common/common.h"
#include "media/cmd_state.h"

struct nand_profile;

bool nand_cmd_is_legal_in_state(enum nand_die_state state, enum nand_cmd_opcode op);

/*
 * Profile-aware legality gate. Rejects any op outside the profile's supported
 * bitmap regardless of state, then delegates to the state-machine matrix. A
 * NULL profile is treated as "no profile gating" and falls through to the
 * pure state check, so transitional callers keep working.
 */
bool nand_cmd_is_legal_for_profile_state(const struct nand_profile *profile, enum nand_die_state state,
                                         enum nand_cmd_opcode op);

enum nand_die_state nand_cmd_next_state_on_accept(enum nand_die_state state, enum nand_cmd_opcode op);

enum nand_die_state nand_cmd_next_state_on_complete(enum nand_die_state state, enum nand_cmd_opcode op);

enum nand_die_state nand_cmd_next_state_on_abort(enum nand_die_state state);

#endif /* __HFSSS_NAND_CMD_LEGALITY_H */
