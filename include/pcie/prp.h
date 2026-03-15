#ifndef HFSSS_PRP_H
#define HFSSS_PRP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define NVME_PAGE_SIZE    4096u
#define NVME_PAGE_MASK    (NVME_PAGE_SIZE - 1)
#define NVME_PAGE_SHIFT   12

/* Maximum scatter-gather segments supported in a PRP list */
#define PRP_MAX_SEGMENTS  256

/* Describes one contiguous memory region extracted from PRP traversal */
struct prp_segment {
    uint64_t addr;   /* base address */
    uint32_t len;    /* length in bytes */
};

/* Result of PRP traversal */
struct prp_list {
    struct prp_segment segs[PRP_MAX_SEGMENTS];
    uint32_t           nseg;      /* number of valid segments */
    uint64_t           total_len; /* sum of segment lengths */
};

/*
 * Build a scatter-gather list from NVMe PRP1/PRP2 dptr fields.
 *
 * prp1        — first PRP entry (always points to data)
 * prp2        — second PRP entry (points to data page OR a PRP list)
 * xfer_len    — total transfer length in bytes
 * page_size   — system page size (typically NVME_PAGE_SIZE)
 * out         — output PRP list (caller-allocated)
 *
 * Rules:
 *   - If xfer_len <= (page_size - (prp1 & page_mask)): PRP1 only
 *   - If xfer_len fits in PRP1 + one PRP2 page: PRP1 + PRP2 direct
 *   - Otherwise: PRP1 + PRP2 points to a PRP list array of uint64_t entries
 *
 * In simulation, PRP entries are virtual addresses and the PRP list
 * array is read via the pointer directly (no DMA/IOMMU needed).
 *
 * Returns HFSSS_OK on success, HFSSS_ERR_INVAL on misalignment or overflow.
 */
int prp_build_list(uint64_t prp1, uint64_t prp2,
                   uint32_t xfer_len, uint32_t page_size,
                   struct prp_list *out);

/*
 * Copy data from host memory described by a PRP list into a flat buffer.
 * Used for Write commands: host_buf -> flat_buf.
 */
int prp_copy_from_host(const struct prp_list *pl,
                        void *flat_buf, uint32_t flat_len);

/*
 * Copy data from a flat buffer into host memory described by a PRP list.
 * Used for Read commands: flat_buf -> host_buf.
 */
int prp_copy_to_host(const struct prp_list *pl,
                      const void *flat_buf, uint32_t flat_len);

/*
 * Validate that all PRP entries in a list are page-aligned (except first).
 * Returns true if valid.
 */
bool prp_validate(const struct prp_list *pl, uint32_t page_size);

#endif /* HFSSS_PRP_H */
