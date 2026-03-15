# HFSSS Architecture Overview (English)

**Project**: High Fidelity Full-Stack SSD Simulator (HFSSS)
**Version**: 1.0

---

## Overview

HFSSS is a high-fidelity SSD simulator written in C that models a complete SSD stack from the host interface down to the NAND flash media. While the original design documents describe a Linux kernel module architecture, the current implementation is a pure user-space library that provides:

- NAND flash media simulation with timing models
- Hardware Abstraction Layer (HAL)
- Flash Translation Layer (FTL) with garbage collection
- Common services (logging, memory pools, message queues, etc.)
- A top-level SSD simulator interface

---

## Actual Implemented Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                     User Application                              │
│  (tests, custom programs using sssim.h)                         │
└────────────────────────┬────────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────────┐
│                   Top-Level Interface (sssim.h)                  │
│  sssim_init(), sssim_read(), sssim_write(), sssim_trim()       │
└────────────────────────┬────────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────────┐
│                   FTL Layer (ftl.h)                              │
│  - Address Mapping (L2P/P2L tables)                             │
│  - Block Management                                               │
│  - Garbage Collection (Greedy algorithm)                         │
│  - Wear Leveling                                                  │
└────────────────────────┬────────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────────┐
│                   HAL Layer (hal.h)                              │
│  - NAND Driver API                                                │
│  - NOR Driver (stub)                                              │
│  - PCI Management (stub)                                          │
│  - Power Management (stub)                                        │
└────────────────────────┬────────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────────┐
│              Media Layer (media.h)                                │
│  - NAND Hierarchy (Channel → Chip → Die → Plane → Block → Page)│
│  - Timing Model (tR, tPROG, tERS)                                │
│  - EAT (Earliest Available Time) Engine                          │
│  - Bad Block Table (BBT)                                          │
│  - Reliability Model (PE cycles, read disturb)                   │
└──────────────────────────────────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────────┐
│              Common Services (common/)                            │
│  - Log System (log.h)                                             │
│  - Memory Pool (mempool.h)                                        │
│  - Message Queue (msgqueue.h)                                     │
│  - Semaphore (semaphore.h)                                        │
│  - Mutex (mutex.h)                                                │
└──────────────────────────────────────────────────────────────────┘
```

---

## Module Descriptions

### 1. Top-Level Interface (`sssim.h`)
Provides a simple interface for applications to interact with the simulated SSD:
- `sssim_init()` - Initialize the SSD simulator
- `sssim_read()` - Read LBAs from the SSD
- `sssim_write()` - Write LBAs to the SSD
- `sssim_trim()` - Discard LBAs (TRIM command)
- `sssim_flush()` - Flush all pending writes
- `sssim_get_stats()` - Get FTL statistics

### 2. FTL Layer (`ftl/`)
Implements the Flash Translation Layer:
- **Mapping**: Page-level L2P (Logical to Physical) mapping
- **Block Management**: Block state tracking, current write block management
- **Garbage Collection**: Greedy victim selection, valid page migration
- **Wear Leveling**: Basic wear leveling support
- **ECC/Error Handling**: Stub implementations

### 3. HAL Layer (`hal/`)
Hardware Abstraction Layer:
- **NAND Driver**: Synchronous NAND read/program/erase operations
- **NOR Driver**: Stub implementation
- **PCI Management**: Stub implementation
- **Power Management**: Stub implementation

### 4. Media Layer (`media/`)
NAND flash media simulation:
- **NAND Hierarchy**: Models Channels, Chips, Dies, Planes, Blocks, Pages
- **Timing Model**: TLC/MLC/SLC timing with LSB/CSB/MSB latency differences
- **EAT Engine**: Tracks earliest available time for each NAND component
- **Bad Block Table**: Initial bad blocks, dynamic bad block marking
- **Reliability Model**: PE cycle tracking, read disturb simulation

### 5. Common Services (`common/`)
Utility modules used by all other layers:
- **Log System**: Multi-level logging (DEBUG/INFO/WARN/ERROR)
- **Memory Pool**: Fixed-size block allocator for performance
- **Message Queue**: Thread-safe queue for inter-component communication
- **Semaphore**: Counting semaphore implementation
- **Mutex**: Recursive mutex implementation

---

## Data Flow Examples

### Write Operation
1. Application calls `sssim_write(ctx, lba, count, data)`
2. SSSIM layer validates parameters and calls `ftl_write()`
3. FTL layer:
   - Allocates physical pages
   - Updates L2P mapping
   - Calls `hal_nand_program_sync()`
4. HAL layer calls `media_nand_program()`
5. Media layer:
   - Calculates EAT (timing)
   - Stores data in DRAM buffer
   - Updates statistics

### Read Operation
1. Application calls `sssim_read(ctx, lba, count, data)`
2. SSSIM layer calls `ftl_read()`
3. FTL layer:
   - Looks up L2P mapping
   - Calls `hal_nand_read_sync()`
4. HAL layer calls `media_nand_read()`
5. Media layer retrieves data from DRAM buffer

---

## Configuration

Default SSD configuration (from `sssim_config_default()`):
- Total LBAs: 262,144 (1GB with 4KB LBA size)
- LBA Size: 4096 bytes
- NAND Page Size: 4096 bytes
- Pages per Block: 256
- Blocks per Plane: 1024
- Planes per Die: 2
- Dies per Chip: 2
- Chips per Channel: 2
- Channel Count: 2
- Over-provisioning: 10%

---

## Building and Testing

```bash
# Build everything
make all

# Run all tests
make test

# Clean build artifacts
make clean
```

Test output shows 362 test cases all passing.

---

## Differences from Original Design Documents

The original Chinese design documents (HLD/LLD) describe a more ambitious system:

| Aspect | Original Design | Actual Implementation |
|--------|-----------------|----------------------|
| Host Interface | Linux kernel module + real NVMe driver | User-space library only |
| PCIe/NVMe Emulation | Full PCIe config space, BARs, MSI-X | Simplified user-space structures |
| Controller Thread | Full multi-threaded controller | Simplified synchronous interface |
| Write Buffer | Full write buffer with background flush | No write buffer (writes go directly) |
| Read Cache | LRU read cache | No read cache |

The current implementation focuses on the core SSD components (FTL, HAL, Media) that are most useful for SSD research.

---

## References

- Original Chinese PRD: `SSD_Simulator_PRD.md`
- Original Chinese HLDs: `docs/HLD_*.md`
- Original Chinese LLDs: `docs/LLD_*.md`
- Test Design: `docs/TEST_*.md`
