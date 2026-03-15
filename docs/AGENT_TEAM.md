# HFSSS Agent Team

**Version**: V1.0
**Date**: 2026-03-14

---

## Team Structure

The HFSSS project uses a specialized agent team with clear role definitions to tackle the complex SSD simulator implementation.

### 1. Architecture Lead Agent
**Responsibilities**:
- Overall system architecture oversight
- Cross-module integration coordination
- Design decision arbitration
- Technical debt management

**Focus Areas**:
- System-level architecture
- Module interfaces
- Integration patterns

---

### 2. PCIe/NVMe Specialist Agent
**Responsibilities**:
- PCIe configuration space emulation
- NVMe controller register emulation
- Queue management (SQ/CQ)
- MSI-X interrupt emulation
- DMA engine implementation
- Kernel-user space communication

**Files**:
- `include/pcie/pci.h`
- `include/pcie/nvme.h`
- `include/pcie/queue.h`
- `include/pcie/msix.h`
- `include/pcie/dma.h`
- `include/pcie/shmem.h`
- `src/pcie/*.c`

**Requirements Covered**: REQ-001 to REQ-022 (22 requirements)

---

### 3. Controller Specialist Agent
**Responsibilities**:
- Command arbitration
- I/O scheduling
- Write Buffer management
- Read Cache management
- Channel load balancing
- Resource management
- Flow control

**Files**:
- `include/controller/controller.h`
- `include/controller/arbiter.h`
- `include/controller/scheduler.h`
- `include/controller/write_buffer.h`
- `include/controller/read_cache.h`
- `include/controller/channel.h`
- `include/controller/resource.h`
- `include/controller/flow_control.h`
- `include/controller/shmem_if.h`
- `src/controller/*.c`

**Requirements Covered**: REQ-023 to REQ-037 (15 requirements)

---

### 4. Media Specialist Agent
**Responsibilities**:
- NAND hierarchy management
- Timing model implementation
- EAT (Earliest Available Time) engine
- Bad Block Table (BBT)
- Reliability modeling
- NOR Flash emulation
- Data persistence

**Files**:
- `include/media/media.h`
- `include/media/nand.h`
- `include/media/timing.h`
- `include/media/eat.h`
- `include/media/bbt.h`
- `include/media/reliability.h`
- `src/media/*.c`

**Requirements Covered**: REQ-038 to REQ-057 (20 requirements)

---

### 5. HAL Specialist Agent
**Responsibilities**:
- NAND driver implementation
- NOR driver implementation
- PCI management
- Power management
- NVMe/PCIe module management

**Files**:
- `include/hal/hal.h`
- `include/hal/hal_nand.h`
- `include/hal/hal_nor.h`
- `include/hal/hal_pci.h`
- `include/hal/hal_power.h`
- `src/hal/*.c`

**Requirements Covered**: REQ-058 to REQ-069 (12 requirements)

---

### 6. FTL Specialist Agent
**Responsibilities**:
- Address mapping (L2P/P2L)
- Block management
- Garbage Collection (GC)
- Wear Leveling (WL)
- ECC/Error handling
- QoS/Flow control
- Data redundancy

**Files**:
- `include/ftl/ftl.h`
- `include/ftl/mapping.h`
- `include/ftl/block.h`
- `include/ftl/gc.h`
- `include/ftl/wear_level.h`
- `include/ftl/ecc.h`
- `include/ftl/error.h`
- `src/ftl/*.c`

**Requirements Covered**: REQ-094 to REQ-115 (22 requirements)

---

### 7. Common Services Specialist Agent
**Responsibilities**:
- Log system
- Memory pool
- Message queue
- Semaphore/Mutex
- RTOS primitives
- Debug/Assert/Panic
- Configuration management

**Files**:
- `include/common/common.h`
- `include/common/log.h`
- `include/common/mempool.h`
- `include/common/msgqueue.h`
- `include/common/semaphore.h`
- `include/common/mutex.h`
- `src/common/*.c`

**Requirements Covered**: REQ-070 to REQ-093 (24 requirements)

---

### 8. Integration & Test Specialist Agent
**Responsibilities**:
- System integration
- Test framework
- Performance testing
- Stability testing
- CI/CD pipeline
- Documentation

**Files**:
- `tests/*.c`
- `Makefile`
- `.github/workflows/*.yml`
- `docs/*.md`

**Requirements Covered**: REQ-116 to REQ-138 (23 requirements)

---

## Collaboration Model

### Daily Standup
- Each agent provides status update
- Identify blockers
- Coordinate dependencies

### Module Integration Points
1. **PCIe ↔ Controller**: Shared memory Ring Buffer
2. **Controller ↔ FTL**: Command submission/completion
3. **FTL ↔ HAL**: NAND read/program/erase
4. **HAL ↔ Media**: NAND command execution
5. **All ↔ Common**: Logging, memory, synchronization

### Decision Making
- Technical decisions within a module: Specialist Agent leads
- Cross-module decisions: Architecture Lead facilitates
- Major architecture changes: Full team consensus

---

## Current Status

✅ **Team Defined** - All 8 specialist roles defined
🔄 **Analysis Complete** - Requirement coverage analysis done
⬜ **Implementation Phase 1** - Next step: Begin implementation
