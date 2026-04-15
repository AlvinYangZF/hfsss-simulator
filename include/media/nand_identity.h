#ifndef __HFSSS_NAND_IDENTITY_H
#define __HFSSS_NAND_IDENTITY_H

#include "common/common.h"
#include "media/cmd_state.h"

struct media_config;
struct nand_device;
struct nand_die_cmd_state;

/*
 * 8-byte NAND ID. Bytes [0..4] follow ONFI 4.2 Table 37 "Read ID Parameters".
 * Bytes [5..7] are reserved for Toggle-extended fields and stay zero in the
 * current phase.
 *
 * Byte 0: manufacturer code (simulator pseudo-vendor)
 * Byte 1: device code derived from nand_type + geometry
 * Byte 2: [7:6] chip_number | [5:4] cell_type | [3:2] simultaneous_pages |
 *         [1] interleave_support | [0] cache_support
 * Byte 3: [7:6] page_size | [5:4] spare_ratio | [3:2] reserved | [1:0] block_size
 * Byte 4: [7:6] plane_number | [5:3] plane_size | [2:0] reserved
 * Bytes 5..7: 0x00 (Toggle-extended reserved)
 */
struct nand_id {
    u8 bytes[8];
};

#define NAND_ID_MFR_SIMULATOR 0xFE

/*
 * ONFI 4.2 cell type encoding, Table 37 byte 2 bits [5:4].
 */
#define NAND_ID_CELL_TYPE_SLC 0x0
#define NAND_ID_CELL_TYPE_MLC 0x1
#define NAND_ID_CELL_TYPE_TLC 0x2
#define NAND_ID_CELL_TYPE_QLC 0x3

/*
 * Parameter page minimum contract from NAND/media design section
 * "Read Parameter Page Minimum Contract". Fields outside this minimum are
 * intentionally omitted in the current phase.
 *
 * The leading revision/size_bytes pair lets callers detect struct growth in
 * later phases without an ABI break.
 */
#define NAND_PARAMETER_PAGE_REVISION 1
#define NAND_PARAMETER_PAGE_MFR_NAME_LEN 12
#define NAND_PARAMETER_PAGE_MODEL_LEN 20

struct nand_parameter_page {
    u16 revision;
    u16 size_bytes;

    /* Identity */
    u8 manufacturer_id;
    u8 device_id;
    char manufacturer_name[NAND_PARAMETER_PAGE_MFR_NAME_LEN];
    char device_model[NAND_PARAMETER_PAGE_MODEL_LEN];

    /* Geometry */
    u32 bytes_per_page;
    u16 spare_bytes_per_page;
    u32 pages_per_block;
    u32 blocks_per_lun;
    u8 luns_per_ce;
    u8 planes_per_die;
    u8 bits_per_cell;

    /* Addressing */
    u8 addr_cycles_row;
    u8 addr_cycles_col;

    /* ECC advertisement */
    u16 ecc_bits_required;
    u16 ecc_codeword_size;

    /* Supported command bitmap — bit index matches enum nand_cmd_opcode */
    u32 supported_cmd_bitmap;

    /* Timing advertisement */
    u64 tR_ns;
    u64 tPROG_ns;
    u64 tBERS_ns;

    /* Integrity */
    u16 crc;
    u8 reserved[8];
};

/*
 * Classic 1-byte status register. Bit layout matches ONFI 4.2 Status Register:
 *   bit 0 = FAIL   (last PROG/ERASE failed when set)
 *   bit 1 = FAILC  (cache/previous op failed)
 *   bit 5 = ARDY   (array ready; 1 = array not busy)
 *   bit 6 = RDY    (device ready; 1 = ready for next command)
 *   bit 7 = WP_N   (write protect not asserted; 1 = writable)
 */
struct nand_status_byte {
    u8 value;
};

#define NAND_STATUS_FAIL (1u << 0)
#define NAND_STATUS_FAILC (1u << 1)
#define NAND_STATUS_ARDY (1u << 5)
#define NAND_STATUS_RDY (1u << 6)
#define NAND_STATUS_WP_N (1u << 7)

/*
 * Decoded structured status. Pure projection of a nand_die_cmd_state snapshot
 * plus the classic status byte. No hidden locking.
 */
struct nand_status_enhanced {
    bool ready;
    bool array_ready;
    bool write_protect_off;
    bool last_op_failed;
    bool suspended_program;
    bool suspended_erase;
    bool resetting;

    enum nand_die_state state;
    enum nand_cmd_phase phase;
    enum nand_cmd_opcode last_opcode;
    u32 suspend_count;
    u64 start_ts_ns;
    u64 phase_start_ts_ns;
    u64 total_budget_ns;
    u64 remaining_ns;
    int result_status;

    u8 classic_status;
};

struct nand_profile;

/*
 * Build the device-level identity and parameter page from the active config.
 * Pure function; performs no locking, no allocation, no I/O. When dev->profile
 * is non-NULL the identity bytes (manufacturer, model, supported_cmd_bitmap,
 * bits_per_cell, ECC contract) come from the profile; geometry fields always
 * come from cfg. Existing zero-initialized callers without a profile fall
 * back to the legacy nand_type-derived defaults.
 */
void nand_identity_build_from_config(struct nand_device *dev, const struct media_config *cfg);

/*
 * Profile-aware variant exposed for callers that hold an explicit profile
 * pointer. The shim above is preserved for back-compat and routes through
 * dev->profile when set.
 */
void nand_identity_build_from_profile(struct nand_device *dev, const struct nand_profile *profile,
                                      const struct media_config *cfg);

/*
 * Decode helpers over a previously taken snapshot. Safe to call without locks.
 */
void nand_status_byte_from_cmd_state(const struct nand_die_cmd_state *s, u8 *out);
void nand_status_enhanced_from_cmd_state(const struct nand_die_cmd_state *s, struct nand_status_enhanced *out);

#endif /* __HFSSS_NAND_IDENTITY_H */
