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

## Host Interface Backends

While the core simulator is a self-contained library, it exposes its functionality to the outside world (e.g., QEMU) via two primary host interface backends. These backends act as servers, translating standard block protocols into calls to the simulator's internal `nvme_uspace` API.

```
┌──────────────────┐   (Unix Socket)   ┌───────────────────┐
│  QEMU via        ├───────────────────►  hfsss-vhost-blk  │
│ vhost-user-blk-pci │                   │ (vhost_user_blk.c)│
└──────────────────┘                   └────────┬──────────┘
                                                  │
┌──────────────────┐   (TCP Socket)    ┌───────────────────┐
│  QEMU via NBD    ├───────────────────►  hfsss-nbd-server │
└──────────────────┘                   │ (hfsss_nbd_server.c)│
                                         └────────┬──────────┘
                                                  │
                               ┌──────────────────▼──────────────────┐
                               │      nvme_uspace_dev Interface      │
                               │ (read, write, flush, trim)          │
                               └──────────────────┬──────────────────┘
                                                  │
                               ┌──────────────────▼──────────────────┐
                               │         Core SSD Simulator          │
                               │ (FTL, HAL, Media)                   │
                               └─────────────────────────────────────┘
```

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

### 6. Vhost-user Backend (`src/vhost/hfsss_vhost_main.c`)
Exposes the simulator as a `vhost-user-blk` device over a Unix socket, designed for high-performance integration with QEMU.
- **Protocol**: Implemented by linking against an external system library (from QEMU or DPDK development packages).
- **Architecture**: Runs as a separate backend process. QEMU connects as a client using a standard `vhost-user-blk-pci` device.
- **Performance**: Enables zero-copy data transfers between the QEMU guest and the simulator, providing high throughput and low latency.

### 7. NBD Backend (`src/vhost/hfsss_nbd_server.c`)
Exposes the simulator as a standard Network Block Device (NBD) over a TCP socket.
- **Protocol**: A self-contained, from-scratch implementation of the NBD "new-style" handshake and command protocol.
- **Architecture**: Runs as a single-threaded, iterative server that handles one client at a time.
- **Features**: Supports read, write, flush, and trim operations. Includes logic to handle unaligned I/O requests via a read-modify-write process. Can leverage the FTL's multi-threaded I/O submission path.

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

Test output shows 431+ test cases all passing (as of Phase 3 completion).

---

## Architecture Decision: User-Space vs. Kernel Module

### Decision

The project uses a **phased architecture** that starts as a pure user-space library and optionally grows into a kernel module:

- **Phases 0–6 (current path)**: User-space library. Simulates the full SSD internal stack (FTL, HAL, Media, Controller) without requiring any kernel changes. Accessible from test programs and benchmarks via `sssim.h`.
- **Phase 7 (optional, future)**: Linux kernel module (`hfsss_nvme.ko`) that presents a real `/dev/nvme0n1` block device to the host OS, enabling nvme-cli and fio integration.

### Rationale

| Concern | User-Space Choice |
|---------|------------------|
| Development velocity | No kernel build cycle; iterate with `make test` in seconds |
| Portability | Runs on macOS (development) and Linux (CI) without a kernel |
| Safety | Bugs cannot panic the host OS |
| Scope | Research use cases (FTL algorithms, GC, WL, fault injection) are fully satisfied without a real block device |

### Comparison Table

| Aspect | PRD / HLD / LLD Spec | Current Implementation (Phases 0–6) | Phase 7 (optional) |
|--------|----------------------|--------------------------------------|---------------------|
| Host interface | Linux kernel NVMe driver | `sssim.h` user-space API | Real `/dev/nvme0n1` |
| PCIe/NVMe emulation | Full config space, BARs, MSI-X via kernel | User-space NVMe command structures | Full kernel PCI driver |
| Command reception | Shared memory Ring Buffer from kernel | Direct function-call API | Ring Buffer via mmap |
| DMA | `dma_map_page` / IOMMU | Buffer pointer passing | Real kernel DMA |
| Interrupt delivery | `apic->send_IPI` | Synchronous return value | Real MSI-X |
| Thread model | 26+ threads, CPU-pinned, SCHED_FIFO | Configurable threads, no affinity | Full multi-core simulation |
| Compatibility | nvme-cli, fio, kernel NVMe driver | Test programs only | Full compatibility |

### Impact on Open Requirements

Requirements REQ-120 through REQ-122 (block device, nvme-cli, fio) remain ❌ until Phase 7. All other requirements are addressable within the user-space architecture.

### Phase 7 Scope (for planning purposes)

- Implement `hfsss_nvme.ko` as a Linux PCI endpoint function driver
- Map BAR0 MMIO to simulated NVMe registers
- Deliver MSI-X interrupts via `pci_irq_send_affinity_hint`
- Bridge kernel-space command reception to user-space simulator via shared memory
- Estimated effort: 4–6 weeks; gated on Phases 1–6 stability

---

## References

- Original Chinese PRD: `SSD_Simulator_PRD.md`
- Original Chinese HLDs: `docs/HLD_*.md`
- Original Chinese LLDs: `docs/LLD_*.md`
- Test Design: `docs/TEST_*.md`
