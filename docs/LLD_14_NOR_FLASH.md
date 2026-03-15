# 高保真全栈SSD模拟器（HFSSS）详细设计文档

**文档名称**：NOR Flash 仿真详细设计
**文档版本**：V1.0
**编制日期**：2026-03-15
**设计阶段**：V1.5
**密级**：内部资料

---

## 目录

1. [模块概述](#1-模块概述)
2. [功能需求详细分解](#2-功能需求详细分解)
3. [数据结构详细设计](#3-数据结构详细设计)
4. [头文件设计](#4-头文件设计)
5. [函数接口详细设计](#5-函数接口详细设计)
6. [流程图](#6-流程图)
7. [与现有模块的集成要点](#7-与现有模块的集成要点)
8. [测试要点](#8-测试要点)

---

## 1. 模块概述

NOR Flash 仿真模块为 HFSSS 提供完整的 NOR Flash 设备行为模拟，覆盖器件规格、固定分区布局、操作命令集和数据持久化四个核心能力。

该模块以 256MB NOR 设备为仿真目标，通过 mmap 将宿主机文件系统上的镜像文件映射到进程地址空间，实现低开销的快速访问；对关键分区的写操作通过 `fsync` 确保数据不因宿主机崩溃而丢失。模块对上层提供两套接口：

1. **原始设备接口**（`nor_flash.h`）：面向 HAL 层，暴露 Read / Program / Erase / ReadStatus 等物理命令语义；
2. **分区级接口**（`hal_nor_full.h`）：面向 FTL 及固件组件，以分区 ID 为寻址单元，屏蔽底层扇区对齐与擦写周期管理细节。

NOR Flash 模块以同步调用方式运行，线程安全由内部 `pthread_mutex_t` 保证，调用方无需额外加锁。

**覆盖需求**：REQ-053（NOR Flash 规格）、REQ-054（分区布局）、REQ-055（操作命令集）、REQ-056（数据持久化）。

---

## 2. 功能需求详细分解

| 需求ID  | 需求描述                                                                 | 优先级 | 目标版本 |
|---------|--------------------------------------------------------------------------|--------|----------|
| REQ-053 | 256MB NOR 设备；512 字节页；64KB 擦除扇区；100K P/E 次数上限             | P0     | V1.5     |
| REQ-054 | 7 个固定分区：Bootloader(4MB)、FwSlotA(64MB)、FwSlotB(64MB)、Config(8MB)、BBT(8MB)、EventLog(16MB)、SysInfo(4MB)，合计 168MB | P0 | V1.5 |
| REQ-055 | 操作命令：Read（字节粒度）、Program（512B 页）、Erase（64KB 扇区）、ReadStatus、Reset、ReadID | P0 | V1.5 |
| REQ-056 | 数据持久化：NOR 镜像以宿主机普通文件存储；mmap 快速访问；关键写操作后执行 fsync | P0 | V1.5 |

---

## 3. 数据结构详细设计

```c
/* NOR device parameters (REQ-053) */
#define NOR_TOTAL_SIZE_MB      256
#define NOR_PAGE_SIZE          512
#define NOR_SECTOR_SIZE        (64 * 1024)   /* 64KB erase granularity */
#define NOR_PAGES_PER_SECTOR   (NOR_SECTOR_SIZE / NOR_PAGE_SIZE)
#define NOR_TOTAL_PAGES        (NOR_TOTAL_SIZE_MB * 1024 * 1024 / NOR_PAGE_SIZE)
#define NOR_TOTAL_SECTORS      (NOR_TOTAL_SIZE_MB * 1024 * 1024 / NOR_SECTOR_SIZE)
#define NOR_PE_CYCLE_LIMIT     100000
#define NOR_VENDOR_ID          0xEF   /* Winbond (simulation) */
#define NOR_DEVICE_ID          0x4019 /* W25Q256 */

/* Partition table (REQ-054) */
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
    uint32_t              offset;              /* byte offset from NOR start */
    uint32_t              size;                /* bytes */
    bool                  read_only_at_runtime; /* true for Bootloader */
};

/* Fixed partition map */
static const struct nor_partition NOR_PARTITIONS[NOR_PART_COUNT] = {
    [NOR_PART_BOOTLOADER] = { NOR_PART_BOOTLOADER, "bootloader", 0,
                               4  * 1024 * 1024, true  },
    [NOR_PART_FW_SLOT_A]  = { NOR_PART_FW_SLOT_A,  "fw_slot_a",
                               4  * 1024 * 1024, 64 * 1024 * 1024, false },
    [NOR_PART_FW_SLOT_B]  = { NOR_PART_FW_SLOT_B,  "fw_slot_b",
                               68 * 1024 * 1024, 64 * 1024 * 1024, false },
    [NOR_PART_CONFIG]     = { NOR_PART_CONFIG,      "config",
                               132* 1024 * 1024,  8 * 1024 * 1024, false },
    [NOR_PART_BBT]        = { NOR_PART_BBT,         "bbt",
                               140* 1024 * 1024,  8 * 1024 * 1024, false },
    [NOR_PART_EVENT_LOG]  = { NOR_PART_EVENT_LOG,   "event_log",
                               148* 1024 * 1024, 16 * 1024 * 1024, false },
    [NOR_PART_SYSINFO]    = { NOR_PART_SYSINFO,     "sysinfo",
                               164* 1024 * 1024,  4 * 1024 * 1024, false },
};

/* NOR operation commands (REQ-055) */
enum nor_cmd {
    NOR_CMD_READ          = 0x03,
    NOR_CMD_FAST_READ     = 0x0B,
    NOR_CMD_PAGE_PROGRAM  = 0x02,
    NOR_CMD_SECTOR_ERASE  = 0x20,  /* 4KB erase (alias for page erase in sim) */
    NOR_CMD_BLOCK_ERASE   = 0xD8,  /* 64KB block erase */
    NOR_CMD_CHIP_ERASE    = 0xC7,
    NOR_CMD_READ_STATUS1  = 0x05,
    NOR_CMD_WRITE_ENABLE  = 0x06,
    NOR_CMD_WRITE_DISABLE = 0x04,
    NOR_CMD_RESET         = 0xFF,
    NOR_CMD_READ_ID       = 0x9F,
};

/* Status register bits (REQ-055) */
#define NOR_STATUS_BUSY     (1u << 0)
#define NOR_STATUS_WEL      (1u << 1)  /* Write Enable Latch */
#define NOR_STATUS_BP0      (1u << 2)  /* Block Protect bits */
#define NOR_STATUS_BP1      (1u << 3)
#define NOR_STATUS_BP2      (1u << 4)

/* Per-sector metadata */
struct nor_sector_meta {
    uint32_t erase_count;
    bool     bad;          /* permanent hardware fault (simulation) */
};

/* NOR device context */
struct nor_dev {
    uint8_t               *image;           /* mmap'd host file OR malloc'd buffer */
    size_t                 image_size;
    char                   image_path[256];
    int                    image_fd;        /* file descriptor for the image file */
    bool                   use_mmap;        /* true when backed by a file */
    struct nor_sector_meta  sectors[NOR_TOTAL_SECTORS];
    uint8_t                status_reg;      /* current status register */
    bool                   write_enabled;   /* WEL bit */
    uint32_t               pe_count_total;  /* aggregate erase count */
    pthread_mutex_t        lock;
};
```

**分区布局说明（REQ-054）**

| 分区 ID             | 名称        | 起始偏移   | 大小   | 运行时只读 |
|---------------------|-------------|------------|--------|------------|
| NOR_PART_BOOTLOADER | bootloader  | 0 MB       | 4 MB   | 是         |
| NOR_PART_FW_SLOT_A  | fw_slot_a   | 4 MB       | 64 MB  | 否         |
| NOR_PART_FW_SLOT_B  | fw_slot_b   | 68 MB      | 64 MB  | 否         |
| NOR_PART_CONFIG     | config      | 132 MB     | 8 MB   | 否         |
| NOR_PART_BBT        | bbt         | 140 MB     | 8 MB   | 否         |
| NOR_PART_EVENT_LOG  | event_log   | 148 MB     | 16 MB  | 否         |
| NOR_PART_SYSINFO    | sysinfo     | 164 MB     | 4 MB   | 否         |
| —                   | （保留）     | 168 MB     | 88 MB  | —          |

总使用 168 MB，NOR 器件总容量 256 MB，剩余 88 MB 保留供未来扩展。

---

## 4. 头文件设计

### `include/media/nor_flash.h`

```c
#ifndef HFSSS_MEDIA_NOR_FLASH_H
#define HFSSS_MEDIA_NOR_FLASH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Constants & macros                                                   */
/* ------------------------------------------------------------------ */

#define NOR_TOTAL_SIZE_MB      256
#define NOR_PAGE_SIZE          512
#define NOR_SECTOR_SIZE        (64 * 1024)
#define NOR_PAGES_PER_SECTOR   (NOR_SECTOR_SIZE / NOR_PAGE_SIZE)
#define NOR_TOTAL_PAGES        (NOR_TOTAL_SIZE_MB * 1024 * 1024 / NOR_PAGE_SIZE)
#define NOR_TOTAL_SECTORS      (NOR_TOTAL_SIZE_MB * 1024 * 1024 / NOR_SECTOR_SIZE)
#define NOR_PE_CYCLE_LIMIT     100000
#define NOR_VENDOR_ID          0xEF
#define NOR_DEVICE_ID          0x4019

#define NOR_MAGIC              "HFSSS_NOR"
#define NOR_MAGIC_LEN          9

#define NOR_STATUS_BUSY        (1u << 0)
#define NOR_STATUS_WEL         (1u << 1)
#define NOR_STATUS_BP0         (1u << 2)
#define NOR_STATUS_BP1         (1u << 3)
#define NOR_STATUS_BP2         (1u << 4)

/* ------------------------------------------------------------------ */
/* Types                                                                */
/* ------------------------------------------------------------------ */

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

enum nor_cmd {
    NOR_CMD_READ          = 0x03,
    NOR_CMD_FAST_READ     = 0x0B,
    NOR_CMD_PAGE_PROGRAM  = 0x02,
    NOR_CMD_SECTOR_ERASE  = 0x20,
    NOR_CMD_BLOCK_ERASE   = 0xD8,
    NOR_CMD_CHIP_ERASE    = 0xC7,
    NOR_CMD_READ_STATUS1  = 0x05,
    NOR_CMD_WRITE_ENABLE  = 0x06,
    NOR_CMD_WRITE_DISABLE = 0x04,
    NOR_CMD_RESET         = 0xFF,
    NOR_CMD_READ_ID       = 0x9F,
};

struct nor_partition {
    enum nor_partition_id id;
    const char           *name;
    uint32_t              offset;
    uint32_t              size;
    bool                  read_only_at_runtime;
};

struct nor_sector_meta {
    uint32_t erase_count;
    bool     bad;
};

struct nor_dev {
    uint8_t                *image;
    size_t                  image_size;
    char                    image_path[256];
    int                     image_fd;
    bool                    use_mmap;
    struct nor_sector_meta   sectors[NOR_TOTAL_SECTORS];
    uint8_t                 status_reg;
    bool                    write_enabled;
    uint32_t                pe_count_total;
    pthread_mutex_t         lock;
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

/**
 * nor_dev_init - Open or create the NOR image file and initialise the device.
 *
 * Opens @image_path (creates if absent), truncates/extends to
 * NOR_TOTAL_SIZE_MB, mmaps the file into @dev->image, verifies or
 * writes the magic header, and initialises sector metadata.
 * Returns 0 on success, -errno on failure.
 */
int nor_dev_init(struct nor_dev *dev, const char *image_path);

/**
 * nor_dev_format - Fill the entire NOR image with 0xFF and reset all metadata.
 *
 * Simulates a factory erase. All erase_count values are reset to zero.
 * Performs msync after completion.
 * Returns 0 on success, -errno on failure.
 */
int nor_dev_format(struct nor_dev *dev);

/**
 * nor_dev_cleanup - Flush and release all resources held by @dev.
 *
 * Calls msync, munmap, and closes the file descriptor.
 * Safe to call even if nor_dev_init partially succeeded.
 */
void nor_dev_cleanup(struct nor_dev *dev);

/* ------------------------------------------------------------------ */
/* Raw device operations (REQ-055)                                      */
/* ------------------------------------------------------------------ */

/**
 * nor_read - Read @len bytes from absolute NOR offset @offset into @buf.
 *
 * Byte-granularity access. No WEL requirement.
 * Returns 0 on success, -ERANGE if offset+len exceeds device size.
 */
int nor_read(struct nor_dev *dev, uint32_t offset, void *buf, size_t len);

/**
 * nor_page_program - Program up to NOR_PAGE_SIZE bytes at a page-aligned offset.
 *
 * NOR semantics: bits can only be cleared (image[i] &= buf[i]).
 * Requires WEL; clears WEL after completion.
 * Calls msync for partitions classified as critical.
 * Returns 0 on success, -EPERM if WEL not set, -EINVAL if misaligned or
 * len > NOR_PAGE_SIZE, -EIO if target sector is bad.
 */
int nor_page_program(struct nor_dev *dev, uint32_t offset,
                     const void *buf, size_t len);

/**
 * nor_sector_erase - Erase the 64KB sector identified by @sector_idx.
 *
 * Sets all bytes in the sector to 0xFF, increments erase_count, and marks
 * the sector bad if erase_count exceeds NOR_PE_CYCLE_LIMIT.
 * Requires WEL; clears WEL after completion. Calls msync.
 * Returns 0 on success, -EPERM if WEL not set, -EINVAL if index out of range,
 * -EIO if sector already bad.
 */
int nor_sector_erase(struct nor_dev *dev, uint32_t sector_idx);

/**
 * nor_chip_erase - Erase all sectors sequentially.
 *
 * Intended for factory reset. Requires WEL.
 * Returns 0 on success, first -EIO encountered if any sector is bad.
 */
int nor_chip_erase(struct nor_dev *dev);

/**
 * nor_read_status - Return the current value of the status register.
 */
uint8_t nor_read_status(struct nor_dev *dev);

/**
 * nor_write_enable - Assert the Write Enable Latch (WEL).
 */
void nor_write_enable(struct nor_dev *dev);

/**
 * nor_write_disable - Deassert the Write Enable Latch (WEL).
 */
void nor_write_disable(struct nor_dev *dev);

/**
 * nor_read_id - Return the JEDEC vendor and device ID bytes into @buf.
 *
 * @buf must be at least 3 bytes: [vendor_id, device_id_high, device_id_low].
 */
void nor_read_id(struct nor_dev *dev, uint8_t buf[3]);

/* ------------------------------------------------------------------ */
/* Persistence helpers (REQ-056)                                        */
/* ------------------------------------------------------------------ */

/**
 * nor_fsync_critical - msync the pages belonging to @part_id.
 *
 * Called automatically after writes to BBT and SysInfo partitions.
 * Returns 0 on success, -errno on failure.
 */
int nor_fsync_critical(struct nor_dev *dev, enum nor_partition_id part_id);

/**
 * nor_image_verify_magic - Check the 8-byte magic at image[0].
 *
 * Returns true if the magic matches NOR_MAGIC, false otherwise.
 */
bool nor_image_verify_magic(struct nor_dev *dev);

/**
 * nor_image_write_magic - Write the NOR_MAGIC string at image[0].
 */
void nor_image_write_magic(struct nor_dev *dev);

#ifdef __cplusplus
}
#endif

#endif /* HFSSS_MEDIA_NOR_FLASH_H */
```

### `include/hal/hal_nor_full.h`

```c
#ifndef HFSSS_HAL_NOR_FULL_H
#define HFSSS_HAL_NOR_FULL_H

#include <stdint.h>
#include <stddef.h>
#include "media/nor_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * nor_partition_read - Read @len bytes from @offset_in_part within @part_id.
 *
 * Translates the partition-relative offset to an absolute NOR offset and
 * delegates to nor_read. Validates that the range does not cross the
 * partition boundary.
 * Returns 0 on success, -ERANGE on boundary violation.
 */
int nor_partition_read(struct nor_dev *dev,
                       enum nor_partition_id part_id,
                       uint32_t offset_in_part,
                       void *buf, size_t len);

/**
 * nor_partition_write - Write @len bytes to @offset_in_part within @part_id.
 *
 * Rejects writes to read-only partitions (e.g., Bootloader).
 * Handles the full read-modify-erase-reprogram cycle across all affected
 * 64KB sectors. For critical partitions (BBT, SysInfo) calls
 * nor_fsync_critical after the final reprogram.
 * Returns 0 on success, -EACCES if partition is read-only,
 * -ERANGE on boundary violation, -EIO on media error.
 */
int nor_partition_write(struct nor_dev *dev,
                        enum nor_partition_id part_id,
                        uint32_t offset_in_part,
                        const void *buf, size_t len);

/**
 * nor_partition_erase - Erase all sectors belonging to @part_id.
 *
 * Rejects erase of the Bootloader partition at runtime.
 * Returns 0 on success, -EACCES if partition is read-only,
 * first -EIO if any sector is bad.
 */
int nor_partition_erase(struct nor_dev *dev, enum nor_partition_id part_id);

/**
 * nor_partition_info - Return a pointer to the static partition descriptor.
 *
 * Returns NULL if @part_id is out of range.
 */
const struct nor_partition *nor_partition_info(enum nor_partition_id part_id);

#ifdef __cplusplus
}
#endif

#endif /* HFSSS_HAL_NOR_FULL_H */
```

---

## 5. 函数接口详细设计

### 5.1 `nor_dev_init(dev, image_path)`

```
输入：dev — 未初始化的 nor_dev 结构体指针
      image_path — 宿主机镜像文件路径
输出：0 成功；-errno 失败

伪代码：
  pthread_mutex_init(&dev->lock, NULL)
  strlcpy(dev->image_path, image_path, 256)
  dev->image_size = NOR_TOTAL_SIZE_MB * 1024 * 1024

  fd = open(image_path, O_RDWR | O_CREAT, 0644)
  若 fd < 0：返回 -errno

  若 file_size(fd) != dev->image_size：
      ftruncate(fd, dev->image_size)

  dev->image = mmap(NULL, dev->image_size, PROT_READ|PROT_WRITE,
                    MAP_SHARED, fd, 0)
  若 mmap 失败：close(fd)；返回 -errno

  dev->image_fd  = fd
  dev->use_mmap  = true

  若 nor_image_verify_magic(dev) 为 false：
      nor_image_write_magic(dev)
      将所有 sector_meta[i].erase_count = 0，bad = false
  否则：
      从 image 尾部元数据区（若有）加载 sector_meta
      （当前 V1.5 无持久化 meta，全部初始化为 0）

  dev->status_reg    = 0
  dev->write_enabled = false
  dev->pe_count_total = 0
  返回 0
```

### 5.2 `nor_dev_format(dev)`

```
输入：dev — 已初始化的 nor_dev
输出：0 成功；-errno 失败

伪代码：
  pthread_mutex_lock(&dev->lock)
  memset(dev->image, 0xFF, dev->image_size)
  对 i in [0, NOR_TOTAL_SECTORS)：
      dev->sectors[i].erase_count = 0
      dev->sectors[i].bad         = false
  dev->pe_count_total = 0
  nor_image_write_magic(dev)
  msync(dev->image, dev->image_size, MS_SYNC)
  pthread_mutex_unlock(&dev->lock)
  返回 0
```

### 5.3 `nor_dev_cleanup(dev)`

```
伪代码：
  若 dev->image != NULL 且 dev->use_mmap：
      msync(dev->image, dev->image_size, MS_SYNC)
      munmap(dev->image, dev->image_size)
      dev->image = NULL
  若 dev->image_fd >= 0：
      close(dev->image_fd)
      dev->image_fd = -1
  pthread_mutex_destroy(&dev->lock)
```

### 5.4 `nor_read(dev, offset, buf, len)`

```
输入：offset — 绝对字节偏移；buf — 目标缓冲区；len — 字节数
输出：0 成功；-ERANGE 越界

伪代码：
  若 offset + len > dev->image_size：返回 -ERANGE
  pthread_mutex_lock(&dev->lock)
  dev->status_reg |= NOR_STATUS_BUSY
  memcpy(buf, dev->image + offset, len)
  dev->status_reg &= ~NOR_STATUS_BUSY
  pthread_mutex_unlock(&dev->lock)
  返回 0
```

### 5.5 `nor_page_program(dev, offset, buf, len)`

```
输入：offset — 页对齐的绝对字节偏移；buf — 源数据；len — ≤ NOR_PAGE_SIZE
输出：0 成功；-EPERM WEL未置；-EINVAL 未对齐或 len 越界；-EIO 坏扇区

伪代码：
  若 !dev->write_enabled：返回 -EPERM
  若 offset % NOR_PAGE_SIZE != 0：返回 -EINVAL
  若 len > NOR_PAGE_SIZE：返回 -EINVAL
  若 offset + len > dev->image_size：返回 -ERANGE

  sector_idx = offset / NOR_SECTOR_SIZE
  若 dev->sectors[sector_idx].bad：返回 -EIO

  pthread_mutex_lock(&dev->lock)
  dev->status_reg |= NOR_STATUS_BUSY

  /* NOR AND 语义：只能清零，不能置一 */
  对 i in [0, len)：
      dev->image[offset + i] &= buf[i]

  dev->write_enabled   = false
  dev->status_reg     &= ~(NOR_STATUS_BUSY | NOR_STATUS_WEL)

  /* 关键分区强制落盘 */
  part_id = nor_addr_to_partition(offset)
  若 part_id == NOR_PART_BBT 或 NOR_PART_SYSINFO：
      nor_fsync_critical(dev, part_id)

  pthread_mutex_unlock(&dev->lock)
  返回 0
```

### 5.6 `nor_sector_erase(dev, sector_idx)`

```
输入：sector_idx — 扇区编号 [0, NOR_TOTAL_SECTORS)
输出：0 成功；-EPERM WEL未置；-EINVAL 越界；-EIO 已坏扇区

伪代码：
  若 !dev->write_enabled：返回 -EPERM
  若 sector_idx >= NOR_TOTAL_SECTORS：返回 -EINVAL
  若 dev->sectors[sector_idx].bad：返回 -EIO

  pthread_mutex_lock(&dev->lock)
  dev->status_reg |= NOR_STATUS_BUSY

  sector_offset = sector_idx * NOR_SECTOR_SIZE
  memset(dev->image + sector_offset, 0xFF, NOR_SECTOR_SIZE)

  dev->sectors[sector_idx].erase_count++
  dev->pe_count_total++

  若 dev->sectors[sector_idx].erase_count > NOR_PE_CYCLE_LIMIT：
      dev->sectors[sector_idx].bad = true

  dev->write_enabled   = false
  dev->status_reg     &= ~(NOR_STATUS_BUSY | NOR_STATUS_WEL)

  msync(dev->image + sector_offset, NOR_SECTOR_SIZE, MS_SYNC)

  pthread_mutex_unlock(&dev->lock)
  返回 0
```

### 5.7 `nor_chip_erase(dev)`

```
输入：dev — 已初始化的 nor_dev；调用前须 nor_write_enable()
输出：0 成功；-EPERM WEL未置；-EIO 任一扇区擦除失败

伪代码：
  若 !dev->write_enabled：返回 -EPERM
  ret = 0
  对 i in [0, NOR_TOTAL_SECTORS)：
      nor_write_enable(dev)
      rc = nor_sector_erase(dev, i)
      若 rc != 0 且 ret == 0：ret = rc
  返回 ret
```

### 5.8 `nor_read_status(dev)`

```
伪代码：
  pthread_mutex_lock(&dev->lock)
  val = dev->status_reg
  pthread_mutex_unlock(&dev->lock)
  返回 val
```

### 5.9 `nor_write_enable(dev)` / `nor_write_disable(dev)`

```
nor_write_enable：
  pthread_mutex_lock(&dev->lock)
  dev->write_enabled = true
  dev->status_reg   |= NOR_STATUS_WEL
  pthread_mutex_unlock(&dev->lock)

nor_write_disable：
  pthread_mutex_lock(&dev->lock)
  dev->write_enabled = false
  dev->status_reg   &= ~NOR_STATUS_WEL
  pthread_mutex_unlock(&dev->lock)
```

### 5.10 `nor_read_id(dev, buf)`

```
输出：buf[0] = NOR_VENDOR_ID (0xEF)
      buf[1] = (NOR_DEVICE_ID >> 8) & 0xFF  (0x40)
      buf[2] =  NOR_DEVICE_ID & 0xFF         (0x19)

伪代码：
  buf[0] = NOR_VENDOR_ID
  buf[1] = (NOR_DEVICE_ID >> 8) & 0xFF
  buf[2] =  NOR_DEVICE_ID & 0xFF
```

### 5.11 `nor_partition_read(dev, part_id, offset_in_part, buf, len)`

```
伪代码：
  part = &NOR_PARTITIONS[part_id]
  若 offset_in_part + len > part->size：返回 -ERANGE
  abs_offset = part->offset + offset_in_part
  返回 nor_read(dev, abs_offset, buf, len)
```

### 5.12 `nor_partition_write(dev, part_id, offset_in_part, buf, len)`

```
伪代码：
  part = &NOR_PARTITIONS[part_id]
  若 part->read_only_at_runtime：返回 -EACCES
  若 offset_in_part + len > part->size：返回 -ERANGE

  abs_start  = part->offset + offset_in_part
  abs_end    = abs_start + len

  first_sector = abs_start / NOR_SECTOR_SIZE
  last_sector  = (abs_end - 1) / NOR_SECTOR_SIZE

  /* 对每个受影响扇区执行 read-modify-erase-reprogram */
  对 s in [first_sector, last_sector]：
      sector_base = s * NOR_SECTOR_SIZE
      tmp[NOR_SECTOR_SIZE] = 临时缓冲区

      /* 1. 读出当前扇区完整内容 */
      memcpy(tmp, dev->image + sector_base, NOR_SECTOR_SIZE)

      /* 2. 将新数据合并到 tmp */
      copy_range = 与 [abs_start, abs_end) 和扇区 s 的交集
      memcpy(tmp + (copy_range.start - sector_base),
             buf  + (copy_range.start - abs_start),
             copy_range.length)

      /* 3. 擦除扇区 */
      nor_write_enable(dev)
      rc = nor_sector_erase(dev, s)
      若 rc != 0：释放 tmp；返回 rc

      /* 4. 逐页回写 */
      对 page_off in [0, NOR_SECTOR_SIZE) 步进 NOR_PAGE_SIZE：
          nor_write_enable(dev)
          nor_page_program(dev, sector_base + page_off,
                           tmp + page_off, NOR_PAGE_SIZE)

  若 part_id == NOR_PART_BBT 或 NOR_PART_SYSINFO：
      nor_fsync_critical(dev, part_id)

  返回 0
```

### 5.13 `nor_partition_erase(dev, part_id)`

```
伪代码：
  part = &NOR_PARTITIONS[part_id]
  若 part->read_only_at_runtime：返回 -EACCES

  first_sector = part->offset / NOR_SECTOR_SIZE
  last_sector  = (part->offset + part->size - 1) / NOR_SECTOR_SIZE
  ret = 0

  对 s in [first_sector, last_sector]：
      nor_write_enable(dev)
      rc = nor_sector_erase(dev, s)
      若 rc != 0 且 ret == 0：ret = rc

  返回 ret
```

### 5.14 `nor_fsync_critical(dev, part_id)`

```
伪代码：
  part = &NOR_PARTITIONS[part_id]
  page_start  = dev->image + part->offset
  msync_size  = part->size
  rc = msync(page_start, msync_size, MS_SYNC)
  若 rc != 0：返回 -errno
  返回 0
```

### 5.15 `nor_image_verify_magic(dev)`

```
伪代码：
  返回 memcmp(dev->image, NOR_MAGIC, NOR_MAGIC_LEN) == 0
```

### 5.16 `nor_image_write_magic(dev)`

```
伪代码：
  memcpy(dev->image, NOR_MAGIC, NOR_MAGIC_LEN)
```

---

## 6. 流程图

### 6.1 NOR 页编程流程

```
调用 nor_page_program(dev, offset, buf, len)
        |
        v
   WEL 已置位？
   /          \
  否            是
  |             |
返回 -EPERM    offset % NOR_PAGE_SIZE == 0
                且 len <= NOR_PAGE_SIZE ？
               /                       \
             否                          是
             |                           |
       返回 -EINVAL            sector 已标记 bad？
                               /                \
                              是                  否
                              |                   |
                        返回 -EIO       image[i] &= buf[i]
                                         (AND 语义写入)
                                                  |
                                                  v
                                       是否关键分区？
                                       (BBT / SysInfo)
                                       /           \
                                      是             否
                                      |              |
                               msync 该分区          |
                                      \             /
                                       清除 WEL 位
                                            |
                                         返回 0
```

### 6.2 NOR 扇区擦除流程

```
调用 nor_sector_erase(dev, sector_idx)
        |
        v
   WEL 已置位？
   /          \
  否            是
  |             |
返回 -EPERM   sector_idx 合法？
               /              \
             否                 是
             |                  |
       返回 -EINVAL    sector 已标记 bad？
                        /             \
                       是               否
                       |                |
                 返回 -EIO      memset(sector, 0xFF)
                                         |
                                   erase_count++
                                         |
                              erase_count > NOR_PE_CYCLE_LIMIT？
                               /                            \
                              是                              否
                              |                               |
                    sector.bad = true                         |
                              \                              /
                               清除 WEL 位 + msync
                                         |
                                      返回 0
```

### 6.3 分区写流程（读-改-擦-写循环）

```
调用 nor_partition_write(dev, part_id, offset_in_part, buf, len)
        |
        v
   分区为只读？
   /          \
  是            否
  |             |
返回 -EACCES  超出分区范围？
               /           \
              是             否
              |              |
        返回 -ERANGE   计算受影响扇区范围
                            [first_sector, last_sector]
                                         |
                              ┌──────────▼──────────┐
                              │  对每个扇区 s 循环   │
                              │                      │
                              │ 1. 读出扇区至 tmp[]  │
                              │ 2. 将新数据覆入 tmp  │
                              │ 3. nor_sector_erase  │
                              │    (失败 → 返回 -EIO)│
                              │ 4. 逐页 nor_page_    │
                              │    program(tmp)      │
                              └──────────┬──────────┘
                                         |
                              关键分区（BBT/SysInfo）？
                               /                    \
                              是                      否
                              |                       |
                     nor_fsync_critical               |
                              \                      /
                               返回 0
```

### 6.4 NOR 设备初始化流程

```
调用 nor_dev_init(dev, image_path)
        |
        v
  open(image_path, O_RDWR|O_CREAT)
        |
        v
   文件大小 == NOR_TOTAL_SIZE？
   /                         \
  否                           是
  |                            |
ftruncate 到目标大小            |
        \                      |
         mmap(PROT_READ|PROT_WRITE, MAP_SHARED)
                     |
                     v
              mmap 成功？
              /           \
            否               是
            |                |
      close(fd)       nor_image_verify_magic？
      返回 -errno      /                   \
                      否                     是
                      |                      |
              nor_image_write_magic   从 image 加载 sector_meta
              memset sector_meta 为 0  （V1.5：全部清零初始化）
                      \                      /
                       初始化 status_reg、lock 等字段
                                  |
                               返回 0
```

---

## 7. 与现有模块的集成要点

| 对端模块 | 集成方式 | 说明 |
|----------|----------|------|
| `hal_nor.c` | 替换存根实现 | 现有 `hal_nor.c` 中的空存根函数全部委托给 `nor_dev` 的原始设备接口；HAL 层不再持有任何 NOR 状态，所有状态由 `nor_dev` 单例管理。 |
| LLD_09_BOOTLOADER | 固件读取 | Bootloader 启动时通过 `nor_partition_read(dev, NOR_PART_FW_SLOT_A/B, ...)` 读取固件镜像；通过 `nor_partition_read(dev, NOR_PART_SYSINFO, ...)` 读取系统信息。 |
| LLD_11_FTL_RELIABILITY | BBT 双写 | `bbt_write_both_slots` 分别调用 `nor_partition_write(dev, NOR_PART_BBT, primary_offset, ...)` 与 `nor_partition_write(dev, NOR_PART_BBT, mirror_offset, ...)`；两次写入均触发 `nor_fsync_critical`，保证 BBT 落盘。 |
| `log.c` | 事件日志追加 | 结构化日志条目以追加方式写入 `NOR_PART_EVENT_LOG`，维护环形缓冲区写指针；日志写入使用 `nor_partition_write`，不要求每次 fsync（非关键分区）。 |

---

## 8. 测试要点

| 测试用例 ID | 测试描述 | 期望结果 |
|-------------|----------|----------|
| NF-001 | `nor_dev_init` 在不存在的路径创建镜像文件 | 文件被创建，大小 = 256MB，magic 头写入成功，返回 0 |
| NF-002 | `nor_dev_init` 打开已存在且含有效 magic 的镜像文件 | magic 验证通过，无格式化操作，返回 0 |
| NF-003 | `nor_dev_format` 后全盘读取 | 所有字节 = 0xFF；所有 erase_count = 0 |
| NF-004 | `nor_read` 越界读取（offset + len > 256MB） | 返回 -ERANGE，buf 内容不变 |
| NF-005 | `nor_page_program` 写入一页后读回，验证 AND 语义 | 读回字节 = 原值 & 写入值；无法将 0 位置回 1 |
| NF-006 | `nor_page_program` 在未调用 `nor_write_enable` 时调用 | 返回 -EPERM，image 内容不变 |
| NF-007 | `nor_page_program` 使用非页对齐偏移 | 返回 -EINVAL |
| NF-008 | `nor_page_program` 写入长度 > NOR_PAGE_SIZE | 返回 -EINVAL |
| NF-009 | `nor_sector_erase` 擦除一个扇区后读取 | 扇区全部 = 0xFF；erase_count 加 1 |
| NF-010 | `nor_sector_erase` 在未调用 `nor_write_enable` 时调用 | 返回 -EPERM，erase_count 不变 |
| NF-011 | `nor_sector_erase` 使用越界 sector_idx | 返回 -EINVAL |
| NF-012 | 注入 `erase_count > NOR_PE_CYCLE_LIMIT`，再次擦除同一扇区 | `sector.bad = true`；第二次 `nor_sector_erase` 返回 -EIO |
| NF-013 | `nor_partition_write` 写入 Bootloader 分区 | 返回 -EACCES，image 不变 |
| NF-014 | `nor_partition_write` 写入跨分区边界（offset_in_part + len > part.size） | 返回 -ERANGE |
| NF-015 | `nor_partition_write` 跨越多个扇区的写入 | 每个受影响扇区依次完成 read-modify-erase-reprogram；读回数据与写入数据一致 |
| NF-016 | 向 BBT 分区写入数据后验证 `msync` 调用 | `nor_fsync_critical` 被触发（可通过 mock fsync 验证），数据落盘 |
| NF-017 | 向 SysInfo 分区写入数据后验证 `msync` 调用 | 同 NF-016 |
| NF-018 | 数据持久化：写入数据，调用 `nor_dev_cleanup`，重新 `nor_dev_init`，读回数据 | 读回数据与写入数据完全一致 |
| NF-019 | `nor_image_verify_magic` 在 image[0] 被损坏后调用 | 返回 false |
| NF-020 | `nor_chip_erase` 后全盘验证 | 所有字节 = 0xFF；所有非坏扇区的 erase_count 均递增 |
| NF-021 | 多线程并发：8 线程同时读，1 线程写编程 | 互斥锁保护，无数据竞争，读线程返回一致数据 |
| NF-022 | `nor_read_id` 返回值验证 | buf[0] = 0xEF，buf[1] = 0x40，buf[2] = 0x19 |

---

**覆盖需求**：REQ-053、REQ-054、REQ-055、REQ-056

**涉及头文件**：`include/media/nor_flash.h`、`include/hal/hal_nor_full.h`

**接口数量**：20+ 个函数

**测试用例数量**：22 个（NF-001 至 NF-022）
