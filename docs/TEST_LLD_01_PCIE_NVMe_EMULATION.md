# 高保真全栈SSD模拟器（HFSSS）测试设计方案

**文档名称**：PCIe/NVMe设备仿真模块测试设计
**文档版本**：V1.0
**编制日期**：2026-03-08
**设计阶段**：V1.0 (Alpha)
**密级**：内部资料

---

## 修订历史

| 版本 | 日期 | 作者 | 修订说明 |
|------|------|------|----------|
| V0.1 | 2026-03-08 | 测试组 | 初稿 |
| V1.0 | 2026-03-08 | 测试组 | 正式发布 |

---

## 目录

1. [测试概述](#1-测试概述)
2. [测试策略](#2-测试策略)
3. [单元测试设计](#3-单元测试设计)
4. [功能测试用例设计](#4-功能测试用例设计)
5. [集成测试设计](#5-集成测试设计)
6. [性能测试设计](#6-性能测试设计)
7. [边界条件测试设计](#7-边界条件测试设计)
8. [异常测试设计](#8-异常测试设计)
9. [回归测试设计](#9-回归测试设计)
10. [Before Check In测试设计](#10-before-check-in测试设计)
11. [代码覆盖度统计](#11-代码覆盖度统计)
12. [测试工具与环境](#12-测试工具与环境)
13. [测试执行计划](#13-测试执行计划)
14. [测试结果报告模板](#14-测试结果报告模板)

---

## 1. 测试概述

### 1.1 测试范围

本测试设计方案针对PCIe/NVMe设备仿真模块（LLD_01），覆盖以下内容：
- PCIe配置空间仿真
- NVMe控制器寄存器仿真
- 队列管理（SQ/CQ）
- PRP/SGL解析引擎
- MSI-X中断机制
- DMA引擎
- 内核-用户空间通信

### 1.2 测试目标

| 目标 | 量化指标 | 说明 |
|------|----------|------|
| 代码覆盖度 | ≥ 90% | 使用gcov/lcov统计 |
| 功能测试通过率 | 100% | 所有功能测试用例通过 |
| 单元测试通过率 | 100% | 所有单元测试用例通过 |
| 性能指标 | 满足PRD要求 | 延迟、IOPS等 |
| Bug密度 | ≤ 5个/千行代码 | 静态代码分析+动态测试 |

### 1.3 测试级别

| 测试级别 | 说明 | 负责人 |
|----------|------|--------|
| 单元测试 | 函数级测试 | 开发工程师 |
| 功能测试 | 功能点测试 | 测试工程师 |
| 集成测试 | 模块间交互 | 测试工程师 |
| 性能测试 | 性能基准 | 性能测试工程师 |
| 回归测试 | 全量回归 | 自动化测试 |
| Before Check In | 提交前检查 | 开发工程师 |

---

## 2. 测试策略

### 2.1 测试方法

- **白盒测试**: 使用gcov/lcov进行代码覆盖度统计
- **黑盒测试**: 基于需求的功能测试
- **灰盒测试**: 结合内部结构的集成测试
- **自动化测试**: 使用Python脚本、CUnit、Google Test等框架
- **手动测试**: 复杂场景、边界条件

### 2.2 测试分层架构

```
┌─────────────────────────────────────────────────────────────┐
│                  应用层测试 (fio/nvme-cli)                  │
├─────────────────────────────────────────────────────────────┤
│                  系统调用层测试 (ioctl/mmap)                 │
├─────────────────────────────────────────────────────────────┤
│                  内核模块测试 (kmod/kselftest)              │
├─────────────────────────────────────────────────────────────┤
│                  单元测试 (CUnit/Google Test)                │
└─────────────────────────────────────────────────────────────┘
```

### 2.3 测试环境矩阵

| OS | Kernel Version | Arch | 编译器 | 测试工具 |
|----|----------------|------|--------|----------|
| Ubuntu 22.04 | 5.15 | x86_64 | GCC 11 | gcov/lcov |
| Ubuntu 22.04 | 5.15 | ARM64 | GCC 11 | gcov/lcov |
| RHEL 9 | 5.14 | x86_64 | GCC 11 | gcov/lcov |

---

## 3. 单元测试设计

### 3.1 单元测试框架

- **C语言**: CUnit / Google Test (GTest)
- **Python**: pytest
- **内核模块**: kselftest框架

### 3.2 PCIe配置空间单元测试

#### 3.2.1 测试对象: pci_config_space_init

**函数声明**:
```c
int pci_config_space_init(struct pci_config_space *cfg);
```

**测试用例**:

| 用例ID | 测试场景 | 前置条件 | 测试步骤 | 预期结果 | 优先级 |
|--------|----------|----------|----------|----------|--------|
| UT_PCI_001_001 | 正常初始化 | cfg指针有效 | 调用pci_config_space_init(cfg) | 返回0，配置空间初始化正确 | P0 |
| UT_PCI_001_002 | NULL指针 | cfg为NULL | 调用pci_config_space_init(NULL) | 返回-EINVAL | P0 |
| UT_PCI_001_003 | Vendor ID检查 | 初始化完成 | 读取cfg->header.vendor_id | 值为0x1D1D | P0 |
| UT_PCI_001_004 | Device ID检查 | 初始化完成 | 读取cfg->header.device_id | 值为0x2001 | P0 |
| UT_PCI_001_005 | Class Code检查 | 初始化完成 | 读取cfg->header.class_code | 值为0x010802 | P0 |
| UT_PCI_001_006 | BAR0检查 | 初始化完成 | 读取cfg->header.bar[0] | 值为BAR0_VALUE | P0 |
| UT_PCI_001_007 | Capabilities Pointer检查 | 初始化完成 | 读取cfg->header.capabilities_ptr | 值为0x40 | P0 |

**测试代码示例**:
```c
#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>
#include "pci.h"

void test_pci_config_space_init_normal(void) {
    struct pci_config_space cfg;
    int ret;

    ret = pci_config_space_init(&cfg);
    CU_ASSERT_EQUAL(ret, 0);
    CU_ASSERT_EQUAL(cfg.header.vendor_id, HFSSS_VENDOR_ID);
    CU_ASSERT_EQUAL(cfg.header.device_id, HFSSS_DEVICE_ID);
    CU_ASSERT_EQUAL(cfg.header.class_code[0], PCI_CLASS_CODE_STORAGE);
    CU_ASSERT_EQUAL(cfg.header.class_code[1], PCI_CLASS_SUBCLASS_NVME);
    CU_ASSERT_EQUAL(cfg.header.class_code[2], PCI_CLASS_INTERFACE_NVME);
}

void test_pci_config_space_init_null(void) {
    int ret;

    ret = pci_config_space_init(NULL);
    CU_ASSERT_EQUAL(ret, -EINVAL);
}

CU_TestInfo pci_tests[] = {
    {"pci_config_space_init normal", test_pci_config_space_init_normal},
    {"pci_config_space_init null", test_pci_config_space_init_null},
    CU_TEST_INFO_NULL
};
```

#### 3.2.2 测试对象: pci_read_config

**函数声明**:
```c
int pci_read_config(struct pci_config_space *cfg, int where, int size, u32 *val);
```

**测试用例**:

| 用例ID | 测试场景 | 测试步骤 | 预期结果 | 优先级 |
|--------|----------|----------|----------|--------|
| UT_PCI_002_001 | 读Vendor ID (1字节) | where=0x00, size=1 | *val=0x1D | P0 |
| UT_PCI_002_002 | 读Vendor ID (2字节) | where=0x00, size=2 | *val=0x1D1D | P0 |
| UT_PCI_002_003 | 读Device ID (2字节) | where=0x02, size=2 | *val=0x2001 | P0 |
| UT_PCI_002_004 | 读Class Code (3字节) | where=0x09, size=3 | *val=0x020801 | P0 |
| UT_PCI_002_005 | 读BAR0 (4字节) | where=0x10, size=4 | *val=BAR0_VALUE | P0 |
| UT_PCI_002_006 | 越界读 (where=0x1000) | where=0x1000, size=4 | 返回-EINVAL | P0 |
| UT_PCI_002_007 | 无效size (size=3) | where=0x00, size=3 | 返回-EINVAL | P0 |

#### 3.2.3 测试对象: pci_write_config

**函数声明**:
```c
int pci_write_config(struct pci_config_space *cfg, int where, int size, u32 val);
```

**测试用例**:

| 用例ID | 测试场景 | 测试步骤 | 预期结果 | 优先级 |
|--------|----------|----------|----------|--------|
| UT_PCI_003_001 | 写Command寄存器 | where=0x04, size=2, val=0x0001 | 返回0，写入成功 | P0 |
| UT_PCI_003_002 | 写只读寄存器 (Status) | where=0x06, size=2, val=0xFFFF | 返回0，写入忽略 | P1 |
| UT_PCI_003_003 | 写BAR0 | where=0x10, size=4, val=0x12345678 | 返回0，写入成功 | P0 |
| UT_PCI_003_004 | 越界写 | where=0x1000, size=4 | 返回-EINVAL | P0 |

### 3.3 NVMe控制器寄存器单元测试

#### 3.3.1 测试对象: nvme_regs_init

**函数声明**:
```c
int nvme_regs_init(struct nvme_controller_regs *regs);
```

**测试用例**:

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_NVME_001_001 | 正常初始化 | 返回0 | P0 |
| UT_NVME_001_002 | NULL指针 | 返回-EINVAL | P0 |
| UT_NVME_001_003 | CAP寄存器检查 | MQES=65535, TO=20 | P0 |
| UT_NVME_001_004 | VS寄存器检查 | 值为0x00020000 (NVMe 2.0) | P0 |
| UT_NVME_001_005 | CSTS寄存器检查 | RDY=0 | P0 |

**测试代码示例**:
```c
void test_nvme_regs_init_cap(void) {
    struct nvme_controller_regs regs;
    uint64_t cap;
    int ret;

    ret = nvme_regs_init(&regs);
    CU_ASSERT_EQUAL(ret, 0);

    cap = regs.regs.cap;
    CU_ASSERT_EQUAL((cap >> NVME_CAP_MQES_SHIFT) & 0xFFFF, 65535);
    CU_ASSERT_EQUAL((cap >> NVME_CAP_TO_SHIFT) & 0xFF, 20);
    CU_ASSERT_EQUAL((cap >> NVME_CAP_CSS_SHIFT) & 0xFF, 1);
}

void test_nvme_regs_init_vs(void) {
    struct nvme_controller_regs regs;
    int ret;

    ret = nvme_regs_init(&regs);
    CU_ASSERT_EQUAL(ret, 0);
    CU_ASSERT_EQUAL(regs.regs.vs, 0x00020000);
}
```

#### 3.3.2 测试对象: hfsss_nvme_mmio_read

**函数声明**:
```c
u64 hfsss_nvme_mmio_read(void *opaque, hwaddr addr, unsigned size);
```

**测试用例**:

| 用例ID | 测试场景 | 测试步骤 | 预期结果 | 优先级 |
|--------|----------|----------|----------|--------|
| UT_NVME_002_001 | 读CAP寄存器 (8字节) | addr=0x00, size=8 | 返回正确的CAP值 | P0 |
| UT_NVME_002_002 | 读VS寄存器 (4字节) | addr=0x08, size=4 | 返回0x00020000 | P0 |
| UT_NVME_002_003 | 读CSTS寄存器 (4字节) | addr=0x1C, size=4 | 返回0 | P0 |
| UT_NVME_002_004 | 读Doorbell (4字节) | addr=0x1000, size=4 | 返回0 | P0 |
| UT_NVME_002_005 | 读保留寄存器 | addr=0x50, size=4 | 返回0 | P1 |

#### 3.3.3 测试对象: hfsss_nvme_mmio_write

**函数声明**:
```c
void hfsss_nvme_mmio_write(void *opaque, hwaddr addr, u64 val, unsigned size);
```

**测试用例**:

| 用例ID | 测试场景 | 测试步骤 | 预期结果 | 优先级 |
|--------|----------|----------|----------|--------|
| UT_NVME_003_001 | 写INTMS寄存器 | addr=0x0C, val=0x1, size=4 | INTMS被置位 | P0 |
| UT_NVME_003_002 | 写INTMC寄存器 | addr=0x10, val=0x1, size=4 | INTMS被清零 | P0 |
| UT_NVME_003_003 | 写CC寄存器 (EN=1) | addr=0x14, val=0x1, size=4 | 控制器初始化 | P0 |
| UT_NVME_003_004 | 写CC寄存器 (EN=0) | addr=0x14, val=0x0, size=4 | 控制器禁用 | P0 |
| UT_NVME_003_005 | 写SQ Tail Doorbell | addr=0x1000, val=0x1, size=4 | 门铃被处理 | P0 |
| UT_NVME_003_006 | 写NSSR寄存器 | addr=0x20, val=0x4E564D45, size=4 | 复位触发 | P0 |
| UT_NVME_003_007 | 写只读寄存器 (CAP) | addr=0x00, val=0x0, size=8 | 写入被忽略 | P1 |

#### 3.3.4 测试对象: hfsss_nvme_cc_write

**函数声明**:
```c
void hfsss_nvme_cc_write(struct hfsss_nvme_dev *dev, u32 val);
```

**测试用例**:

| 用例ID | 测试场景 | 前置条件 | 测试步骤 | 预期结果 | 优先级 |
|--------|----------|----------|----------|----------|--------|
| UT_NVME_004_001 | EN从0→1 | EN=0 | 写CC.EN=1 | CSTS.RDY=1 | P0 |
| UT_NVME_004_002 | EN从1→0 | EN=1 | 写CC.EN=0 | CSTS.RDY=0 | P0 |
| UT_NVME_004_003 | 无效CSS | EN=0 | 写CC.CSS=2 | CSTS.CFS=1 | P0 |
| UT_NVME_004_004 | 无效MPS | EN=0 | 写CC.MPS=5 | CSTS.CFS=1 | P0 |
| UT_NVME_004_005 | 无效IOSQES | EN=0 | 写CC.IOSQES=5 | CSTS.CFS=1 | P0 |
| UT_NVME_004_006 | 无效IOCQES | EN=0 | 写CC.IOCQES=3 | CSTS.CFS=1 | P0 |

**测试代码示例**:
```c
void test_nvme_cc_write_en_0_to_1(void) {
    struct hfsss_nvme_dev dev;
    int ret;

    ret = hfsss_nvme_dev_init(&dev);
    CU_ASSERT_EQUAL(ret, 0);

    /* Set Admin Queue attributes */
    dev.nvme_regs.regs.aqa = (15 << NVME_AQA_ACQS_SHIFT) | 15;
    dev.nvme_regs.regs.asq = 0x10000000;
    dev.nvme_regs.regs.acq = 0x10001000;

    hfsss_nvme_cc_write(&dev, 0x1);
    CU_ASSERT((dev.nvme_regs.regs.csts & NVME_CSTS_RDY_MASK) != 0);
}

void test_nvme_cc_write_en_1_to_0(void) {
    struct hfsss_nvme_dev dev;
    int ret;

    ret = hfsss_nvme_dev_init(&dev);
    CU_ASSERT_EQUAL(ret, 0);

    /* First enable */
    dev.nvme_regs.regs.aqa = (15 << NVME_AQA_ACQS_SHIFT) | 15;
    dev.nvme_regs.regs.asq = 0x10000000;
    dev.nvme_regs.regs.acq = 0x10001000;
    hfsss_nvme_cc_write(&dev, 0x1);
    CU_ASSERT((dev.nvme_regs.regs.csts & NVME_CSTS_RDY_MASK) != 0);

    /* Then disable */
    hfsss_nvme_cc_write(&dev, 0x0);
    CU_ASSERT((dev.nvme_regs.regs.csts & NVME_CSTS_RDY_MASK) == 0);
}
```

### 3.4 队列管理单元测试

#### 3.4.1 测试对象: nvme_sq_create

**函数声明**:
```c
struct nvme_sq *nvme_sq_create(struct hfsss_nvme_dev *dev, u16 qid, u64 dma_addr, u16 qsize, u32 entry_size);
```

**测试用例**:

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_QUEUE_001_001 | 正常创建Admin SQ | 返回非NULL指针 | P0 |
| UT_QUEUE_001_002 | 正常创建I/O SQ | 返回非NULL指针 | P0 |
| UT_QUEUE_001_003 | 无效qsize (0) | 返回NULL | P0 |
| UT_QUEUE_001_004 | 无效entry_size (0) | 返回NULL | P0 |
| UT_QUEUE_001_005 | qid=0, qsize=16 | SQ创建成功，qsize=16 | P0 |
| UT_QUEUE_001_006 | qid=1, qsize=256 | SQ创建成功，qsize=256 | P0 |

#### 3.4.2 测试对象: nvme_cq_create

**函数声明**:
```c
struct nvme_cq *nvme_cq_create(struct hfsss_nvme_dev *dev, u16 qid, u64 dma_addr, u16 qsize, u32 entry_size, u16 irq_vector);
```

**测试用例**:

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_QUEUE_002_001 | 正常创建Admin CQ | 返回非NULL指针 | P0 |
| UT_QUEUE_002_002 | 正常创建I/O CQ | 返回非NULL指针 | P0 |
| UT_QUEUE_002_003 | irq_vector=0 | CQ创建成功 | P0 |
| UT_QUEUE_002_004 | irq_vector=1 | CQ创建成功 | P0 |

#### 3.4.3 测试对象: nvme_sq_destroy / nvme_cq_destroy

**测试用例**:

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_QUEUE_003_001 | 销毁正常的SQ | 资源释放 | P0 |
| UT_QUEUE_003_002 | 销毁NULL的SQ | 安全返回 | P0 |
| UT_QUEUE_003_003 | 销毁正常的CQ | 资源释放 | P0 |
| UT_QUEUE_003_004 | 销毁NULL的CQ | 安全返回 | P0 |

### 3.5 MSI-X中断单元测试

#### 3.5.1 测试对象: hfsss_msix_init

**测试用例**:

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_MSIX_001_001 | 正常初始化 | 返回0 | P0 |
| UT_MSIX_001_002 | NULL指针 | 返回-EINVAL | P0 |

#### 3.5.2 测试对象: hfsss_msix_enable / hfsss_msix_disable

**测试用例**:

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_MSIX_002_001 | enable 1 vector | 返回0 | P0 |
| UT_MSIX_002_002 | enable 16 vectors | 返回0 | P0 |
| UT_MSIX_002_003 | enable 64 vectors | 返回0 | P0 |
| UT_MSIX_002_004 | disable | 成功禁用 | P0 |

#### 3.5.3 测试对象: hfsss_msix_post_irq

**测试用例**:

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_MSIX_003_001 | post vector 0 | 中断投递 | P0 |
| UT_MSIX_003_002 | post vector 1 | 中断投递 | P0 |
| UT_MSIX_003_003 | post invalid vector | 返回-EINVAL | P1 |

### 3.6 DMA引擎单元测试

#### 3.6.1 测试对象: hfsss_dma_init

**测试用例**:

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_DMA_001_001 | 正常初始化 | 返回0 | P0 |
| UT_DMA_001_002 | NULL指针 | 返回-EINVAL | P0 |

#### 3.6.2 测试对象: hfsss_dma_map_prp

**测试用例**:

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_DMA_002_001 | PRP1单页 (4K) | 映射成功 | P0 |
| UT_DMA_002_002 | PRP1+PRP2 (8K) | 映射成功 | P0 |
| UT_DMA_002_003 | PRP1+PRP List (16K) | 映射成功 | P0 |
| UT_DMA_002_004 | length=0 | 返回-EINVAL | P0 |

#### 3.6.3 测试对象: hfsss_dma_copy_from_iter / hfsss_dma_copy_to_iter

**测试用例**:

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_DMA_003_001 | copy 4K from iter | 数据正确 | P0 |
| UT_DMA_003_002 | copy 4K to iter | 数据正确 | P0 |
| UT_DMA_003_003 | copy 128K from iter | 数据正确 | P0 |

### 3.7 共享内存单元测试

#### 3.7.1 测试对象: hfsss_shmem_init

**测试用例**:

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_SHMEM_001_001 | 正常初始化 | 返回0 | P0 |
| UT_SHMEM_001_002 | NULL指针 | 返回-EINVAL | P0 |

#### 3.7.2 测试对象: hfsss_shmem_put_cmd / hfsss_shmem_get_cmd

**测试用例**:

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_SHMEM_002_001 | put 1个命令 | 返回0 | P0 |
| UT_SHMEM_002_002 | get 1个命令 | 返回0 | P0 |
| UT_SHMEM_002_003 | put满队列 | 返回-EAGAIN | P0 |
| UT_SHMEM_002_004 | get空队列 | 返回-EAGAIN | P0 |
| UT_SHMEM_002_005 | put 1024个命令 | 全部成功 | P1 |
| UT_SHMEM_002_006 | 并发put/get | 数据一致性 | P1 |

### 3.8 Admin命令处理单元测试

#### 3.8.1 测试对象: hfsss_admin_handle_identify

**测试用例**:

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_ADMIN_001_001 | Identify Controller | 返回正确的数据 | P0 |
| UT_ADMIN_001_002 | Identify Namespace | 返回正确的数据 | P0 |
| UT_ADMIN_001_003 | CNS=0xFF (invalid) | 返回错误 | P0 |

#### 3.8.2 测试对象: hfsss_admin_create_io_sq / hfsss_admin_create_io_cq

**测试用例**:

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_ADMIN_002_001 | Create I/O SQ (qid=1) | 成功 | P0 |
| UT_ADMIN_002_002 | Create I/O CQ (qid=1) | 成功 | P0 |
| UT_ADMIN_002_003 | Create I/O SQ (qid=63) | 成功 | P0 |
| UT_ADMIN_002_004 | Create I/O SQ (qid=64) | 返回错误 | P0 |
| UT_ADMIN_002_005 | Create SQ without CQ | 返回错误 | P0 |

#### 3.8.3 测试对象: hfsss_admin_delete_io_sq / hfsss_admin_delete_io_cq

**测试用例**:

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_ADMIN_003_001 | Delete I/O SQ (qid=1) | 成功 | P0 |
| UT_ADMIN_003_002 | Delete I/O CQ (qid=1) | 成功 | P0 |
| UT_ADMIN_003_003 | Delete Admin SQ | 返回错误 | P0 |
| UT_ADMIN_003_004 | Delete non-existent SQ | 返回错误 | P0 |

### 3.9 I/O命令处理单元测试

#### 3.9.1 测试对象: hfsss_io_read

**测试用例**:

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_IO_001_001 | Read 1 block (4K) | 成功 | P0 |
| UT_IO_001_002 | Read 8 blocks (32K) | 成功 | P0 |
| UT_IO_001_003 | Read 256 blocks (1M) | 成功 | P0 |
| UT_IO_001_004 | Read invalid LBA | 返回错误 | P0 |
| UT_IO_001_005 | Read LBA=0 | 成功 | P0 |

#### 3.9.2 测试对象: hfsss_io_write

**测试用例**:

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_IO_002_001 | Write 1 block (4K) | 成功 | P0 |
| UT_IO_002_002 | Write 8 blocks (32K) | 成功 | P0 |
| UT_IO_002_003 | Write 256 blocks (1M) | 成功 | P0 |
| UT_IO_002_004 | Write invalid LBA | 返回错误 | P0 |

#### 3.9.3 测试对象: hfsss_io_flush

**测试用例**:

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_IO_003_001 | Flush (NSID=1) | 成功 | P0 |
| UT_IO_003_002 | Flush (NSID=0xFFFFFFFF) | 成功 | P0 |

### 3.10 单元测试执行脚本

```bash
#!/bin/bash
# run_unit_tests.sh - 单元测试执行脚本

MODULE="pcie_nvme_emulation"
TEST_FRAMEWORK="CUnit"
COVERAGE_TOOL="gcov"

echo "============================================="
echo "HFSSS $MODULE 单元测试"
echo "============================================="

# 1. 编译测试程序
echo "[1/5] 编译测试程序..."
make clean
make CFLAGS="-fprofile-arcs -ftest-coverage -g" test_${MODULE}

if [ $? -ne 0 ]; then
    echo "ERROR: 编译失败"
    exit 1
fi

# 2. 运行单元测试
echo "[2/5] 运行单元测试..."
./test_${MODULE}

if [ $? -ne 0 ]; then
    echo "ERROR: 单元测试失败"
    exit 1
fi

# 3. 生成代码覆盖度报告
echo "[3/5] 生成代码覆盖度报告..."
lcov --capture --directory . --output-file coverage.info
lcov --extract coverage.info '*/src/*' --output-file coverage_filtered.info
genhtml coverage_filtered.info --output-directory coverage_report

# 4. 统计覆盖度
echo "[4/5] 统计代码覆盖度..."
COVERAGE=$(lcov --summary coverage_filtered.info 2>&1 | grep "lines......" | awk '{print $2}' | tr -d '%')

echo "代码覆盖度: ${COVERAGE}%"

if (( $(echo "$COVERAGE < 90" | bc -l) )); then
    echo "WARNING: 代码覆盖度低于90%"
else
    echo "SUCCESS: 代码覆盖度达标"
fi

# 5. 生成测试报告
echo "[5/5] 生成测试报告..."
cat > test_report.txt << EOF
HFSSS $MODULE 单元测试报告
=================================
日期: $(date)
测试框架: $TEST_FRAMEWORK
代码覆盖度: ${COVERAGE}%
状态: $(if (( $(echo "$COVERAGE >= 90" | bc -l) )); then echo "PASS"; else echo "FAIL"; fi)
EOF

echo "============================================="
echo "测试完成！"
echo "报告: test_report.txt"
echo "覆盖度: coverage_report/index.html"
echo "============================================="
```

### 3.11 单元测试覆盖率目标

| 文件 | 函数覆盖度 | 行覆盖度 | 分支覆盖度 |
|------|------------|----------|------------|
| pci.c | ≥ 95% | ≥ 90% | ≥ 85% |
| nvme.c | ≥ 95% | ≥ 90% | ≥ 85% |
| queue.c | ≥ 95% | ≥ 90% | ≥ 85% |
| msix.c | ≥ 95% | ≥ 90% | ≥ 85% |
| dma.c | ≥ 95% | ≥ 90% | ≥ 85% |
| shmem.c | ≥ 95% | ≥ 90% | ≥ 85% |
| admin.c | ≥ 95% | ≥ 90% | ≥ 85% |
| io.c | ≥ 95% | ≥ 90% | ≥ 85% |
| **总体** | **≥ 95%** | **≥ 90%** | **≥ 85%** |

---

## 4. 功能测试用例设计

### 4.1 PCIe配置空间功能测试

| 用例ID | 测试名称 | 测试步骤 | 预期结果 | 优先级 | 可自动化 |
|--------|----------|----------|----------|--------|----------|
| FT_PCI_001 | lspci识别设备 | 1. 加载内核模块<br>2. 运行lspci -nn | 设备被识别，VID=0x1D1D, DID=0x2001 | P0 | 是 |
| FT_PCI_002 | lspci详细信息 | 1. 加载内核模块<br>2. 运行lspci -vvv | Class Code=0x010802, BAR0=16KB | P0 | 是 |
| FT_PCI_003 | setpci读写 | 1. setpci -s <bdf> 0x04.w=0x0001<br>2. setpci -s <bdf> 0x04.w | 读取值为0x0001 | P0 | 是 |

### 4.2 NVMe控制器功能测试

| 用例ID | 测试名称 | 测试步骤 | 预期结果 | 优先级 | 可自动化 |
|--------|----------|----------|----------|--------|----------|
| FT_NVME_001 | nvme list识别 | 1. 加载内核模块<br>2. 运行nvme list | 设备出现在列表中 | P0 | 是 |
| FT_NVME_002 | nvme id-ctrl | 1. 运行nvme id-ctrl /dev/nvme0 | 返回Identify Controller数据 | P0 | 是 |
| FT_NVME_003 | nvme id-ns | 1. 运行nvme id-ns /dev/nvme0 -n 1 | 返回Identify Namespace数据 | P0 | 是 |
| FT_NVME_004 | NVMe Version | 检查Identify Controller的VS字段 | VS=0x00020000 (NVMe 2.0) | P0 | 是 |
| FT_NVME_005 | 最大队列数 | 检查CAP.MQES | MQES=65535 | P0 | 是 |
| FT_NVME_006 | 控制器启用/禁用 | 1. 写入CC.EN=1<br>2. 检查CSTS.RDY<br>3. 写入CC.EN=0<br>4. 检查CSTS.RDY | RDY正常变化 | P0 | 是 |

### 4.3 队列管理功能测试

| 用例ID | 测试名称 | 测试步骤 | 预期结果 | 优先级 | 可自动化 |
|--------|----------|----------|----------|--------|----------|
| FT_QUEUE_001 | Admin Queue创建 | 1. 加载模块<br>2. 检查dmesg | Admin SQ/CQ创建成功 | P0 | 是 |
| FT_QUEUE_002 | Create I/O SQ/CQ | 1. nvme create-sq /dev/nvme0<br>2. nvme create-cq /dev/nvme0 | 命令成功 | P0 | 是 |
| FT_QUEUE_003 | Delete I/O SQ/CQ | 1. nvme delete-sq /dev/nvme0<br>2. nvme delete-cq /dev/nvme0 | 命令成功 | P0 | 是 |
| FT_QUEUE_004 | 创建64个队列对 | 1. 循环创建64个I/O SQ/CQ | 全部创建成功 | P1 | 是 |
| FT_QUEUE_005 | 队列深度65535 | 1. 创建QD=65535的SQ | 创建成功 | P1 | 是 |

### 4.4 MSI-X中断功能测试

| 用例ID | 测试名称 | 测试步骤 | 预期结果 | 优先级 | 可自动化 |
|--------|----------|----------|----------|--------|----------|
| FT_MSIX_001 | MSI-X启用 | 1. 检查/proc/interrupts | 设备使用MSI-X | P0 | 是 |
| FT_MSIX_002 | 中断投递 | 1. 发送I/O命令<br>2. 检查/proc/interrupts | 中断计数增加 | P0 | 是 |
| FT_MSIX_003 | 多中断向量 | 1. 创建多个CQ，每个不同向量<br>2. 发送命令到各队列 | 各向量都有中断 | P1 | 是 |

### 4.5 Admin命令功能测试

| 用例ID | 测试名称 | 测试步骤 | 预期结果 | 优先级 | 可自动化 |
|--------|----------|----------|----------|--------|----------|
| FT_ADMIN_001 | Identify Controller | nvme id-ctrl /dev/nvme0 | 成功，数据完整 | P0 | 是 |
| FT_ADMIN_002 | Identify Namespace | nvme id-ns /dev/nvme0 -n 1 | 成功，数据完整 | P0 | 是 |
| FT_ADMIN_003 | Get Log Page | nvme get-log /dev/nvme0 -i 1 -l 512 | 成功 | P0 | 是 |
| FT_ADMIN_004 | Set Features | nvme set-feature /dev/nvme0 -f 7 -v 1 | 成功 | P0 | 是 |
| FT_ADMIN_005 | Get Features | nvme get-feature /dev/nvme0 -f 7 | 成功 | P0 | 是 |
| FT_ADMIN_006 | Keep Alive | nvme keep-alive /dev/nvme0 | 成功 | P1 | 是 |
| FT_ADMIN_007 | Async Event | 等待AEN通知 | AEN正常工作 | P1 | 否 |

### 4.6 I/O命令功能测试

| 用例ID | 测试名称 | 测试步骤 | 预期结果 | 优先级 | 可自动化 |
|--------|----------|----------|----------|--------|----------|
| FT_IO_001 | 4K随机读 | fio --filename=/dev/nvme0n1 --rw=randread --bs=4k --numjobs=1 --iodepth=1 --size=1G | IO正常完成 | P0 | 是 |
| FT_IO_002 | 4K随机写 | fio --filename=/dev/nvme0n1 --rw=randwrite --bs=4k --numjobs=1 --iodepth=1 --size=1G | IO正常完成 | P0 | 是 |
| FT_IO_003 | 128K顺序读 | fio --filename=/dev/nvme0n1 --rw=read --bs=128k --numjobs=1 --iodepth=1 --size=1G | IO正常完成 | P0 | 是 |
| FT_IO_004 | 128K顺序写 | fio --filename=/dev/nvme0n1 --rw=write --bs=128k --numjobs=1 --iodepth=1 --size=1G | IO正常完成 | P0 | 是 |
| FT_IO_005 | 70/30混合读写 | fio --filename=/dev/nvme0n1 --rw=randrw --rwmixread=70 --bs=4k | IO正常完成 | P0 | 是 |
| FT_IO_006 | QD=32随机读 | fio --iodepth=32 --rw=randread | IO正常完成 | P0 | 是 |
| FT_IO_007 | QD=128随机读 | fio --iodepth=128 --rw=randread | IO正常完成 | P1 | 是 |
| FT_IO_008 | QD=1024随机读 | fio --iodepth=1024 --rw=randread | IO正常完成 | P1 | 是 |
| FT_IO_009 | Flush命令 | nvme flush /dev/nvme0 -n 1 | 成功 | P0 | 是 |
| FT_IO_010 | DSM (Trim) | nvme dsm /dev/nvme0 -n 1 -d 0 -b 100 | 成功 | P1 | 是 |
| FT_IO_011 | Verify命令 | nvme verify /dev/nvme0 -n 1 -s 0 -c 100 | 成功 | P1 | 是 |

### 4.7 数据完整性测试

| 用例ID | 测试名称 | 测试步骤 | 预期结果 | 优先级 | 可自动化 |
|--------|----------|----------|----------|--------|----------|
| FT_DATA_001 | 写后读验证 | 1. 写入已知数据<br>2. 读回数据<br>3. 比较 | 数据一致 | P0 | 是 |
| FT_DATA_002 | 大规模数据验证 | 1. 写入10GB数据<br>2. 读回并校验 | 数据一致 | P0 | 是 |
| FT_DATA_003 | 随机模式验证 | 1. 写入伪随机数据<br>2. 读回并校验 | 数据一致 | P0 | 是 |

---

## 5. 集成测试设计

### 5.1 集成测试策略

- **自底向上集成**: 先测试底层模块，再逐步集成上层模块
- **自顶向下集成**: 先测试顶层接口，再逐步测试底层模块
- **三明治集成**: 结合上述两种策略

### 5.2 内核-用户空间集成测试

| 用例ID | 测试名称 | 测试步骤 | 预期结果 | 优先级 | 可自动化 |
|--------|----------|----------|----------|--------|----------|
| IT_SHMEM_001 | Ring Buffer收发 | 1. 内核模块放命令<br>2. 用户空间收命令<br>3. 用户空间放回执<br>4. 内核模块收回执 | 整个流程成功 | P0 | 是 |
| IT_SHMEM_002 | 高吞吐量测试 | 1. 连续发送100K个命令<br>2. 检查吞吐量 | 吞吐量≥100K IOPS | P0 | 是 |
| IT_SHMEM_003 | 并发测试 | 1. 多个内核线程放命令<br>2. 多个用户线程收命令 | 无数据竞争，无丢失 | P1 | 是 |

### 5.3 PCIe/NVMe + 主控线程集成测试

| 用例ID | 测试名称 | 测试步骤 | 预期结果 | 优先级 | 可自动化 |
|--------|----------|----------|----------|--------|----------|
| IT_INTG_001 | 完整命令流 | 1. 主机发NVMe命令<br>2. 内核模块接收<br>3. 主控线程处理<br>4. 命令完成返回 | 整个流程成功 | P0 | 是 |
| IT_INTG_002 | fio完整测试 | 1. 运行fio 4k randread 10分钟<br>2. 检查结果 | 无错误，性能稳定 | P0 | 是 |
| IT_INTG_003 | nvme-cli完整测试 | 1. 运行所有nvme-cli命令 | 全部成功 | P0 | 是 |

### 5.4 端到端集成测试

| 用例ID | 测试名称 | 测试步骤 | 预期结果 | 优先级 | 可自动化 |
|--------|----------|----------|----------|--------|----------|
| IT_E2E_001 | 端到端读路径 | 1. 应用层读<br>2. 文件系统<br>3. NVMe驱动<br>4. 内核模块<br>5. 主控线程<br>6. FTL<br>7. HAL<br>8. 介质层<br>9. 数据返回 | 完整路径成功 | P0 | 是 |
| IT_E2E_002 | 端到端写路径 | 1. 应用层写<br>2. 文件系统<br>3. NVMe驱动<br>4. 内核模块<br>5. 主控线程<br>6. FTL<br>7. HAL<br>8. 介质层<br>9. 完成返回 | 完整路径成功 | P0 | 是 |
| IT_E2E_003 | 长时间稳定性测试 | 1. 运行fio 7x24小时<br>2. 检查错误日志 | 无crash，无错误 | P0 | 否 |

---

## 6. 性能测试设计

### 6.1 性能基准测试

| 测试项 | 测试配置 | 目标值 | 测量方法 | 优先级 |
|--------|----------|--------|----------|--------|
| 4K随机读IOPS | QD=32, numjobs=1 | ≥ 500K | fio | P0 |
| 4K随机读IOPS | QD=128, numjobs=1 | ≥ 1M | fio | P0 |
| 4K随机写IOPS | QD=32, numjobs=1 | ≥ 300K | fio | P0 |
| 4K随机写IOPS | QD=128, numjobs=1 | ≥ 500K | fio | P0 |
| 128K顺序读带宽 | QD=32, numjobs=1 | ≥ 3.5GB/s | fio | P0 |
| 128K顺序写带宽 | QD=32, numjobs=1 | ≥ 3.0GB/s | fio | P0 |
| 4K读延迟 | QD=1, numjobs=1 | ≤ 50μs | fio | P0 |
| 4K写延迟 | QD=1, numjobs=1 | ≤ 100μs | fio | P0 |
| 命令处理延迟 | 端到端 | ≤ 10μs | trace | P0 |
| 中断投递延迟 | CQ写回→中断 | ≤ 5μs | trace | P0 |

### 6.2 性能测试脚本示例

```bash
#!/bin/bash
# run_perf_tests.sh - 性能测试脚本

DEV="/dev/nvme0n1"
RESULTS_DIR="perf_results_$(date +%Y%m%d_%H%M%S)"
mkdir -p $RESULTS_DIR

echo "============================================="
echo "HFSSS PCIe/NVMe性能测试"
echo "============================================="

# 1. 4K随机读 - QD=32
echo "[1/10] 4K随机读, QD=32..."
fio --name=4k_randread_qd32 \
    --filename=$DEV \
    --rw=randread \
    --bs=4k \
    --iodepth=32 \
    --numjobs=1 \
    --size=10G \
    --runtime=60 \
    --time_based \
    --direct=1 \
    --ioengine=libaio \
    --output=$RESULTS_DIR/4k_randread_qd32.txt

# 2. 4K随机读 - QD=128
echo "[2/10] 4K随机读, QD=128..."
fio --name=4k_randread_qd128 \
    --filename=$DEV \
    --rw=randread \
    --bs=4k \
    --iodepth=128 \
    --numjobs=1 \
    --size=10G \
    --runtime=60 \
    --time_based \
    --direct=1 \
    --ioengine=libaio \
    --output=$RESULTS_DIR/4k_randread_qd128.txt

# 3. 4K随机写 - QD=32
echo "[3/10] 4K随机写, QD=32..."
fio --name=4k_randwrite_qd32 \
    --filename=$DEV \
    --rw=randwrite \
    --bs=4k \
    --iodepth=32 \
    --numjobs=1 \
    --size=10G \
    --runtime=60 \
    --time_based \
    --direct=1 \
    --ioengine=libaio \
    --output=$RESULTS_DIR/4k_randwrite_qd32.txt

# 4-10: 更多测试...

echo "============================================="
echo "性能测试完成！"
echo "结果目录: $RESULTS_DIR"
echo "============================================="
```

---

## 7. 边界条件测试设计

### 7.1 PCIe/NVMe边界测试

| 用例ID | 测试名称 | 测试条件 | 预期结果 | 优先级 | 可自动化 |
|--------|----------|----------|----------|--------|----------|
| BC_PCI_001 | 配置空间边界读 | where=0xFF, size=1 | 成功 | P0 | 是 |
| BC_PCI_002 | 配置空间越界读 | where=0x1000, size=1 | 返回错误 | P0 | 是 |
| BC_NVME_001 | 最大队列深度 | QD=65535 | 系统稳定 | P0 | 是 |
| BC_NVME_002 | 最大队列数 | 64个I/O队列 | 系统稳定 | P0 | 是 |
| BC_NVME_003 | 零长度传输 | len=0 | 正确处理 | P0 | 是 |
| BC_NVME_004 | 最大传输长度 | len=128K×1024=128M | 成功 | P1 | 是 |
| BC_NVME_005 | LBA=0 | 起始LBA=0 | 成功 | P0 | 是 |
| BC_NVME_006 | 最大LBA | LBA=容量-1 | 成功 | P0 | 是 |
| BC_NVME_007 | 越界LBA | LBA=容量 | 返回错误 | P0 | 是 |

### 7.2 共享内存边界测试

| 用例ID | 测试名称 | 测试条件 | 预期结果 | 优先级 | 可自动化 |
|--------|----------|----------|----------|--------|----------|
| BC_SHMEM_001 | 空队列get | 队列空时get | 返回-EAGAIN | P0 | 是 |
| BC_SHMEM_002 | 满队列put | 队列满时put | 返回-EAGAIN | P0 | 是 |
| BC_SHMEM_003 | 连续put满 | 连续put 16384个命令 | 第16385个返回-EAGAIN | P0 | 是 |

---

## 8. 异常测试设计

### 8.1 错误注入测试

| 用例ID | 测试名称 | 错误注入点 | 预期结果 | 优先级 | 可自动化 |
|--------|----------|------------|----------|--------|----------|
| ERR_INJ_001 | 无效命令opcode | 注入无效Admin opcode | 返回Invalid Command | P0 | 是 |
| ERR_INJ_002 | 无效队列ID | 访问无效SQID | 返回Invalid Queue | P0 | 是 |
| ERR_INJ_003 | PRP越界 | 注入无效PRP地址 | 返回Data Transfer Error | P0 | 是 |
| ERR_INJ_004 | SGL越界 | 注入无效SGL | 返回Data Transfer Error | P1 | 是 |
| ERR_INJ_005 | 命令超时 | 阻止命令完成 | 超时检测触发 | P1 | 否 |

### 8.2 压力测试

| 用例ID | 测试名称 | 测试配置 | 持续时间 | 预期结果 | 优先级 |
|--------|----------|----------|----------|----------|--------|
| STRESS_001 | 高QD压力 | QD=65535, 4k randrw | 1小时 | 无crash，无错误 | P0 |
| STRESS_002 | 高并发压力 | numjobs=32, QD=1024 | 1小时 | 无crash，无错误 | P0 |
| STRESS_003 | 长时间稳定性 | 70/30混合读写 | 7x24小时 | 无crash，无错误 | P0 |
| STRESS_004 | 快速复位测试 | 每5分钟复位一次 | 100次 | 每次都成功复位 | P1 |

---

## 9. 回归测试设计

### 9.1 回归测试策略

- **每次提交**: 运行快速回归测试（< 10分钟）
- **每日构建**: 运行完整回归测试（< 2小时）
- **每周构建**: 运行压力测试和性能测试（< 24小时）

### 9.2 快速回归测试套件（Before Check In）

| 套件名称 | 测试用例数 | 执行时间 | 覆盖率目标 |
|----------|------------|----------|------------|
| 单元测试快速套件 | 50 | < 5分钟 | ≥ 70% |
| 功能测试快速套件 | 20 | < 5分钟 | - |
| **总计** | **70** | **< 10分钟** | - |

**快速回归测试列表**:
- 所有P0单元测试
- lspci识别
- nvme list识别
- nvme id-ctrl
- 4K随机读（QD=1, 10秒）
- 4K随机写（QD=1, 10秒）

### 9.3 完整回归测试套件

| 套件名称 | 测试用例数 | 执行时间 |
|----------|------------|----------|
| 单元测试完整套件 | 200+ | < 30分钟 |
| 功能测试完整套件 | 100+ | < 60分钟 |
| 集成测试套件 | 30+ | < 30分钟 |
| **总计** | **330+** | **< 2小时** |

### 9.4 回归测试脚本

```bash
#!/bin/bash
# run_regression.sh - 回归测试脚本

SUITE="$1"

if [ -z "$SUITE" ]; then
    SUITE="full"
fi

echo "============================================="
echo "HFSSS PCIe/NVMe回归测试 - $SUITE"
echo "============================================="

case "$SUITE" in
    "quick")
        echo "运行快速回归测试..."
        ./run_unit_tests.sh --quick
        ./run_functional_tests.sh --quick
        ;;
    "full")
        echo "运行完整回归测试..."
        ./run_unit_tests.sh
        ./run_functional_tests.sh
        ./run_integration_tests.sh
        ;;
    "nightly")
        echo "运行夜间回归测试..."
        ./run_unit_tests.sh
        ./run_functional_tests.sh
        ./run_integration_tests.sh
        ./run_perf_tests.sh --quick
        ;;
    *)
        echo "未知套件: $SUITE"
        exit 1
        ;;
esac

echo "============================================="
echo "回归测试完成"
echo "============================================="
```

---

## 10. Before Check In测试设计

### 10.1 Before Check In检查清单

- [ ] 代码编译通过（无警告）
- [ ] 静态代码分析通过（cppcheck/clang-analyzer）
- [ ] 快速单元测试通过
- [ ] 快速功能测试通过
- [ ] 代码格式检查通过（clang-format）
- [ ] Commit message规范
- [ ] 没有新增的编译器警告

### 10.2 Before Check In脚本

```bash
#!/bin/bash
# before_checkin.sh - 提交前检查脚本

echo "============================================="
echo "HFSSS Before Check In检查"
echo "============================================="

PASS=0
FAIL=0

# 1. 代码编译检查
echo "[1/6] 检查代码编译..."
make clean
make -j$(nproc) 2>&1 | tee build.log
if [ $? -ne 0 ]; then
    echo "FAIL: 编译失败"
    ((FAIL++))
else
    echo "PASS: 编译成功"
    ((PASS++))
fi

# 2. 静态代码分析
echo "[2/6] 运行静态代码分析..."
cppcheck --enable=all --error-exitcode=1 src/ 2>&1 | tee cppcheck.log
if [ $? -ne 0 ]; then
    echo "FAIL: 静态代码分析发现问题"
    ((FAIL++))
else
    echo "PASS: 静态代码分析通过"
    ((PASS++))
fi

# 3. 快速单元测试
echo "[3/6] 运行快速单元测试..."
./run_unit_tests.sh --quick
if [ $? -ne 0 ]; then
    echo "FAIL: 单元测试失败"
    ((FAIL++))
else
    echo "PASS: 单元测试通过"
    ((PASS++))
fi

# 4. 快速功能测试
echo "[4/6] 运行快速功能测试..."
./run_functional_tests.sh --quick
if [ $? -ne 0 ]; then
    echo "FAIL: 功能测试失败"
    ((FAIL++))
else
    echo "PASS: 功能测试通过"
    ((PASS++))
fi

# 5. 代码格式检查
echo "[5/6] 检查代码格式..."
clang-format --dry-run --Werror src/*.c src/*.h 2>&1 | tee format.log
if [ $? -ne 0 ]; then
    echo "FAIL: 代码格式不规范"
    ((FAIL++))
else
    echo "PASS: 代码格式规范"
    ((PASS++))
fi

# 6. 检查编译器警告
echo "[6/6] 检查编译器警告..."
WARN_COUNT=$(grep -c "warning:" build.log)
if [ $WARN_COUNT -gt 0 ]; then
    echo "FAIL: 发现 $WARN_COUNT 个编译器警告"
    ((FAIL++))
else
    echo "PASS: 无编译器警告"
    ((PASS++))
fi

echo "============================================="
echo "检查结果: $PASS 通过, $FAIL 失败"
echo "============================================="

if [ $FAIL -ne 0 ]; then
    echo ""
    echo "请修复以上问题后再提交！"
    exit 1
else
    echo ""
    echo "可以提交！"
    exit 0
fi
```

---

## 11. 代码覆盖度统计

### 11.1 覆盖度工具链

| 工具 | 用途 | 版本要求 |
|------|------|----------|
| gcc | 编译器，支持-fprofile-arcs -ftest-coverage | ≥ 7.0 |
| gcov | 生成覆盖率数据 | ≥ 7.0 |
| lcov | 处理gcov数据，生成info文件 | ≥ 1.14 |
| genhtml | 生成HTML报告 | ≥ 1.14 |
| gcovr | 生成XML/JSON报告 | ≥ 5.0 |

### 11.2 覆盖度统计脚本

```bash
#!/bin/bash
# run_coverage.sh - 代码覆盖度统计脚本

OUTPUT_DIR="coverage_report_$(date +%Y%m%d_%H%M%S)"
mkdir -p $OUTPUT_DIR

echo "============================================="
echo "HFSSS代码覆盖度统计"
echo "============================================="

# 1. 清理旧数据
echo "[1/5] 清理旧数据..."
find . -name "*.gcda" -delete
find . -name "*.gcno" -delete

# 2. 编译带覆盖率的版本
echo "[2/5] 编译带覆盖率的版本..."
make clean
make CFLAGS="-fprofile-arcs -ftest-coverage -g -O0"

# 3. 运行所有测试
echo "[3/5] 运行所有测试..."
./run_unit_tests.sh
./run_functional_tests.sh
./run_integration_tests.sh

# 4. 生成覆盖率报告
echo "[4/5] 生成覆盖率报告..."
lcov --capture --directory . --output-file $OUTPUT_DIR/coverage.info
lcov --extract $OUTPUT_DIR/coverage.info '*/src/*' --output-file $OUTPUT_DIR/coverage_filtered.info
genhtml $OUTPUT_DIR/coverage_filtered.info --output-directory $OUTPUT_DIR/html

# 5. 生成覆盖率摘要
echo "[5/5] 生成覆盖率摘要..."
lcov --summary $OUTPUT_DIR/coverage_filtered.info 2>&1 | tee $OUTPUT_DIR/summary.txt

echo "============================================="
echo "代码覆盖度统计完成！"
echo "报告: $OUTPUT_DIR/html/index.html"
echo "============================================="
```

### 11.3 覆盖度报告示例

```
=============================================
HFSSS PCIe/NVMe代码覆盖度报告
=============================================
日期: 2026-03-08
模块: pcie_nvme_emulation

总体统计:
  行数:     10,000
  已覆盖:   9,250
  覆盖率:   92.5% ✓

  函数:     200
  已覆盖:   195
  覆盖率:   97.5% ✓

  分支:     500
  已覆盖:   440
  覆盖率:   88.0% ✓

按文件统计:
  pci.c:     95.0% ✓
  nvme.c:    93.5% ✓
  queue.c:   91.0% ✓
  msix.c:    90.5% ✓
  dma.c:     94.0% ✓
  shmem.c:   92.0% ✓
  admin.c:   89.5% ⚠
  io.c:      91.5% ✓

结论: 整体覆盖度达标 ✓
=============================================
```

---

## 12. 测试工具与环境

### 12.1 测试工具清单

| 工具 | 用途 | 来源 |
|------|------|------|
| CUnit | C语言单元测试框架 | 开源 |
| Google Test | C++单元测试框架 | 开源 |
| pytest | Python测试框架 | 开源 |
| fio | IO性能测试工具 | 开源 |
| nvme-cli | NVMe管理工具 | 开源 |
| lspci | PCI设备列表工具 | 开源 |
| gcov/lcov | 代码覆盖度工具 | 开源 |
| clang-format | 代码格式化工具 | 开源 |
| cppcheck | 静态代码分析工具 | 开源 |
| clang-analyzer | 静态代码分析工具 | 开源 |
| perf | 性能分析工具 | 开源 |
| trace-cmd | Trace工具 | 开源 |

### 12.2 测试环境配置

```bash
#!/bin/bash
# setup_test_env.sh - 测试环境配置脚本

echo "============================================="
echo "配置HFSSS测试环境"
echo "============================================="

# 安装依赖
echo "[1/4] 安装依赖包..."
apt-get update
apt-get install -y \
    build-essential \
    linux-headers-$(uname -r) \
    cunit \
    libcunit1-dev \
    fio \
    nvme-cli \
    pciutils \
    lcov \
    clang-format \
    cppcheck \
    clang \
    trace-cmd \
    linux-tools-common \
    linux-tools-$(uname -r) \
    python3-pytest

# 配置内核参数
echo "[2/4] 配置内核参数..."
cat > /etc/sysctl.d/99-hfsss.conf << EOF
# 允许IO调度器调整
kernel.sched_rt_runtime_us = -1
# 增加文件描述符限制
fs.nr_open = 1048576
fs.file-max = 1048576
EOF

sysctl -p /etc/sysctl.d/99-hfsss.conf

# 配置用户限制
echo "[3/4] 配置用户限制..."
cat > /etc/security/limits.d/99-hfsss.conf << EOF
* soft memlock unlimited
* hard memlock unlimited
* soft nofile 1048576
* hard nofile 1048576
EOF

# 隔离CPU用于测试
echo "[4/4] 配置isolcpus..."
# 注意: 需要手动修改grub配置添加isolcpus=2-3

echo "============================================="
echo "测试环境配置完成！"
echo "请重启系统以应用内核参数。"
echo "============================================="
```

---

## 13. 测试执行计划

### 13.1 测试阶段

| 阶段 | 任务 | 开始日期 | 结束日期 | 负责人 |
|------|------|----------|----------|--------|
| 阶段1 | 单元测试开发 | 2026-03-10 | 2026-03-15 | 开发组 |
| 阶段2 | 功能测试开发 | 2026-03-15 | 2026-03-20 | 测试组 |
| 阶段3 | 集成测试开发 | 2026-03-20 | 2026-03-25 | 测试组 |
| 阶段4 | 单元测试执行 | 2026-03-25 | 2026-03-26 | 开发组 |
| 阶段5 | 功能测试执行 | 2026-03-26 | 2026-03-28 | 测试组 |
| 阶段6 | 集成测试执行 | 2026-03-28 | 2026-03-30 | 测试组 |
| 阶段7 | 性能测试执行 | 2026-03-30 | 2026-04-01 | 性能测试组 |
| 阶段8 | 压力测试执行 | 2026-04-01 | 2026-04-08 | 测试组 |

---

## 14. 测试结果报告模板

### 14.1 测试摘要报告

```
=============================================
HFSSS PCIe/NVMe设备仿真模块测试报告
=============================================
报告版本: V1.0
测试日期: 2026-03-08
测试人员: 测试组
模块: PCIe/NVMe设备仿真 (LLD_01)

测试摘要:
  单元测试用例:      200+
  单元测试通过:      200
  单元测试失败:      0
  单元测试通过率:    100.0% ✓

  功能测试用例:      100+
  功能测试通过:      100
  功能测试失败:      0
  功能测试通过率:    100.0% ✓

  集成测试用例:      30+
  集成测试通过:      30
  集成测试失败:      0
  集成测试通过率:    100.0% ✓

代码覆盖度:
  行覆盖度:          92.5% ✓
  函数覆盖度:        97.5% ✓
  分支覆盖度:        88.0% ✓

性能测试结果:
  4K随机读 (QD=32):  550K IOPS ✓
  4K随机读 (QD=128): 1.1M IOPS ✓
  4K随机写 (QD=32):  350K IOPS ✓
  4K读延迟 (QD=1):   45μs ✓
  4K写延迟 (QD=1):   90μs ✓

问题统计:
  发现Bug总数:       5
  严重Bug:           0
  主要Bug:           2
  次要Bug:           3
  已修复:            5

结论:
  测试状态:          通过 ✓
  发布建议:          可以发布
=============================================
```

---

## 15. 附录

### 15.1 测试数据生成脚本

```python
#!/usr/bin/env python3
"""测试数据生成脚本"""

import random
import os

def generate_test_pattern(size, pattern="random"):
    """生成测试数据模式"""
    if pattern == "random":
        return bytes(random.randint(0, 255) for _ in range(size))
    elif pattern == "zeros":
        return b'\x00' * size
    elif pattern == "ones":
        return b'\xFF' * size
    elif pattern == "increment":
        return bytes(i % 256 for i in range(size))
    else:
        raise ValueError(f"Unknown pattern: {pattern}")

def generate_test_file(filename, size, pattern="random"):
    """生成测试文件"""
    data = generate_test_pattern(size, pattern)
    with open(filename, "wb") as f:
        f.write(data)

if __name__ == "__main__":
    # 生成4K测试文件
    generate_test_file("test_4k.bin", 4096, "random")
    generate_test_file("test_4k_zeros.bin", 4096, "zeros")
    generate_test_file("test_4k_inc.bin", 4096, "increment")

    # 生成1M测试文件
    generate_test_file("test_1m.bin", 1024*1024, "random")

    print("测试数据生成完成！")
```

---

**文档统计**：
- 总字数：约35,000字
- 测试用例总数：330+
- 单元测试用例：200+
- 功能测试用例：100+
- 集成测试用例：30+
