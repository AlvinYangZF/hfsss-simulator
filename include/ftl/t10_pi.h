#ifndef __HFSSS_T10_PI_H
#define __HFSSS_T10_PI_H

#include "common/common.h"

/*
 * T10 Protection Information (DIF) support.
 *
 * Implements PI Type 1/2/3 per T10 SBC-4 and NVMe specification.
 * Guard tag uses CRC-16 (T10 DIF polynomial 0x8BB7).
 */

/* PI types per NVMe/T10 specification */
enum pi_type {
    PI_TYPE_NONE = 0,
    PI_TYPE_1    = 1,   /* Guard + App Tag + Ref Tag (LBA-based) */
    PI_TYPE_2    = 2,   /* Guard + App Tag + Ref Tag (app-defined) */
    PI_TYPE_3    = 3,   /* Guard + App Tag only (no ref tag check) */
};

/* Protection Information tuple (8 bytes) */
struct t10_pi_tuple {
    u16 guard;          /* CRC-16 of the data block */
    u16 app_tag;        /* Application tag */
    u32 ref_tag;        /* Reference tag */
};

/* Per-namespace PI configuration */
struct ns_pi_config {
    enum pi_type type;
    bool enabled;
    u16  guard_seed;    /* Initial CRC seed (typically 0) */
};

/*
 * pi_generate - Generate protection information for a data block.
 *
 * @pi_out:   Output PI tuple.
 * @data:     Data block to protect.
 * @data_len: Length of data block in bytes.
 * @lba:      Logical block address (used for ref_tag in Type 1/2).
 * @type:     PI type to generate.
 *
 * Returns HFSSS_OK on success.
 */
int pi_generate(struct t10_pi_tuple *pi_out, const u8 *data, u32 data_len,
                u64 lba, enum pi_type type);

/*
 * pi_verify - Verify protection information for a data block.
 *
 * @pi:           PI tuple to verify.
 * @data:         Data block to verify against.
 * @data_len:     Length of data block in bytes.
 * @expected_lba: Expected LBA (for ref_tag verification in Type 1/2).
 * @type:         PI type to verify.
 *
 * Returns HFSSS_OK on success, HFSSS_ERR_PI_GUARD / HFSSS_ERR_PI_REFTAG /
 * HFSSS_ERR_PI_APPTAG on verification failure.
 */
int pi_verify(const struct t10_pi_tuple *pi, const u8 *data, u32 data_len,
              u64 expected_lba, enum pi_type type);

#endif /* __HFSSS_T10_PI_H */
