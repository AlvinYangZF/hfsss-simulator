#ifndef __HFSSS_NAND_PROFILE_H
#define __HFSSS_NAND_PROFILE_H

#include "common/common.h"
#include "media/cmd_state.h"
#include "media/nand_identity.h"
#include "media/timing.h"

/*
 * Profile-driven NAND capability model (Phase 6 of the NAND command-coverage
 * design). A profile is the authoritative source for identity, capability,
 * timing, multi-plane rules and reset policy. Geometry and topology live in
 * struct media_config and are NOT duplicated here.
 *
 * Phase 6 delivers four compile-time generic profiles that differ only in
 * interface_family and identity strings; Toggle vs ONFI do not diverge in
 * supported_ops_bitmap yet — that expansion is scheduled for Phase 7.
 */

enum nand_interface_family {
    NAND_IF_ONFI = 0,
    NAND_IF_TOGGLE_EQ,
};

enum nand_class {
    NAND_CLASS_ENTERPRISE_TLC = 0,
    NAND_CLASS_ENTERPRISE_QLC,
};

enum nand_profile_id {
    NAND_PROFILE_GENERIC_ONFI_TLC = 0,
    NAND_PROFILE_GENERIC_ONFI_QLC,
    NAND_PROFILE_GENERIC_TOGGLE_TLC,
    NAND_PROFILE_GENERIC_TOGGLE_QLC,
    NAND_PROFILE_COUNT,
};

struct nand_profile_identity {
    u8 manufacturer_id;
    u8 device_id;
    char manufacturer_name[NAND_PARAMETER_PAGE_MFR_NAME_LEN];
    char device_model[NAND_PARAMETER_PAGE_MODEL_LEN];
};

struct nand_profile_capability {
    u32 supported_ops_bitmap;
    u8 bits_per_cell;
    u16 ecc_bits_required;
    u16 ecc_codeword_size;
};

struct nand_profile_timing {
    struct timing_params slc_params;
    struct timing_params mlc_params;
    struct timing_params qlc_params;
    struct tlc_timing tlc_timing;
};

struct nand_profile_mp_rules {
    bool allow_cross_block;
    u32 plane_addr_mask;
    u8 max_planes_per_cmd;
};

struct nand_profile_reset_policy {
    bool abort_inflight_on_reset;
    bool preserve_partial_program;
};

struct nand_profile {
    const char *id_string;
    enum nand_profile_id id;
    enum nand_interface_family interface_family;
    enum nand_class nand_class;
    struct nand_profile_identity identity;
    struct nand_profile_capability capability;
    struct nand_profile_timing timing;
    struct nand_profile_mp_rules mp_rules;
    struct nand_profile_reset_policy reset_policy;
};

#define NAND_PROFILE_OP_BIT(op) (1u << (u32)(op))

const struct nand_profile *nand_profile_get(enum nand_profile_id id);
const struct nand_profile *nand_profile_get_default_for_type(enum nand_type type);
bool nand_profile_supports_op(const struct nand_profile *profile, enum nand_cmd_opcode op);

/*
 * Map a stable short CLI name (onfi-tlc, onfi-qlc, toggle-tlc, toggle-qlc)
 * to its enum id. Returns NAND_PROFILE_COUNT when the name is NULL, empty,
 * or not a recognized alias. Callers treat the count sentinel as "invalid".
 */
enum nand_profile_id nand_profile_id_from_name(const char *name);

#endif /* __HFSSS_NAND_PROFILE_H */
