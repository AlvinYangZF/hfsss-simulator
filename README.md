# HFSSS - High Fidelity Full-Stack SSD Simulator

[![Build and Test](https://github.com/AlvinYangZF/hfsss-simulator/actions/workflows/build.yml/badge.svg)](https://github.com/AlvinYangZF/hfsss-simulator/actions/workflows/build.yml)

HFSSS is a high-fidelity, full-stack SSD simulator written in pure C. It models a complete SSD from the NVMe host interface down to individual NAND flash pages, with all data stored in DRAM for fast simulation. The default configuration simulates a **1 TB TLC SSD** with 8 channels and 20% over-provisioning.

## Features

- **NVMe Interface**: User-space NVMe device with admin commands (Identify, Get/Set Features, Get Log Page, Firmware Update, Format NVM, Sanitize) and IO commands (Read, Write, Flush, Trim/Deallocate)
- **SSD Controller**: Command arbitration, scheduling (FIFO/Greedy/Deadline/WRR), write buffering, read caching, flow control with backpressure and QoS
- **Flash Translation Layer**: Page-level L2P/P2L mapping, garbage collection (Greedy, Cost-Benefit, FIFO policies), dynamic and static wear leveling, ECC framework, read retry with voltage offset
- **NAND Flash Media**: Full hierarchy simulation (channel/chip/die/plane/block/page), TLC/MLC/SLC/QLC timing models, Earliest Available Time (EAT) engine, bad block table, reliability model (PE aging, read disturb, data retention)
- **Hardware Abstraction Layer**: Clean synchronous driver interface for NAND, NOR flash, PCI, and power management
- **Common Services**: Logging, memory pools, message queues, mutexes, semaphores, configuration parser, fault injection framework, bootloader, OOB management, real-time services, watchdog
- **Persistence**: Media save/load, incremental checkpointing, checkpoint restore
- **Performance Validation**: Built-in benchmark engine with IOPS, bandwidth, latency histograms, and percentile measurements

## Architecture

```
┌──────────────────────────────────────────────────┐
│              Application / Tests                  │
├──────────────────────────────────────────────────┤
│          NVMe User-Space Interface                │
│  (Admin + IO commands, Queue management)          │
├──────────────────────────────────────────────────┤
│              SSD Controller                       │
│  (Arbiter, Scheduler, Write Buffer, Read Cache)   │
├──────────────────────────────────────────────────┤
│         Flash Translation Layer (FTL)             │
│  (L2P Mapping, GC, Wear Leveling, ECC)            │
├──────────────────────────────────────────────────┤
│      Hardware Abstraction Layer (HAL)             │
├──────────────────────────────────────────────────┤
│           NAND Flash Media Simulation             │
│  (Timing, Reliability, BBT, EAT)                  │
├──────────────────────────────────────────────────┤
│              Common Services                      │
│  (Logging, MemPool, Mutex, Config, Fault Inject)  │
└──────────────────────────────────────────────────┘
```

## Default SSD Configuration

| Parameter | Value |
|---|---|
| Raw Capacity | 1 TB |
| User Capacity | ~800 GB (20% OP) |
| NAND Type | TLC |
| Channels | 8 |
| Chips / Channel | 4 |
| Dies / Chip | 2 |
| Planes / Die | 2 |
| Blocks / Plane | 2048 |
| Pages / Block | 256 |
| Page Size | 16 KB |
| LBA Size | 4 KB |
| GC Policy | FIFO |
| Data Storage | DRAM (heap memory) |

All NAND page data is stored in `malloc`'d buffers in memory. Optional file-based persistence is available via `media_save()` / `media_load()` for checkpointing.

## Quick Start

```bash
# Clone the repository
git clone https://github.com/AlvinYangZF/hfsss-simulator.git
cd hfsss-simulator

# Build everything (8 libraries + 28 test/stress programs)
make all

# Run unit tests (1,150+ assertions)
make test

# Run system-level tests (616 assertions)
make systest
```

### Build Requirements

- GCC 5+ or Clang 3.8+ (C11 standard)
- POSIX threads (pthread)
- Linux: librt (for timers)
- No external dependencies

### Supported Platforms

- Linux (x86_64, ARM)
- macOS (Apple Silicon, x86_64)

## API Example

```c
#include "sssim.h"

int main(void)
{
    struct sssim_ctx ctx;
    struct sssim_config config;
    u8 write_buf[4096], read_buf[4096];

    sssim_config_default(&config);
    config.channel_count = 2;       /* smaller config for demo */
    config.chips_per_channel = 1;
    config.blocks_per_plane = 128;
    config.total_lbas = 1024;

    sssim_init(&ctx, &config);

    /* Write, read, verify */
    memset(write_buf, 0xAA, 4096);
    sssim_write(&ctx, 0, 1, write_buf);
    sssim_read(&ctx, 0, 1, read_buf);

    /* Trim and flush */
    sssim_trim(&ctx, 0, 1);
    sssim_flush(&ctx);

    /* Check statistics */
    struct ftl_stats stats;
    sssim_get_stats(&ctx, &stats);

    sssim_cleanup(&ctx);
    return 0;
}
```

### NVMe User-Space API

```c
#include "pcie/nvme_uspace.h"

struct nvme_uspace_dev dev;
struct nvme_uspace_config cfg;
nvme_uspace_config_default(&cfg);

nvme_uspace_dev_init(&dev, &cfg);
nvme_uspace_dev_start(&dev);
nvme_uspace_create_io_cq(&dev, 1, 256, false);
nvme_uspace_create_io_sq(&dev, 1, 256, 1, 0);

/* NVMe IO commands */
nvme_uspace_write(&dev, 1, lba, count, data);
nvme_uspace_read(&dev, 1, lba, count, data);
nvme_uspace_trim(&dev, 1, &dsm_range, 1);
nvme_uspace_flush(&dev, 1);

/* NVMe Admin commands */
nvme_uspace_identify_ctrl(&dev, &ctrl_id);
nvme_uspace_identify_ns(&dev, 1, &ns_id);
nvme_uspace_format_nvm(&dev, 1);
nvme_uspace_sanitize(&dev, sanact);
nvme_uspace_fw_download(&dev, offset, data, len);
nvme_uspace_fw_commit(&dev, slot, action);
nvme_uspace_get_log_page(&dev, 1, lid, buf, len);
nvme_uspace_get_features(&dev, fid, &value);
nvme_uspace_set_features(&dev, fid, value);
```

## Testing

### Test Summary

| Category | Files | Assertions | Description |
|---|---|---|---|
| Unit Tests | 20 | 1,150+ | Per-module coverage across all layers |
| Stress Tests | 4 | — | 10-30 minute sustained workloads |
| System Tests | 3 | 616 | End-to-end integration tests |
| **Total** | **27** | **1,750+** | |

### Unit Tests (`make test`)

| Module | Test File | Coverage |
|---|---|---|
| Common | test_common | Logging, mempool, mutex, semaphore, memory, watchdog |
| Media | test_media | NAND hierarchy, timing, BBT, reliability, persistence |
| HAL | test_hal | NAND/NOR operations, power states, PCI |
| FTL | test_ftl | Mapping, block management, GC, integrity |
| Controller | test_controller | Arbiter, scheduler, write buffer, read cache, flow control |
| PCIe/NVMe | test_pcie_nvme | Registers, queues, DMA, MSI-X |
| SSSIM | test_sssim | End-to-end read/write/trim/flush |
| Specialized | 13 files | NVMe uspace, boot, NOR flash, FTL reliability, RT services, OOB, config, fault injection, reliability, perf validation, DSM, PRP, FTL integrity |

### Stress Tests

| Test | Duration | Workload |
|---|---|---|
| stress_rw | 15 min | Random write with data integrity verification |
| stress_mixed | 10 min | Mixed read/write with configurable ratio |
| stress_mixed_trim | 30 min | Mixed read/write/trim with NOENT verification |
| stress_admin_mix | 30 min | NVMe admin commands interleaved with IO |

### System Tests (`make systest`)

| Test | Assertions | Coverage |
|---|---|---|
| systest_data_integrity | 32 | E2E data path, GC integrity at 95% utilization, trim correctness, format/sanitize completeness, multi-LBA atomicity |
| systest_nvme_compliance | 48 | Identify, Features, SMART log, FW update, queue stress, boundary IO, edge cases, multi-range trim |
| systest_error_boundary | 536 | NOSPC recovery, min/max geometry, low OP (5%), all NAND types (SLC/MLC/TLC/QLC), edge LBA values |

## Project Structure

```
.
├── include/               # Header files
│   ├── common/           # Common service headers
│   ├── media/            # NAND/NOR media headers
│   ├── hal/              # HAL headers
│   ├── ftl/              # FTL headers (mapping, gc, wear_level, ecc, error)
│   ├── controller/       # Controller headers (arbiter, scheduler, cache)
│   ├── pcie/             # PCIe/NVMe headers (nvme, queue, dma, prp, msix)
│   ├── perf/             # Performance validation headers
│   └── sssim.h           # Top-level simulator interface
├── src/                   # Source code (~16,300 lines)
│   ├── common/           # 14 source files
│   ├── media/            # 7 source files
│   ├── hal/              # 5 source files
│   ├── ftl/              # 8 source files
│   ├── controller/       # 9 source files
│   ├── pcie/             # 12 source files
│   ├── perf/             # 1 source file
│   └── sssim.c           # Top-level entry point
├── tests/                 # 27 test files
├── docs/                  # 47 design documents
│   ├── ARCHITECTURE.md   # English architecture overview
│   ├── USER_GUIDE.md     # Practical usage guide
│   ├── SYSTEM_TEST_PLAN.md # 73-case system test plan
│   ├── HLD_*.md          # High-level design (6 modules)
│   ├── LLD_*.md          # Low-level design (16 modules)
│   └── TEST_*.md         # Test design documents
├── .github/workflows/     # CI/CD (build + test on Ubuntu & macOS)
├── Makefile               # Build system
└── SSD_Simulator_PRD.md   # Product requirements document
```

### Libraries Built

| Library | Purpose |
|---|---|
| libhfsss-common.a | Common utilities, logging, synchronization |
| libhfsss-media.a | NAND/NOR flash media simulation |
| libhfsss-hal.a | Hardware abstraction layer |
| libhfsss-ftl.a | Flash translation layer |
| libhfsss-controller.a | SSD controller components |
| libhfsss-pcie.a | PCIe/NVMe protocol stack |
| libhfsss-sssim.a | Top-level simulator interface |
| libhfsss-perf.a | Performance validation framework |

## Documentation

- **[Architecture Overview](docs/ARCHITECTURE.md)** — System architecture and layer interactions
- **[User Guide](docs/USER_GUIDE.md)** — API usage with code examples
- **[System Test Plan](docs/SYSTEM_TEST_PLAN.md)** — 73-case test plan across 10 categories
- **[README_CODE.md](README_CODE.md)** — Chinese documentation (original)

## CI/CD

GitHub Actions runs on every push and pull request:
- Matrix build on **Ubuntu** and **macOS**
- Full build (`make all`) + unit tests (`make test`)
- Clean rebuild verification

## License

Internal use only.
