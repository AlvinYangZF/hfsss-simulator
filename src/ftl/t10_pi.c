#include "ftl/t10_pi.h"

/* ------------------------------------------------------------------ */
/* pi_generate                                                         */
/* ------------------------------------------------------------------ */

int pi_generate(struct t10_pi_tuple *pi_out, const u8 *data, u32 data_len,
                u64 lba, enum pi_type type)
{
    if (!pi_out || !data || data_len == 0 || type == PI_TYPE_NONE) {
        return HFSSS_ERR_INVAL;
    }

    /* Guard tag: CRC-16 over the data block */
    pi_out->guard = t10_crc16(data, data_len);

    switch (type) {
    case PI_TYPE_1:
        /* Type 1: ref_tag = lower 32 bits of LBA */
        pi_out->ref_tag = (u32)(lba & 0xFFFFFFFF);
        pi_out->app_tag = 0;
        break;

    case PI_TYPE_2:
        /* Type 2: ref_tag = application-defined initial ref tag */
        pi_out->ref_tag = (u32)(lba & 0xFFFFFFFF);
        pi_out->app_tag = 0;
        break;

    case PI_TYPE_3:
        /* Type 3: no ref_tag check, set to 0xFFFFFFFF */
        pi_out->ref_tag = 0xFFFFFFFF;
        pi_out->app_tag = 0;
        break;

    default:
        return HFSSS_ERR_INVAL;
    }

    return HFSSS_OK;
}

/* ------------------------------------------------------------------ */
/* pi_verify                                                           */
/* ------------------------------------------------------------------ */

int pi_verify(const struct t10_pi_tuple *pi, const u8 *data, u32 data_len,
              u64 expected_lba, enum pi_type type)
{
    u16 computed_guard;

    if (!pi || !data || data_len == 0 || type == PI_TYPE_NONE) {
        return HFSSS_ERR_INVAL;
    }

    /* App tag 0xFFFF means "do not check" per T10 spec */
    if (pi->app_tag == 0xFFFF) {
        return HFSSS_OK;
    }

    /* Verify guard tag (CRC-16) */
    computed_guard = t10_crc16(data, data_len);
    if (computed_guard != pi->guard) {
        return HFSSS_ERR_PI_GUARD;
    }

    /* Verify reference tag based on PI type */
    switch (type) {
    case PI_TYPE_1:
        /* Type 1: ref_tag must match lower 32 bits of expected LBA */
        if (pi->ref_tag != (u32)(expected_lba & 0xFFFFFFFF)) {
            return HFSSS_ERR_PI_REFTAG;
        }
        break;

    case PI_TYPE_2:
        /* Type 2: ref_tag must match expected ref tag */
        if (pi->ref_tag != (u32)(expected_lba & 0xFFFFFFFF)) {
            return HFSSS_ERR_PI_REFTAG;
        }
        break;

    case PI_TYPE_3:
        /* Type 3: ref_tag is not checked (0xFFFFFFFF = skip) */
        break;

    default:
        return HFSSS_ERR_INVAL;
    }

    return HFSSS_OK;
}
