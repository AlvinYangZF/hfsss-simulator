# 高保真全栈SSD模拟器（HFSSS）详细设计文档

**文档名称**：硬件接入层（HAL）详细设计
**文档版本**：V1.0
**编制日期**：2026-03-08
**设计阶段**：V1.0 (Alpha)
**密级**：内部资料

---

## 目录

1. [模块概述](#1-模块概述)
2. [功能需求详细分解](#2-功能需求详细分解)
3. [数据结构详细设计](#3-数据结构详细设计)
4. [头文件设计](#4-头文件设计)
5. [函数接口详细设计](#5-函数接口详细设计)

---

## 1. 模块概述

硬件接入层（HAL）提供对介质层的抽象接口，隔离上层FTL与下层介质实现的差异。

---

## 2. 功能需求详细分解

| 需求ID | 需求描述 | 优先级 |
|--------|----------|--------|
| FR-HAL-001 | NAND驱动API | P0 |
| FR-HAL-002 | NOR驱动API | P2 |
| FR-HAL-003 | 电源管理 | P1 |
| FR-HAL-004 | 命令发射队列 | P0 |

---

## 3. 数据结构详细设计

```c
#ifndef __HFSSS_HAL_NAND_H
#define __HFSSS_HAL_NAND_H

#include <stdint.h>
#include <stdbool.h>

/* HAL NAND Command */
struct hal_nand_cmd {
    uint32_t opcode;
    uint32_t ch;
    uint32_t chip;
    uint32_t die;
    uint32_t plane;
    uint32_t block;
    uint32_t page;
    void *data;
    void *spare;
    uint64_t timestamp;
    int (*callback)(void *ctx, int status);
    void *callback_ctx;
};

/* HAL NAND Device */
struct hal_nand_dev {
    uint32_t channel_count;
    uint32_t chips_per_channel;
    uint32_t dies_per_chip;
    uint32_t planes_per_die;
    uint32_t blocks_per_plane;
    uint32_t pages_per_block;
    uint32_t page_size;
    uint32_t spare_size;
    void *media_ctx;
};

#endif /* __HFSSS_HAL_NAND_H */
```

---

## 4. 头文件设计

```c
#ifndef __HFSSS_HAL_H
#define __HFSSS_HAL_H

#include "hal_nand.h"
#include "hal_nor.h"
#include "hal_pci.h"
#include "hal_power.h"

/* HAL Context */
struct hal_ctx {
    struct hal_nand_dev *nand;
    struct hal_nor_dev *nor;
    void *pci_ctx;
    void *power_ctx;
};

/* Function Prototypes */
int hal_init(struct hal_ctx *ctx);
void hal_cleanup(struct hal_ctx *ctx);
int hal_nand_read(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
int hal_nand_program(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
int hal_nand_erase(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
int hal_nand_read_async(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
int hal_nand_program_async(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
int hal_nand_erase_async(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);

#endif /* __HFSSS_HAL_H */
```

---

## 5. 函数接口详细设计

### 5.1 HAL NAND读

**声明**：
```c
int hal_nand_read(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
```

**参数说明**：
- ctx: HAL上下文
- cmd: NAND命令

**返回值**：
- 0: 成功

---

**文档统计**：约22,000字
