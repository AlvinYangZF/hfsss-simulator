#ifndef __HFSSS_MEMORY_H
#define __HFSSS_MEMORY_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Memory allocation flags */
#define MEM_ALLOC_NONE        0x0000
#define MEM_ALLOC_HUGETLB     0x0001  /* Use huge pages if available */
#define MEM_ALLOC_POPULATE    0x0002  /* Populate pages immediately */
#define MEM_ALLOC_LOCK        0x0004  /* Lock memory (mlock) */
#define MEM_ALLOC_ZERO        0x0008  /* Zero-initialize memory */

/* Huge page size (default 2MB) */
#ifndef HUGEPAGE_SIZE
#define HUGEPAGE_SIZE (2UL * 1024UL * 1024UL)
#endif

/* Memory region structure */
struct mem_region {
    void *addr;          /* Start address of the region */
    size_t size;         /* Total size in bytes */
    size_t allocated;    /* Bytes allocated (for simple bump allocator) */
    int flags;           /* Allocation flags */
    int is_hugetlb;      /* Whether this region uses huge pages */
};

/* Function Prototypes */

/**
 * Allocate a large memory region using mmap
 *
 * @param region Pointer to mem_region structure to initialize
 * @param size Size in bytes to allocate
 * @param flags Allocation flags (MEM_ALLOC_*)
 * @return HFSSS_OK on success, error code on failure
 */
int mem_region_alloc(struct mem_region *region, size_t size, int flags);

/**
 * Free a previously allocated memory region
 *
 * @param region Pointer to mem_region structure to free
 */
void mem_region_free(struct mem_region *region);

/**
 * Allocate memory from a region (simple bump allocator)
 *
 * @param region Pointer to mem_region structure
 * @param size Size in bytes to allocate
 * @return Pointer to allocated memory, or NULL on failure
 */
void *mem_region_bump_alloc(struct mem_region *region, size_t size);

/**
 * Reset a region's bump allocator (deallocates all)
 *
 * @param region Pointer to mem_region structure
 */
void mem_region_bump_reset(struct mem_region *region);

/**
 * Check if huge pages are available on the system
 *
 * @return true if huge pages are available, false otherwise
 */
bool mem_hugetlb_available(void);

/**
 * Get the available huge page size
 *
 * @return Huge page size in bytes, or 0 if not available
 */
size_t mem_hugetlb_page_size(void);

#ifdef __cplusplus
}
#endif

#endif /* __HFSSS_MEMORY_H */
