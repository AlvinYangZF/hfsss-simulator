# Enlarge Disk Capacity — Implementation Plan

**Goal:** Raise HFSSS simulator capacity from 4 GB to 16-64 GB configurable.
**Spec:** `docs/superpowers/specs/2026-04-04-enlarge-disk-capacity-design.md`
**Target repo:** `/Users/zifengyang/Desktop/hfsss-simulator`

---

## File Map

| Action | Path | Responsibility |
|--------|------|---------------|
| Modify | `include/ftl/mapping.h` | Raise L2P/P2L table sizes |
| Modify | `include/media/eat.h` | Raise MAX_CHANNELS, MAX_CHIPS, MAX_DIES, MAX_PLANES |
| Modify | `include/media/bbt.h` | Raise MAX_* to match eat.h + MAX_BLOCKS |
| Modify | `include/media/nand.h` | Raise MAX_BLOCKS_PER_PLANE, MAX_PAGES_PER_BLOCK |
| Modify | `src/media/nand.c` | Update validation bounds |
| Modify | `src/vhost/hfsss_nbd_server.c` | Auto-size NAND geometry from -s flag |
| Create | `tests/test_large_capacity.c` | Large capacity unit test |
| Modify | `Makefile` | Add test_large_capacity target |

---

### Task 1: Raise L2P/P2L Table Sizes

**Files:** `include/ftl/mapping.h`

- [ ] **Step 1:** Change `L2P_TABLE_SIZE` from `(1ULL << 20)` to `(1ULL << 24)`
- [ ] **Step 2:** Change `P2L_TABLE_SIZE` from `(1ULL << 24)` to `(1ULL << 25)`
- [ ] **Step 3:** Build and verify no compile errors

---

### Task 2: Raise NAND MAX_* Constants

**Files:** `include/media/eat.h`, `include/media/bbt.h`, `include/media/nand.h`

- [ ] **Step 1:** In `eat.h`: MAX_CHANNELS=64, MAX_CHIPS_PER_CHANNEL=16, MAX_DIES_PER_CHIP=8, MAX_PLANES_PER_DIE=4
- [ ] **Step 2:** In `bbt.h`: match eat.h values + MAX_BLOCKS_PER_PLANE=4096
- [ ] **Step 3:** In `nand.h`: MAX_BLOCKS_PER_PLANE=4096, MAX_PAGES_PER_BLOCK=1024
- [ ] **Step 4:** Build and verify no compile errors

---

### Task 3: Update NAND Validation Bounds

**Files:** `src/media/nand.c`

- [ ] **Step 1:** Update `nand_device_init()` validation to use new MAX_* values
- [ ] **Step 2:** Build and run `make test` — all existing tests must pass

---

### Task 4: NBD Server Auto-Sizing

**Files:** `src/vhost/hfsss_nbd_server.c`

- [ ] **Step 1:** Add `nbd_auto_geometry()` function that computes NAND geometry from target size in MB
- [ ] **Step 2:** Call it from `main()` to set `config.sssim_cfg.*` based on `-s` value
- [ ] **Step 3:** Print computed geometry at startup for visibility
- [ ] **Step 4:** Build and verify NBD server starts with `-s 16384` (16 GB)

---

### Task 5: Large Capacity Unit Test

**Files:** `tests/test_large_capacity.c`, `Makefile`

- [ ] **Step 1:** Write test that inits 16 GB geometry (8ch, 4chip, 2die, 2plane, 4096blk, 256pg, 4096B page)
- [ ] **Step 2:** Verify L2P table can map all LBAs for this geometry
- [ ] **Step 3:** Write + read boundary LBAs (0, last, middle) through FTL
- [ ] **Step 4:** Add Makefile target
- [ ] **Step 5:** Build and run test

---

### Task 6: Integration Test (QEMU + fio)

- [ ] **Step 1:** Start NBD server with `-s 16384`
- [ ] **Step 2:** Boot QEMU, verify NVMe device shows 16 GB
- [ ] **Step 3:** Run fio verify test on 16 GB device
- [ ] **Step 4:** Commit all changes

---

### Task 7: Final Verification

- [ ] **Step 1:** `make clean && make` — zero errors
- [ ] **Step 2:** `make test` — all tests pass
- [ ] **Step 3:** Run test_large_capacity
- [ ] **Step 4:** Verify git log
