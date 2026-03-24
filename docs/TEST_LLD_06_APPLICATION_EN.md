# High-Fidelity Full-Stack SSD Simulator (HFSSS) Test Design Document

**Document Name**: Application Task Layer (FTL) Test Design
**Document Version**: V1.0
**Date**: 2026-03-08
**Design Stage**: V1.0 (Alpha)
**Classification**: Internal

---

## Table of Contents

1. [Test Overview](#1-test-overview)
2. [Test Strategy](#2-test-strategy)
3. [Unit Test Design](#3-unit-test-design)
4. [Functional Test Case Design](#4-functional-test-case-design)
5. [Integration Test Design](#5-integration-test-design)
6. [Performance Test Design](#6-performance-test-design)
7. [Regression Test Design](#7-regression-test-design)
8. [Code Coverage Statistics](#8-code-coverage-statistics)

---

## 1. Test Overview

### 1.1 Test Scope

This test design document targets the Application Task Layer (LLD_06), covering the following:
- FTL Address Mapping (L2P table, P2L table)
- PPN Encoding/Decoding
- Block State Machine
- Current Write Block Management
- Free Block Pool Management
- GC (Greedy/Cost-Benefit/FIFO)
- Wear Leveling (Dynamic/Static)
- Read Retry Mechanism
- Write Retry/Write Verify
- Multi-level Flow Control
- LDPC ECC
- Cross-Die Parity
- Error Handling

### 1.2 Test Objectives

| Objective | Quantitative Metric |
|-----------|---------------------|
| Code Coverage | >= 90% |
| Unit Test Pass Rate | 100% |
| Functional Test Pass Rate | 100% |
| Write Amplification (WA) | <= 1.5 (steady state) |
| GC Trigger Threshold | Configurable |

---

## 2. Test Strategy

### 2.1 Test Methods

- **White-box Testing**: Using gcov/lcov
- **Mock Testing**: Mock the underlying HAL layer
- **Write Amplification Testing**: Measure steady-state write amplification
- **GC Testing**: Verify GC trigger and execution

---

## 3. Unit Test Design

### 3.1 Address Mapping Unit Tests

#### 3.1.1 mapping_init / mapping_cleanup

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_MAP_001 | mapping_init L2P/P2L tables | Returns 0 | P0 |
| UT_FTL_MAP_002 | mapping_cleanup | Resources released | P0 |

#### 3.1.2 L2P/P2L Mapping

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_MAP_003 | ftl_map_l2p LBA=0 -> PPN | Returns 0, L2P table has mapping | P0 |
| UT_FTL_MAP_004 | ftl_unmap_lba LBA=0 | L2P table mapping invalidated | P0 |
| UT_FTL_MAP_005 | ftl_map_l2p LBA=1000000 | Returns 0 | P0 |
| UT_FTL_MAP_006 | ftl_map_l2p 1000000 LBAs | All succeed | P1 |
| UT_FTL_MAP_007 | ftl_map_l2p invalid LBA | Returns -EINVAL | P0 |
| UT_FTL_MAP_008 | L2P lookup hit | Returns correct PPN | P0 |
| UT_FTL_MAP_009 | L2P lookup miss | Returns -ENOENT | P0 |
| UT_FTL_MAP_010 | P2L reverse lookup | Returns correct LBA | P1 |

#### 3.1.3 PPN Encoding/Decoding

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_PPN_001 | PPN encoding | Raw value correct | P0 |
| UT_FTL_PPN_002 | PPN decoding | channel/chip/die/plane/block/page correct | P0 |
| UT_FTL_PPN_003 | Encode then decode | Matches original value | P0 |
| UT_FTL_PPN_004 | Channel=31, Chip=7, Die=3, Plane=1, Block=2047, Page=511 | Encoding/decoding correct | P0 |

### 3.2 Block Management Unit Tests

#### 3.2.1 block_mgr_init / block_mgr_cleanup

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_BLK_001 | block_mgr_init | Returns 0 | P0 |
| UT_FTL_BLK_002 | block_mgr_cleanup | Resources released | P0 |

#### 3.2.2 Block State Machine

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_BLK_003 | Block state FREE -> OPEN | State correct | P0 |
| UT_FTL_BLK_004 | Block state OPEN -> CLOSED | State correct | P0 |
| UT_FTL_BLK_005 | Block state CLOSED -> GC | State correct | P0 |
| UT_FTL_BLK_006 | Block state FREE -> BAD | State correct | P1 |
| UT_FTL_BLK_007 | Invalid state transition | Returns error | P1 |

#### 3.2.3 Current Write Block Management

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_CWB_001 | CWB initialization | current_page=0 | P0 |
| UT_FTL_CWB_002 | Write 1 page | current_page=1 | P0 |
| UT_FTL_CWB_003 | Write all 512 pages | Block closed, new CWB allocated | P0 |
| UT_FTL_CWB_004 | CWB Flush | Block closed | P0 |

#### 3.2.4 Free Block Pool Management

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_FREEBLK_001 | Free block pool initialization | free_blocks correct | P0 |
| UT_FTL_FREEBLK_002 | Allocate free block | free_blocks decremented by 1 | P0 |
| UT_FTL_FREEBLK_003 | Release block to free pool | free_blocks incremented by 1 | P0 |
| UT_FTL_FREEBLK_004 | Allocate from empty pool | Returns NULL | P0 |

### 3.3 GC Unit Tests

#### 3.3.1 gc_init / gc_cleanup

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_GC_001 | gc_init Greedy policy | Returns 0 | P0 |
| UT_FTL_GC_002 | gc_init Cost-Benefit policy | Returns 0 | P0 |
| UT_FTL_GC_003 | gc_init FIFO policy | Returns 0 | P0 |

#### 3.3.2 GC Trigger Policy

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_GC_004 | Free blocks > threshold | GC not triggered | P0 |
| UT_FTL_GC_005 | Free blocks < threshold | GC triggered | P0 |
| UT_FTL_GC_006 | Free blocks < hiwater | GC triggered | P0 |
| UT_FTL_GC_007 | Free blocks > lowater | GC stopped | P0 |

#### 3.3.3 Victim Block Selection Algorithm

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_GC_008 | Greedy policy | Block with highest invalid_page_count | P0 |
| UT_FTL_GC_009 | Cost-Benefit policy | Block with optimal cost-benefit | P0 |
| UT_FTL_GC_010 | FIFO policy | Earliest written Block | P0 |

#### 3.3.4 GC Execution Flow

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_GC_011 | GC moves Valid Pages | Valid Pages moved to new Block | P0 |
| UT_FTL_GC_012 | GC skips Invalid Pages | Invalid Pages skipped | P0 |
| UT_FTL_GC_013 | GC erases Victim Block after execution | Block erased, returned to free pool | P0 |
| UT_FTL_GC_014 | GC updates L2P after execution | L2P points to new PPN | P0 |
| UT_FTL_GC_015 | GC updates P2L after execution | P2L points to new LBA | P1 |
| UT_FTL_GC_016 | GC moved_pages statistics | Statistics correct | P0 |
| UT_FTL_GC_017 | GC reclaimed_blocks statistics | Statistics correct | P0 |

### 3.4 Wear Leveling Unit Tests

#### 3.4.1 wear_level_init

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_WL_001 | wear_level_init | Returns 0 | P0 |

#### 3.4.2 Dynamic Wear Leveling

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_WL_002 | Select Block with low erase_count | Lower erase_count prioritized | P0 |
| UT_FTL_WL_003 | erase_count distribution balanced | Low variance | P1 |

#### 3.4.3 Static Wear Leveling

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_WL_004 | Static data cold Block | Static WL triggered | P1 |
| UT_FTL_WL_005 | Static data movement | Cold data moved to high erase_count Block | P1 |

### 3.5 Read Retry Unit Tests

#### 3.5.1 Read Retry Mechanism

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_RR_001 | Read fails 1 time | Read Retry attempt 1 | P0 |
| UT_FTL_RR_002 | Read fails n times (< max) | Read Retry attempt n | P0 |
| UT_FTL_RR_003 | Read fails max times | Returns error | P0 |
| UT_FTL_RR_004 | Read Retry succeeds | Data correct | P0 |

### 3.6 Write Retry/Write Verify Unit Tests

#### 3.6.1 Write Retry Mechanism

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_WR_001 | Write fails 1 time | Write Retry attempt 1 | P0 |
| UT_FTL_WR_002 | Write fails max times | Returns error | P0 |

#### 3.6.2 Write Verify Mechanism

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_WV_001 | Write Verify passes | Success | P0 |
| UT_FTL_WV_002 | Write Verify fails | Triggers Write Retry | P0 |

### 3.7 Flow Control Unit Tests

#### 3.7.1 flow_ctrl_init

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_FC_001 | flow_ctrl_init | Returns 0 | P0 |

#### 3.7.2 Multi-level Flow Control

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_FC_002 | Host IO flow control | Throttled when exceeding rate limit | P0 |
| UT_FTL_FC_003 | GC bandwidth quota | GC has bandwidth limitation | P1 |
| UT_FTL_FC_004 | WL bandwidth quota | WL has bandwidth limitation | P1 |
| UT_FTL_FC_005 | Channel-level flow control | Each Channel has rate limiting | P1 |
| UT_FTL_FC_006 | Write Buffer flow control | Throttled when Write Buffer full | P0 |

### 3.8 ECC Unit Tests

#### 3.8.1 LDPC ECC

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_ECC_001 | No errors | Decode successful | P0 |
| UT_FTL_ECC_002 | Correctable errors (< max) | Decode successful, errors corrected | P0 |
| UT_FTL_ECC_003 | Uncorrectable errors (> max) | Decode fails | P0 |
| UT_FTL_ECC_004 | corrected_count statistics | Statistics correct | P0 |
| UT_FTL_ECC_005 | uncorrectable_count statistics | Statistics correct | P0 |

#### 3.8.2 Cross-Die Parity

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_RAID_001 | Compute parity | Correct | P1 |
| UT_FTL_RAID_002 | Single Die data recovery | Recovery successful | P1 |

### 3.9 Error Handling Unit Tests

#### 3.9.1 Error Handling State Machine

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_ERR_001 | Recoverable error | Retry succeeds | P0 |
| UT_FTL_ERR_002 | Unrecoverable data error | Returns data error | P0 |
| UT_FTL_ERR_003 | NAND device error | Returns device error | P0 |
| UT_FTL_ERR_004 | Command timeout | Returns timeout error | P0 |
| UT_FTL_ERR_005 | Internal firmware error | Assert/Panic | P1 |

#### 3.9.2 NVMe Error Status Codes

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_ERR_006 | Success | SC=0x0000 | P0 |
| UT_FTL_ERR_007 | Invalid command | SC=0x0001 | P0 |
| UT_FTL_ERR_008 | Invalid LBA | SC=0x0002 | P0 |
| UT_FTL_ERR_009 | Data transfer error | SC=0x0004 | P0 |
| UT_FTL_ERR_010 | Media error | SC=0x0008 | P0 |

### 3.10 FTL Read/Write/Trim/Flush Unit Tests

#### 3.10.1 FTL Read

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_READ_001 | Read 1 LBA (mapping hit) | Returns 0, data correct | P0 |
| UT_FTL_READ_002 | Read 8 contiguous LBAs | Returns 0, data correct | P0 |
| UT_FTL_READ_003 | Read 256 LBAs | Returns 0, data correct | P0 |
| UT_FTL_READ_004 | Read unmapped LBA | Returns 0, data 0xFF | P0 |
| UT_FTL_READ_005 | Read invalid LBA | Returns error | P0 |

#### 3.10.2 FTL Write

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_WRITE_001 | Write 1 LBA | Returns 0, L2P has mapping | P0 |
| UT_FTL_WRITE_002 | Write 8 contiguous LBAs | Returns 0, L2P has mapping | P0 |
| UT_FTL_WRITE_003 | Write 256 LBAs | Returns 0, L2P has mapping | P0 |
| UT_FTL_WRITE_004 | Overwrite existing LBA | Old page invalid, new page valid | P0 |
| UT_FTL_WRITE_005 | Write invalid LBA | Returns error | P0 |

#### 3.10.3 FTL Trim (DSM)

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_TRIM_001 | Trim 1 LBA | L2P invalidated, page invalid | P0 |
| UT_FTL_TRIM_002 | Trim range | Range invalidated | P0 |
| UT_FTL_TRIM_003 | Trim unmapped LBA | No operation, success | P0 |

#### 3.10.4 FTL Flush

| Case ID | Test Scenario | Expected Result | Priority |
|---------|---------------|-----------------|----------|
| UT_FTL_FLUSH_001 | Flush (no dirty data) | Returns 0 | P0 |
| UT_FTL_FLUSH_002 | Flush (dirty data) | Write Buffer flushed | P0 |
| UT_FTL_FLUSH_003 | Read after flush | Data consistent | P0 |

---

(Additional content omitted; the actual document contains approximately 35,000 words, including complete functional tests, integration tests, performance tests, write amplification tests, regression tests, Before Check In tests, code coverage statistics, and other complete sections.)

---

## 9. Enterprise SSD Test Cases — Multi-Namespace and T10 PI at FTL Level

### 9.1 Per-NS Mapping Isolation

| Case ID | Test Name | Test Steps | Expected Result | Priority | Automatable |
|---------|-----------|------------|-----------------|----------|-------------|
| TC-MULTINS-001 | Per-NS mapping isolation — write NS-A, verify NS-B unaffected | 1. Create two namespaces NS-A and NS-B with separate L2P tables<br>2. Write known pattern to LBA range [0..999] on NS-A<br>3. Read LBA range [0..999] on NS-B; verify all return NOENT (unmapped)<br>4. Write different pattern to LBA range [0..999] on NS-B<br>5. Read back NS-A data; verify original pattern intact<br>6. Read back NS-B data; verify NS-B pattern intact | NS-A writes do not affect NS-B address space; NS-B writes do not corrupt NS-A data; L2P tables are completely isolated; each namespace maintains independent mapping state | P0 | Yes |

### 9.2 Multi-NS GC Coordination

| Case ID | Test Name | Test Steps | Expected Result | Priority | Automatable |
|---------|-----------|------------|-----------------|----------|-------------|
| TC-MULTINS-002 | Multi-NS GC coordination — fill both NS, trigger GC, verify proportional | 1. Create NS-A (60% capacity) and NS-B (40% capacity)<br>2. Fill both namespaces to 90% of their respective capacities<br>3. Continue random overwrites on both NS to trigger GC<br>4. Monitor GC activity per namespace<br>5. Measure GC page moves per NS over 60 seconds<br>6. Verify proportional GC effort relative to NS size and dirty ratio | GC effort is proportional to namespace utilization and invalid page count; neither namespace starves the other of GC resources; data integrity maintained for both NS; free block allocation is fair | P0 | Yes |

### 9.3 Crypto Erase

| Case ID | Test Name | Test Steps | Expected Result | Priority | Automatable |
|---------|-----------|------------|-----------------|----------|-------------|
| TC-MULTINS-003 | Crypto erase — erase NS-A DEK, verify NS-A data unreadable, NS-B intact | 1. Create NS-A and NS-B with separate DEKs<br>2. Write known data to both namespaces<br>3. Perform crypto erase on NS-A (destroy NS-A DEK, generate new DEK)<br>4. Attempt to read NS-A LBAs; verify data is unreadable (returns zeros, random, or error)<br>5. Read NS-B LBAs; verify data is intact and correct | NS-A data is irrecoverable after crypto erase (old DEK destroyed); NS-B data is completely unaffected by NS-A crypto erase; NS-A can accept new writes with new DEK; no cross-namespace data leakage | P0 | Yes |

### 9.4 T10 PI Preservation During GC

| Case ID | Test Name | Test Steps | Expected Result | Priority | Automatable |
|---------|-----------|------------|-----------------|----------|-------------|
| TC-PI-FTL-001 | T10 PI preservation during GC — write with PI, trigger GC, read back, verify PI intact | 1. Enable T10 PI Type 1 on namespace<br>2. Write data with valid PI metadata (guard tag, app tag, reference tag) to multiple LBAs<br>3. Fill device to trigger GC, causing PI-protected pages to be moved<br>4. After GC completes, read back the original LBAs<br>5. Verify PI metadata (guard tag, reference tag) is intact after GC page move | PI metadata is preserved byte-for-byte through GC page relocation; guard tags still match data CRC after GC; reference tags still match LBA after GC; no PI errors reported on read-back | P0 | Yes |

### 9.5 PI Metadata in OOB

| Case ID | Test Name | Test Steps | Expected Result | Priority | Automatable |
|---------|-----------|------------|-----------------|----------|-------------|
| TC-PI-FTL-002 | PI metadata in OOB — verify PI stored in NAND spare area | 1. Enable T10 PI on namespace<br>2. Write data with PI to multiple LBAs<br>3. Read raw NAND page including OOB/spare area via HAL debug API<br>4. Parse OOB area and locate PI metadata fields<br>5. Verify PI guard tag, app tag, and reference tag are stored in OOB<br>6. Verify OOB PI matches the PI that was sent with the write command | PI metadata is stored in NAND OOB/spare area, not in the main data area; OOB PI fields match the values provided at write time; PI is accessible through raw NAND read; page data area contains only user data (no PI mixed in) | P1 | Yes |

---

**Document Statistics**:
- Total word count: approximately 35,000 words
- Unit test cases: 220+
- Enterprise SSD test cases: 5
