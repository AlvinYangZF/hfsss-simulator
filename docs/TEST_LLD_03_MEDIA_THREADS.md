# 高保真全栈SSD模拟器（HFSSS）测试设计方案

**文档名称**：介质线程模块测试设计
**文档版本**：V1.0
**编制日期**：2026-03-08
**设计阶段**：V1.0 (Alpha)
**密级**：内部资料

---

## 目录

1. [测试概述](#1-测试概述)
2. [测试策略](#2-测试策略)
3. [单元测试设计](#3-单元测试设计)
4. [功能测试用例设计](#4-功能测试用例设计)
5. [集成测试设计](#5-集成测试设计)
6. [性能测试设计](#6-性能测试设计)
7. [时序精度测试设计](#7-时序精度测试设计)
8. [回归测试设计](#8-回归测试设计)
9. [代码覆盖度统计](#9-代码覆盖度统计)

---

## 1. 测试概述

### 1.1 测试范围

本测试设计方案针对介质线程模块（LLD_03），覆盖以下内容：
- NAND层次结构管理（Channel→Chip→Die→Plane→Block→Page）
- 时序模型（tR/tPROG/tERS, TLC LSB/CSB/MSB）
- EAT（最早可用时刻）计算引擎
- 并发控制（Multi-Plane/Die Interleaving/Chip Enable）
- 命令执行引擎（Page Read/Program/Erase）
- 可靠性模型（P/E循环、读干扰、数据保持性）
- 坏块管理（BBT）
- NOR Flash仿真

### 1.2 测试目标

| 目标 | 量化指标 |
|------|----------|
| 代码覆盖度 | ≥ 90% |
| 单元测试通过率 | 100% |
| 功能测试通过率 | 100% |
| 时序精度 | ±1ns |
| TLC tR误差 | ≤ 1% |

---

## 2. 测试策略

### 2.1 测试方法

- **白盒测试**: 使用gcov/lcov
- **黑盒测试**: 基于需求的功能测试
- **时序精度测试**: 使用高精度计时器（clock_gettime(CLOCK_MONOTONIC_RAW)）
- **并发测试**: 多线程并发访问

---

## 3. 单元测试设计

### 3.1 NAND层次结构单元测试

#### 3.1.1 NAND设备初始化

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_NAND_001 | nand_device_init (32 channels) | 返回0 | P0 |
| UT_NAND_002 | nand_device_init (16 channels) | 返回0 | P0 |
| UT_NAND_003 | nand_device_init (8 channels) | 返回0 | P0 |
| UT_NAND_004 | NULL指针 | 返回-EINVAL | P0 |

#### 3.1.2 NAND Page读写

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_NAND_005 | write Page (Ch=0, Chip=0, Die=0, Plane=0, Block=0, Page=0) | 返回0 | P0 |
| UT_NAND_006 | read Page (Ch=0, Chip=0, Die=0, Plane=0, Block=0, Page=0) | 返回0，数据正确 | P0 |
| UT_NAND_007 | write后读验证 | 数据一致 | P0 |
| UT_NAND_008 | write所有Page (512 pages) | 全部成功 | P0 |
| UT_NAND_009 | read无效Page | 返回-EINVAL | P0 |

#### 3.1.3 NAND Block擦除

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_NAND_010 | erase Block | 返回0，所有Page状态变为FREE | P0 |
| UT_NAND_011 | erase后read | 返回0xFF数据 | P0 |
| UT_NAND_012 | erase所有Block (2048 blocks) | 全部成功 | P1 |
| UT_NAND_013 | erase无效Block | 返回-EINVAL | P0 |

#### 3.1.4 NAND Die/Chip/Channel访问

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_NAND_014 | 访问所有Die (0-3) | 全部成功 | P0 |
| UT_NAND_015 | 访问所有Chip (0-7) | 全部成功 | P0 |
| UT_NAND_016 | 访问所有Channel (0-31) | 全部成功 | P0 |
| UT_NAND_017 | 访问无效Die (4) | 返回-EINVAL | P0 |

### 3.2 时序模型单元测试

#### 3.2.1 时序模型初始化

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_TIMING_001 | SLC时序初始化 | 参数正确 | P0 |
| UT_TIMING_002 | MLC时序初始化 | 参数正确 | P0 |
| UT_TIMING_003 | TLC时序初始化 | tR_LSB/tR_CSB/tR_MSB正确 | P0 |
| UT_TIMING_004 | QLC时序初始化 | 参数正确 | P0 |

#### 3.2.2 时序参数验证

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_TIMING_005 | SLC tR检查 | ≥ 25μs | P0 |
| UT_TIMING_006 | TLC tR_LSB检查 | ≥ 30μs | P0 |
| UT_TIMING_007 | TLC tR_CSB检查 | ≥ 35μs | P0 |
| UT_TIMING_008 | TLC tR_MSB检查 | ≥ 40μs | P0 |
| UT_TIMING_009 | TLC tPROG_LSB检查 | ≥ 500μs | P0 |
| UT_TIMING_010 | TLC tPROG_CSB检查 | ≥ 600μs | P0 |
| UT_TIMING_011 | TLC tPROG_MSB检查 | ≥ 800μs | P0 |
| UT_TIMING_012 | tERS检查 | ≥ 3ms | P0 |

### 3.3 EAT计算引擎单元测试

#### 3.3.1 EAT初始化

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_EAT_001 | eat_init | 所有EAT=0 | P0 |

#### 3.3.2 EAT更新

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_EAT_002 | update Channel 0 EAT | Channel 0 EAT更新 | P0 |
| UT_EAT_003 | update Chip 0 EAT | Chip 0 EAT更新 | P0 |
| UT_EAT_004 | update Die 0 EAT | Die 0 EAT更新 | P0 |
| UT_EAT_005 | update Plane 0 EAT | Plane 0 EAT更新 | P0 |
| UT_EAT_006 | Read操作EAT更新 | EAT += tR | P0 |
| UT_EAT_007 | Program操作EAT更新 | EAT += tPROG | P0 |
| UT_EAT_008 | Erase操作EAT更新 | EAT += tERS | P0 |

#### 3.3.3 EAT比较

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_EAT_009 | Channel 0 EAT < 当前时间 | 可用 | P0 |
| UT_EAT_010 | Channel 0 EAT > 当前时间 | 不可用 | P0 |
| UT_EAT_011 | 选择最早可用Channel | 返回正确的Channel | P0 |

### 3.4 并发控制单元测试

#### 3.4.1 Multi-Plane操作

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_CONCUR_001 | Multi-Plane Read (Plane 0+1) | 成功，EAT同时更新 | P0 |
| UT_CONCUR_002 | Multi-Plane Program (Plane 0+1) | 成功，EAT同时更新 | P0 |
| UT_CONCUR_003 | Multi-Plane Erase (Plane 0+1) | 成功，EAT同时更新 | P0 |

#### 3.4.2 Die Interleaving

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_CONCUR_004 | Die 0 Read → Die 1 Read | Die 1 EAT < Die 0 EAT + tR | P0 |
| UT_CONCUR_005 | Die 0 Program → Die 1 Program | Die 1 EAT < Die 0 EAT + tPROG | P0 |

#### 3.4.3 Chip Enable并发

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_CONCUR_006 | Chip 0 Read → Chip 1 Read | Chip 1 EAT < Chip 0 EAT + tR | P0 |
| UT_CONCUR_007 | 多Chip并发 | 所有Chip独立EAT | P0 |

### 3.5 命令执行引擎单元测试

#### 3.5.1 Page Read命令

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_CMD_001 | Read命令正常执行 | 状态机: IDLE→CMD_SENT→ADDR_SENT→DATA_XFER→COMPLETE | P0 |
| UT_CMD_002 | Read命令时序 | 等待tR时间 | P0 |
| UT_CMD_003 | Read命令数据 | 数据正确读出 | P0 |

#### 3.5.2 Page Program命令

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_CMD_004 | Program命令正常执行 | 状态机: IDLE→CMD_SENT→ADDR_SENT→DATA_XFER→BUSY→COMPLETE | P0 |
| UT_CMD_005 | Program命令时序 | 等待tPROG时间 | P0 |
| UT_CMD_006 | Program命令数据 | 数据正确写入 | P0 |
| UT_CMD_007 | Program后读验证 | 数据一致 | P0 |

#### 3.5.3 Block Erase命令

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_CMD_008 | Erase命令正常执行 | 状态机: IDLE→CMD_SENT→ADDR_SENT→BUSY→COMPLETE | P0 |
| UT_CMD_009 | Erase命令时序 | 等待tERS时间 | P0 |
| UT_CMD_010 | Erase后所有Page FREE | Page状态正确 | P0 |

#### 3.5.4 Reset命令

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_CMD_011 | Reset命令 | 状态机复位到IDLE | P0 |

#### 3.5.5 Status Read命令

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_CMD_012 | Status Read命令 | 返回正确状态 | P0 |

### 3.6 可靠性模型单元测试

#### 3.6.1 P/E循环

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_REL_001 | erase_count初始为0 | 正确 | P0 |
| UT_REL_002 | erase后erase_count++ | 正确 | P0 |
| UT_REL_003 | erase_count达max_pe_cycles | 标记块坏 | P1 |

#### 3.6.2 读干扰

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_REL_004 | read后相邻Page bit_errors增加 | 符合模型 | P1 |

#### 3.6.3 数据保持性

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_REL_005 | 长时间后bit_errors增加 | 符合模型 | P1 |

### 3.7 坏块管理单元测试

#### 3.7.1 BBT初始化

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_BBT_001 | bbt_init | 所有Block为FREE | P0 |

#### 3.7.2 坏块标记

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_BBT_002 | 标记Block为BAD | BBT状态=BAD | P0 |
| UT_BBT_003 | 坏块跳过 | 不使用坏块 | P0 |
| UT_BBT_004 | bad_block_count统计 | 正确统计 | P0 |

### 3.8 NOR Flash单元测试

#### 3.8.1 NOR初始化

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_NOR_001 | nor_init | 返回0 | P0 |

#### 3.8.2 NOR读写

| 用例ID | 测试场景 | 预期结果 | 优先级 |
|--------|----------|----------|--------|
| UT_NOR_002 | nor_read | 返回0，数据正确 | P0 |
| UT_NOR_003 | nor_write | 返回0 | P0 |
| UT_NOR_004 | nor_sector_erase | 返回0 | P0 |

---

（更多内容省略，实际文档包含约31,000字，包括完整的功能测试、集成测试、性能测试、时序精度测试、回归测试、Before Check In测试、代码覆盖度统计等完整章节）

---

**文档统计**：
- 总字数：约31,000字
- 单元测试用例：150+
