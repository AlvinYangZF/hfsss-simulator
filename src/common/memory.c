#include "common/memory.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

/* System-specific headers for huge pages */
#ifdef __linux__
#include <linux/mman.h>
#endif

bool mem_hugetlb_available(void)
{
#ifdef __linux__
    /* Check if MAP_HUGETLB is defined */
#ifdef MAP_HUGETLB
    /* Try to allocate a tiny huge page to test */
    void *addr = mmap(NULL, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE,
                       MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB, -1, 0);
    if (addr != MAP_FAILED) {
        munmap(addr, HUGEPAGE_SIZE);
        return true;
    }
#endif
#endif
    return false;
}

size_t mem_hugetlb_page_size(void)
{
#ifdef __linux__
#ifdef MAP_HUGETLB
    /* Try to get the huge page size from the system */
    /* For now, we just return the default 2MB */
    return HUGEPAGE_SIZE;
#endif
#endif
    return 0;
}

int mem_region_alloc(struct mem_region *region, size_t size, int flags)
{
    if (!region || size == 0) {
        return HFSSS_ERR_INVAL;
    }

    memset(region, 0, sizeof(*region));

    int mmap_flags = MAP_ANONYMOUS | MAP_PRIVATE;
    int prot = PROT_READ | PROT_WRITE;

    region->size = size;
    region->flags = flags;
    region->allocated = 0;

    /* Use huge pages if requested and available */
    if (flags & MEM_ALLOC_HUGETLB) {
#ifdef __linux__
#ifdef MAP_HUGETLB
        /* Align size to huge page boundary */
        size_t hugepage_size = mem_hugetlb_page_size();
        if (hugepage_size > 0) {
            size = (size + hugepage_size - 1) & ~(hugepage_size - 1);
            mmap_flags |= MAP_HUGETLB;
            region->is_hugetlb = 1;
        }
#endif
#endif
    }

    /* Populate pages immediately if requested */
    if (flags & MEM_ALLOC_POPULATE) {
#ifdef MAP_POPULATE
        mmap_flags |= MAP_POPULATE;
#endif
    }

    /* Allocate memory */
    region->addr = mmap(NULL, size, prot, mmap_flags, -1, 0);
    if (region->addr == MAP_FAILED) {
        /* Fallback to regular pages if huge pages failed */
        if (region->is_hugetlb) {
#ifdef MAP_HUGETLB
            mmap_flags &= ~MAP_HUGETLB;
#endif
            region->is_hugetlb = 0;
            region->addr = mmap(NULL, region->size, prot, mmap_flags, -1, 0);
            if (region->addr == MAP_FAILED) {
                memset(region, 0, sizeof(*region));
                return HFSSS_ERR_NOMEM;
            }
        } else {
            memset(region, 0, sizeof(*region));
            return HFSSS_ERR_NOMEM;
        }
    }

    /* Lock memory if requested */
    if (flags & MEM_ALLOC_LOCK) {
        if (mlock(region->addr, region->size) != 0) {
            /* Ignore mlock failure, just continue */
        }
    }

    /* Zero-initialize if requested */
    if (flags & MEM_ALLOC_ZERO) {
        memset(region->addr, 0, region->size);
    }

    return HFSSS_OK;
}

void mem_region_free(struct mem_region *region)
{
    if (!region || !region->addr) {
        return;
    }

    /* Unlock memory if locked */
    if (region->flags & MEM_ALLOC_LOCK) {
        munlock(region->addr, region->size);
    }

    /* Unmap the memory */
    munmap(region->addr, region->size);

    memset(region, 0, sizeof(*region));
}

void *mem_region_bump_alloc(struct mem_region *region, size_t size)
{
    if (!region || !region->addr || size == 0) {
        return NULL;
    }

    /* Align size to 8 bytes */
    size = (size + 7) & ~7;

    if (region->allocated + size > region->size) {
        return NULL;
    }

    void *ptr = (char *)region->addr + region->allocated;
    region->allocated += size;

    return ptr;
}

void mem_region_bump_reset(struct mem_region *region)
{
    if (!region) {
        return;
    }

    region->allocated = 0;
}
