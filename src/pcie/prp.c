#include "pcie/prp.h"
#include "common/common.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

int prp_build_list(uint64_t prp1, uint64_t prp2,
                   uint32_t xfer_len, uint32_t page_size,
                   struct prp_list *out)
{
    if (!out || page_size == 0)
        return HFSSS_ERR_INVAL;

    uint64_t page_mask = (uint64_t)(page_size - 1);
    uint64_t offset1 = prp1 & page_mask;
    uint64_t remain_in_page1 = page_size - offset1;

    memset(out, 0, sizeof(*out));

    if (xfer_len == 0) {
        out->nseg = 0;
        out->total_len = 0;
        return HFSSS_OK;
    }

    if ((uint64_t)xfer_len <= remain_in_page1) {
        /* PRP1 only */
        out->segs[0].addr = prp1;
        out->segs[0].len  = xfer_len;
        out->nseg = 1;
        out->total_len = xfer_len;
        return HFSSS_OK;
    }

    if ((uint64_t)xfer_len <= remain_in_page1 + page_size) {
        /* PRP1 + PRP2 direct: PRP2 is a data page, not a list */
        out->segs[0].addr = prp1;
        out->segs[0].len  = (uint32_t)remain_in_page1;
        out->segs[1].addr = prp2;
        out->segs[1].len  = xfer_len - (uint32_t)remain_in_page1;
        out->nseg = 2;
        out->total_len = xfer_len;
        return HFSSS_OK;
    }

    /* PRP1 + PRP2 is a list pointer */
    out->segs[0].addr = prp1;
    out->segs[0].len  = (uint32_t)remain_in_page1;
    out->nseg = 1;
    out->total_len = remain_in_page1;

    uint32_t remaining = xfer_len - (uint32_t)remain_in_page1;
    uint64_t *list_ptr = (uint64_t *)(uintptr_t)prp2;
    uint32_t list_idx = 0;

    while (remaining > 0) {
        if (out->nseg >= PRP_MAX_SEGMENTS)
            return HFSSS_ERR_INVAL;

        uint64_t entry = list_ptr[list_idx++];
        uint32_t chunk = (remaining < page_size) ? remaining : page_size;

        out->segs[out->nseg].addr = entry;
        out->segs[out->nseg].len  = chunk;
        out->nseg++;
        out->total_len += chunk;
        remaining -= chunk;
    }

    return HFSSS_OK;
}

int prp_copy_from_host(const struct prp_list *pl,
                        void *flat_buf, uint32_t flat_len)
{
    if (!pl || !flat_buf)
        return HFSSS_ERR_INVAL;

    if ((uint64_t)flat_len != pl->total_len)
        return HFSSS_ERR_INVAL;

    uint32_t offset = 0;
    for (uint32_t i = 0; i < pl->nseg; i++) {
        void *src = (void *)(uintptr_t)pl->segs[i].addr;
        uint32_t len = pl->segs[i].len;
        memcpy((char *)flat_buf + offset, src, len);
        offset += len;
    }

    return HFSSS_OK;
}

int prp_copy_to_host(const struct prp_list *pl,
                      const void *flat_buf, uint32_t flat_len)
{
    if (!pl || !flat_buf)
        return HFSSS_ERR_INVAL;

    if ((uint64_t)flat_len != pl->total_len)
        return HFSSS_ERR_INVAL;

    uint32_t offset = 0;
    for (uint32_t i = 0; i < pl->nseg; i++) {
        void *dst = (void *)(uintptr_t)pl->segs[i].addr;
        uint32_t len = pl->segs[i].len;
        memcpy(dst, (const char *)flat_buf + offset, len);
        offset += len;
    }

    return HFSSS_OK;
}

bool prp_validate(const struct prp_list *pl, uint32_t page_size)
{
    if (!pl || page_size == 0)
        return false;

    uint64_t page_mask = (uint64_t)(page_size - 1);

    /* All segments except the first must be page-aligned */
    for (uint32_t i = 1; i < pl->nseg; i++) {
        if (pl->segs[i].addr & page_mask)
            return false;
    }

    return true;
}
