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

## Code Coverage

Coverage measurement is driven by `gcov + lcov` through a parallel `build-cov/` build variant
(`-O0 --coverage`) that does not interfere with the production `-O2` build. Three HTML reports
are produced (UT-only, E2E-only, merged) scoped to HFSSS **firmware** only — `src/vhost/`,
`src/kernel/`, `src/tools/`, `tests/`, and system headers are filtered out via `lcov --remove`.

### Quick start

```bash
# Install lcov (one-time)
brew install lcov         # macOS
sudo apt install lcov     # Linux

# UT coverage only (fast — 1-3 min)
make coverage-ut
open build-cov/coverage/ut/index.html

# Full flow: UT + E2E + merged report (slow — 10-20 min, needs QEMU image)
make coverage

# Run the coverage infrastructure self-tests
make coverage-selftest
```

### CI ratchet

`.coverage-baseline.json` tracks the reference coverage. Every PR runs `make coverage-ut` in CI
(`.github/workflows/coverage.yml`) and **fails** if line, function, or branch coverage drops more
than 2 percentage points below the baseline. See [docs/coverage.md](docs/coverage.md) for baseline
update workflow, full scope details, and troubleshooting.

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

## QEMU NVMe Block Device (Live Simulator)

The simulator can be exposed as a real `/dev/nvme0n1` block device inside a QEMU virtual machine. **Every I/O goes through the full simulator FTL/NAND stack** — this is not a static disk image, but the live simulator processing each read, write, trim, and flush.

### QEMU Black-Box Soak

The repository includes reusable guest-visible black-box runners for `nvme-cli` and `fio`:

```bash
make qemu-blackbox-list
make qemu-blackbox BLACKBOX_ARGS="--guest-dir /path/to/guest --case nvme/001_nvme_cli_smoke.sh"
make qemu-blackbox-soak BLACKBOX_ARGS="--guest-dir /path/to/guest --rounds 10 --nbd-port 10840 --ssh-port 10040"
```

The soak runner starts one isolated `QEMU + hfsss-nbd-server` environment, reuses it across rounds, and stores per-round artifacts under `build/blackbox-tests/...` so intermittent end-to-end failures can be diagnosed after the run.

### Architecture

```
Mac Studio (macOS, Apple Silicon)
│
├── hfsss-nbd-server (C process)     ← Simulator with full FTL/NAND stack
│   ├── NBD protocol server (TCP :10809)
│   ├── nvme_uspace_dev (NVMe command handling)
│   └── sssim_ctx (FTL → GC → Wear Leveling → NAND media)
│
└── QEMU (HVF hardware-accelerated)
    ├── -device nvme              NVMe controller emulation
    ├── -drive driver=nbd         NBD client → simulator
    └── Linux Guest VM (Alpine Linux)
        ├── /dev/nvme0n1          Real NVMe block device
        └── fio, nvme-cli         Standard NVMe tooling
```

**Data path:** `fio → /dev/nvme0n1 → Linux NVMe driver → QEMU NVMe → NBD TCP → hfsss-nbd-server → FTL → NAND simulation`

### One-Command Quick Start

After initial setup, launch everything with one command:

```bash
./scripts/start_nvme_test.sh
```

This starts the NBD server (simulator), boots QEMU, waits for SSH, and prints connection instructions. `Ctrl-C` to shut everything down.

SSH into the guest:
```bash
ssh -i /tmp/hfsss_qemu_key -p 2222 root@127.0.0.1
```

### Initial Setup (One-Time)

```bash
# 1. Install QEMU
brew install qemu

# 2. Build the simulator
make all

# 3. Download and prepare Alpine Linux guest image
cd guest/
curl -LO https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/cloud/generic_alpine-3.21.2-aarch64-uefi-cloudinit-r0.qcow2
mv generic_alpine-3.21.2-aarch64-uefi-cloudinit-r0.qcow2 alpine.qcow2
dd if=/dev/zero of=ovmf_vars.fd bs=1m count=64

# 4. Create cloud-init ISO with SSH key
ssh-keygen -t ed25519 -f /tmp/hfsss_qemu_key -N "" -q
mkdir -p cidata
cat > cidata/meta-data << 'EOF'
instance-id: hfsss-003
local-hostname: hfsss
EOF
cat > cidata/user-data << EOF
#cloud-config
ssh_pwauth: true
disable_root: false
chpasswd:
  expire: false
  users:
    - name: root
      password: hfsss
      type: text
ssh_authorized_keys:
  - $(cat /tmp/hfsss_qemu_key.pub)
EOF
hdiutil makehybrid -iso -joliet -default-volume-name cidata -o cidata.iso cidata/
cd ..

# 5. First boot: install fio, then save the image
# (follow the manual QEMU launch below, install fio, then save)
```

### Manual QEMU Launch

```bash
# Terminal 1: Start NBD server (simulator in data path)
./build/bin/hfsss-nbd-server -p 10809 -s 512 2>&1 | tee nbd_server.log

# Terminal 2: Start QEMU
qemu-system-aarch64 \
    -M virt,gic-version=3 -accel hvf -cpu host \
    -m 2G -smp 2 \
    -drive if=pflash,format=raw,file=/opt/homebrew/share/qemu/edk2-aarch64-code.fd,readonly=on \
    -drive if=pflash,format=raw,file=guest/ovmf_vars.fd \
    -drive file=guest/alpine-hfsss.qcow2,if=virtio,format=qcow2,snapshot=on \
    -drive file=guest/cidata.iso,if=virtio,media=cdrom \
    -drive "driver=nbd,server.type=inet,server.host=127.0.0.1,server.port=10809,if=none,id=nvm0,discard=unmap" \
    -device nvme,serial=HFSSS0001,drive=nvm0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0 \
    -nographic
```

### fio Test Examples

```bash
# SSH into the guest
ssh -i /tmp/hfsss_qemu_key -p 2222 root@127.0.0.1

# Random 4K mixed R/W — 60 seconds
fio --name=mixed --rw=randrw --rwmixread=70 --bs=4k --size=512M \
    --filename=/dev/nvme0n1 --direct=1 --ioengine=sync \
    --runtime=60 --time_based

# Sequential write — 60 seconds
fio --name=seq_write --rw=write --bs=128k --size=512M \
    --filename=/dev/nvme0n1 --direct=1 --ioengine=sync \
    --runtime=60 --time_based

# Long stress with trim — 30 minutes
fio --name=stress --rw=randrw --rwmixread=60 --bs=4k --size=512M \
    --filename=/dev/nvme0n1 --direct=1 --ioengine=sync \
    --trim_percentage=20 --runtime=1800 --time_based --loops=0

# NVMe admin commands
apk add nvme-cli
nvme list
nvme id-ctrl /dev/nvme0
nvme smart-log /dev/nvme0
```

### Simulator Logs

The NBD server logs all I/O activity to `nbd_server.log`. On the Mac host:

```bash
tail -f nbd_server.log          # Live I/O log
ps aux | grep hfsss-nbd         # Check CPU/memory usage
```

### Saving the Guest Image

After installing packages (fio, nvme-cli), save the image so future runs skip setup:

```bash
# Stop QEMU first, then:
qemu-img convert -O qcow2 guest/alpine-fresh.qcow2 guest/alpine-hfsss.qcow2
cp guest/ovmf_vars.fd guest/ovmf_vars-saved.fd
```

The `start_nvme_test.sh` script uses `snapshot=on` so the base image stays clean between runs.

### Extensible Guest Black-Box Test Runner

For repeatable guest-visible NVMe regression testing, use the black-box runner:

```bash
./scripts/run_qemu_blackbox_tests.sh --list
./scripts/run_qemu_blackbox_tests.sh --guest-dir /path/to/guest --mode mt
./scripts/run_qemu_blackbox_tests.sh --guest-dir /path/to/guest --case nvme/001_nvme_cli_smoke.sh
./scripts/run_qemu_blackbox_ci.sh --guest-dir /path/to/guest --skip-build
```

This framework starts `hfsss-nbd-server`, boots QEMU, runs discovered guest-side test cases, and stores per-case artifacts under `build/blackbox-tests/`.

The default QEMU runtime is tuned for Apple Silicon macOS. For Linux or other local environments, override `HFSSS_QEMU_BIN`, `HFSSS_QEMU_ACCEL`, and `HFSSS_QEMU_CPU` instead of editing the cases.

If the default test ports are busy because other environments are running, the runner will automatically choose the next free ports and record the resolved values in `environment.txt` inside the suite artifact directory.

See [docs/QEMU_BLACKBOX_TESTING.md](docs/QEMU_BLACKBOX_TESTING.md) for the framework layout and how to add new `nvme-cli`, `fio`, or future `SPDK` cases.

## CI/CD

GitHub Actions runs on every push and pull request:
- Matrix build on **Ubuntu** and **macOS**
- Full build (`make all`) + unit tests (`make test`)
- Clean rebuild verification

## License

This project is licensed under the [MIT License](LICENSE).
