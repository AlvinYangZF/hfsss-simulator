#include "media/nand_identity.h"

#include "common/common.h"
#include "media/cmd_state.h"
#include "media/media.h"
#include "media/nand.h"
#include "media/nand_profile.h"
#include "media/timing.h"

#include <stddef.h>
#include <string.h>

/*
 * CRC-16-CCITT (polynomial 0x1021, init 0xFFFF). Small enough to keep
 * self-contained; avoids pulling in a dedicated CRC translation unit for a
 * single call site.
 */
static u16 crc16_ccitt(const void *buf, size_t len)
{
    const u8 *p = (const u8 *)buf;
    u16 crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (u16)p[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) {
                crc = (u16)((crc << 1) ^ 0x1021);
            } else {
                crc = (u16)(crc << 1);
            }
        }
    }
    return crc;
}

static u8 cell_type_from_nand_type(enum nand_type t)
{
    switch (t) {
    case NAND_TYPE_SLC:
        return NAND_ID_CELL_TYPE_SLC;
    case NAND_TYPE_MLC:
        return NAND_ID_CELL_TYPE_MLC;
    case NAND_TYPE_TLC:
        return NAND_ID_CELL_TYPE_TLC;
    case NAND_TYPE_QLC:
        return NAND_ID_CELL_TYPE_QLC;
    default:
        return NAND_ID_CELL_TYPE_TLC;
    }
}

static u8 bits_per_cell_from_nand_type(enum nand_type t)
{
    switch (t) {
    case NAND_TYPE_SLC:
        return 1;
    case NAND_TYPE_MLC:
        return 2;
    case NAND_TYPE_TLC:
        return 3;
    case NAND_TYPE_QLC:
        return 4;
    default:
        return 3;
    }
}

static u16 ecc_bits_from_nand_type(enum nand_type t)
{
    switch (t) {
    case NAND_TYPE_SLC:
        return 1;
    case NAND_TYPE_MLC:
        return 8;
    case NAND_TYPE_TLC:
        return 60;
    case NAND_TYPE_QLC:
        return 120;
    default:
        return 60;
    }
}

static u8 log2_u32(u32 v)
{
    u8 n = 0;
    while (v > 1) {
        v >>= 1;
        n++;
    }
    return n;
}

static u8 encode_field_log2(u32 value, u8 base_log2, u8 max_code)
{
    if (value == 0) {
        return 0;
    }
    u8 lg = log2_u32(value);
    if (lg < base_log2) {
        return 0;
    }
    u8 code = (u8)(lg - base_log2);
    if (code > max_code) {
        code = max_code;
    }
    return code;
}

/*
 * Pack the 8-byte ID per ONFI 4.2 Table 37. The simulator supports enterprise
 * TLC/QLC geometries whose page/block sizes commonly exceed the 2-bit encoded
 * values defined by ONFI 4.2. Where that happens we clamp to the maximum legal
 * code and mirror the true value in the parameter page. When a profile is
 * provided, manufacturer_id and device_id come from the profile so two
 * profiles sharing the same nand_type still expose distinct identities.
 */
static void build_nand_id(struct nand_id *id, const struct media_config *cfg, const struct nand_profile *profile)
{
    memset(id, 0, sizeof(*id));
    if (profile) {
        id->bytes[0] = profile->identity.manufacturer_id;
    } else {
        id->bytes[0] = NAND_ID_MFR_SIMULATOR;
    }

    u8 cell_type = cell_type_from_nand_type(cfg->nand_type);
    u8 device_code = profile ? profile->identity.device_id : (u8)(0x10 | (cell_type & 0x03));
    id->bytes[1] = device_code;

    u32 dies_per_ce = cfg->chips_per_channel * cfg->dies_per_chip;
    u8 chip_number_code = encode_field_log2(dies_per_ce, 0, 0x3);
    u8 cache_support = 0;
    u8 interleave_support = cfg->enable_die_interleaving ? 1 : 0;
    u8 simultaneous_pages = encode_field_log2(cfg->planes_per_die, 0, 0x3);
    id->bytes[2] = (u8)((chip_number_code << 6) | ((cell_type & 0x3) << 4) | ((simultaneous_pages & 0x3) << 2) |
                        ((interleave_support & 0x1) << 1) | (cache_support & 0x1));

    u8 page_size_code = encode_field_log2(cfg->page_size, 10, 0x3);
    u8 spare_ratio_code = 0;
    if (cfg->page_size > 0) {
        u32 spare_per_512 = (cfg->spare_size * 512u) / cfg->page_size;
        if (spare_per_512 >= 24) {
            spare_ratio_code = 0x2;
        } else if (spare_per_512 >= 16) {
            spare_ratio_code = 0x1;
        } else {
            spare_ratio_code = 0x0;
        }
    }
    u32 block_bytes = cfg->page_size * cfg->pages_per_block;
    u8 block_size_code = encode_field_log2(block_bytes, 16, 0x3);
    id->bytes[3] =
        (u8)(((page_size_code & 0x3) << 6) | ((spare_ratio_code & 0x3) << 4) | ((block_size_code & 0x3) << 0));

    u8 plane_number_code = encode_field_log2(cfg->planes_per_die, 0, 0x3);
    u8 plane_size_code = encode_field_log2(cfg->blocks_per_plane, 0, 0x7);
    id->bytes[4] = (u8)(((plane_number_code & 0x3) << 6) | ((plane_size_code & 0x7) << 3));
}

static void build_parameter_page(struct nand_parameter_page *pp, const struct media_config *cfg,
                                 const struct nand_id *id, struct timing_model *timing,
                                 const struct nand_profile *profile)
{
    memset(pp, 0, sizeof(*pp));

    pp->revision = NAND_PARAMETER_PAGE_REVISION;
    pp->size_bytes = (u16)sizeof(*pp);

    pp->manufacturer_id = id->bytes[0];
    pp->device_id = id->bytes[1];

    if (profile) {
        memcpy(pp->manufacturer_name, profile->identity.manufacturer_name, NAND_PARAMETER_PAGE_MFR_NAME_LEN);
        memcpy(pp->device_model, profile->identity.device_model, NAND_PARAMETER_PAGE_MODEL_LEN);
    } else {
        memcpy(pp->manufacturer_name, "HFSSS-SIM   ", NAND_PARAMETER_PAGE_MFR_NAME_LEN);
        const char *model = "GENERIC-NAND-0001   ";
        switch (cfg->nand_type) {
        case NAND_TYPE_SLC:
            model = "GENERIC-SLC-0001    ";
            break;
        case NAND_TYPE_MLC:
            model = "GENERIC-MLC-0001    ";
            break;
        case NAND_TYPE_TLC:
            model = "GENERIC-TLC-0001    ";
            break;
        case NAND_TYPE_QLC:
            model = "GENERIC-QLC-0001    ";
            break;
        default:
            break;
        }
        memcpy(pp->device_model, model, NAND_PARAMETER_PAGE_MODEL_LEN);
    }

    pp->bytes_per_page = cfg->page_size;
    pp->spare_bytes_per_page = (u16)cfg->spare_size;
    pp->pages_per_block = cfg->pages_per_block;
    pp->blocks_per_lun = cfg->blocks_per_plane * cfg->planes_per_die;
    pp->luns_per_ce = (u8)(cfg->dies_per_chip);
    pp->planes_per_die = (u8)cfg->planes_per_die;
    pp->bits_per_cell =
        profile ? profile->capability.bits_per_cell : bits_per_cell_from_nand_type(cfg->nand_type);

    /* Row address space spans pages × blocks × LUNs so the advertised cycle
     * count can address every page in the CE. Use u64 to avoid overflow on
     * enterprise TLC/QLC geometries. */
    u32 luns = pp->luns_per_ce ? pp->luns_per_ce : 1;
    u64 row_addresses = (u64)pp->pages_per_block * (u64)pp->blocks_per_lun * (u64)luns;
    u8 row_bits = 0;
    u64 tmp = row_addresses ? row_addresses : 1;
    while (tmp > 1) {
        tmp >>= 1;
        row_bits++;
    }
    pp->addr_cycles_row = (u8)((row_bits + 7) / 8);
    if (pp->addr_cycles_row == 0) {
        pp->addr_cycles_row = 1;
    }

    u32 col_addresses = cfg->page_size + cfg->spare_size;
    u8 col_bits = log2_u32(col_addresses ? col_addresses : 1);
    pp->addr_cycles_col = (u8)((col_bits + 7) / 8);
    if (pp->addr_cycles_col == 0) {
        pp->addr_cycles_col = 1;
    }

    if (profile) {
        pp->ecc_bits_required = profile->capability.ecc_bits_required;
        pp->ecc_codeword_size = profile->capability.ecc_codeword_size;
        pp->supported_cmd_bitmap = profile->capability.supported_ops_bitmap;
    } else {
        pp->ecc_bits_required = ecc_bits_from_nand_type(cfg->nand_type);
        pp->ecc_codeword_size = 1024;
        pp->supported_cmd_bitmap =
            (1u << NAND_OP_READ) | (1u << NAND_OP_PROG) | (1u << NAND_OP_ERASE) | (1u << NAND_OP_RESET) |
            (1u << NAND_OP_READ_STATUS) | (1u << NAND_OP_READ_STATUS_ENHANCED) | (1u << NAND_OP_READ_ID) |
            (1u << NAND_OP_READ_PARAM_PAGE) | (1u << NAND_OP_PROG_SUSPEND) | (1u << NAND_OP_PROG_RESUME) |
            (1u << NAND_OP_ERASE_SUSPEND) | (1u << NAND_OP_ERASE_RESUME) | (1u << NAND_OP_CACHE_READ) |
            (1u << NAND_OP_CACHE_READ_END) | (1u << NAND_OP_CACHE_PROG);
    }

    if (timing) {
        pp->tR_ns = timing_get_read_latency(timing, 0);
        pp->tPROG_ns = timing_get_prog_latency(timing, 0);
        pp->tBERS_ns = timing_get_erase_latency(timing);
    }

    pp->crc = crc16_ccitt(pp, offsetof(struct nand_parameter_page, crc));
}

void nand_identity_build_from_profile(struct nand_device *dev, const struct nand_profile *profile,
                                      const struct media_config *cfg)
{
    if (!dev || !cfg) {
        return;
    }
    build_nand_id(&dev->nand_id, cfg, profile);
    build_parameter_page(&dev->param_page, cfg, &dev->nand_id, dev->timing, profile);
}

void nand_identity_build_from_config(struct nand_device *dev, const struct media_config *cfg)
{
    if (!dev || !cfg) {
        return;
    }
    nand_identity_build_from_profile(dev, dev->profile, cfg);
}

void nand_status_byte_from_cmd_state(const struct nand_die_cmd_state *s, u8 *out)
{
    if (!out) {
        return;
    }
    u8 value = NAND_STATUS_WP_N;
    if (!s) {
        *out = (u8)(value | NAND_STATUS_RDY | NAND_STATUS_ARDY);
        return;
    }

    bool array_busy = false;
    bool suspended = false;
    switch (s->state) {
    case DIE_IDLE:
        break;
    case DIE_SUSPENDED_PROG:
    case DIE_SUSPENDED_ERASE:
        suspended = true;
        break;
    case DIE_RESETTING:
    case DIE_READ_SETUP:
    case DIE_READ_ARRAY_BUSY:
    case DIE_READ_DATA_XFER:
    case DIE_PROG_SETUP:
    case DIE_PROG_ARRAY_BUSY:
    case DIE_ERASE_SETUP:
    case DIE_ERASE_ARRAY_BUSY:
    default:
        array_busy = true;
        break;
    }

    if (!array_busy && !suspended) {
        value |= NAND_STATUS_ARDY;
    }
    if (!array_busy || suspended) {
        value |= NAND_STATUS_RDY;
    }

    /* ONFI 4.2 FAIL semantics: the latch is sticky across intervening
     * non-write operations and is cleared only by the next successful
     * PROG/ERASE completion or by RESET. The engine sets/clears
     * latched_fail at PROG/ERASE completion and on RESET. */
    if (s->latched_fail) {
        value |= NAND_STATUS_FAIL;
    }

    *out = value;
}

void nand_status_enhanced_from_cmd_state(const struct nand_die_cmd_state *s, struct nand_status_enhanced *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    if (!s) {
        out->ready = true;
        out->array_ready = true;
        out->write_protect_off = true;
        out->state = DIE_IDLE;
        out->phase = CMD_PHASE_NONE;
        nand_status_byte_from_cmd_state(NULL, &out->classic_status);
        return;
    }

    out->state = s->state;
    out->phase = s->phase;
    out->last_opcode = s->opcode;
    out->suspend_count = s->suspend_count;
    out->start_ts_ns = s->start_ts_ns;
    out->phase_start_ts_ns = s->phase_start_ts_ns;
    out->total_budget_ns = s->total_budget_ns;
    out->remaining_ns = s->remaining_ns;
    out->result_status = s->result_status;
    out->write_protect_off = true;

    out->suspended_program = (s->state == DIE_SUSPENDED_PROG);
    out->suspended_erase = (s->state == DIE_SUSPENDED_ERASE);
    out->resetting = (s->state == DIE_RESETTING);

    bool array_busy = false;
    switch (s->state) {
    case DIE_IDLE:
    case DIE_SUSPENDED_PROG:
    case DIE_SUSPENDED_ERASE:
        break;
    default:
        array_busy = true;
        break;
    }
    bool suspended = out->suspended_program || out->suspended_erase;
    out->array_ready = !array_busy && !suspended;
    out->ready = !array_busy || suspended;

    out->last_op_failed = s->latched_fail;

    nand_status_byte_from_cmd_state(s, &out->classic_status);
}
