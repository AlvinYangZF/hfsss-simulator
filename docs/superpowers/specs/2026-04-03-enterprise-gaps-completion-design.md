# Enterprise Gaps Completion — Design Spec

**Date:** 2026-04-03
**Status:** Draft
**Author:** AlvinYangZF + Claude

## Problem Statement

The hfsss-simulator has all 40 enterprise requirements (REQ-139~178) implemented, and most core requirements coded. However:

1. **Requirements matrix traceability is broken** — all 174 HLD/LLD Reference columns are empty
2. **5 requirements have partial implementations** — REQ-119, REQ-124, REQ-126, REQ-132, REQ-134
3. **NAND geometry defaults are undersized** — 32GB raw NAND doesn't match a realistic enterprise SSD; the Mac Studio has enough DRAM to simulate a much larger device

## Scope

### Workstream 1: Requirements Matrix Traceability

Fill in HLD Reference and LLD Reference columns for all 174 requirements in `REQUIREMENTS_MATRIX_EN.csv`.

**Module → Document Mapping:**

| Module | HLD Reference | LLD Reference(s) |
|--------|---------------|-------------------|
| PCIe/NVMe Device Emulation | HLD_01 | LLD_01 |
| Controller Thread | HLD_02 | LLD_02 |
| Media Thread | HLD_03 | LLD_03 |
| Hardware Abstraction Layer | HLD_04 | LLD_04, LLD_13 |
| Common Platform Layer | HLD_05 | LLD_05, LLD_07, LLD_08, LLD_09, LLD_12, LLD_14, LLD_15 |
| Algorithm Task Layer (FTL) | HLD_06 | LLD_06, LLD_11 |
| Performance Requirements | HLD_06 | LLD_10 |
| Product Interface | HLD_05 | LLD_07, LLD_16 |
| Fault Injection Framework | HLD_05 | LLD_08 |
| System Reliability and Stability | HLD_05 | LLD_11 |
| UPLP | HLD_05 | LLD_17 |
| QoS | HLD_02 | LLD_18 |
| T10 DIF/PI | HLD_06 | LLD_11 |
| Security | HLD_02 | LLD_19 |
| Multi-Namespace Management | HLD_06 | LLD_06 |
| Thermal Management and Telemetry | HLD_05 | LLD_12 |

**Approach:** Python script reads CSV, matches module name → HLD/LLD docs, writes updated CSV. Fine-grained mapping (per-requirement → specific LLD section) applied where the requirement number disambiguates.

### Workstream 2: NAND Geometry Scale-Up

Scale defaults from 32GB to a realistic enterprise QLC SSD profile, since the Mac Studio has ample DRAM.

**Current defaults (32GB raw):**

```
channels=8, chips_per_channel=8, dies_per_chip=2, planes_per_die=2
blocks_per_plane=1024, pages_per_block=256, page_size=4096
```

**New defaults (4TB raw — enterprise QLC SSD profile):**

```
channels=16, chips_per_channel=8, dies_per_chip=4, planes_per_die=2
blocks_per_plane=2048, pages_per_block=512, page_size=16384
```

Calculation: 16 × 8 × 4 × 2 × 2048 × 512 × 16KB = **4TB raw NAND**

With 7% OP → ~3.72TB user capacity (typical enterprise QLC SSD).

**DRAM impact:** The L2P table is the main DRAM consumer. At 16KB page size with 4TB raw:
- Total pages = 16 × 8 × 4 × 2 × 2048 × 512 = 268,435,456 pages
- L2P entry = 8 bytes (u64 PPN) → L2P table = **2GB**
- Plus page metadata, BBT, EAT, block manager → ~3-4GB total DRAM
- Mac Studio has 96GB DRAM and 8TB filesystem — handles this with headroom to spare

**Changes required:**
- `src/common/hfsss_config.c`: Update `hfsss_config_defaults()` with new values
- `include/media/nand.h`: Update `MAX_PAGES_PER_BLOCK` from 512 (already ok), `PAGE_SIZE_TLC` from 16384 (already ok)
- `include/media/bbt.h`: `MAX_BLOCKS_PER_PLANE` from 2048 (already ok)
- `include/media/eat.h`: `MAX_DIES_PER_CHIP` from 4 (already ok)
- Compile-time limits already accommodate the new defaults — no MAX constant changes needed

**Configurable:** Users can still override via YAML config file or keep smaller configs for quick testing.

### Workstream 3: Complete Partial Requirements

#### REQ-119: Resource Utilization Monitoring

**Gap:** Per-core CPU utilization metrics not exported via OOB.

**Design:** Add `resource_get_cpu_stats()` to `src/controller/resource.c` that returns per-role CPU usage (NAND threads, FTL threads, PCIe threads). Export via OOB JSON-RPC method `status.cpu_utilization`. The simulator tracks simulated cycle counts per role, not actual host CPU — this is a firmware-level metric simulation.

**Files:** `src/controller/resource.c`, `include/controller/resource.h`

#### REQ-124: /proc Filesystem Interface (Simulated)

**Gap:** No `/proc/hfsss/` interface.

**Design:** Since this runs on macOS (no procfs), implement as file-based simulation writing to a configurable directory (default: `/tmp/hfsss/proc/`). Files updated periodically:

| Virtual File | Content |
|-------------|---------|
| `status` | Device state, uptime, firmware version |
| `config` | Current NAND geometry, GC policy, QoS settings |
| `perf_counters` | IOPS, bandwidth, latency percentiles |
| `latency_hist` | Latency histogram (read-only) |
| `ftl_stats` | L2P hit rate, GC count, WAF, wear stats |

**Files:** New `src/common/proc_interface.c` + `include/common/proc_interface.h`

Lightweight implementation: ~150 lines. A background thread writes these files every 1 second. Existing `oob.c` already has the data — `proc_interface` is a read-only file exporter that pulls from the same sources.

#### REQ-126: YAML Config File

**Gap:** Need to verify config load/save works end-to-end.

**Design:** `hfsss_config.c` already implements YAML-subset parsing (key: value, sections, comments). The gap is: no test coverage for load/save round-trip. Add test cases in `tests/test_config.c` to verify:
- Write default config → save → reload → compare
- Override nand.channel_count via file → verify post-load value
- Invalid key produces warning, not crash
- Missing file returns error

**Files:** `tests/test_config.c` (extend existing)

#### REQ-132 & REQ-134: Stability Testing Infrastructure

**Gap:** No long-duration stress test harness with failure tracking.

**Design:** New `tests/stress_stability.c` that:
1. Configurable duration (default 60s for CI, `STRESS_DURATION=259200` for 72h)
2. Mixed workload: random read/write (70/30), sequential write bursts, trim
3. Periodic fault injection (bad blocks, bit flips, power loss simulation)
4. Data integrity verification every N operations (md5sum of written vs read)
5. Memory leak tracking: snapshot `malloc` count at start and end, delta < threshold
6. Reports: total ops, errors, integrity failures, memory delta

**Makefile target:** `make stress-long STRESS_DURATION=3600`

**Files:** New `tests/stress_stability.c`, Makefile update

### Test Plan

| Workstream | Verification |
|------------|-------------|
| Traceability | Python script validates no empty HLD/LLD cells remain; spot-check 10 random rows |
| NAND scale-up | `make test` passes with new defaults; verify L2P table allocation succeeds |
| REQ-119 | Unit test: `resource_get_cpu_stats()` returns valid per-role percentages |
| REQ-124 | Unit test: files created in `/tmp/hfsss/proc/`, content is valid JSON/text |
| REQ-126 | Round-trip test: save → load → compare all fields equal |
| REQ-132/134 | Short stress run (60s) completes with 0 integrity failures, memory delta < 1MB |

## Out of Scope

- Actual kernel module `/proc` filesystem (requires Linux)
- Real AES-XTS cryptography (simulator uses XOR-sim by design)
- 720-hour actual test run (we build the harness; the user runs it when needed)

## Dependencies

No new external dependencies. All implementations use existing project patterns and the C standard library.
