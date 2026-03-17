# HFSSS System-Level Test Plan

## 1. Test Plan Overview

### 1.1 Objectives

This test plan defines system-level tests that exercise the HFSSS simulator end-to-end across its full stack: NVMe user-space interface, FTL (mapping, GC, wear leveling, ECC, error handling), HAL, and NAND media simulation. The goal is to close 15 identified coverage gaps that unit tests and the four existing stress tests do not address.

### 1.2 Scope

**In scope:**
- All test categories listed in section 2, exercising the full stack through the `sssim_*` and `nvme_uspace_*` APIs.
- Configurations spanning multiple NAND geometries, GC policies, NAND types, and over-provisioning ratios.
- Automated pass/fail criteria for every test case.

**Out of scope:**
- Kernel module testing (`src/kernel/hfsss_nvme_kmod.c` and related).
- OOB management server (socket-based JSON-RPC testing).
- Host OS driver integration.

### 1.3 Approach

All system-level tests are standalone C programs following the existing pattern established by `stress_rw.c`, `stress_mixed_trim.c`, `stress_admin_mix.c`, and the unit test harness (`TEST_ASSERT` macro). Each test initializes the full stack via `nvme_uspace_dev_init` (or `sssim_init` for FTL-level tests), runs its workload, checks invariants, and exits 0 on pass, 1 on fail. Tests are added to the Makefile and can be run individually or as a suite.

### 1.4 Test Environments

| Environment | Description | When Used |
|---|---|---|
| **Dev local** | macOS/Linux, `make all`, single machine | During development |
| **CI fast** | GitHub Actions, unit + P0 system tests | Every push/PR |
| **CI nightly** | GitHub Actions (extended), P0 + P1 tests | Nightly schedule |
| **CI weekly** | Self-hosted runner, full suite including P2/P3 and long-duration stress | Weekly schedule |

### 1.5 Conventions

- **Test file naming**: `systest_<category>_<name>.c` placed in `tests/`
- **Binary naming**: `$(BIN_DIR)/systest_<category>_<name>`
- **All tests**: deterministic (seeded PRNG), self-contained, no external dependencies beyond the HFSSS libraries.
- **Timeout enforcement**: each test declares its expected wall-clock limit; CI enforces it.

---

## 2. Test Categories

---

### Category A: Data Integrity and Correctness [P0]

**Description:** Verify that data written through the full NVMe-to-NAND stack is returned correctly under all conditions including GC, trim, format, and high utilization. This is the most critical category because any failure here represents silent data corruption.

**Estimated complexity: M-L**

| ID | Name | Description | Preconditions | Test Steps | Expected Results | Pass/Fail Criteria |
|---|---|---|---|---|---|---|
| **DI-001** | End-to-end data path, all layers | Write known patterns through `nvme_uspace_write`, read back through `nvme_uspace_read`, verify every byte. Cover sequential, random, and strided access. | Device initialized, IO queues created. | 1. Write LBAs 0..N-1 with pattern(lba, seed). 2. Read back all LBAs, verify pattern. 3. Overwrite every 4th LBA with new pattern. 4. Read all, verify current generation. 5. Flush, read all again. | All reads return the most recently written pattern. No corruption, no IO errors. | 0 corrupt reads, 0 IO errors. |
| **DI-002** | GC data integrity at 95% utilization | Fill device to 95% user capacity, then perform sustained random overwrites to force heavy GC, verifying data after every N batches. | Device initialized with default 20% OP. | 1. Write 95% of total_lbas. 2. Random overwrite loop for 5 minutes. 3. Periodic read-verify of 256 random LBAs. 4. Final full-sweep verify. | All data matches last written generation despite GC moving pages. WAF > 1.0 confirms GC ran. | 0 corrupt, gc_count > 0, final sweep 0 errors. |
| **DI-003** | GC data integrity at 99% utilization | Same as DI-002 but fill to 99%. Extreme space pressure. | 20% OP device. | Same as DI-002 with 99% fill. Many NOSPC events expected. | Data integrity preserved despite extreme GC pressure. NOSPC is acceptable; corruption is not. | 0 corrupt, 0 stale reads. |
| **DI-004** | Trim correctness: no stale data leaks | Write all LBAs, trim random ranges, verify trimmed LBAs return `HFSSS_ERR_NOENT`, then re-write and verify new data. | Device initialized, all LBAs written. | 1. Warm-up write all LBAs. 2. Trim 1000 random ranges (1-16 LBAs each). 3. Read each trimmed LBA: must be NOENT. 4. Re-write 500 of those LBAs. 5. Read: re-written must match, still-trimmed must be NOENT. | No stale data visible after trim. Re-written data correct. | 0 stale reads, 0 corrupt. |
| **DI-005** | Trim correctness after GC | Write all LBAs, trim 50%, continue writing to force GC on blocks containing trimmed pages, verify no ghost data appears. | 20% OP device, all LBAs written. | 1. Warm-up. 2. Trim 50% of address space. 3. Write new data to remaining 50% in a loop to trigger GC. 4. Re-read trimmed LBAs. | Trimmed LBAs remain NOENT even after GC processes their source blocks. | 0 stale reads after GC. |
| **DI-006** | Format NVM completeness | Write all LBAs, format, verify all LBAs return NOENT, write new data, verify new data. | Device with data. | 1. Write all LBAs. 2. `nvme_uspace_format_nvm`. 3. Read 100% of LBAs: all NOENT. 4. Write new patterns. 5. Read back, verify. | Format erases all logical mappings. New data is correct. | 0 readable LBAs after format, 0 corrupt after re-write. |
| **DI-007** | Sanitize completeness (all sanact values) | For each sanact (1=block erase, 2=crypto erase, 3=overwrite): write, sanitize, verify NOENT. | Device with data. | For sanact in {1,2,3}: 1. Write all LBAs. 2. `nvme_uspace_sanitize(sanact)`. 3. Read 100%: all NOENT. | All three sanitize actions clear all mappings. | 0 readable LBAs after each sanitize. |
| **DI-008** | L2P/P2L mapping consistency | After sustained mixed write/trim/GC workload, dump L2P and P2L tables and cross-check bidirectional consistency. | FTL-level test using `ftl_ctx` internals. | 1. Run 10K random write + 2K trim + forced GC cycles. 2. Walk L2P: for every valid entry, look up PPN in P2L, confirm lba matches. 3. Walk P2L: for every valid entry, look up LBA in L2P, confirm PPN matches. | Bidirectional consistency: L2P(lba) = ppn implies P2L(ppn) = lba. | 0 mismatches. |
| **DI-009** | Multi-LBA write atomicity | Write 4 contiguous LBAs in a single `sssim_write(ctx, lba, 4, buf)`, read back, verify all 4 are consistent. | Device initialized. | 1. Write LBAs [100..103] with pattern gen=1. 2. Overwrite [100..103] with gen=2. 3. Read [100..103]: all must be gen=2. 4. Repeat 1000 times with random base LBA. | Multi-LBA writes are atomic at the API level. | 0 partial/mixed generation reads. |

---

### Category B: Error Handling and Recovery [P0]

**Description:** Inject faults using the `fault_registry` framework during active IO and verify the system handles errors gracefully.

**Estimated complexity: L**

| ID | Name | Description | Preconditions | Test Steps | Expected Results | Pass/Fail Criteria |
|---|---|---|---|---|---|---|
| **EH-001** | Bad block injection during writes | Register FAULT_BAD_BLOCK on specific blocks, perform writes targeting those blocks, verify FTL retires the block and remaps. | `fault_registry` initialized. | 1. Warm-up write. 2. Inject FAULT_BAD_BLOCK (sticky) on blocks [ch=0, block=5] and [ch=1, block=10]. 3. Continue random writes for 2 min. 4. Full verify. 5. Check faulted blocks marked BAD. | Writes succeed (remapped). No corruption. Faulted blocks in BAD state. | 0 corrupt, faulted blocks state == BAD. |
| **EH-002** | Read error injection and retry | Inject FAULT_READ_ERROR (one-shot) on a specific page, read that LBA, verify read retry succeeds. | LBA written, fault registered. | 1. Write LBA 42. 2. Inject FAULT_READ_ERROR one-shot. 3. Read LBA 42. 4. Verify data correct. 5. Check `read_retry_count > 0`. | Read succeeds after retry. Data correct. | read_retry_count incremented, data matches. |
| **EH-003** | Program error injection | Inject FAULT_PROGRAM_ERROR on a block, attempt writes, verify FTL handles gracefully. | Device initialized. | 1. Inject FAULT_PROGRAM_ERROR sticky on block [ch=0, block=3]. 2. Write data targeting that region. 3. Read back non-faulted LBAs. | Non-faulted LBA data is intact. Faulted block retired. | 0 corrupt on non-faulted LBAs, process exits cleanly. |
| **EH-004** | Erase error injection during GC | Inject FAULT_ERASE_ERROR on a GC victim candidate, force GC, verify it handles the failure. | Device at high utilization. | 1. Fill to 95%. 2. Inject FAULT_ERASE_ERROR sticky on likely victim. 3. Continue writes to trigger GC. 4. Verify data integrity. | GC completes despite erase failure. Block retired. No data loss. | gc_count > 0, 0 corrupt, faulted block retired. |
| **EH-005** | NOSPC handling and recovery | Fill device to 100%, write more until NOSPC, trim some LBAs, verify writes succeed again. | Device initialized. | 1. Write all total_lbas. 2. Attempt 100 more writes: expect NOSPC. 3. Trim 20%. 4. Flush. 5. Write 100 LBAs: expect success. 6. Verify all data. | NOSPC returned when full. Recovery after trim. Data consistent. | NOSPC count > 0, 0 errors after recovery, 0 corrupt. |
| **EH-006** | GC under extreme space pressure | Configure low OP (5%), fill nearly full, force GC with minimal free blocks. | Device with 5% OP. | 1. Fill to max. 2. Trim 2 blocks worth of LBAs. 3. Write to trigger GC. 4. Full verify. | GC succeeds with minimal headroom. No data corruption. | 0 corrupt, gc_count > 0. |
| **EH-007** | Bad block accumulation over lifetime | Simulate 10000 erase cycles with FAULT_BAD_BLOCK probability=0.001 per erase. | Device with fault injection. | 1. Register probabilistic FAULT_BAD_BLOCK. 2. Run write/erase loop for 10K GC cycles. 3. Track bad block count. 4. Verify data on good blocks. | Bad blocks accumulate gradually. Data on good blocks correct. Eventually NOSPC. | Monotonically increasing bad block count, 0 corrupt on good blocks. |
| **EH-008** | Concurrent fault injection during stress IO | Register multiple fault types simultaneously while running mixed R/W/Trim workload. | Device initialized, faults registered. | 1. Register: 5 bad blocks (sticky), 10 read errors (one-shot), 3 bit flips (one-shot). 2. Run mixed workload 5 min. 3. Full verify. | System handles all fault types without crash. | Process does not crash, 0 corrupt on non-faulted LBAs. |

---

### Category C: NVMe Command Compliance [P1]

**Description:** Systematically test every NVMe admin and IO command exposed by the `nvme_uspace_*` API, including edge cases.

**Estimated complexity: M**

| ID | Name | Description | Preconditions | Test Steps | Expected Results | Pass/Fail Criteria |
|---|---|---|---|---|---|---|
| **NC-001** | Identify Controller fields | Verify all mandatory Identify Controller fields. | Device started. | 1. `nvme_uspace_identify_ctrl`. 2. Check vid, sn, mn, fr, mdts, ver. | All mandatory fields have valid values. | All checked fields non-zero/valid. |
| **NC-002** | Identify Namespace fields | Verify nsze, ncap, lbaf[0] for nsid=1. | Device started. | 1. `nvme_uspace_identify_ns(nsid=1)`. 2. Verify nsze == total_lbas. 3. Verify lbaf[0].lbads. | Namespace size matches config. | nsze == total_lbas. |
| **NC-003** | Identify invalid NSID | Call Identify Namespace for nsid=0 and nsid=0xFFFFFFFF. | Device started. | 1. Identify with nsid=0. 2. Identify with nsid=0xFFFFFFFF. | Returns appropriate error code. | Return != HFSSS_OK for nsid=0. |
| **NC-004** | Get/Set Features round-trip | For each supported FID, set a value, get it back, verify match. | Device started. | For each fid: 1. `set_features(fid, val)`. 2. `get_features(fid, &got)`. 3. Assert got == val. | All supported features persist. | 0 mismatches. |
| **NC-005** | Get Log Page (SMART) counters | Verify SMART counters increment after IO. | Device started. | 1. Get SMART log. 2. Write 100 LBAs. 3. Get SMART log again. 4. Verify data_units_written increased. | SMART counters reflect IO activity. | Counters increase monotonically. |
| **NC-006** | Firmware Download + Commit round-trip | Download firmware, commit, verify identify_ctrl.fr updated. | Device started. | 1. Fill fw_buf. 2. `fw_download`. 3. `fw_commit`. 4. Check fr[0]. | Firmware revision updated. | fr[0] matches pattern byte. |
| **NC-007** | Queue create/delete stress | Create/delete IO CQ/SQ pairs 100 times, do IO between cycles. | Device started. | For 100 iterations: create, IO, delete. Verify primary queue works. | Stable under repeated create/delete. | 0 failures in 100 cycles. |
| **NC-008** | IO at boundary LBAs | Read/write at LBA=0, LBA=total_lbas-1, out-of-range LBA. | Device started. | 1. Write/read LBA 0. 2. Write/read LBA total_lbas-1. 3. Write at total_lbas (expect error). | Boundary LBAs accessible, out-of-range rejected. | Correct return codes. |
| **NC-009** | Zero-length read/write | Issue read and write with count=0. | Device started. | 1. Write count=0. 2. Read count=0. | Defined behavior, no crash. | No segfault. |
| **NC-010** | DSM/Trim with multiple ranges | Single Trim command with nr_ranges=4. | Device with data. | 1. Write 4 regions. 2. Trim with 4 ranges. 3. Read all: NOENT. | Multi-range trim works. | All trimmed LBAs return NOENT. |
| **NC-011** | DSM/Trim with overlapping ranges | Trim with overlapping ranges. | Device with data. | 1. Write region. 2. Trim overlapping ranges. 3. Read: all NOENT. | Overlapping handled correctly. | All LBAs in union return NOENT. |
| **NC-012** | Format NVM followed by IO | Format, then immediately write and read. | Device with data. | 1. Write. 2. Format. 3. Write new. 4. Read, verify. | Device functional after format. | 0 errors post-format. |

---

### Category D: Performance and Scalability [P1]

**Description:** Validate performance metrics and scaling with geometry using `bench_run` / `perf_validate_requirements`.

**Estimated complexity: L**

| ID | Name | Description | Preconditions | Test Steps | Expected Results | Pass/Fail Criteria |
|---|---|---|---|---|---|---|
| **PS-001** | Large geometry: 8ch x 4chip x 2die | Init with large config, run sequential write + read benchmark. | None. | 1. Init large. 2. `bench_run` SEQ_WRITE 60s. 3. `bench_run` SEQ_READ 60s. | Completes without error. Performance recorded. | Init succeeds, IOPS > 0. |
| **PS-002** | Sustained write WAF measurement | Fill to 80%, random write 5 min, measure WAF. | Default geometry. | 1. Fill 80%. 2. Random write 5 min. 3. Read WAF. | WAF > 1.0 (GC running). | waf > 1.0. |
| **PS-003** | WAF: sequential vs random | Compare WAF for sequential and random writes. | Default geometry. | 1. Sequential 5 min -> WAF_seq. 2. Re-init. Random 5 min -> WAF_rand. | WAF_seq < WAF_rand. | WAF_seq < WAF_rand. |
| **PS-004** | Latency percentiles (p50, p99, p999) | Mixed workload latency histogram. | Warm-up done. | 1. `bench_run` MIXED, 120s. 2. Extract percentiles. | p50 < p99 < p999. | All > 0, ordered correctly. |
| **PS-005** | IOPS scaling with channel count | Measure SEQ_READ IOPS for 1, 2, 4, 8 channels. | Multiple configs. | For each ch_count: bench 60s, record IOPS. | IOPS increases with channels. | IOPS(N) >= IOPS(N/2). |
| **PS-006** | Performance SLO validation | Run `perf_validation_run_all`. | Default geometry. | 1. Run all benchmarks. 2. Check report. | All requirements pass. | report.failed == 0. |
| **PS-007** | NAND timing accuracy | Verify simulated timing within tolerance. | None. | 1. Measure timing error. 2. Assert < 5%. | Within 5% of model. | err < 5.0. |
| **PS-008** | Zipfian workload WAF and latency | Run skewed workload, compare with uniform random. | Default geometry. | 1. Uniform random 120s. 2. Zipfian 120s. | Both complete, metrics recorded. | Runs complete. |

---

### Category E: Wear Leveling and Lifetime [P1]

**Description:** Verify wear leveling distributes erase counts evenly and triggers alerts at thresholds.

**Estimated complexity: M-L**

| ID | Name | Description | Preconditions | Test Steps | Expected Results | Pass/Fail Criteria |
|---|---|---|---|---|---|---|
| **WL-001** | PE distribution evenness | Write hot LBA range for 10K GC cycles, measure PE distribution. | WL enabled. | 1. Hot writes. 2. After 10K GC cycles, measure max-min erase. | PE spread < 20% of average. | (max-min)/avg < 0.20. |
| **WL-002** | Static wear leveling trigger | Create PE imbalance > threshold, verify static WL reduces it. | Static WL enabled, threshold=100. | 1. Create imbalance. 2. Trigger static WL. 3. Measure improvement. | Delta decreases after WL. | delta_after < delta_before. |
| **WL-003** | Wear alert threshold notification | Simulate high erase counts, verify alerts fire. | WL monitoring enabled. | 1. Set thresholds 90%/95%. 2. Erase to reach thresholds. 3. Check alert type. | Correct alert at each threshold. | Alert type matches. |
| **WL-004** | Block retirement rate over lifetime | Simulate 50K erase cycles, track retirement curve. | Reliability model active. | 1. Run 50K cycles. 2. Check bad blocks every 5K. | Retirement accelerates near max PE. | Bad blocks increase monotonically. |
| **WL-005** | WL disabled baseline | Same workload as WL-001 with WL disabled. | WL disabled. | 1. Hot writes without WL. 2. Measure PE delta. | PE delta much larger without WL. | delta(disabled) > 2x delta(enabled). |

---

### Category F: GC Policy Comparison [P2]

**Description:** Compare Greedy, Cost-Benefit, and FIFO GC policies under identical workloads.

**Estimated complexity: L**

| ID | Name | Description | Preconditions | Test Steps | Expected Results | Pass/Fail Criteria |
|---|---|---|---|---|---|---|
| **GC-001** | WAF comparison across policies | For each policy, fill to 80%, random write 5 min, measure WAF. | Same seed for all. | Run workload per policy, record WAF. | All complete without corruption. WAF values differ. | 0 corrupt for all three. |
| **GC-002** | GC efficiency: pages moved per block reclaimed | Compute moved_pages / reclaimed_blocks per policy. | Same as GC-001. | Run workload, compute ratio. | Cost-Benefit should be best for hot/cold workloads. | Ratio computed, no corruption. |
| **GC-003** | GC impact on read latency | Measure read latency during GC vs before GC. | 90% utilization. | Interleave reads during GC-heavy writes. | Read latency bounded during GC. | Latency recorded, 0 corrupt. |
| **GC-004** | GC correctness under each policy | Run DI-002 equivalent per policy. | Same as DI-002. | Data integrity test per policy. | Integrity preserved under all policies. | 0 corrupt per policy. |

---

### Category G: Persistence and Recovery [P2]

**Description:** Test media save/load, checkpoint/restore, and simulated power loss recovery.

**Estimated complexity: L-XL**

| ID | Name | Description | Preconditions | Test Steps | Expected Results | Pass/Fail Criteria |
|---|---|---|---|---|---|---|
| **PR-001** | Media save/load round-trip | Write, save, cleanup, re-init, load, verify. | Tmp file path. | 1. Write 1000 LBAs. 2. Save. 3. Cleanup. 4. Re-init + load. 5. Read, verify. | All data survives save/load. | 0 corrupt. |
| **PR-002** | Incremental save correctness | Full save + incremental save + load, verify all data. | Device initialized. | 1. Write A. 2. Full save. 3. Write B. 4. Incremental save. 5. Load. 6. Verify A+B. | Incremental captures delta. | 0 corrupt. |
| **PR-003** | Checkpoint create/restore | Write, checkpoint, write more, restore, verify rollback. | Checkpoint dir. | 1. Write A. 2. Checkpoint. 3. Write B, overwrite A. 4. Restore. 5. Verify only A. | Restore rolls back to checkpoint. | Pre-checkpoint data correct, post-checkpoint data gone. |
| **PR-004** | Incremental checkpoint correctness | Full + incremental checkpoint + restore. | Device initialized. | 1. Data. 2. Full ckpt. 3. More data. 4. Inc ckpt. 5. More data. 6. Restore from inc. 7. Verify. | Correct delta captured. | Data matches post-incremental state. |
| **PR-005** | Simulated power loss: crash marker + WAL | Write crash marker, re-init with boot, verify BOOT_RECOVERY. | Boot context. | 1. Write data. 2. Write crash marker. 3. Boot. 4. Verify recovery type. | Boot detects crash. Recovery executes. | boot_type == BOOT_RECOVERY. |
| **PR-006** | Persistence under stress | Save periodically during active IO, load last save, verify. | Device initialized. | 1. Write loop 2 min. 2. Save every 30s. 3. Load last save. 4. Verify. | Periodic saves capture consistent state. | 0 corrupt at last save point. |
| **PR-007** | Boot sequence: first/normal/recovery | Exercise all three boot types. | None. | 1. First boot. 2. Clean shutdown -> normal boot. 3. Crash marker -> recovery boot. | Each type correctly detected. | Correct boot_type for each. |

---

### Category H: Reliability Model Validation [P2]

**Description:** Verify the reliability model correctly simulates bit errors, read disturb, retention, and PE aging.

**Estimated complexity: M**

| ID | Name | Description | Preconditions | Test Steps | Expected Results | Pass/Fail Criteria |
|---|---|---|---|---|---|---|
| **RM-001** | Bit error + ECC response | Inject bit flips, verify ECC corrects or reports uncorrectable. | Data written, fault registered. | 1. Inject correctable flip. 2. Read. 3. Inject uncorrectable flip. 4. Read. | ECC corrects within capability. Uncorrectable reported. | Corrected data matches for correctable case. |
| **RM-002** | Read disturb accumulation | Read same page 100K times, verify bit errors increase. | Reliability model initialized. | 1. Base errors at read_count=0. 2. Errors at read_count=100K. | disturb_errors > base_errors. | Strict increase. |
| **RM-003** | Data retention degradation | Calculate errors with increasing retention_ns. | Reliability model initialized. | Compute errors for 0, 1 day, 1 week, 1 month, 1 year. | Monotonically increasing. | errors(t+1) >= errors(t). |
| **RM-004** | PE cycle aging | Calculate errors from 0 to max_pe_cycles in 10 steps. | Reliability model initialized. | Compute errors at each step. | Monotonically increasing. | Strict ordering. |
| **RM-005** | NAND type comparison | Compare SLC/MLC/TLC/QLC reliability at same conditions. | Reliability model initialized. | Compute errors for each type at same PE/read/retention. | SLC < MLC < TLC < QLC. | Strict ordering. |
| **RM-006** | End-to-end fault + reliability + verify | Inject elevated disturb and retention faults, run workload, verify data. | Full stack with faults. | 1. Register amplified faults. 2. Mixed workload 5 min. 3. Full verify. | System handles elevated error rates. | 0 silent corruption. |

---

### Category I: Controller Integration [P3]

**Description:** Exercise write buffer, read cache, and flow control subsystems.

**Estimated complexity: M**

| ID | Name | Description | Preconditions | Test Steps | Expected Results | Pass/Fail Criteria |
|---|---|---|---|---|---|---|
| **CI-001** | Write buffer hit | Write to WB, read back before flush, verify served from buffer. | WB initialized. | 1. Write. 2. Lookup -> true. 3. Read from WB. 4. Verify. | WB serves reads for dirty entries. | Data matches. |
| **CI-002** | Write buffer flush persistence | Write to WB, flush, read through FTL, verify. | Controller + FTL. | 1. Write through controller. 2. Flush. 3. Read via FTL. 4. Verify. | Flushed data in NAND. | 0 corrupt. |
| **CI-003** | Read cache effectiveness | Read same LBA 1000 times, verify hit count. | RC initialized. | 1. Read LBA. 2. Read 999 more. 3. Check hit_count >= 999. | Cache serves repeated reads. | hit_count >= 999. |
| **CI-004** | Read cache invalidation on write | Populate cache, write new data, verify no stale reads. | RC initialized. | 1. Write gen=1. 2. Read (cache). 3. Write gen=2. 4. Invalidate. 5. Read: gen=2. | No stale reads. | Data matches gen=2. |
| **CI-005** | Flow control backpressure | Fill WB to 90%, check backpressure state. | Flow control enabled. | 1. Fill WB. 2. Check BP >= MEDIUM. 3. Flush. 4. Check BP reduced. | BP reflects WB occupancy. | Correct state transitions. |
| **CI-006** | Flow control rate limiting | Configure rate limit, burst writes, verify throttling. | Flow control enabled. | 1. Set limit. 2. Burst 200 writes. 3. Check throttle count > 0. | Rate limiter throttles excess. | total_throttled > 0. |
| **CI-007** | Scheduler fairness | Submit mixed reads, writes, admin. Verify all complete. | Scheduler initialized. | 1. Queue 100 reads, 100 writes, 10 admin. 2. Process all. 3. Verify completion. | No starvation. | All 210 completed. |

---

### Category J: Configuration and Boundary [P3]

**Description:** Test extreme or unusual configurations.

**Estimated complexity: S-M**

| ID | Name | Description | Preconditions | Test Steps | Expected Results | Pass/Fail Criteria |
|---|---|---|---|---|---|---|
| **CB-001** | Minimum geometry | 1ch, 1chip, 1die, 1plane, 4 blocks, 4 pages. Write, GC, verify. | None. | 1. Init tiny. 2. Write all. 3. Overwrite to trigger GC. 4. Verify. | Works on tiny device. | 0 corrupt. |
| **CB-002** | Maximum geometry | 8ch, 4chip, 2die, 2plane, 2048 blocks, 512 pages. | None. | 1. Init large. 2. Write 1000 random LBAs. 3. Verify. | Large geometry works. | Init succeeds, 0 corrupt. |
| **CB-003** | OP ratio: 5% | Minimal OP, high GC pressure. | 5% OP config. | 1. Init. 2. Fill to 95%. 3. Random overwrite 2 min. 4. Verify. | Works with low OP. | 0 corrupt. |
| **CB-004** | OP ratio: 50% | High OP, minimal GC. Measure WAF. | 50% OP config. | 1. Init. 2. Fill to 50%. 3. Random overwrite 2 min. 4. Check WAF. | Low WAF. | WAF close to 1.0. |
| **CB-005** | All NAND types: SLC, MLC, TLC, QLC | Init, write, read, verify for each type. | None. | For each type: init, write 100, read, verify, check timing. | All types work. Timing differs. | 0 corrupt, timing order: SLC < MLC < TLC < QLC. |
| **CB-006** | Edge LBA values | LBA=0, max-1, max, U64_MAX. | Device initialized. | 1. Write/read LBA 0. 2. Write/read total_lbas-1. 3. Write total_lbas -> INVAL. 4. Write U64_MAX -> INVAL. | Boundary works, out-of-range rejected. | Correct return codes. |
| **CB-007** | Config file round-trip | Write YAML config, load, validate, save, re-load, compare. | Temp file. | 1. Set defaults. 2. Save. 3. Load. 4. Compare all fields. | Lossless serialization. | All fields match. |

---

## 3. Test Environment and Configuration Matrix

### 3.1 Geometry Configurations

| Config | Channels | Chips/Ch | Dies/Chip | Planes/Die | Blocks/Plane | Pages/Block | Page Size | Approx Raw Size | Used In |
|---|---|---|---|---|---|---|---|---|---|
| **tiny** | 1 | 1 | 1 | 1 | 4 | 4 | 4096 | 64 KB | CB-001 |
| **small** | 2 | 1 | 1 | 1 | 128 | 256 | 4096 | 256 MB | DI-*, EH-*, NC-*, GC-*, PR-*, RM-*, CI-* |
| **medium** | 4 | 2 | 1 | 1 | 256 | 256 | 4096 | 2 GB | PS-001, WL-* |
| **large** | 8 | 4 | 2 | 1 | 256 | 256 | 4096 | 16 GB | PS-001, PS-005, CB-002 |
| **max** | 8 | 4 | 2 | 2 | 2048 | 512 | 4096 | 256 GB | CB-002 |

### 3.2 GC Policies

| Policy | Tested In |
|---|---|
| Greedy | All DI-*, EH-*, default |
| Cost-Benefit | GC-001 through GC-004 |
| FIFO | GC-001 through GC-004 |

### 3.3 NAND Types

| Type | Tested In |
|---|---|
| SLC | CB-005, RM-005 |
| MLC | CB-005, RM-005 |
| TLC | CB-005, RM-005 (default) |
| QLC | CB-005, RM-005 |

### 3.4 Over-Provisioning Ratios

| OP% | Tested In |
|---|---|
| 5% | CB-003, EH-006 |
| 20% | Default |
| 50% | CB-004 |

---

## 4. Test Execution Strategy

### 4.1 Recommended Execution Order

**Tier 1 — Every CI run (< 5 minutes):**
1. All existing unit tests (`make test`)
2. DI-001 (basic data path)
3. DI-004 (trim correctness)
4. DI-006 (format completeness)
5. NC-001, NC-002, NC-008, NC-009 (basic NVMe compliance)
6. CB-006 (edge LBAs)
7. EH-005 (NOSPC handling)

**Tier 2 — Nightly (< 60 minutes):**
1. All Tier 1
2. DI-002, DI-003, DI-005, DI-008, DI-009 (GC integrity, mapping consistency)
3. EH-001 through EH-006 (fault injection)
4. NC-003 through NC-012 (full NVMe compliance)
5. PR-001 through PR-004 (persistence round-trips)
6. RM-002 through RM-005 (reliability calculations)
7. CI-001 through CI-004 (write buffer, read cache)
8. CB-001 through CB-005 (boundary configs)

**Tier 3 — Weekly (< 8 hours):**
1. All Tier 2
2. EH-007, EH-008 (long fault accumulation)
3. PS-001 through PS-008 (performance and scalability)
4. WL-001 through WL-005 (wear leveling)
5. GC-001 through GC-004 (GC policy comparison)
6. PR-005 through PR-007 (power loss, boot sequences)
7. RM-001, RM-006 (end-to-end reliability)
8. CI-005 through CI-007 (flow control, scheduler)
9. CB-007 (config round-trip)

### 4.2 Estimated Total Run Times

| Tier | Test Count | Estimated Duration |
|---|---|---|
| Tier 1 (CI/push) | 9 tests | ~3 minutes |
| Tier 2 (nightly) | 45 tests | ~55 minutes |
| Tier 3 (weekly) | 73 tests (full suite) | ~6 hours |

### 4.3 CI/CD Integration

1. **Makefile targets**: `make systest-tier1`, `make systest-tier2`, `make systest-all`
2. **GitHub Actions**: On push: `make test + systest-tier1`; Nightly: `systest-tier2`; Weekly: `systest-all`
3. **Timeout enforcement**: Tier 1 tests 60s each; Tier 2 300s; weekly tests documented duration + 50% margin
4. **Artifact collection**: Stdout/stderr to `build/systest_results/<test_name>.log`
5. **Performance regression**: PS-* tests output JSON summary; CI flags regressions > 10%

---

## 5. Risk Assessment

### 5.1 Technical Risks

| Risk | Impact | Probability | Mitigation |
|---|---|---|---|
| Large geometry tests exhaust memory | Test cannot run on CI | High | Cap at "large" config for CI. Run "max" on dedicated hardware only. |
| Fault injection APIs not wired to full stack | EH-* tests pass trivially | Medium | Verify `fault_check` called in `media_nand_read/program/erase`. Wire in if absent. |
| Controller write buffer/read cache are stubs | CI-* tests find unimplemented functionality | Medium | Verify implementations before CI-* tests. Implement if stubbed. |
| Media save/load not battle-tested | PR-* tests reveal serialization bugs | Medium | Start with PR-001 (simplest). Fix bugs before PR-006. |
| WAL replay not implemented | PR-005, PR-007 fail | Medium | Check boot.c. Defer if absent. |

### 5.2 Process Risks

| Risk | Impact | Probability | Mitigation |
|---|---|---|---|
| 73 tests too many to implement at once | Nothing ships | High | Implement P0 first (17 tests), then P1, then P2/P3. |
| Long-running tests block CI | Developer velocity drops | Medium | Keep Tier 1 < 5 min. Tier 2+ nightly/weekly only. |
| Flaky tests | False failures erode trust | Medium | Deterministic seeds. Operation counts, not durations. |

### 5.3 Prerequisite Development Work

1. **Fault injection wiring** — Ensure `fault_check` on hot path in media layer
2. **Media persistence verification** — Manual round-trip test before automation
3. **boot.c WAL replay** — Verify functional; defer PR-005/PR-007 if not
4. **Controller-FTL integration** — Wire write buffer and read cache for CI-* tests

---

## Summary

| Priority | Category | Tests | Duration |
|---|---|---|---|
| P0 | A: Data Integrity | 9 | 25 min |
| P0 | B: Error Handling | 8 | 30 min |
| P1 | C: NVMe Compliance | 12 | 10 min |
| P1 | D: Performance | 8 | 60 min |
| P1 | E: Wear Leveling | 5 | 60 min |
| P2 | F: GC Policy | 4 | 60 min |
| P2 | G: Persistence | 7 | 30 min |
| P2 | H: Reliability Model | 6 | 15 min |
| P3 | I: Controller Integration | 7 | 15 min |
| P3 | J: Configuration/Boundary | 7 | 10 min |
| **Total** | | **73** | **~5.25 hours** |
