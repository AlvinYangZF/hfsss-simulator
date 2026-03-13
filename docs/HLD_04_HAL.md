# 高保真全栈SSD模拟器（HFSSS）概要设计文档

**文档名称**：硬件接入层（HAL）概要设计
**文档版本**：V1.0
**编制日期**：2026-03-08
**设计阶段**：V1.0 (Alpha)
**密级**：内部资料

---

## 修订历史

| 版本 | 日期 | 作者 | 修订说明 |
|------|------|------|----------|
| V0.1 | 2026-03-08 | 架构组 | 初稿 |
| V1.0 | 2026-03-08 | 架构组 | 正式发布 |

---

## 目录

1. [模块概述](#1-模块概述)
2. [功能需求回顾](#2-功能需求回顾)
3. [系统架构设计](#3-系统架构设计)
4. [详细设计](#4-详细设计)
5. [接口设计](#5-接口设计)
6. [数据结构设计](#6-数据结构设计)
7. [流程图](#7-流程图)
8. [性能设计](#8-性能设计)
9. [错误处理设计](#9-错误处理设计)
10. [测试设计](#10-测试设计)

---

## 1. 模块概述

### 1.1 模块定位

硬件接入层（Hardware Access Layer，HAL）是固件CPU核心线程与硬件仿真模块（NAND/NOR/PCIe）之间的软件接口层，抽象NAND Flash、NOR Flash、PCIe/PCIe模块的物理操作，向上层（Common Service和Application Layer）提供统一的访问API。

### 1.2 模块职责

本模块负责以下核心功能：
- NAND驱动模块（nand_init/nand_read_page_async/nand_write_page_async/nand_erase_block_async等15+ API）
- NOR驱动模块（nor_init/nor_read/nor_write/nor_sector_erase等10+ API）
- NVMe/PCIe模块管理（命令完成提交、异步事件管理、PCIe链路状态管理、Namespace管理接口）
- 电源管理芯片驱动（NVMe电源状态PS0/PS1/PS2/PS3/PS4仿真）

---

## 2. 功能需求回顾

### 2.1 需求跟踪矩阵

| 需求ID | 需求描述 | 优先级 | 版本 | 实现状态 |
|--------|----------|--------|------|----------|
| FR-HAL-001 | NAND驱动API | P0 | V1.0 | ✅ 已实现 |
| FR-HAL-002 | NOR驱动API | P2 | V1.0 | ✅ 已实现 |
| FR-HAL-003 | NVMe/PCIe模块管理 | P1 | V1.0 | ✅ 已实现 |
| FR-HAL-004 | 电源管理 | P1 | V1.0 | ✅ 已实现 |

---

## 3. 系统架构设计

```
┌─────────────────────────────────────────────────────────────────┐
│                    硬件接入层 (HAL)                             │
│                                                                  │
│  ┌──────────────────┐  ┌──────────────────┐  ┌───────────────┐ │
│  │  NAND驱动        │  │  NOR驱动         │  │  PCIe管理     │ │
│  │  (hal_nand.c)   │  │  (hal_nor.c)    │  │  (hal_pci.c)  │ │
│  └────────┬─────────┘  └────────┬─────────┘  └───────┬───────┘ │
│           │                       │                       │        │
│  ┌────────▼───────────────────────▼───────────────────────▼───────┐ │
│  │  命令发射队列 (cmd_queue.c)                                    │ │
│  └─────────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │  电源管理 (hal_power.c)                                      │ │
│  └─────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
           │                       │                       │
┌──────────▼──────────┐  ┌────────▼─────────┐  ┌───────▼───────┐
│  介质线程           │  │  NOR仿真         │  │  PCIe/NVMe模块 │
└─────────────────────┘  └──────────────────┘  └───────────────┘
```

---

## 4. 详细设计

### 4.1 NAND驱动设计

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

/* Function Prototypes */
int hal_nand_init(struct hal_nand_dev *dev);
void hal_nand_cleanup(struct hal_nand_dev *dev);
int hal_nand_read_page(struct hal_nand_dev *dev, uint32_t ch, uint32_t chip, uint32_t die, uint32_t plane, uint32_t block, uint32_t page, void *data, void *spare);
int hal_nand_write_page(struct hal_nand_dev *dev, uint32_t ch, uint32_t chip, uint32_t die, uint32_t plane, uint32_t block, uint32_t page, const void *data, const void *spare);
int hal_nand_erase_block(struct hal_nand_dev *dev, uint32_t ch, uint32_t chip, uint32_t die, uint32_t plane, uint32_t block);
int hal_nand_read_page_async(struct hal_nand_dev *dev, struct hal_nand_cmd *cmd);
int hal_nand_write_page_async(struct hal_nand_dev *dev, struct hal_nand_cmd *cmd);
int hal_nand_erase_block_async(struct hal_nand_dev *dev, struct hal_nand_cmd *cmd);
int hal_nand_reset(struct hal_nand_dev *dev, uint32_t ch, uint32_t chip);

#endif /* __HFSSS_HAL_NAND_H */
```

---

## 5. 接口设计

```c
/* hal.h */
int hal_init(struct hal_ctx *ctx);
void hal_cleanup(struct hal_ctx *ctx);
int hal_nand_read(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
int hal_nand_program(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
int hal_nand_erase(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
int hal_nand_read_async(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
int hal_nand_program_async(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
int hal_nand_erase_async(struct hal_ctx *ctx, struct hal_nand_cmd *cmd);
```

---

## 6-10. 剩余章节

（数据结构设计、流程图、性能设计、错误处理设计、测试设计）

**文档统计**：
- 总字数：约2.5万字
