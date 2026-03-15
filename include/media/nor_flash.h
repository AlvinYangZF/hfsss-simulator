#ifndef HFSSS_MEDIA_NOR_FLASH_H
#define HFSSS_MEDIA_NOR_FLASH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * Device parameters (REQ-053)
 * ------------------------------------------------------------------ */
#define NOR_TOTAL_SIZE_MB    256
#define NOR_PAGE_SIZE        512
#define NOR_SECTOR_SIZE      (64 * 1024)
#define NOR_PAGES_PER_SECTOR (NOR_SECTOR_SIZE / NOR_PAGE_SIZE)
#define NOR_TOTAL_PAGES      ((uint32_t)(NOR_TOTAL_SIZE_MB * 1024 * 1024 / NOR_PAGE_SIZE))
#define NOR_TOTAL_SECTORS    ((uint32_t)(NOR_TOTAL_SIZE_MB * 1024 * 1024 / NOR_SECTOR_SIZE))
#define NOR_PE_CYCLE_LIMIT   100000u
#define NOR_VENDOR_ID        0xEFu
#define NOR_DEVICE_ID        0x4019u

/* In test mode allocate a much smaller image (8 MB) to keep CI fast */
#ifdef HFSSS_NOR_TEST_MODE
#define NOR_IMAGE_SIZE_IMPL  (8ULL * 1024 * 1024)
#else
#define NOR_IMAGE_SIZE_IMPL  ((size_t)NOR_TOTAL_SIZE_MB * 1024 * 1024)
#endif

/* Status register bits (REQ-055) */
#define NOR_STATUS_BUSY (1u << 0)
#define NOR_STATUS_WEL  (1u << 1)
#define NOR_STATUS_BP0  (1u << 2)
#define NOR_STATUS_BP1  (1u << 3)
#define NOR_STATUS_BP2  (1u << 4)

/* ------------------------------------------------------------------
 * Partition table (REQ-054)
 * ------------------------------------------------------------------ */
enum nor_partition_id {
    NOR_PART_BOOTLOADER = 0,
    NOR_PART_FW_SLOT_A,
    NOR_PART_FW_SLOT_B,
    NOR_PART_CONFIG,
    NOR_PART_BBT,
    NOR_PART_EVENT_LOG,
    NOR_PART_SYSINFO,
    NOR_PART_COUNT
};

struct nor_partition {
    enum nor_partition_id id;
    const char           *name;
    uint32_t              offset;               /* byte offset from NOR start */
    uint32_t              size;                 /* bytes */
    bool                  read_only_at_runtime;
};

/* ------------------------------------------------------------------
 * Per-sector metadata
 * ------------------------------------------------------------------ */
struct nor_sector_meta {
    uint32_t erase_count;
    bool     bad;
};

/* ------------------------------------------------------------------
 * NOR device context
 * ------------------------------------------------------------------ */
struct nor_dev {
    uint8_t              *image;
    size_t                image_size;
    char                  image_path[256];
    int                   image_fd;
    bool                  use_mmap;
    struct nor_sector_meta sectors[NOR_TOTAL_SECTORS];
    uint8_t               status_reg;
    bool                  write_enabled;
    uint32_t              pe_count_total;
    bool                  initialized;
    pthread_mutex_t       lock;
};

/* ------------------------------------------------------------------
 * API (REQ-055, REQ-056)
 * ------------------------------------------------------------------ */

/* Lifecycle */
int  nor_dev_init(struct nor_dev *dev, const char *image_path);
void nor_dev_cleanup(struct nor_dev *dev);

/* Operations */
int     nor_read(struct nor_dev *dev, uint64_t offset,
                 void *buf, uint32_t len);
int     nor_program(struct nor_dev *dev, uint64_t offset,
                    const void *buf, uint32_t len);
int     nor_sector_erase(struct nor_dev *dev, uint64_t offset);
int     nor_chip_erase(struct nor_dev *dev);
uint8_t nor_read_status(struct nor_dev *dev);
int     nor_write_enable(struct nor_dev *dev);
int     nor_reset(struct nor_dev *dev);
void    nor_read_id(struct nor_dev *dev,
                    uint8_t *vendor_id, uint16_t *device_id);

/* Partition helpers */
int  nor_get_partition(enum nor_partition_id id,
                       uint32_t *offset_out, uint32_t *size_out);
int  nor_partition_read(struct nor_dev *dev, enum nor_partition_id id,
                        uint32_t rel_offset, void *buf, uint32_t len);
int  nor_partition_write(struct nor_dev *dev, enum nor_partition_id id,
                         uint32_t rel_offset, const void *buf, uint32_t len);

/* Persistence (REQ-056) */
int  nor_sync(struct nor_dev *dev);

#ifdef __cplusplus
}
#endif

#endif /* HFSSS_MEDIA_NOR_FLASH_H */
