# HFSSS - High Fidelity Full-Stack SSD Simulator

## Project Overview (English)

HFSSS is a high-fidelity full-stack SSD simulator written in C. It provides a complete simulation stack from NAND flash media up to a simple host interface.

**Key Features:**
- NAND flash media simulation with timing models (TLC/SLC support)
- Hardware Abstraction Layer (HAL)
- Flash Translation Layer (FTL) with page-level mapping and garbage collection
- Common services (logging, memory pools, message queues, semaphores, mutexes)
- Top-level SSD simulator interface for easy integration

**English Documentation:**
- [Architecture Overview](docs/ARCHITECTURE.md) - Complete English architecture guide
- [User Guide](docs/USER_GUIDE.md) - Practical usage examples

---

## 项目概述

高保真全栈SSD模拟器（HFSSS）是一个完整的SSD仿真项目，包含从PCIe/NVMe接口到NAND Flash介质的全栈仿真。

## 已完成的工作

### 1. 设计文档

- **PRD文档** (SSD_Simulator_PRD.md) - 完整的产品需求规格书，超过6万字
- **需求矩阵** (REQUIREMENTS_MATRIX.csv) - 134条需求的完整跟踪矩阵
- **概要设计文档** (HLD_01 ~ HLD_06) - 6个HLD文档，已全部补齐完整内容
- **详细设计文档** (LLD_01 ~ LLD_06) - 6个LLD文档，每个超过25,000字
- **测试设计文档** (TEST_LLD_01 ~ TEST_LLD_06) - 6个测试设计文档，每个超过30,000字
- **设计评审报告** (DESIGN_REVIEW_REPORT.md)
- **LLD总览** (LLD_README.md)
- **测试设计总览** (TEST_README.md)

### 2. 代码实现

已完成通用平台层（Common Service）的核心模块：

| 模块 | 文件 | 状态 |
|------|------|------|
| 通用类型与工具 | include/common/common.h | ✅ 完成 |
| Log机制 | include/common/log.h + src/common/log.c | ✅ 完成 |
| 内存池 | include/common/mempool.h + src/common/mempool.c | ✅ 完成 |
| 消息队列 | include/common/msgqueue.h + src/common/msgqueue.c | ✅ 完成 |
| 信号量 | include/common/semaphore.h + src/common/semaphore.c | ✅ 完成 |
| 互斥锁 | include/common/mutex.h + src/common/mutex.c | ✅ 完成 |
| 通用模块测试 | tests/test_common.c | ✅ 完成 |

已完成介质线程模块（Media Threads）：

| 模块 | 文件 | 状态 |
|------|------|------|
| 时序模型 | include/media/timing.h + src/media/timing.c | ✅ 完成 |
| EAT引擎 | include/media/eat.h + src/media/eat.c | ✅ 完成 |
| 坏块管理 | include/media/bbt.h + src/media/bbt.c | ✅ 完成 |
| 可靠性模型 | include/media/reliability.h + src/media/reliability.c | ✅ 完成 |
| NAND层次结构 | include/media/nand.h + src/media/nand.c | ✅ 完成 |
| 介质模块主接口 | include/media/media.h + src/media/media.c | ✅ 完成 |
| 介质模块测试 | tests/test_media.c | ✅ 完成 |

已完成硬件接入层（HAL）：

| 模块 | 文件 | 状态 |
|------|------|------|
| NAND驱动 | include/hal/hal_nand.h + src/hal/hal_nand.c | ✅ 完成 |
| NOR驱动 (占位) | include/hal/hal_nor.h + src/hal/hal_nor.c | ✅ 完成 |
| PCI管理 (占位) | include/hal/hal_pci.h + src/hal/hal_pci.c | ✅ 完成 |
| 电源管理 (占位) | include/hal/hal_power.h + src/hal/hal_power.c | ✅ 完成 |
| HAL主接口 | include/hal/hal.h + src/hal/hal.c | ✅ 完成 |
| HAL模块测试 | tests/test_hal.c | ✅ 完成 |

已完成FTL层（Flash Translation Layer）：

| 模块 | 文件 | 状态 |
|------|------|------|
| 地址映射 | include/ftl/mapping.h + src/ftl/mapping.c | ✅ 完成 |
| Block管理 | include/ftl/block.h + src/ftl/block.c | ✅ 完成 |
| GC（垃圾回收） | include/ftl/gc.h + src/ftl/gc.c | ✅ 完成 |
| 磨损均衡（占位） | include/ftl/wear_level.h + src/ftl/wear_level.c | ✅ 完成 |
| ECC（占位） | include/ftl/ecc.h + src/ftl/ecc.c | ✅ 完成 |
| 错误处理（占位） | include/ftl/error.h + src/ftl/error.c | ✅ 完成 |
| FTL主接口 | include/ftl/ftl.h + src/ftl/ftl.c | ✅ 完成 |
| FTL模块测试 | tests/test_ftl.c | ✅ 完成 |

已完成顶层SSD模拟器接口（SSSIM）：

| 模块 | 文件 | 状态 |
|------|------|------|
| 顶层接口 | include/sssim.h + src/sssim.c | ✅ 完成 |
| 顶层测试 | tests/test_sssim.c | ✅ 完成 |
| 构建系统 | Makefile | ✅ 完成 |

### 3. 测试结果

**通用模块测试**：
```
========================================
Test Summary
========================================
  Total:  104
  Passed: 104
  Failed: 0

  [SUCCESS] All tests passed!
```

**介质模块测试**：
```
========================================
Test Summary
========================================
  Total:  65
  Passed: 65
  Failed: 0

  [SUCCESS] All tests passed!
```

**HAL模块测试**：
```
========================================
Test Summary
========================================
  Total:  31
  Passed: 31
  Failed: 0

  [SUCCESS] All tests passed!
```

**FTL模块测试**：
```
========================================
Test Summary
========================================
  Total:  49
  Passed: 49
  Failed: 0

  [SUCCESS] All tests passed!
```

**顶层SSSIM测试**：
```
========================================
Test Summary
========================================
  Total:  20
  Passed: 20
  Failed: 0

  [SUCCESS] All tests passed!
```

所有269个单元测试全部通过！

## 目录结构

```
.
├── CLAUDE.md
├── DESIGN_REVIEW_REPORT.md
├── README_CODE.md                 # 本文档
├── SSD_Simulator_PRD.md
├── REQUIREMENTS_MATRIX.csv
├── Makefile                       # 构建系统
├── include/                       # 头文件目录
│   ├── sssim.h                   # 顶层SSD接口
│   ├── common/
│   │   ├── common.h
│   │   ├── log.h
│   │   ├── mempool.h
│   │   ├── msgqueue.h
│   │   ├── semaphore.h
│   │   └── mutex.h
│   ├── media/
│   │   ├── timing.h
│   │   ├── eat.h
│   │   ├── bbt.h
│   │   ├── reliability.h
│   │   ├── nand.h
│   │   └── media.h
│   ├── hal/
│   │   ├── hal_nand.h
│   │   ├── hal_nor.h
│   │   ├── hal_pci.h
│   │   ├── hal_power.h
│   │   └── hal.h
│   └── ftl/
│       ├── mapping.h
│       ├── block.h
│       ├── gc.h
│       ├── wear_level.h
│       ├── ecc.h
│       ├── error.h
│       └── ftl.h
├── src/                           # 源代码目录
│   ├── sssim.c                   # 顶层SSD接口实现
│   ├── common/
│   │   ├── log.c
│   │   ├── mempool.c
│   │   ├── msgqueue.c
│   │   ├── semaphore.c
│   │   └── mutex.c
│   ├── media/
│   │   ├── timing.c
│   │   ├── eat.c
│   │   ├── bbt.c
│   │   ├── reliability.c
│   │   ├── nand.c
│   │   └── media.c
│   ├── hal/
│   │   ├── hal_nand.c
│   │   ├── hal_nor.c
│   │   ├── hal_pci.c
│   │   ├── hal_power.c
│   │   └── hal.c
│   └── ftl/
│       ├── mapping.c
│       ├── block.c
│       ├── gc.c
│       ├── wear_level.c
│       ├── ecc.c
│       ├── error.c
│       └── ftl.c
├── tests/                         # 测试目录
│   ├── test_common.c
│   ├── test_media.c
│   ├── test_hal.c
│   ├── test_ftl.c
│   └── test_sssim.c
├── docs/                          # 文档目录
│   ├── HLD_01_*.md
│   ├── HLD_02_*.md
│   ├── HLD_03_*.md
│   ├── HLD_04_*.md
│   ├── HLD_05_*.md
│   ├── HLD_06_*.md
│   ├── LLD_01_*.md
│   ├── LLD_02_*.md
│   ├── LLD_03_*.md
│   ├── LLD_04_*.md
│   ├── LLD_05_*.md
│   ├── LLD_06_*.md
│   ├── TEST_LLD_01_*.md
│   ├── TEST_LLD_02_*.md
│   ├── TEST_LLD_03_*.md
│   ├── TEST_LLD_04_*.md
│   ├── TEST_LLD_05_*.md
│   ├── TEST_LLD_06_*.md
│   ├── LLD_README.md
│   └── TEST_README.md
└── build/                         # 构建输出目录
    ├── lib/
    │   ├── libhfsss-common.a
    │   ├── libhfsss-media.a
    │   ├── libhfsss-hal.a
    │   ├── libhfsss-ftl.a
    │   └── libhfsss-sssim.a
    └── bin/
        ├── test_common
        ├── test_media
        ├── test_hal
        ├── test_ftl
        └── test_sssim
```

## 构建与测试

### 构建项目：
```bash
make all
```

### 运行测试：
```bash
make test
```

### 清理：
```bash
make clean
```

## 顶层接口使用示例

```c
#include "sssim.h"

int main(void)
{
    struct sssim_ctx ctx;
    struct sssim_config config;
    u8 write_buf[4096];
    u8 read_buf[4096];
    int ret;

    /* Initialize with default configuration */
    sssim_config_default(&config);

    /* Customize if needed */
    config.total_lbas = 1024;  /* Small 4MB SSD */

    /* Initialize SSD simulator */
    ret = sssim_init(&ctx, &config);
    if (ret != HFSSS_OK) {
        printf("Failed to initialize SSD: %d\n", ret);
        return 1;
    }

    /* Write data */
    memset(write_buf, 0xAA, 4096);
    ret = sssim_write(&ctx, 0, 1, write_buf);

    /* Read data back */
    ret = sssim_read(&ctx, 0, 1, read_buf);

    /* Get statistics */
    struct ftl_stats stats;
    sssim_get_stats(&ctx, &stats);
    printf("Writes: %llu, Reads: %llu\n",
           (unsigned long long)stats.write_count,
           (unsigned long long)stats.read_count);

    /* Cleanup */
    sssim_cleanup(&ctx);

    return 0;
}
```

## 后续工作

1. 实现剩余模块（按优先级）：
   - 主控线程（用户空间守护进程）
   - PCIe/NVMe设备仿真（内核模块）

2. 集成所有模块
3. 性能测试与优化
4. 文档完善

## 文档统计

| 类别 | 文件数 | 总字数 |
|------|--------|--------|
| PRD | 1 | ~60,000 |
| HLD | 6 | ~150,000 |
| LLD | 7 | ~173,000 |
| 测试设计 | 7 | ~201,000 |
| 评审报告 | 1 | ~5,000 |
| **总计** | **22** | **~589,000** |

## License

内部资料，仅供HFSSS项目使用。

