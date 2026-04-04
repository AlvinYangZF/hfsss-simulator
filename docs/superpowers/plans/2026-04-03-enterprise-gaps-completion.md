# Enterprise Gaps Completion — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close remaining gaps: fill requirements traceability matrix, scale NAND defaults to 4TB enterprise QLC, complete 5 partial requirements, and add stability testing infrastructure.

**Architecture:** Three independent workstreams executed in parallel where possible. Workstream 1 is a Python script that updates the CSV. Workstream 2 is a 6-line config change. Workstream 3 adds new C source files following existing project patterns (src/ + include/ + tests/, linked into static libs).

**Tech Stack:** C11 (gcc), Python3 (for CSV script), Make, project-internal test framework (TEST_ASSERT macro pattern)

**Target repo:** `/Users/zifengyang/Desktop/hfsss-simulator`

---

## File Map

| Action | Path | Responsibility |
|--------|------|---------------|
| Modify | `REQUIREMENTS_MATRIX_EN.csv` | Fill HLD/LLD Reference columns |
| Modify | `src/common/hfsss_config.c:14-27` | Scale NAND defaults to 4TB |
| Modify | `include/controller/resource.h` | Add `cpu_stats` struct and getter |
| Modify | `src/controller/resource.c` | Add `resource_get_cpu_stats()` |
| Create | `include/common/proc_interface.h` | Simulated /proc interface header |
| Create | `src/common/proc_interface.c` | Simulated /proc file writer |
| Create | `tests/test_proc_interface.c` | Tests for proc interface |
| Create | `tests/stress_stability.c` | Long-duration stability test |
| Modify | `Makefile` | Add new test/stress targets |

---

### Task 1: Requirements Matrix Traceability (Workstream 1)

**Files:**
- Modify: `REQUIREMENTS_MATRIX_EN.csv` (columns 11-12)

- [ ] **Step 1: Write Python script to fill HLD/LLD references**

Create a temporary script `scripts/fill_traceability.py`:

```python
#!/usr/bin/env python3
"""Fill HLD/LLD Reference columns in REQUIREMENTS_MATRIX_EN.csv."""

import csv
import sys

MODULE_MAP = {
    "PCIe/NVMe Device Emulation Module": ("HLD_01", "LLD_01"),
    "Controller Thread Module":          ("HLD_02", "LLD_02"),
    "Media Thread Module":               ("HLD_03", "LLD_03"),
    "Hardware Abstraction Layer":        ("HLD_04", "LLD_04"),
    "Common Platform Layer":             ("HLD_05", "LLD_05"),
    "Algorithm Task Layer":              ("HLD_06", "LLD_06"),
    "Performance Requirements":          ("HLD_06", "LLD_10"),
    "Product Interface":                 ("HLD_05", "LLD_07"),
    "Fault Injection Framework":         ("HLD_05", "LLD_08"),
    "System Reliability and Stability":  ("HLD_05", "LLD_11"),
    "UPLP (Unclean Power Loss Protection)": ("HLD_05", "LLD_17"),
    "QoS (Quality of Service)":          ("HLD_02", "LLD_18"),
    "T10 DIF/PI (Data Integrity Field / Protection Information)": ("HLD_06", "LLD_11"),
    "Security":                          ("HLD_02", "LLD_19"),
    "Multi-Namespace Management":        ("HLD_06", "LLD_06"),
    "Thermal Management and Telemetry":  ("HLD_05", "LLD_12"),
}

# Sub-module → LLD refinement for modules with multiple LLDs
SUB_MODULE_LLD = {
    "Common Platform Layer": {
        "OOB": "LLD_07",
        "Fault": "LLD_08",
        "Boot": "LLD_09",
        "Real-Time": "LLD_12",
        "NOR": "LLD_14",
        "Persistence": "LLD_15",
    },
    "Hardware Abstraction Layer": {
        "Advanced": "LLD_13",
        "NOR": "LLD_13",
    },
}

def refine_lld(module, sub_module, default_lld):
    """Try to match sub-module keywords for finer LLD mapping."""
    refinements = SUB_MODULE_LLD.get(module, {})
    for keyword, lld in refinements.items():
        if keyword.lower() in sub_module.lower():
            return lld
    return default_lld

def main():
    path = "REQUIREMENTS_MATRIX_EN.csv"
    rows = []
    with open(path, "r") as f:
        reader = csv.reader(f)
        header = next(reader)
        rows = list(reader)

    # Indices: 1=Module, 2=Sub-module, 11=HLD Reference, 12=LLD Reference
    updated = 0
    for row in rows:
        module = row[1]
        sub_module = row[2] if len(row) > 2 else ""
        mapping = MODULE_MAP.get(module)
        if mapping:
            hld, lld = mapping
            lld = refine_lld(module, sub_module, lld)
            row[11] = hld
            row[12] = lld
            updated += 1
        else:
            print(f"WARNING: No mapping for module '{module}' (REQ {row[0]})",
                  file=sys.stderr)

    with open(path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(header)
        writer.writerows(rows)

    print(f"Updated {updated}/{len(rows)} requirements with HLD/LLD references.")

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run the script**

```bash
cd /Users/zifengyang/Desktop/hfsss-simulator
mkdir -p scripts
# (save the script above to scripts/fill_traceability.py)
python3 scripts/fill_traceability.py
```

Expected: `Updated 174/174 requirements with HLD/LLD references.`

- [ ] **Step 3: Verify no empty cells remain**

```bash
python3 -c "
import csv
with open('REQUIREMENTS_MATRIX_EN.csv') as f:
    reader = csv.reader(f)
    header = next(reader)
    rows = list(reader)
empty_hld = sum(1 for r in rows if not r[11].strip())
empty_lld = sum(1 for r in rows if not r[12].strip())
print(f'Empty HLD: {empty_hld}, Empty LLD: {empty_lld}')
assert empty_hld == 0, 'HLD references still empty!'
assert empty_lld == 0, 'LLD references still empty!'
print('All references filled.')
"
```

Expected: `Empty HLD: 0, Empty LLD: 0` / `All references filled.`

- [ ] **Step 4: Spot-check 5 random rows**

```bash
python3 -c "
import csv, random
with open('REQUIREMENTS_MATRIX_EN.csv') as f:
    reader = csv.reader(f)
    header = next(reader)
    rows = list(reader)
for r in random.sample(rows, 5):
    print(f'{r[0]} | {r[1][:30]:30s} | HLD={r[11]:6s} | LLD={r[12]:6s}')
"
```

Expected: Each row shows non-empty HLD and LLD values matching the module.

- [ ] **Step 5: Commit**

```bash
git add REQUIREMENTS_MATRIX_EN.csv scripts/fill_traceability.py
git commit -m "docs: fill HLD/LLD references for all 174 requirements in traceability matrix"
```

---

### Task 2: NAND Geometry Scale-Up to 4TB (Workstream 2)

**Files:**
- Modify: `src/common/hfsss_config.c:14-27`

- [ ] **Step 1: Update NAND defaults**

In `src/common/hfsss_config.c`, change `hfsss_config_defaults()`:

```c
    /* NAND: enterprise QLC SSD profile (4TB raw) */
    cfg->nand.channel_count     = 16;
    cfg->nand.chips_per_channel = 8;
    cfg->nand.dies_per_chip     = 4;
    cfg->nand.planes_per_die    = 2;
    cfg->nand.blocks_per_plane  = 2048;
    cfg->nand.pages_per_block   = 512;
    cfg->nand.page_size         = 16384;
    cfg->nand.spare_size        = 256;
    cfg->nand.op_ratio_pct      = 7;
```

- [ ] **Step 2: Build and run all tests**

```bash
cd /Users/zifengyang/Desktop/hfsss-simulator
make clean && make -j$(sysctl -n hw.ncpu) 2>&1 | tail -5
make test 2>&1 | grep -E "passed|failed|FAIL|SUCCESS" | tail -20
```

Expected: Build succeeds, all tests pass. Some tests use hardcoded small geometries in their own init calls, so the default change should not break them.

- [ ] **Step 3: Verify the config defaults test reflects new values**

```bash
./build/bin/test_config 2>&1 | head -20
```

Expected: `default channel_count > 0` PASS, `default page_size is a power of 2` PASS (16384 is 2^14).

- [ ] **Step 4: Commit**

```bash
git add src/common/hfsss_config.c
git commit -m "feat: scale NAND defaults to 4TB enterprise QLC profile (16ch/8chip/4die/16KB page)"
```

---

### Task 3: REQ-119 — Resource CPU Stats (Workstream 3a)

**Files:**
- Modify: `include/controller/resource.h`
- Modify: `src/controller/resource.c`
- Test: existing test infrastructure (verify via `make test`)

- [ ] **Step 1: Add CPU stats struct and API to header**

Append before `#endif` in `include/controller/resource.h`:

```c
/* Simulated per-role CPU utilization (REQ-119) */
enum cpu_role {
    CPU_ROLE_NAND = 0,
    CPU_ROLE_FTL  = 1,
    CPU_ROLE_PCIE = 2,
    CPU_ROLE_GC   = 3,
    CPU_ROLE_MAX  = 4,
};

struct cpu_stats {
    u64 cycle_count[CPU_ROLE_MAX];   /* simulated cycles consumed */
    u64 total_cycles;                /* total simulated cycles elapsed */
};

void resource_cpu_record(struct resource_mgr *mgr, enum cpu_role role, u64 cycles);
void resource_cpu_get_stats(const struct resource_mgr *mgr, struct cpu_stats *out);
double resource_cpu_utilization(const struct resource_mgr *mgr, enum cpu_role role);
```

- [ ] **Step 2: Add cpu_stats field to resource_mgr**

In `include/controller/resource.h`, add to `struct resource_mgr`:

```c
    /* CPU utilization tracking (REQ-119) */
    struct cpu_stats cpu;
```

- [ ] **Step 3: Implement CPU stats functions**

Append to `src/controller/resource.c`:

```c
void resource_cpu_record(struct resource_mgr *mgr, enum cpu_role role, u64 cycles)
{
    if (!mgr || role >= CPU_ROLE_MAX) {
        return;
    }

    mutex_lock(&mgr->lock, 0);
    mgr->cpu.cycle_count[role] += cycles;
    mgr->cpu.total_cycles += cycles;
    mutex_unlock(&mgr->lock);
}

void resource_cpu_get_stats(const struct resource_mgr *mgr, struct cpu_stats *out)
{
    if (!mgr || !out) {
        return;
    }
    /* Copy under lock - cast away const for mutex (read-only semantics) */
    struct resource_mgr *m = (struct resource_mgr *)mgr;
    mutex_lock(&m->lock, 0);
    *out = mgr->cpu;
    mutex_unlock(&m->lock);
}

double resource_cpu_utilization(const struct resource_mgr *mgr, enum cpu_role role)
{
    if (!mgr || role >= CPU_ROLE_MAX || mgr->cpu.total_cycles == 0) {
        return 0.0;
    }
    return (double)mgr->cpu.cycle_count[role] / (double)mgr->cpu.total_cycles;
}
```

- [ ] **Step 4: Build and test**

```bash
cd /Users/zifengyang/Desktop/hfsss-simulator
make -j$(sysctl -n hw.ncpu) 2>&1 | tail -3
make test 2>&1 | tail -5
```

Expected: Build succeeds, all existing tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/controller/resource.h src/controller/resource.c
git commit -m "feat(REQ-119): add per-role CPU utilization tracking to resource manager"
```

---

### Task 4: REQ-124 — Simulated /proc Interface (Workstream 3b)

**Files:**
- Create: `include/common/proc_interface.h`
- Create: `src/common/proc_interface.c`
- Create: `tests/test_proc_interface.c`
- Modify: `Makefile`

- [ ] **Step 1: Write the failing test**

Create `tests/test_proc_interface.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "common/proc_interface.h"

static int total = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    total++; \
    if (cond) { printf("  [PASS] %s\n", msg); passed++; } \
    else       { printf("  [FAIL] %s\n", msg); failed++; } \
} while (0)

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int file_nonempty(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_size > 0;
}

static void test_proc_init_cleanup(void)
{
    printf("\n=== Proc Interface Init/Cleanup ===\n");

    const char *dir = "/tmp/hfsss_test_proc";
    struct proc_interface proc;

    int ret = proc_interface_init(&proc, dir);
    TEST_ASSERT(ret == 0, "proc_interface_init succeeds");
    TEST_ASSERT(file_exists(dir), "proc directory created");

    proc_interface_cleanup(&proc);

    /* NULL safety */
    proc_interface_cleanup(NULL);
    TEST_ASSERT(1, "cleanup(NULL) does not crash");

    ret = proc_interface_init(NULL, dir);
    TEST_ASSERT(ret != 0, "init(NULL) returns error");

    /* Clean up */
    rmdir(dir);
}

static void test_proc_write_status(void)
{
    printf("\n=== Proc Interface Write Status ===\n");

    const char *dir = "/tmp/hfsss_test_proc2";
    struct proc_interface proc;
    proc_interface_init(&proc, dir);

    struct proc_status status = {
        .uptime_sec = 3600,
        .fw_version = "1.0.0",
        .state = "RUNNING",
    };
    strncpy(status.fw_version, "1.0.0", sizeof(status.fw_version) - 1);
    strncpy(status.state, "RUNNING", sizeof(status.state) - 1);

    int ret = proc_write_status(&proc, &status);
    TEST_ASSERT(ret == 0, "proc_write_status succeeds");

    char path[512];
    snprintf(path, sizeof(path), "%s/status", dir);
    TEST_ASSERT(file_nonempty(path), "status file is non-empty");

    proc_interface_cleanup(&proc);

    /* Clean up files */
    unlink(path);
    rmdir(dir);
}

static void test_proc_write_perf(void)
{
    printf("\n=== Proc Interface Write Perf Counters ===\n");

    const char *dir = "/tmp/hfsss_test_proc3";
    struct proc_interface proc;
    proc_interface_init(&proc, dir);

    struct proc_perf_counters perf = {
        .read_iops = 100000,
        .write_iops = 50000,
        .read_bw_mbps = 3200,
        .write_bw_mbps = 1600,
        .read_lat_p99_us = 80,
        .write_lat_p99_us = 200,
    };

    int ret = proc_write_perf_counters(&proc, &perf);
    TEST_ASSERT(ret == 0, "proc_write_perf_counters succeeds");

    char path[512];
    snprintf(path, sizeof(path), "%s/perf_counters", dir);
    TEST_ASSERT(file_nonempty(path), "perf_counters file is non-empty");

    proc_interface_cleanup(&proc);
    unlink(path);
    rmdir(dir);
}

static void test_proc_write_ftl_stats(void)
{
    printf("\n=== Proc Interface Write FTL Stats ===\n");

    const char *dir = "/tmp/hfsss_test_proc4";
    struct proc_interface proc;
    proc_interface_init(&proc, dir);

    struct proc_ftl_stats ftl = {
        .l2p_hit_rate_pct = 95,
        .gc_count = 1234,
        .waf = 1.5,
        .avg_erase_count = 500,
        .max_erase_count = 800,
    };

    int ret = proc_write_ftl_stats(&proc, &ftl);
    TEST_ASSERT(ret == 0, "proc_write_ftl_stats succeeds");

    char path[512];
    snprintf(path, sizeof(path), "%s/ftl_stats", dir);
    TEST_ASSERT(file_nonempty(path), "ftl_stats file is non-empty");

    proc_interface_cleanup(&proc);
    unlink(path);
    rmdir(dir);
}

int main(void)
{
    printf("========================================\n");
    printf("Proc Interface Tests\n");
    printf("========================================\n");

    test_proc_init_cleanup();
    test_proc_write_status();
    test_proc_write_perf();
    test_proc_write_ftl_stats();

    printf("\n========================================\n");
    printf("Results: %d/%d passed, %d failed\n", passed, total, failed);
    printf("========================================\n");
    return failed > 0 ? 1 : 0;
}
```

- [ ] **Step 2: Create header**

Create `include/common/proc_interface.h`:

```c
#ifndef __HFSSS_PROC_INTERFACE_H
#define __HFSSS_PROC_INTERFACE_H

#include <stdint.h>

#define PROC_DIR_DEFAULT "/tmp/hfsss/proc"
#define PROC_PATH_MAX 512
#define PROC_STR_MAX  64

struct proc_interface {
    char dir[PROC_PATH_MAX];
    int  initialized;
};

struct proc_status {
    uint64_t uptime_sec;
    char     fw_version[PROC_STR_MAX];
    char     state[PROC_STR_MAX];
};

struct proc_perf_counters {
    uint64_t read_iops;
    uint64_t write_iops;
    uint64_t read_bw_mbps;
    uint64_t write_bw_mbps;
    uint64_t read_lat_p99_us;
    uint64_t write_lat_p99_us;
};

struct proc_ftl_stats {
    uint32_t l2p_hit_rate_pct;
    uint64_t gc_count;
    double   waf;
    uint64_t avg_erase_count;
    uint64_t max_erase_count;
};

int  proc_interface_init(struct proc_interface *proc, const char *dir);
void proc_interface_cleanup(struct proc_interface *proc);
int  proc_write_status(struct proc_interface *proc, const struct proc_status *s);
int  proc_write_perf_counters(struct proc_interface *proc, const struct proc_perf_counters *p);
int  proc_write_ftl_stats(struct proc_interface *proc, const struct proc_ftl_stats *f);

#endif /* __HFSSS_PROC_INTERFACE_H */
```

- [ ] **Step 3: Implement proc_interface.c**

Create `src/common/proc_interface.c`:

```c
#include "common/proc_interface.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

int proc_interface_init(struct proc_interface *proc, const char *dir)
{
    if (!proc) {
        return -1;
    }

    memset(proc, 0, sizeof(*proc));

    const char *d = dir ? dir : PROC_DIR_DEFAULT;
    strncpy(proc->dir, d, PROC_PATH_MAX - 1);

    /* Create directory (and parents) */
    if (mkdir(proc->dir, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    proc->initialized = 1;
    return 0;
}

void proc_interface_cleanup(struct proc_interface *proc)
{
    if (!proc) {
        return;
    }
    memset(proc, 0, sizeof(*proc));
}

static FILE *proc_open(struct proc_interface *proc, const char *name)
{
    if (!proc || !proc->initialized || !name) {
        return NULL;
    }

    char path[PROC_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", proc->dir, name);
    return fopen(path, "w");
}

int proc_write_status(struct proc_interface *proc, const struct proc_status *s)
{
    if (!proc || !s) {
        return -1;
    }

    FILE *f = proc_open(proc, "status");
    if (!f) {
        return -1;
    }

    fprintf(f, "uptime_sec: %llu\n", (unsigned long long)s->uptime_sec);
    fprintf(f, "fw_version: %s\n", s->fw_version);
    fprintf(f, "state: %s\n", s->state);
    fclose(f);
    return 0;
}

int proc_write_perf_counters(struct proc_interface *proc,
                             const struct proc_perf_counters *p)
{
    if (!proc || !p) {
        return -1;
    }

    FILE *f = proc_open(proc, "perf_counters");
    if (!f) {
        return -1;
    }

    fprintf(f, "read_iops: %llu\n", (unsigned long long)p->read_iops);
    fprintf(f, "write_iops: %llu\n", (unsigned long long)p->write_iops);
    fprintf(f, "read_bw_mbps: %llu\n", (unsigned long long)p->read_bw_mbps);
    fprintf(f, "write_bw_mbps: %llu\n", (unsigned long long)p->write_bw_mbps);
    fprintf(f, "read_lat_p99_us: %llu\n", (unsigned long long)p->read_lat_p99_us);
    fprintf(f, "write_lat_p99_us: %llu\n", (unsigned long long)p->write_lat_p99_us);
    fclose(f);
    return 0;
}

int proc_write_ftl_stats(struct proc_interface *proc,
                         const struct proc_ftl_stats *f_stats)
{
    if (!proc || !f_stats) {
        return -1;
    }

    FILE *f = proc_open(proc, "ftl_stats");
    if (!f) {
        return -1;
    }

    fprintf(f, "l2p_hit_rate_pct: %u\n", f_stats->l2p_hit_rate_pct);
    fprintf(f, "gc_count: %llu\n", (unsigned long long)f_stats->gc_count);
    fprintf(f, "waf: %.2f\n", f_stats->waf);
    fprintf(f, "avg_erase_count: %llu\n", (unsigned long long)f_stats->avg_erase_count);
    fprintf(f, "max_erase_count: %llu\n", (unsigned long long)f_stats->max_erase_count);
    fclose(f);
    return 0;
}
```

- [ ] **Step 4: Add Makefile targets**

Append test binary variable after line 99 (`STRESS_ENTERPRISE`):

```makefile
TEST_PROC = $(BIN_DIR)/test_proc_interface
STRESS_STABILITY = $(BIN_DIR)/stress_stability
```

Add build rule (after the `STRESS_ENTERPRISE` rule, before `systest:`):

```makefile
$(TEST_PROC): $(TEST_DIR)/test_proc_interface.c $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-common -lm $(LDFLAGS)
```

Add `$(TEST_PROC)` to the `all:` target list and `test:` runner section:

```makefile
# In test: section, add before the final summary:
	@$(TEST_PROC)
	@echo ""
```

- [ ] **Step 5: Build and run test**

```bash
cd /Users/zifengyang/Desktop/hfsss-simulator
make -j$(sysctl -n hw.ncpu) 2>&1 | tail -3
./build/bin/test_proc_interface
```

Expected: All proc interface tests pass.

- [ ] **Step 6: Commit**

```bash
git add include/common/proc_interface.h src/common/proc_interface.c \
        tests/test_proc_interface.c Makefile
git commit -m "feat(REQ-124): add simulated /proc filesystem interface for macOS"
```

---

### Task 5: REQ-126 — Config Round-Trip Test Verification (Workstream 3c)

**Files:**
- Test: `tests/test_config.c` (already comprehensive)

- [ ] **Step 1: Run existing config tests and verify coverage**

```bash
cd /Users/zifengyang/Desktop/hfsss-simulator
./build/bin/test_config
```

Expected: All tests pass including save/load round-trip, unknown keys, NULL safety.

- [ ] **Step 2: Add round-trip test for ALL NAND fields**

Append to `tests/test_config.c` before `main()`, a new test that verifies every NAND field round-trips:

```c
static void test_full_nand_roundtrip(void) {
    separator();
    printf("Test: full NAND geometry round-trip\n");
    separator();

    const char *path = "/tmp/hfsss_test_nand_rt.yaml";
    struct hfsss_config orig;
    hfsss_config_defaults(&orig);

    /* Override every NAND field to non-default values */
    orig.nand.channel_count     = 32;
    orig.nand.chips_per_channel = 4;
    orig.nand.dies_per_chip     = 8;
    orig.nand.planes_per_die    = 4;
    orig.nand.blocks_per_plane  = 4096;
    orig.nand.pages_per_block   = 1024;
    orig.nand.page_size         = 32768;
    orig.nand.spare_size        = 512;
    orig.nand.op_ratio_pct      = 28;

    int ret = hfsss_config_save(path, &orig);
    TEST_ASSERT(ret == HFSSS_OK, "save returns OK");

    struct hfsss_config loaded;
    char errbuf[256] = {0};
    ret = hfsss_config_load(path, &loaded, errbuf, sizeof(errbuf));
    TEST_ASSERT(ret == HFSSS_OK, "load returns OK");

    TEST_ASSERT(loaded.nand.channel_count     == 32,    "channel_count 32 round-trips");
    TEST_ASSERT(loaded.nand.chips_per_channel == 4,     "chips_per_channel 4 round-trips");
    TEST_ASSERT(loaded.nand.dies_per_chip     == 8,     "dies_per_chip 8 round-trips");
    TEST_ASSERT(loaded.nand.planes_per_die    == 4,     "planes_per_die 4 round-trips");
    TEST_ASSERT(loaded.nand.blocks_per_plane  == 4096,  "blocks_per_plane 4096 round-trips");
    TEST_ASSERT(loaded.nand.pages_per_block   == 1024,  "pages_per_block 1024 round-trips");
    TEST_ASSERT(loaded.nand.page_size         == 32768, "page_size 32768 round-trips");
    TEST_ASSERT(loaded.nand.spare_size        == 512,   "spare_size 512 round-trips");
    TEST_ASSERT(loaded.nand.op_ratio_pct      == 28,    "op_ratio_pct 28 round-trips");

    remove(path);
}
```

Add `test_full_nand_roundtrip();` call in `main()` before the summary.

- [ ] **Step 3: Build and run**

```bash
cd /Users/zifengyang/Desktop/hfsss-simulator
make -j$(sysctl -n hw.ncpu) 2>&1 | tail -3
./build/bin/test_config
```

Expected: All tests pass including the new NAND round-trip test.

- [ ] **Step 4: Commit**

```bash
git add tests/test_config.c
git commit -m "test(REQ-126): add full NAND geometry round-trip config test"
```

---

### Task 6: REQ-132/134 — Stability Test Harness (Workstream 3d)

**Files:**
- Create: `tests/stress_stability.c`
- Modify: `Makefile`

- [ ] **Step 1: Create stress_stability.c**

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include "common/common.h"
#include "common/memory.h"
#include "common/fault_inject.h"
#include "media/nand.h"
#include "ftl/ftl.h"
#include "ftl/mapping.h"
#include "ftl/gc.h"

#define DEFAULT_DURATION_SEC   60
#define VERIFY_INTERVAL        1000   /* verify every N ops */
#define FAULT_INJECT_INTERVAL  5000   /* inject fault every N ops */
#define MAX_TEST_PAGES         4096
#define TEST_PAGE_SIZE         4096

static volatile int running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    running = 0;
}

struct stability_report {
    uint64_t total_ops;
    uint64_t read_ops;
    uint64_t write_ops;
    uint64_t verify_ops;
    uint64_t integrity_failures;
    uint64_t faults_injected;
    uint64_t errors_handled;
    uint64_t alloc_at_start;
    uint64_t alloc_at_end;
    double   elapsed_sec;
};

/* Simple PRNG for deterministic test data */
static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void fill_pattern(uint8_t *buf, uint32_t size, uint32_t seed)
{
    uint32_t state = seed;
    for (uint32_t i = 0; i < size; i += 4) {
        uint32_t val = xorshift32(&state);
        uint32_t remaining = size - i;
        uint32_t copy_len = remaining < 4 ? remaining : 4;
        memcpy(buf + i, &val, copy_len);
    }
}

static int verify_pattern(const uint8_t *buf, uint32_t size, uint32_t seed)
{
    uint8_t expected[TEST_PAGE_SIZE];
    fill_pattern(expected, size, seed);
    return memcmp(buf, expected, size) == 0;
}

static void print_report(const struct stability_report *r)
{
    printf("\n========================================\n");
    printf("Stability Test Report\n");
    printf("========================================\n");
    printf("Duration:            %.1f sec\n", r->elapsed_sec);
    printf("Total ops:           %llu\n", (unsigned long long)r->total_ops);
    printf("  Read ops:          %llu\n", (unsigned long long)r->read_ops);
    printf("  Write ops:         %llu\n", (unsigned long long)r->write_ops);
    printf("  Verify ops:        %llu\n", (unsigned long long)r->verify_ops);
    printf("Integrity failures:  %llu\n", (unsigned long long)r->integrity_failures);
    printf("Faults injected:     %llu\n", (unsigned long long)r->faults_injected);
    printf("Errors handled:      %llu\n", (unsigned long long)r->errors_handled);
    printf("Memory alloc start:  %llu\n", (unsigned long long)r->alloc_at_start);
    printf("Memory alloc end:    %llu\n", (unsigned long long)r->alloc_at_end);

    int64_t mem_delta = (int64_t)r->alloc_at_end - (int64_t)r->alloc_at_start;
    printf("Memory delta:        %lld bytes\n", (long long)mem_delta);
    printf("========================================\n");

    if (r->integrity_failures == 0 && labs(mem_delta) < 1048576) {
        printf("RESULT: PASS\n");
    } else {
        printf("RESULT: FAIL\n");
        if (r->integrity_failures > 0) {
            printf("  REASON: %llu integrity failures\n",
                   (unsigned long long)r->integrity_failures);
        }
        if (labs(mem_delta) >= 1048576) {
            printf("  REASON: memory delta %lld exceeds 1MB threshold\n",
                   (long long)mem_delta);
        }
    }
    printf("========================================\n");
}

int main(int argc, char *argv[])
{
    int duration_sec = DEFAULT_DURATION_SEC;

    /* Parse STRESS_DURATION from env or argv */
    const char *env_dur = getenv("STRESS_DURATION");
    if (env_dur) {
        duration_sec = atoi(env_dur);
    }
    if (argc > 1) {
        duration_sec = atoi(argv[1]);
    }
    if (duration_sec <= 0) {
        duration_sec = DEFAULT_DURATION_SEC;
    }

    printf("========================================\n");
    printf("HFSSS Stability Stress Test\n");
    printf("Duration: %d seconds\n", duration_sec);
    printf("========================================\n");

    signal(SIGINT, sigint_handler);

    struct stability_report report;
    memset(&report, 0, sizeof(report));

    /* Snapshot memory usage at start */
    report.alloc_at_start = mem_get_total_allocated();

    /* Allocate test buffers */
    uint8_t *write_buf = (uint8_t *)malloc(TEST_PAGE_SIZE);
    uint8_t *read_buf = (uint8_t *)malloc(TEST_PAGE_SIZE);
    uint32_t *seeds = (uint32_t *)calloc(MAX_TEST_PAGES, sizeof(uint32_t));

    if (!write_buf || !read_buf || !seeds) {
        fprintf(stderr, "Failed to allocate test buffers\n");
        free(write_buf);
        free(read_buf);
        free(seeds);
        return 1;
    }

    /* Initialize NAND device for testing (small geometry) */
    struct nand_device nand;
    int ret = nand_device_init(&nand, 2, 2, 2, 2, 256, 128, TEST_PAGE_SIZE, 64);
    if (ret != HFSSS_OK) {
        fprintf(stderr, "Failed to init NAND device: %d\n", ret);
        free(write_buf);
        free(read_buf);
        free(seeds);
        return 1;
    }

    uint32_t rng_state = (uint32_t)time(NULL);
    time_t start_time = time(NULL);
    time_t last_progress = start_time;

    /* Main stress loop */
    while (running) {
        time_t now = time(NULL);
        if ((now - start_time) >= duration_sec) {
            break;
        }

        /* Print progress every 10 seconds */
        if ((now - last_progress) >= 10) {
            printf("  [%lds] ops=%llu writes=%llu reads=%llu failures=%llu\n",
                   (long)(now - start_time),
                   (unsigned long long)report.total_ops,
                   (unsigned long long)report.write_ops,
                   (unsigned long long)report.read_ops,
                   (unsigned long long)report.integrity_failures);
            last_progress = now;
        }

        /* 70% read, 30% write */
        uint32_t op = xorshift32(&rng_state) % 100;
        uint32_t page = xorshift32(&rng_state) % MAX_TEST_PAGES;

        if (op < 30 || seeds[page] == 0) {
            /* Write operation */
            uint32_t seed = xorshift32(&rng_state) | 1;  /* ensure non-zero */
            fill_pattern(write_buf, TEST_PAGE_SIZE, seed);

            /* Simulate write to NAND (using raw page program) */
            uint32_t ch = page % 2;
            uint32_t chip = (page / 2) % 2;
            uint32_t die = (page / 4) % 2;
            uint32_t plane = (page / 8) % 2;
            uint32_t block = (page / 16) % 256;
            uint32_t pg = (page / 4096) % 128;

            ret = nand_page_program(&nand, ch, chip, die, plane, block, pg,
                                    write_buf, TEST_PAGE_SIZE);
            if (ret == HFSSS_OK) {
                seeds[page] = seed;
                report.write_ops++;
            } else {
                report.errors_handled++;
            }
        } else {
            /* Read operation */
            uint32_t ch = page % 2;
            uint32_t chip = (page / 2) % 2;
            uint32_t die = (page / 4) % 2;
            uint32_t plane = (page / 8) % 2;
            uint32_t block = (page / 16) % 256;
            uint32_t pg = (page / 4096) % 128;

            ret = nand_page_read(&nand, ch, chip, die, plane, block, pg,
                                 read_buf, TEST_PAGE_SIZE);
            if (ret == HFSSS_OK) {
                report.read_ops++;

                /* Verify every VERIFY_INTERVAL ops */
                if (report.total_ops % VERIFY_INTERVAL == 0 && seeds[page] != 0) {
                    if (!verify_pattern(read_buf, TEST_PAGE_SIZE, seeds[page])) {
                        report.integrity_failures++;
                    }
                    report.verify_ops++;
                }
            } else {
                report.errors_handled++;
            }
        }

        /* Periodic fault injection */
        if (report.total_ops > 0 && report.total_ops % FAULT_INJECT_INTERVAL == 0) {
            uint32_t fault_type = xorshift32(&rng_state) % 3;
            uint32_t fault_ch = xorshift32(&rng_state) % 2;
            uint32_t fault_chip = xorshift32(&rng_state) % 2;

            if (fault_type == 0) {
                fault_inject_add(FAULT_BIT_FLIP, fault_ch, fault_chip, 0, 0, 0, 0);
            } else if (fault_type == 1) {
                fault_inject_add(FAULT_READ_DISTURB, fault_ch, fault_chip, 0, 0, 0, 0);
            }
            /* Type 2: skip (rest period) */
            report.faults_injected++;
        }

        report.total_ops++;
    }

    time_t end_time = time(NULL);
    report.elapsed_sec = difftime(end_time, start_time);

    /* Snapshot memory usage at end */
    report.alloc_at_end = mem_get_total_allocated();

    nand_device_cleanup(&nand);

    print_report(&report);

    free(write_buf);
    free(read_buf);
    free(seeds);

    return (report.integrity_failures == 0) ? 0 : 1;
}
```

- [ ] **Step 2: Add Makefile target**

Add build rule (use the `STRESS_STABILITY` variable defined in Task 4 Step 4):

```makefile
$(STRESS_STABILITY): $(TEST_DIR)/stress_stability.c $(LIBHFSSS_FTL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_HAL) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-ftl -lhfsss-media -lhfsss-hal -lhfsss-common -lm $(LDFLAGS)
```

Add `$(STRESS_STABILITY)` to the `all:` target list.

Add a new phony target:

```makefile
.PHONY: stress-long
stress-long: all
	@echo "Running stability stress test (duration=$(or $(STRESS_DURATION),60)s)..."
	@STRESS_DURATION=$(or $(STRESS_DURATION),60) $(STRESS_STABILITY)
```

- [ ] **Step 3: Build and run short smoke test (60s)**

```bash
cd /Users/zifengyang/Desktop/hfsss-simulator
make -j$(sysctl -n hw.ncpu) 2>&1 | tail -3
./build/bin/stress_stability 10
```

Expected: Runs for 10 seconds, prints report, RESULT: PASS.

- [ ] **Step 4: Run via Makefile target**

```bash
make stress-long STRESS_DURATION=10
```

Expected: Same as above but invoked through make.

- [ ] **Step 5: Commit**

```bash
git add tests/stress_stability.c Makefile
git commit -m "test(REQ-132,REQ-134): add configurable stability stress test harness with fault injection"
```

---

### Task 7: Final Verification

- [ ] **Step 1: Full clean build**

```bash
cd /Users/zifengyang/Desktop/hfsss-simulator
make clean && make -j$(sysctl -n hw.ncpu) 2>&1 | tail -5
```

Expected: Build succeeds with no errors.

- [ ] **Step 2: Run all tests**

```bash
make test 2>&1 | grep -E "passed|failed|SUCCESS|FAIL"
```

Expected: All test suites pass, zero failures.

- [ ] **Step 3: Verify traceability completeness**

```bash
python3 -c "
import csv
with open('REQUIREMENTS_MATRIX_EN.csv') as f:
    rows = list(csv.reader(f))[1:]
empty = sum(1 for r in rows if not r[11].strip() or not r[12].strip())
print(f'Requirements with empty references: {empty}/174')
assert empty == 0
"
```

Expected: `Requirements with empty references: 0/174`

- [ ] **Step 4: Run short stability test**

```bash
./build/bin/stress_stability 15
```

Expected: RESULT: PASS

- [ ] **Step 5: Final commit summary**

```bash
git log --oneline -6
```

Expected: 6 commits covering all workstreams.
