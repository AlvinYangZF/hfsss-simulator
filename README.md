# HFSSS - High Fidelity Full-Stack SSD Simulator

[![Build and Test](.github/workflows/build.yml/badge.svg)](.github/workflows/build.yml)

HFSSS is a high-fidelity full-stack SSD simulator written in C. It provides a complete simulation stack from NAND flash media up to a simple host interface.

## Features

- **NAND Flash Media Simulation**: TLC/SLC support with timing models, bad block management, and reliability modeling
- **Hardware Abstraction Layer (HAL)**: Clean driver interface for NAND operations
- **Flash Translation Layer (FTL)**: Page-level mapping, garbage collection, wear leveling
- **Common Services**: Logging, memory pools, message queues, semaphores, mutexes
- **362 Test Cases**: All passing ✅

## Quick Start

```bash
# Clone the repository
git clone https://github.com/AlvinYangZF/hfsss-simulator.git
cd hfsss-simulator

# Build everything
make all

# Run tests
make test
```

## Documentation

- **[Architecture Overview](docs/ARCHITECTURE.md)** - Complete English architecture guide
- **[User Guide](docs/USER_GUIDE.md)** - Practical usage examples
- **[README_CODE.md](README_CODE.md)** - Chinese documentation (original)

## Simple Example

```c
#include "sssim.h"

int main(void)
{
    struct sssim_ctx ctx;
    struct sssim_config config;
    u8 write_buf[4096];
    u8 read_buf[4096];

    /* Initialize with default configuration */
    sssim_config_default(&config);

    /* Initialize SSD simulator */
    sssim_init(&ctx, &config);

    /* Write data */
    memset(write_buf, 0xAA, 4096);
    sssim_write(&ctx, 0, 1, write_buf);

    /* Read data back */
    sssim_read(&ctx, 0, 1, read_buf);

    /* Cleanup */
    sssim_cleanup(&ctx);

    return 0;
}
```

## Project Structure

```
.
├── include/           # Header files
│   ├── common/       # Common service headers
│   ├── media/        # Media simulation headers
│   ├── hal/          # HAL headers
│   ├── ftl/          # FTL headers
│   ├── controller/   # Controller headers
│   ├── pcie/         # PCIe/NVMe headers
│   └── sssim.h       # Top-level interface
├── src/              # Source code
├── tests/            # Test files
├── docs/             # Design documents
└── Makefile          # Build system
```

## License

Internal use only.

## Acknowledgments

This project includes comprehensive Chinese design documents (PRD, HLD, LLD, Test Design) totaling over 580,000 words.
