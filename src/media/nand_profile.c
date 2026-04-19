#include "media/nand_profile.h"
#include <string.h>

/*
 * Phase 6 generic profile table. Timing constants mirror the legacy tables in
 * timing.c so that legacy callers (nand_type-keyed init path) observe zero
 * drift when routed through a profile. Identity strings give each profile a
 * distinct on-the-wire persona so upper layers can tell them apart via
 * Read ID and Read Parameter Page.
 */

/*
 * Core opcodes both ONFI 3.5 and JEDEC Toggle standardize: page read,
 * page program, block erase, reset, classic status, read id, read
 * parameter page, and the suspend/resume + cache pipelines that both
 * specs formalize (under different wire encodings, same capability).
 */
static const u32 k_core_supported_ops =
    NAND_PROFILE_OP_BIT(NAND_OP_READ) | NAND_PROFILE_OP_BIT(NAND_OP_PROG) | NAND_PROFILE_OP_BIT(NAND_OP_ERASE)
    | NAND_PROFILE_OP_BIT(NAND_OP_RESET) | NAND_PROFILE_OP_BIT(NAND_OP_READ_STATUS)
    | NAND_PROFILE_OP_BIT(NAND_OP_READ_ID) | NAND_PROFILE_OP_BIT(NAND_OP_READ_PARAM_PAGE)
    | NAND_PROFILE_OP_BIT(NAND_OP_PROG_SUSPEND) | NAND_PROFILE_OP_BIT(NAND_OP_PROG_RESUME)
    | NAND_PROFILE_OP_BIT(NAND_OP_ERASE_SUSPEND) | NAND_PROFILE_OP_BIT(NAND_OP_ERASE_RESUME)
    | NAND_PROFILE_OP_BIT(NAND_OP_CACHE_READ) | NAND_PROFILE_OP_BIT(NAND_OP_CACHE_READ_END)
    | NAND_PROFILE_OP_BIT(NAND_OP_CACHE_PROG);

/*
 * ONFI 3.5 adds Read Status Enhanced (78h) on top of the core set.
 * ONFI 3.5 section 5.17 defines it as a status read that carries a
 * target address, letting a multi-LUN host disambiguate which target
 * a pending interrupt belongs to. Toggle does not standardize this.
 */
static const u32 k_onfi_supported_ops =
    k_core_supported_ops | NAND_PROFILE_OP_BIT(NAND_OP_READ_STATUS_ENHANCED);

/*
 * JEDEC Toggle supports the core opcodes but does not standardize
 * Read Status Enhanced; Toggle hosts use classic Read Status (70h)
 * and address target disambiguation via chip-enable. The bitmap
 * divergence here makes the engine reject READ_STATUS_ENHANCED on
 * Toggle profiles deterministically (via nand_cmd_is_legal_for_profile_state
 * in cmd_legality.c, which already consults supported_ops_bitmap).
 */
static const u32 k_toggle_supported_ops = k_core_supported_ops;

/*
 * Multi-plane rules per family. Both protocols describe a per-command
 * plane cap; the simulator diverges the cap so IS-09 can exercise the
 * profile-aware engine rejection path:
 *   - ONFI generic profiles carry a 4-plane cap, matching ONFI 3.5
 *     enterprise-class parts that publish 4 planes per die
 *   - Toggle generic profiles carry a 2-plane cap, matching the
 *     dual-plane baseline common in Toggle 2.x parts
 * plane_addr_mask and allow_cross_block stay at their Phase 6 defaults
 * (strict) — the struct nand_cmd_target in this simulator carries one
 * shared block and page across all planes, so there is no cross-block
 * multi-plane command surface to exercise relaxed-addressing rules.
 */
static const struct nand_profile_mp_rules k_onfi_mp_rules = {
    .allow_cross_block = false,
    .plane_addr_mask = 0x01,
    .max_planes_per_cmd = 4,
};

static const struct nand_profile_mp_rules k_toggle_mp_rules = {
    .allow_cross_block = false,
    .plane_addr_mask = 0x01,
    .max_planes_per_cmd = 2,
};

static const struct timing_params k_slc_timing = {
    .tCCS = 100,
    .tR = 25000,
    .tPROG = 200000,
    .tERS = 1500000,
    .tWC = 30,
    .tRC = 30,
    .tADL = 100,
    .tWB = 100,
    .tWHR = 60,
    .tRHW = 60,
    .tSSBSY = 5000,
    .tRSBSY = 5000,
    .tCBSY = 100000,
    .tDCBSYR1 = 15000,
};

static const struct timing_params k_mlc_timing = {
    .tCCS = 150,
    .tR = 50000,
    .tPROG = 600000,
    .tERS = 3000000,
    .tWC = 40,
    .tRC = 40,
    .tADL = 150,
    .tWB = 150,
    .tWHR = 80,
    .tRHW = 80,
    .tSSBSY = 10000,
    .tRSBSY = 10000,
    .tCBSY = 300000,
    .tDCBSYR1 = 30000,
};

static const struct tlc_timing k_tlc_timing = {
    .tR_LSB = 60000,
    .tR_CSB = 70000,
    .tR_MSB = 80000,
    .tPROG_LSB = 900000,
    .tPROG_CSB = 1100000,
    .tPROG_MSB = 1300000,
    .tSSBSY = 25000,
    .tRSBSY = 25000,
    .tCBSY = 500000,
    .tDCBSYR1 = 40000,
};

static const struct timing_params k_qlc_timing = {
    .tCCS = 200,
    .tR = 120000,
    .tPROG = 2000000,
    .tERS = 4500000,
    .tWC = 50,
    .tRC = 50,
    .tADL = 200,
    .tWB = 200,
    .tWHR = 100,
    .tRHW = 100,
    .tSSBSY = 50000,
    .tRSBSY = 50000,
    .tCBSY = 1000000,
    .tDCBSYR1 = 80000,
};

static const struct nand_profile k_profiles[NAND_PROFILE_COUNT] = {
    [NAND_PROFILE_GENERIC_ONFI_TLC] =
        {
            .id_string = "generic_onfi_enterprise_tlc",
            .id = NAND_PROFILE_GENERIC_ONFI_TLC,
            .interface_family = NAND_IF_ONFI,
            .nand_class = NAND_CLASS_ENTERPRISE_TLC,
            .identity =
                {
                    .manufacturer_id = NAND_ID_MFR_SIMULATOR,
                    .device_id = 0x30,
                    .manufacturer_name = "HFSSS ONFI ",
                    .device_model = "HFSSS-ONFI-TLC-ENT ",
                },
            .capability =
                {
                    .supported_ops_bitmap = k_onfi_supported_ops,
                    .bits_per_cell = 3,
                    .ecc_bits_required = 40,
                    .ecc_codeword_size = 1024,
                },
            .timing =
                {
                    .slc_params = k_slc_timing,
                    .mlc_params = k_mlc_timing,
                    .qlc_params = k_qlc_timing,
                    .tlc_timing = k_tlc_timing,
                },
            .mp_rules = k_onfi_mp_rules,
            .reset_policy =
                {
                    .abort_inflight_on_reset = true,
                    .preserve_partial_program = false,
                },
        },
    [NAND_PROFILE_GENERIC_ONFI_QLC] =
        {
            .id_string = "generic_onfi_enterprise_qlc",
            .id = NAND_PROFILE_GENERIC_ONFI_QLC,
            .interface_family = NAND_IF_ONFI,
            .nand_class = NAND_CLASS_ENTERPRISE_QLC,
            .identity =
                {
                    .manufacturer_id = NAND_ID_MFR_SIMULATOR,
                    .device_id = 0x40,
                    .manufacturer_name = "HFSSS ONFI ",
                    .device_model = "HFSSS-ONFI-QLC-ENT ",
                },
            .capability =
                {
                    .supported_ops_bitmap = k_onfi_supported_ops,
                    .bits_per_cell = 4,
                    .ecc_bits_required = 72,
                    .ecc_codeword_size = 1024,
                },
            .timing =
                {
                    .slc_params = k_slc_timing,
                    .mlc_params = k_mlc_timing,
                    .qlc_params = k_qlc_timing,
                    .tlc_timing = k_tlc_timing,
                },
            .mp_rules = k_onfi_mp_rules,
            .reset_policy =
                {
                    .abort_inflight_on_reset = true,
                    .preserve_partial_program = false,
                },
        },
    [NAND_PROFILE_GENERIC_TOGGLE_TLC] =
        {
            .id_string = "generic_toggle_enterprise_tlc",
            .id = NAND_PROFILE_GENERIC_TOGGLE_TLC,
            .interface_family = NAND_IF_TOGGLE_EQ,
            .nand_class = NAND_CLASS_ENTERPRISE_TLC,
            .identity =
                {
                    .manufacturer_id = NAND_ID_MFR_SIMULATOR,
                    .device_id = 0x31,
                    .manufacturer_name = "HFSSS TGL  ",
                    .device_model = "HFSSS-TGL-TLC-ENT  ",
                },
            .capability =
                {
                    .supported_ops_bitmap = k_toggle_supported_ops,
                    .bits_per_cell = 3,
                    .ecc_bits_required = 40,
                    .ecc_codeword_size = 1024,
                },
            .timing =
                {
                    .slc_params = k_slc_timing,
                    .mlc_params = k_mlc_timing,
                    .qlc_params = k_qlc_timing,
                    .tlc_timing = k_tlc_timing,
                },
            .mp_rules = k_toggle_mp_rules,
            .reset_policy =
                {
                    .abort_inflight_on_reset = true,
                    .preserve_partial_program = false,
                },
        },
    [NAND_PROFILE_GENERIC_TOGGLE_QLC] =
        {
            .id_string = "generic_toggle_enterprise_qlc",
            .id = NAND_PROFILE_GENERIC_TOGGLE_QLC,
            .interface_family = NAND_IF_TOGGLE_EQ,
            .nand_class = NAND_CLASS_ENTERPRISE_QLC,
            .identity =
                {
                    .manufacturer_id = NAND_ID_MFR_SIMULATOR,
                    .device_id = 0x41,
                    .manufacturer_name = "HFSSS TGL  ",
                    .device_model = "HFSSS-TGL-QLC-ENT  ",
                },
            .capability =
                {
                    .supported_ops_bitmap = k_toggle_supported_ops,
                    .bits_per_cell = 4,
                    .ecc_bits_required = 72,
                    .ecc_codeword_size = 1024,
                },
            .timing =
                {
                    .slc_params = k_slc_timing,
                    .mlc_params = k_mlc_timing,
                    .qlc_params = k_qlc_timing,
                    .tlc_timing = k_tlc_timing,
                },
            .mp_rules = k_toggle_mp_rules,
            .reset_policy =
                {
                    .abort_inflight_on_reset = true,
                    .preserve_partial_program = false,
                },
        },
};

const struct nand_profile *nand_profile_get(enum nand_profile_id id)
{
    if ((unsigned)id >= (unsigned)NAND_PROFILE_COUNT) {
        return NULL;
    }
    return &k_profiles[id];
}

const struct nand_profile *nand_profile_get_default_for_type(enum nand_type type)
{
    switch (type) {
    case NAND_TYPE_QLC:
        return &k_profiles[NAND_PROFILE_GENERIC_ONFI_QLC];
    case NAND_TYPE_TLC:
    case NAND_TYPE_SLC:
    case NAND_TYPE_MLC:
    default:
        return &k_profiles[NAND_PROFILE_GENERIC_ONFI_TLC];
    }
}

bool nand_profile_supports_op(const struct nand_profile *profile, enum nand_cmd_opcode op)
{
    if (!profile || (unsigned)op >= (unsigned)NAND_OP_COUNT) {
        return false;
    }
    return (profile->capability.supported_ops_bitmap & NAND_PROFILE_OP_BIT(op)) != 0;
}

/*
 * Short CLI aliases. The enum is the source of truth; the table here maps
 * user-facing names to ids without letting callers depend on enum integer
 * order. Keep aliases ASCII-only and hyphen-separated to match the
 * hfsss-nbd-server -P convention.
 */
struct nand_profile_alias {
    const char *name;
    enum nand_profile_id id;
};

static const struct nand_profile_alias k_profile_aliases[] = {
    {"onfi-tlc", NAND_PROFILE_GENERIC_ONFI_TLC},
    {"onfi-qlc", NAND_PROFILE_GENERIC_ONFI_QLC},
    {"toggle-tlc", NAND_PROFILE_GENERIC_TOGGLE_TLC},
    {"toggle-qlc", NAND_PROFILE_GENERIC_TOGGLE_QLC},
};

enum nand_profile_id nand_profile_id_from_name(const char *name)
{
    if (!name || !name[0]) {
        return NAND_PROFILE_COUNT;
    }
    for (size_t i = 0; i < sizeof(k_profile_aliases) / sizeof(k_profile_aliases[0]); ++i) {
        if (strcmp(name, k_profile_aliases[i].name) == 0) {
            return k_profile_aliases[i].id;
        }
    }
    return NAND_PROFILE_COUNT;
}
