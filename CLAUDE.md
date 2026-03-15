# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview
HFSSS (High Fidelity Full-Stack SSD Simulator) is a complete SSD simulation project that includes full-stack simulation from PCIe/NVMe interface to NAND Flash media.

## Build Commands
- `make all` - Build all libraries and test programs
- `make test` - Build and run all tests
- `make clean` - Clean build directory

## CI/CD
GitHub Actions is set up to automatically build and test on every push and pull request.

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

## Test Status
All 362 tests pass as of the latest build.
