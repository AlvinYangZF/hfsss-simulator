#include "media/nor_flash.h"
#include "common/log.h"
#include "common/common.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

/* ------------------------------------------------------------------
 * Fixed partition table (REQ-054)
 * ------------------------------------------------------------------ */
static const struct nor_partition NOR_PARTS[NOR_PART_COUNT] = {
    [NOR_PART_BOOTLOADER] = { NOR_PART_BOOTLOADER, "bootloader",
                               0,
                               4u * 1024u * 1024u, true  },
    [NOR_PART_FW_SLOT_A]  = { NOR_PART_FW_SLOT_A,  "fw_slot_a",
                               4u * 1024u * 1024u,
                               64u * 1024u * 1024u, false },
    [NOR_PART_FW_SLOT_B]  = { NOR_PART_FW_SLOT_B,  "fw_slot_b",
                               68u * 1024u * 1024u,
                               64u * 1024u * 1024u, false },
    [NOR_PART_CONFIG]     = { NOR_PART_CONFIG,     "config",
                               132u * 1024u * 1024u,
                               8u * 1024u * 1024u, false },
    [NOR_PART_BBT]        = { NOR_PART_BBT,        "bbt",
                               140u * 1024u * 1024u,
                               8u * 1024u * 1024u, false },
    [NOR_PART_EVENT_LOG]  = { NOR_PART_EVENT_LOG,  "event_log",
                               148u * 1024u * 1024u,
                               16u * 1024u * 1024u, false },
    [NOR_PART_SYSINFO]    = { NOR_PART_SYSINFO,    "sysinfo",
                               164u * 1024u * 1024u,
                               4u * 1024u * 1024u, false },
    [NOR_PART_KEYS]       = { NOR_PART_KEYS,       "keys",
                               168u * 1024u * 1024u,
                               4u * 1024u * 1024u, false },
};

/* ------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */
int nor_dev_init(struct nor_dev *dev, const char *image_path) {
    if (!dev) return HFSSS_ERR_INVAL;

    memset(dev, 0, sizeof(*dev));
    dev->image_fd   = -1;
    dev->image_size = NOR_IMAGE_SIZE_IMPL;

    if (image_path) {
        strncpy(dev->image_path, image_path, sizeof(dev->image_path) - 1);
        /* Try to open existing file; create if absent */
        dev->image_fd = open(image_path, O_RDWR | O_CREAT, 0600);
        if (dev->image_fd < 0) {
            HFSSS_LOG_WARN("NOR", "cannot open image %s: falling back to malloc",
                           image_path);
            goto malloc_fallback;
        }
        /* Extend file to image_size if smaller */
        struct stat st;
        if (fstat(dev->image_fd, &st) == 0 &&
            (size_t)st.st_size < dev->image_size) {
            if (ftruncate(dev->image_fd, (off_t)dev->image_size) != 0) {
                close(dev->image_fd);
                dev->image_fd = -1;
                goto malloc_fallback;
            }
        }
        dev->image = (uint8_t *)mmap(NULL, dev->image_size,
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED, dev->image_fd, 0);
        if (dev->image == MAP_FAILED) {
            dev->image = NULL;
            close(dev->image_fd);
            dev->image_fd = -1;
            goto malloc_fallback;
        }
        dev->use_mmap = true;
        /* Fill uninitialized regions with 0xFF (erased state) */
        struct stat st2;
        if (fstat(dev->image_fd, &st2) == 0 && st2.st_size == 0) {
            memset(dev->image, 0xFF, dev->image_size);
        }
        goto done;
    }

malloc_fallback:
    dev->image = (uint8_t *)malloc(dev->image_size);
    if (!dev->image) {
        return HFSSS_ERR_NOMEM;
    }
    memset(dev->image, 0xFF, dev->image_size);

done:
    for (uint32_t i = 0; i < NOR_TOTAL_SECTORS; i++) {
        dev->sectors[i].erase_count = 0;
        dev->sectors[i].bad         = false;
    }
    pthread_mutex_init(&dev->lock, NULL);
    dev->initialized = true;
    HFSSS_LOG_INFO("NOR", "NOR device initialized: %zu MB (%s)",
                   dev->image_size / (1024 * 1024),
                   dev->use_mmap ? "mmap" : "malloc");
    return HFSSS_OK;
}

void nor_dev_cleanup(struct nor_dev *dev) {
    if (!dev || !dev->initialized) return;

    if (dev->use_mmap && dev->image) {
        msync(dev->image, dev->image_size, MS_SYNC);
        munmap(dev->image, dev->image_size);
    } else {
        free(dev->image);
    }
    dev->image = NULL;

    if (dev->image_fd >= 0) {
        close(dev->image_fd);
        dev->image_fd = -1;
    }
    pthread_mutex_destroy(&dev->lock);
    memset(dev, 0, sizeof(*dev));
}

/* ------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------ */
static int check_range(const struct nor_dev *dev,
                        uint64_t offset, uint32_t len) {
    if (offset + len > dev->image_size) {
        return HFSSS_ERR_INVAL;
    }
    return HFSSS_OK;
}

static uint32_t sector_of(uint64_t offset) {
    return (uint32_t)(offset / NOR_SECTOR_SIZE);
}

/* ------------------------------------------------------------------
 * Read (REQ-055)
 * ------------------------------------------------------------------ */
int nor_read(struct nor_dev *dev, uint64_t offset, void *buf, uint32_t len) {
    if (!dev || !dev->initialized || !buf) return HFSSS_ERR_INVAL;
    if (check_range(dev, offset, len) != HFSSS_OK) return HFSSS_ERR_INVAL;

    pthread_mutex_lock(&dev->lock);
    memcpy(buf, dev->image + offset, len);
    pthread_mutex_unlock(&dev->lock);
    return HFSSS_OK;
}

/* ------------------------------------------------------------------
 * Program — AND-semantics: can only clear bits (REQ-055)
 * ------------------------------------------------------------------ */
int nor_program(struct nor_dev *dev, uint64_t offset,
                const void *buf, uint32_t len) {
    if (!dev || !dev->initialized || !buf) return HFSSS_ERR_INVAL;
    if (check_range(dev, offset, len) != HFSSS_OK) return HFSSS_ERR_INVAL;

    pthread_mutex_lock(&dev->lock);

    /* Check PE cycle limit for each sector touched */
    uint32_t sec_start = sector_of(offset);
    uint32_t sec_end   = sector_of(offset + len - 1);
    for (uint32_t s = sec_start; s <= sec_end && s < NOR_TOTAL_SECTORS; s++) {
        if (dev->sectors[s].erase_count >= NOR_PE_CYCLE_LIMIT) {
            HFSSS_LOG_WARN("NOR", "sector %u exceeded PE limit", s);
            pthread_mutex_unlock(&dev->lock);
            return HFSSS_ERR_IO;
        }
    }

    /* AND-semantics: only clear bits */
    const uint8_t *src = (const uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) {
        dev->image[offset + i] &= src[i];
    }

    pthread_mutex_unlock(&dev->lock);
    return HFSSS_OK;
}

/* ------------------------------------------------------------------
 * Sector erase — restores 0xFF (REQ-055)
 * ------------------------------------------------------------------ */
int nor_sector_erase(struct nor_dev *dev, uint64_t offset) {
    if (!dev || !dev->initialized) return HFSSS_ERR_INVAL;

    /* Align offset to sector boundary */
    uint64_t sector_start = (offset / NOR_SECTOR_SIZE) * NOR_SECTOR_SIZE;
    if (sector_start + NOR_SECTOR_SIZE > dev->image_size) {
        return HFSSS_ERR_INVAL;
    }
    uint32_t sec_idx = sector_of(sector_start);

    pthread_mutex_lock(&dev->lock);

    if (dev->sectors[sec_idx].erase_count >= NOR_PE_CYCLE_LIMIT) {
        HFSSS_LOG_WARN("NOR", "sector %u PE limit reached (%u)",
                       sec_idx, dev->sectors[sec_idx].erase_count);
        pthread_mutex_unlock(&dev->lock);
        return HFSSS_ERR_IO;
    }

    memset(dev->image + sector_start, 0xFF, NOR_SECTOR_SIZE);
    dev->sectors[sec_idx].erase_count++;
    dev->pe_count_total++;

    pthread_mutex_unlock(&dev->lock);
    return HFSSS_OK;
}

/* ------------------------------------------------------------------
 * Chip erase (REQ-055)
 * ------------------------------------------------------------------ */
int nor_chip_erase(struct nor_dev *dev) {
    if (!dev || !dev->initialized) return HFSSS_ERR_INVAL;

    pthread_mutex_lock(&dev->lock);
    memset(dev->image, 0xFF, dev->image_size);
    for (uint32_t i = 0; i < NOR_TOTAL_SECTORS; i++) {
        dev->sectors[i].erase_count++;
    }
    pthread_mutex_unlock(&dev->lock);
    return HFSSS_OK;
}

/* ------------------------------------------------------------------
 * Status register (REQ-055)
 * ------------------------------------------------------------------ */
uint8_t nor_read_status(struct nor_dev *dev) {
    if (!dev || !dev->initialized) return 0xFF;
    return dev->status_reg & ~NOR_STATUS_BUSY;  /* never busy in sim */
}

int nor_write_enable(struct nor_dev *dev) {
    if (!dev || !dev->initialized) return HFSSS_ERR_INVAL;
    dev->status_reg |= NOR_STATUS_WEL;
    dev->write_enabled = true;
    return HFSSS_OK;
}

int nor_reset(struct nor_dev *dev) {
    if (!dev || !dev->initialized) return HFSSS_ERR_INVAL;
    dev->status_reg    = 0;
    dev->write_enabled = false;
    return HFSSS_OK;
}

void nor_read_id(struct nor_dev *dev, uint8_t *vendor_id, uint16_t *device_id) {
    (void)dev;
    if (vendor_id) *vendor_id = NOR_VENDOR_ID;
    if (device_id) *device_id = NOR_DEVICE_ID;
}

/* ------------------------------------------------------------------
 * Partition helpers (REQ-054)
 * ------------------------------------------------------------------ */
int nor_get_partition(enum nor_partition_id id,
                      uint32_t *offset_out, uint32_t *size_out) {
    if (id >= NOR_PART_COUNT) return HFSSS_ERR_INVAL;
    if (offset_out) *offset_out = NOR_PARTS[id].offset;
    if (size_out)   *size_out   = NOR_PARTS[id].size;
    return HFSSS_OK;
}

int nor_partition_read(struct nor_dev *dev, enum nor_partition_id id,
                       uint32_t rel_offset, void *buf, uint32_t len) {
    if (id >= NOR_PART_COUNT) return HFSSS_ERR_INVAL;
    uint64_t abs_offset = (uint64_t)NOR_PARTS[id].offset + rel_offset;
    return nor_read(dev, abs_offset, buf, len);
}

int nor_partition_write(struct nor_dev *dev, enum nor_partition_id id,
                        uint32_t rel_offset, const void *buf, uint32_t len) {
    if (id >= NOR_PART_COUNT) return HFSSS_ERR_INVAL;
    if (NOR_PARTS[id].read_only_at_runtime) return HFSSS_ERR_NOTSUPP;
    uint64_t abs_offset = (uint64_t)NOR_PARTS[id].offset + rel_offset;
    return nor_program(dev, abs_offset, buf, len);
}

/* ------------------------------------------------------------------
 * Persistence sync (REQ-056)
 * ------------------------------------------------------------------ */
int nor_sync(struct nor_dev *dev) {
    if (!dev || !dev->initialized) return HFSSS_ERR_INVAL;
    if (dev->use_mmap && dev->image) {
        if (msync(dev->image, dev->image_size, MS_SYNC) != 0) {
            return HFSSS_ERR_IO;
        }
    }
    return HFSSS_OK;
}
