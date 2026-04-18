# fio 014/015 Segmented Bisection — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the workload-sweep and pipeline-segmentation tooling described in `docs/superpowers/specs/2026-04-16-fio-014-015-segmented-bisection-design.md`, execute the staged bisection, and produce a decision document that identifies the failing segment and root-cause candidate for fio tests 014/015.

**Architecture:** Two stages. Stage W is a single-axis fio parameter scan against the existing QEMU blackbox harness, summarized into a matrix. Stage P introduces a compile-time trace ring (per-thread, lockless, dump-on-shutdown), five instrumentation sites in the NBD/FTL/media path, a Python analyzer, an fio-over-NBD bypass harness, and a C program that drives the FTL API directly without NBD or QEMU.

**Tech Stack:** C11 (GNU), POSIX pthreads, bash, python3 (stdlib only), fio, nvme-cli, QEMU 8.x with HVF on macOS.

**Out of scope for this plan:** The actual root-cause fix. Fix specifics depend on Seg-3 trace evidence; they will be written into a follow-up plan after Task 15 completes.

---

## File Structure

**New files:**

- `scripts/qemu_blackbox/sweep/matrix.json` — sweep point definition
- `scripts/qemu_blackbox/sweep/summarize.py` — fio JSON + stderr → matrix.md aggregator
- `scripts/qemu_blackbox/sweep/fio_sweep.sh` — Stage W driver (QEMU reuse loop)
- `scripts/qemu_blackbox/sweep/test_summarize.py` — unit test for summarize
- `include/common/trace.h` — public API for the trace ring (header lives under `include/` by project convention)
- `src/common/trace.c` — per-thread lockless ring + dump-on-shutdown
- `tests/test_trace.c` — unit test for the ring
- `scripts/qemu_blackbox/phase_a/fio_over_nbd.sh` — Seg-1 host-side fio harness
- `scripts/qemu_blackbox/phase_a/analyze_trace.py` — Seg-3 analyzer
- `scripts/qemu_blackbox/phase_a/test_analyze_trace.py` — unit test for analyzer
- `tools/ftl_mfc_repro.c` — Seg-2 direct FTL harness

**Modified files:**

- `Makefile` — add `TRACE` build flag (`-DHFSSS_DEBUG_TRACE=1` when set), add `tests/test_trace.c` target, add `tools/ftl_mfc_repro.c` target
- `src/vhost/hfsss_nbd_server.c` — T1 trace site (after request parse)
- `src/ftl/ftl_worker.c` — T2 trace site (after dequeue)
- `src/ftl/mapping.c` or `src/ftl/ftl.c` — T3 trace site (after PPN resolved)
- `src/ftl/ftl.c` — T4 trace site (before HAL write/read)
- `src/hal/hal_nand.c` or the media caller site — T5 trace site (after HAL returns)

Each instrumentation edit is guarded by `#ifdef HFSSS_DEBUG_TRACE` and is a no-op when the flag is off.

**Output artifacts (runtime, not committed):**

- `artifacts/sweep-<ts>/matrix.md`
- `artifacts/sweep-<ts>/decision.md`
- `artifacts/phase-a-<ts>/seg{1,2,3,4}-results.md`
- `artifacts/phase-a-<ts>/trace.bin`
- `artifacts/phase-a-<ts>/final-decision.md`

---

### Task 1: Baseline sanity and directory scaffolding

**Files:**
- Create: `scripts/qemu_blackbox/sweep/.gitkeep`
- Create: `scripts/qemu_blackbox/phase_a/.gitkeep`
- Create: `tools/.gitkeep`
- Create: `artifacts/.gitkeep`
- Modify: `.gitignore` — ignore `artifacts/*` contents except `.gitkeep`

- [ ] **Step 1: Confirm branch**

Run: `git status && git branch --show-current`
Expected: on `chore/fio-014-015-investigation-plan`, working tree clean except the spec file committed.

- [ ] **Step 2: Confirm baseline builds and tests pass**

Run: `make clean && make all 2>&1 | tail -20`
Expected: no errors; `build/bin/hfsss-nbd-server` produced.

Run: `make test 2>&1 | tail -20`
Expected: all tests report PASS.

- [ ] **Step 3: Create empty directories with .gitkeep**

```bash
mkdir -p scripts/qemu_blackbox/sweep scripts/qemu_blackbox/phase_a tools artifacts
touch scripts/qemu_blackbox/sweep/.gitkeep scripts/qemu_blackbox/phase_a/.gitkeep tools/.gitkeep artifacts/.gitkeep
```

- [ ] **Step 4: Update .gitignore**

Append to `.gitignore`:
```
# Investigation artifacts (runtime only)
artifacts/*
!artifacts/.gitkeep
```

- [ ] **Step 5: Commit**

```bash
git add scripts/qemu_blackbox/sweep/.gitkeep scripts/qemu_blackbox/phase_a/.gitkeep tools/.gitkeep artifacts/.gitkeep .gitignore
git commit -m "chore: scaffold directories for 014/015 bisection tooling"
```

---

### Task 2: Stage W sweep matrix definition

**Files:**
- Create: `scripts/qemu_blackbox/sweep/matrix.json`

- [ ] **Step 1: Write the matrix file**

Create `scripts/qemu_blackbox/sweep/matrix.json` with:

```json
{
  "_comment": "Stage W single-axis sweep definition for 014/015 bisection. See docs/superpowers/specs/2026-04-16-fio-014-015-segmented-bisection-design.md",
  "baseline": {
    "rw": "randrw",
    "rwmixread": 70,
    "bs": "4k",
    "direct": 1,
    "ioengine": "libaio",
    "iodepth": 64,
    "numjobs": 1,
    "size": "1G",
    "io_size": "2G",
    "verify": "crc32c",
    "verify_fatal": 0,
    "verify_async": 4,
    "do_verify": 1,
    "randrepeat": 0,
    "name": "sweep"
  },
  "axes": [
    {"name": "iodepth", "points": [1, 4, 16, 64]},
    {"name": "rwmix", "fio_param": "rwmixread", "points": [0, 70, 100]},
    {"name": "numjobs", "points": [1, 2, 4]},
    {"name": "verify_async", "points": [0, 2, 4]},
    {"name": "bs", "points": ["4k", "16k"]}
  ],
  "repeats": 3,
  "format_between_axis_switch": true
}
```

**Note:** The plan originally used YAML for readability, but PyYAML is not available on this host's system Python (PEP 668 on macOS). JSON removes that dependency and remains readable.

- [ ] **Step 2: Validate JSON parses**

Run: `python3 -c "import json; json.load(open('scripts/qemu_blackbox/sweep/matrix.json'))"`
Expected: no output, exit 0. (No third-party dependency needed — JSON is stdlib.)

- [ ] **Step 3: Commit**

```bash
git add scripts/qemu_blackbox/sweep/matrix.json
git commit -m "feat: define Stage W fio sweep matrix"
```

---

### Task 3: Stage W summarizer (Python)

**Files:**
- Create: `scripts/qemu_blackbox/sweep/summarize.py`
- Create: `scripts/qemu_blackbox/sweep/test_summarize.py`

- [ ] **Step 1: Write the failing test**

Create `scripts/qemu_blackbox/sweep/test_summarize.py`:

```python
#!/usr/bin/env python3
"""Unit tests for summarize.py."""
import json
import os
import tempfile
import unittest
from pathlib import Path

from summarize import count_verify_errors, classify_point, summarize_run


class TestCountVerifyErrors(unittest.TestCase):
    def test_zero_errors_stderr(self):
        stderr = "some normal output\nno errors here\n"
        self.assertEqual(count_verify_errors(stderr), 0)

    def test_one_verify_error(self):
        stderr = "verify: bad magic header 0x1234, wanted 0xacca\n"
        self.assertEqual(count_verify_errors(stderr), 1)

    def test_fio_verify_error(self):
        stderr = "fio: verify type mismatch at offset 4096\n"
        self.assertEqual(count_verify_errors(stderr), 1)

    def test_multiple_errors(self):
        stderr = (
            "verify: bad magic header 0x1\n"
            "verify: hdr_fail wanted 0xacca\n"
            "fio: verify type mismatch\n"
        )
        self.assertEqual(count_verify_errors(stderr), 3)


class TestClassifyPoint(unittest.TestCase):
    def test_all_pass(self):
        self.assertEqual(classify_point([0, 0, 0]), "PASS")

    def test_one_fail(self):
        self.assertEqual(classify_point([0, 0, 3]), "FLAKY")

    def test_two_fail(self):
        self.assertEqual(classify_point([0, 5, 2]), "SUSPECT")

    def test_three_fail(self):
        self.assertEqual(classify_point([1, 5, 2]), "FAIL")


class TestSummarizeRun(unittest.TestCase):
    def test_run_with_errors(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            (td / "run.stderr").write_text("verify: bad magic header\nverify: hdr_fail\n")
            (td / "run.json").write_text(json.dumps({
                "jobs": [{"error": 84, "total_err": 2}]
            }))
            result = summarize_run(td / "run")
            self.assertEqual(result["err_stderr"], 2)
            self.assertEqual(result["err_json"], 2)
            self.assertEqual(result["json_error_code"], 84)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd scripts/qemu_blackbox/sweep && python3 -m unittest test_summarize -v`
Expected: ImportError — `summarize` module not found.

- [ ] **Step 3: Write the implementation**

Create `scripts/qemu_blackbox/sweep/summarize.py`:

```python
#!/usr/bin/env python3
"""Aggregate fio sweep run output into a markdown matrix.

Usage:
    summarize.py --artifact-dir <path> --matrix <matrix.json> --out <matrix.md>

Consumes per-run <stem>.stderr and <stem>.json files produced by fio_sweep.sh
and emits a markdown table of axis x point x status.
"""
import argparse
import json
import re
import sys
from pathlib import Path
from typing import Dict, List


VERIFY_RE = re.compile(r"^(verify:|fio: verify)", re.MULTILINE)


def count_verify_errors(stderr_text: str) -> int:
    """Count fio-emitted verify error lines in stderr output."""
    return len(VERIFY_RE.findall(stderr_text))


def classify_point(err_counts: List[int]) -> str:
    """Classify a sweep point given per-repeat error counts."""
    failing = sum(1 for c in err_counts if c > 0)
    if failing == 0:
        return "PASS"
    if failing == 1:
        return "FLAKY"
    if failing == 2:
        return "SUSPECT"
    return "FAIL"


def summarize_run(stem: Path) -> Dict:
    """Summarize one run given the path stem (no extension).

    Reads <stem>.stderr and <stem>.json.
    """
    stderr_path = Path(str(stem) + ".stderr")
    json_path = Path(str(stem) + ".json")
    err_stderr = 0
    if stderr_path.exists():
        err_stderr = count_verify_errors(stderr_path.read_text(errors="replace"))
    err_json = 0
    json_error_code = 0
    if json_path.exists():
        try:
            data = json.loads(json_path.read_text())
            for j in data.get("jobs", []):
                err_json += int(j.get("total_err", 0))
                if int(j.get("error", 0)) != 0:
                    json_error_code = int(j.get("error", 0))
        except json.JSONDecodeError:
            pass
    return {
        "stem": str(stem),
        "err_stderr": err_stderr,
        "err_json": err_json,
        "json_error_code": json_error_code,
    }


def build_matrix(artifact_dir: Path, matrix_json: Path) -> str:
    cfg = json.loads(matrix_json.read_text())
    repeats = int(cfg.get("repeats", 3))
    out_lines = ["# Stage W Sweep Matrix", ""]
    for axis in cfg["axes"]:
        axis_name = axis["name"]
        param = axis.get("fio_param", axis_name)
        out_lines.append(f"## Axis: {axis_name} ({param})")
        out_lines.append("")
        out_lines.append("| Point | " + " | ".join(f"Rep{i+1}" for i in range(repeats))
                         + " | Status |")
        out_lines.append("|" + "---|" * (repeats + 2))
        for point in axis["points"]:
            counts = []
            for rep in range(repeats):
                stem = artifact_dir / f"{axis_name}_{point}_rep{rep+1}"
                r = summarize_run(stem)
                counts.append(r["err_stderr"])
            status = classify_point(counts)
            row = "| " + str(point) + " | "
            row += " | ".join(str(c) for c in counts) + f" | {status} |"
            out_lines.append(row)
        out_lines.append("")
    return "\n".join(out_lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--artifact-dir", type=Path, required=True)
    ap.add_argument("--matrix", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    md = build_matrix(args.artifact_dir, args.matrix)
    args.out.write_text(md + "\n")
    print(f"Wrote {args.out}")


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Run the tests and verify they pass**

Run: `cd scripts/qemu_blackbox/sweep && python3 -m unittest test_summarize -v`
Expected: 3 test cases, all PASS.

- [ ] **Step 5: Commit**

```bash
git add scripts/qemu_blackbox/sweep/summarize.py scripts/qemu_blackbox/sweep/test_summarize.py
git commit -m "feat: sweep summarizer with error-count classification"
```

---

### Task 4: Stage W sweep driver (bash)

**Files:**
- Create: `scripts/qemu_blackbox/sweep/fio_sweep.sh`

- [ ] **Step 1: Write the driver**

Create `scripts/qemu_blackbox/sweep/fio_sweep.sh`:

```bash
#!/bin/bash
# Stage W fio parameter sweep driver.
#
# Requires a running QEMU + NBD environment (see scripts/qemu_blackbox/run.sh).
# Reuses the guest for the full sweep. Formats the NVMe namespace only between
# axis switches.
#
# Usage:
#   fio_sweep.sh --matrix <matrix.json> --artifact-dir <dir> [--dry-run]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
# shellcheck source=../lib/env.sh
. "$REPO_ROOT/scripts/qemu_blackbox/lib/env.sh"

MATRIX=""
ARTIFACT_DIR=""
DRY_RUN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --matrix) MATRIX="$2"; shift 2;;
        --artifact-dir) ARTIFACT_DIR="$2"; shift 2;;
        --dry-run) DRY_RUN=1; shift;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done

[ -n "$MATRIX" ] || { echo "--matrix required" >&2; exit 2; }
[ -n "$ARTIFACT_DIR" ] || { echo "--artifact-dir required" >&2; exit 2; }
mkdir -p "$ARTIFACT_DIR"

run_one() {
    local axis="$1" point="$2" rep="$3" fio_args="$4"
    local stem="$ARTIFACT_DIR/${axis}_${point}_rep${rep}"
    echo "[sweep] $axis=$point rep=$rep"
    if [ "$DRY_RUN" = "1" ]; then
        echo "DRY: fio $fio_args > $stem.stdout 2> $stem.stderr"
        return 0
    fi
    hfsss_guest_capture "$stem.stdout" "$stem.stderr" \
        "fio --output-format=json $fio_args" || true
    python3 - "$stem.stdout" "$stem.json" <<'PY' || true
import json, pathlib, sys
raw = pathlib.Path(sys.argv[1]).read_text(errors="replace")
start = raw.find("{"); end = raw.rfind("}")
if start >= 0 and end > start:
    pathlib.Path(sys.argv[2]).write_text(
        json.dumps(json.loads(raw[start:end+1]), indent=2) + "\n")
PY
}

format_ns() {
    echo "[sweep] nvme format /dev/nvme0n1"
    if [ "$DRY_RUN" = "1" ]; then
        echo "DRY: nvme format /dev/nvme0n1 -s 0 -f"
        return 0
    fi
    hfsss_guest_capture /dev/null /dev/null \
        "nvme format /dev/nvme0n1 -s 0 -f" || true
}

python3 - "$MATRIX" "$ARTIFACT_DIR" "$DRY_RUN" <<'PY'
import sys, json, subprocess, os
matrix_path, artifact_dir, dry_run = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
cfg = json.load(open(matrix_path))
base = cfg["baseline"]
repeats = int(cfg.get("repeats", 3))
first_axis = True
for axis in cfg["axes"]:
    axis_name = axis["name"]
    fio_param = axis.get("fio_param", axis_name)
    if cfg.get("format_between_axis_switch") and not first_axis:
        subprocess.run(
            ["bash", "-c",
             "source scripts/qemu_blackbox/sweep/fio_sweep.sh; format_ns"],
            check=False)
    first_axis = False
    for point in axis["points"]:
        merged = dict(base)
        merged[fio_param] = point
        fio_args = " ".join(
            f"--{k}={v}" for k, v in merged.items() if v is not None)
        fio_args += f" --filename=$HFSSS_GUEST_NVME_DEV"
        for rep in range(1, repeats + 1):
            stem = os.path.join(artifact_dir, f"{axis_name}_{point}_rep{rep}")
            script = (f"bash -c 'source scripts/qemu_blackbox/sweep/fio_sweep.sh; "
                      f"run_one {axis_name} {point} {rep} \"{fio_args}\"'")
            subprocess.run(["bash", "-c", script], check=False)
PY

echo "[sweep] done. Artifacts in $ARTIFACT_DIR"
```

- [ ] **Step 2: Make executable**

Run: `chmod +x scripts/qemu_blackbox/sweep/fio_sweep.sh`

- [ ] **Step 3: Smoke test with dry-run**

Run:
```bash
mkdir -p /tmp/sweep_dry && ./scripts/qemu_blackbox/sweep/fio_sweep.sh \
    --matrix scripts/qemu_blackbox/sweep/matrix.json \
    --artifact-dir /tmp/sweep_dry --dry-run 2>&1 | head -20
```
Expected: output contains `DRY: fio ... --iodepth=1 ...`, `DRY: fio ... --iodepth=4 ...`, etc.

- [ ] **Step 4: Commit**

```bash
git add scripts/qemu_blackbox/sweep/fio_sweep.sh
git commit -m "feat: Stage W sweep driver with QEMU guest reuse"
```

---

### Task 5: Trace ring core (per-thread lockless)

**Files:**
- Create: `include/common/trace.h`
- Create: `src/common/trace.c`
- Modify: `Makefile` (add `TRACE` build flag)
- Create: `tests/test_trace.c`

- [ ] **Step 1: Write the public header**

Create `include/common/trace.h`:

```c
/* Per-thread lockless trace ring for IO path data-flow debugging.
 * Enabled only when compiled with -DHFSSS_DEBUG_TRACE=1; otherwise a no-op.
 */
#ifndef HFSSS_COMMON_TRACE_H
#define HFSSS_COMMON_TRACE_H

#include <stdint.h>
#include <stddef.h>

enum trace_point_id {
    TRACE_POINT_T1_NBD_RECV = 1,
    TRACE_POINT_T2_WORKER_DEQ = 2,
    TRACE_POINT_T3_PPN_DONE = 3,
    TRACE_POINT_T4_PRE_HAL = 4,
    TRACE_POINT_T5_POST_HAL = 5,
};

enum trace_op {
    TRACE_OP_READ = 0,
    TRACE_OP_WRITE = 1,
    TRACE_OP_TRIM = 2,
};

#pragma pack(push, 1)
struct trace_record {
    uint64_t tsc;
    uint64_t lba;
    uint64_t ppn_or_len;
    uint32_t point_id;
    uint32_t op;
    uint32_t crc32c;
    uint32_t extra;
    uint32_t thread_id;
    uint32_t _pad;
}; /* 48 bytes */
#pragma pack(pop)

#ifdef HFSSS_DEBUG_TRACE

void trace_init(const char *dump_path);
void trace_shutdown(void);

void trace_emit(uint32_t point_id, uint32_t op, uint64_t lba,
                uint64_t ppn_or_len, uint32_t crc32c, uint32_t extra);

uint32_t trace_crc32c(const void *data, size_t len);

#define TRACE_EMIT(pid, op, lba, extra_u64, crc, extra_u32) \
    trace_emit((pid), (op), (lba), (extra_u64), (crc), (extra_u32))

#else /* !HFSSS_DEBUG_TRACE */

#define trace_init(p) ((void)0)
#define trace_shutdown() ((void)0)
#define TRACE_EMIT(pid, op, lba, extra_u64, crc, extra_u32) ((void)0)

static inline uint32_t trace_crc32c(const void *data, size_t len) {
    (void)data; (void)len; return 0;
}

#endif /* HFSSS_DEBUG_TRACE */

#endif /* HFSSS_COMMON_TRACE_H */
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_trace.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "common/trace.h"

static int total = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    total++; \
    if (cond) { printf("  [PASS] %s\n", msg); passed++; } \
    else      { printf("  [FAIL] %s\n", msg); failed++; } \
} while (0)

#ifdef HFSSS_DEBUG_TRACE

static void *worker(void *arg)
{
    int tid = *(int *)arg;
    for (int i = 0; i < 100; i++) {
        TRACE_EMIT(TRACE_POINT_T1_NBD_RECV, TRACE_OP_WRITE,
                   /* lba */ (uint64_t)tid * 1000 + i,
                   /* ppn_or_len */ 4096,
                   /* crc */ 0xdeadbeef,
                   /* extra */ (uint32_t)tid);
    }
    return NULL;
}

static void test_multi_thread_dump(void)
{
    printf("\n=== trace: multi-thread dump ===\n");
    const char *path = "/tmp/test_trace_dump.bin";
    unlink(path);
    trace_init(path);
    pthread_t t[4];
    int ids[4] = {1, 2, 3, 4};
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, &ids[i]);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    trace_shutdown();
    FILE *f = fopen(path, "rb");
    TEST_ASSERT(f != NULL, "dump file created");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fclose(f);
        TEST_ASSERT(size == 4 * 100 * (long)sizeof(struct trace_record),
                    "dump file has 400 records");
    }
}

static void test_crc32c_stable(void)
{
    printf("\n=== trace: crc32c stable ===\n");
    const char *s = "hello world";
    uint32_t a = trace_crc32c(s, 11);
    uint32_t b = trace_crc32c(s, 11);
    TEST_ASSERT(a == b, "same input -> same crc");
    TEST_ASSERT(a != 0, "crc is non-zero for non-empty input");
}

int main(void)
{
    test_crc32c_stable();
    test_multi_thread_dump();
    printf("\nResults: %d/%d passed, %d failed\n", passed, total, failed);
    return failed > 0 ? 1 : 0;
}

#else

int main(void)
{
    printf("trace instrumentation disabled (HFSSS_DEBUG_TRACE=0), skipping\n");
    return 0;
}

#endif
```

- [ ] **Step 3: Write the implementation**

Create `src/common/trace.c`:

```c
/* Per-thread lockless trace ring. Each thread lazily allocates a ring on
 * first emit. A global registry (mutex-protected, contended only at ring
 * birth/death) tracks all rings; trace_shutdown walks the registry and
 * dumps each ring to the configured file path in a single pass.
 */
#ifdef HFSSS_DEBUG_TRACE

#include "common/trace.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>

#define TRACE_RING_CAPACITY (64 * 1024) /* 64K records/thread * 48B = 3 MiB */

struct trace_ring {
    struct trace_record recs[TRACE_RING_CAPACITY];
    atomic_ulong head;   /* next write slot; monotonically increasing */
    uint32_t thread_id;
    struct trace_ring *next;
};

static __thread struct trace_ring *tls_ring = NULL;
static struct trace_ring *g_ring_list = NULL;
static pthread_mutex_t g_ring_mtx = PTHREAD_MUTEX_INITIALIZER;
static char g_dump_path[512];
static atomic_uint g_next_tid = 0;

static uint64_t read_tsc(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void free_tls_ring(void *ring)
{
    (void)ring;
    /* rings are freed by trace_shutdown; TLS destructor not needed */
}

static struct trace_ring *get_or_create_ring(void)
{
    if (tls_ring) return tls_ring;
    struct trace_ring *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->thread_id = atomic_fetch_add(&g_next_tid, 1) + 1;
    atomic_store(&r->head, 0);
    pthread_mutex_lock(&g_ring_mtx);
    r->next = g_ring_list;
    g_ring_list = r;
    pthread_mutex_unlock(&g_ring_mtx);
    tls_ring = r;
    return r;
}

void trace_init(const char *dump_path)
{
    if (dump_path) {
        strncpy(g_dump_path, dump_path, sizeof(g_dump_path) - 1);
        g_dump_path[sizeof(g_dump_path) - 1] = '\0';
    }
}

void trace_emit(uint32_t point_id, uint32_t op, uint64_t lba,
                uint64_t ppn_or_len, uint32_t crc32c, uint32_t extra)
{
    struct trace_ring *r = get_or_create_ring();
    if (!r) return;
    unsigned long slot = atomic_fetch_add(&r->head, 1) % TRACE_RING_CAPACITY;
    struct trace_record *rec = &r->recs[slot];
    rec->tsc = read_tsc();
    rec->lba = lba;
    rec->ppn_or_len = ppn_or_len;
    rec->point_id = point_id;
    rec->op = op;
    rec->crc32c = crc32c;
    rec->extra = extra;
    rec->thread_id = r->thread_id;
    rec->_pad = 0;
}

void trace_shutdown(void)
{
    if (g_dump_path[0] == '\0') return;
    FILE *f = fopen(g_dump_path, "wb");
    if (!f) return;
    pthread_mutex_lock(&g_ring_mtx);
    for (struct trace_ring *r = g_ring_list; r; r = r->next) {
        unsigned long head = atomic_load(&r->head);
        unsigned long count = head < TRACE_RING_CAPACITY ? head : TRACE_RING_CAPACITY;
        unsigned long start = head < TRACE_RING_CAPACITY ? 0 : head % TRACE_RING_CAPACITY;
        for (unsigned long i = 0; i < count; i++) {
            unsigned long idx = (start + i) % TRACE_RING_CAPACITY;
            fwrite(&r->recs[idx], sizeof(struct trace_record), 1, f);
        }
    }
    pthread_mutex_unlock(&g_ring_mtx);
    fclose(f);
    /* Free rings. */
    pthread_mutex_lock(&g_ring_mtx);
    struct trace_ring *r = g_ring_list;
    while (r) {
        struct trace_ring *n = r->next;
        free(r);
        r = n;
    }
    g_ring_list = NULL;
    tls_ring = NULL;
    pthread_mutex_unlock(&g_ring_mtx);
    (void)free_tls_ring; /* silence unused */
}

/* Software CRC32C (Castagnoli). Correct and stable; speed is not critical
 * because trace is a debug-only build.
 */
uint32_t trace_crc32c(const void *data, size_t len)
{
    static const uint32_t POLY = 0x82F63B78u;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (POLY & mask);
        }
    }
    return ~crc;
}

#endif /* HFSSS_DEBUG_TRACE */
```

- [ ] **Step 4: Add Makefile TRACE flag**

Find the ASan block in `Makefile` (near the `ASAN ?= 0` section, around the coverage/sanitizer variant section) and append a similar block:

```makefile
# Trace instrumentation build variant
TRACE ?= 0
ifeq ($(TRACE),1)
    CFLAGS += -DHFSSS_DEBUG_TRACE=1
endif
```

- [ ] **Step 5: Add test_trace Makefile rule**

Find the test rules section (look for `$(TEST_TAA):` and similar) and add:

```makefile
TEST_TRACE = $(BIN_DIR)/test_trace
$(TEST_TRACE): $(TEST_DIR)/test_trace.c $(LIBHFSSS_COMMON)
	@mkdir -p $(BIN_DIR)
	@$(CC) $(CFLAGS) $(TEST_DIR)/test_trace.c -o $@ -L$(LIB_DIR) -lhfsss-common $(LDFLAGS)
```

And add `$(TEST_TRACE)` to the `test` target's dependency list (near other `$(TEST_*)` entries).

Run: `grep -n '^test:' Makefile`
Expected: locate the `test:` target. Append `$(TEST_TRACE)` to its dependency list and invocation list.

- [ ] **Step 6: Run the test to verify it fails (trace disabled mode)**

Run: `make clean && make test 2>&1 | tail -20`
Expected: `test_trace` runs and reports "trace instrumentation disabled, skipping". PASS path.

- [ ] **Step 7: Run the test with TRACE=1 to verify real-trace path**

Run: `make clean && TRACE=1 make $(BIN_DIR)/test_trace && ./build/bin/test_trace`
(if command substitution fails, just: `TRACE=1 make all && ./build/bin/test_trace`)
Expected: `Results: 4/4 passed, 0 failed`. `/tmp/test_trace_dump.bin` contains 400 × 48 = 19200 bytes.

Run: `ls -la /tmp/test_trace_dump.bin`
Expected: size 19200.

- [ ] **Step 8: Commit**

```bash
git add include/common/trace.h src/common/trace.c tests/test_trace.c Makefile
git commit -m "feat: per-thread lockless trace ring with HFSSS_DEBUG_TRACE gate"
```

---

### Task 6: Install trace instrumentation at T1–T5

**Files:**
- Modify: `src/vhost/hfsss_nbd_server.c` (T1)
- Modify: `src/ftl/ftl_worker.c` (T2)
- Modify: `src/ftl/mapping.c` or `src/ftl/ftl.c` (T3, wherever TAA insert/update returns PPN on the write hot-path)
- Modify: `src/ftl/ftl.c` (T4, just before calling HAL write/read)
- Modify: `src/hal/hal_nand.c` (T5, just after `media_nand_*` returns)

- [ ] **Step 1: Locate exact insertion sites**

Run: `grep -nE 'nbd_send_reply|handle_read|handle_write|ftl_write|ftl_read|hal_nand_(program|read)' src/vhost/hfsss_nbd_server.c src/ftl/ftl_worker.c src/ftl/ftl.c src/ftl/mapping.c src/hal/hal_nand.c | head -40`

Expected: Lines identifying entry points. The exact function names vary by file. The insertion rule:

- **T1**: in `hfsss_nbd_server.c`, immediately after the NBD protocol handler has decoded an IO request and has `op`, `lba (or offset)`, `len`, and the `data buffer pointer` (for writes). For reads, the crc is 0 at T1 (data not yet known).
- **T2**: in `ftl_worker.c`, immediately after the worker has popped a request from its queue and knows `worker_id`.
- **T3**: in `ftl.c` / `mapping.c`, immediately after the code that resolves or allocates the PPN (look for `taa_insert` / `taa_update` / CWB allocation).
- **T4**: in `ftl.c`, immediately before the call site that hands the buffer to the HAL (look for `hal_nand_program` or `hal_nand_read`).
- **T5**: in `hal_nand.c`, immediately after `media_nand_program` / `media_nand_read` returns.

- [ ] **Step 2: Add T1 trace**

In `src/vhost/hfsss_nbd_server.c`, add `#include "common/trace.h"` near the other headers. At the T1 insertion point (after the NBD request is decoded), insert:

```c
#ifdef HFSSS_DEBUG_TRACE
{
    uint32_t crc = (op == TRACE_OP_WRITE && data != NULL)
                   ? trace_crc32c(data, len)
                   : 0;
    TRACE_EMIT(TRACE_POINT_T1_NBD_RECV, op, (uint64_t)offset / 4096,
               (uint64_t)len, crc, 0);
}
#endif
```

Adapt `op`, `offset`, `len`, `data` to the local variable names in the function; add a comment noting the adaptation.

- [ ] **Step 3: Add T2 trace**

In `src/ftl/ftl_worker.c`, add `#include "common/trace.h"`. After the worker dequeues a request and has `worker_id` and the decoded request, insert:

```c
#ifdef HFSSS_DEBUG_TRACE
TRACE_EMIT(TRACE_POINT_T2_WORKER_DEQ, req->op, req->lba, req->len, 0,
           worker_id);
#endif
```

- [ ] **Step 4: Add T3 trace**

In the file that resolves PPN on the write path (typically `src/ftl/ftl.c` around `taa_insert` on writes, or `src/ftl/mapping.c`), add `#include "common/trace.h"`. After PPN is determined and before it goes to HAL:

```c
#ifdef HFSSS_DEBUG_TRACE
TRACE_EMIT(TRACE_POINT_T3_PPN_DONE, op, lba, (uint64_t)ppn.raw, 0,
           (uint32_t)shard_id);
#endif
```

On the read path, same pattern: T3 fires after `taa_lookup` returns PPN.

- [ ] **Step 5: Add T4 trace**

In `src/ftl/ftl.c` (or wherever the HAL call is made), immediately before the `hal_nand_program` / `hal_nand_read` call:

```c
#ifdef HFSSS_DEBUG_TRACE
{
    uint32_t crc = (op == TRACE_OP_WRITE && buf != NULL)
                   ? trace_crc32c(buf, 4096)
                   : 0;
    TRACE_EMIT(TRACE_POINT_T4_PRE_HAL, op, lba, (uint64_t)ppn.raw, crc, 0);
}
#endif
```

- [ ] **Step 6: Add T5 trace**

In `src/hal/hal_nand.c`, immediately after the `media_nand_program` / `media_nand_read` return:

```c
#ifdef HFSSS_DEBUG_TRACE
{
    uint32_t crc = (op == TRACE_OP_READ && buf != NULL)
                   ? trace_crc32c(buf, 4096)
                   : 0;
    TRACE_EMIT(TRACE_POINT_T5_POST_HAL, op, /* lba not known here, pass 0 */ 0,
               (uint64_t)ppn.raw, crc, (uint32_t)rc);
}
#endif
```

If `lba` is not in scope in the HAL function, pass 0; the analyzer joins on `(op, ppn)` for T4/T5 pairing.

- [ ] **Step 7: Verify build with trace off**

Run: `make clean && make all 2>&1 | tail -5`
Expected: no warnings, no errors. All libs and binaries built.

- [ ] **Step 8: Verify build with trace on**

Run: `make clean && TRACE=1 make all 2>&1 | tail -5`
Expected: no warnings, no errors. Builds with `-DHFSSS_DEBUG_TRACE=1`.

- [ ] **Step 9: Verify no regression with trace off**

Run: `make clean && make test 2>&1 | grep -E 'failed|passed' | tail -5`
Expected: all tests PASS, no regressions.

- [ ] **Step 10: Commit**

```bash
git add src/vhost/hfsss_nbd_server.c src/ftl/ftl_worker.c src/ftl/ftl.c src/ftl/mapping.c src/hal/hal_nand.c
git commit -m "feat: instrument T1-T5 trace sites on NBD/FTL/HAL IO path"
```

---

### Task 7: Trace analyzer (Python)

**Files:**
- Create: `scripts/qemu_blackbox/phase_a/analyze_trace.py`
- Create: `scripts/qemu_blackbox/phase_a/test_analyze_trace.py`

- [ ] **Step 1: Write the failing test**

Create `scripts/qemu_blackbox/phase_a/test_analyze_trace.py`:

```python
#!/usr/bin/env python3
"""Unit tests for analyze_trace.py."""
import struct
import tempfile
import unittest
from pathlib import Path

from analyze_trace import (
    RECORD_FMT, RECORD_SIZE, read_records, build_chains,
    first_corrupt_hop,
)

OP_WRITE, OP_READ = 1, 0
T1, T2, T3, T4, T5 = 1, 2, 3, 4, 5


def pack(tsc, lba, ppn, pid, op, crc, extra, tid):
    return struct.pack(RECORD_FMT, tsc, lba, ppn, pid, op, crc, extra, tid, 0)


class TestReadRecords(unittest.TestCase):
    def test_record_size(self):
        self.assertEqual(RECORD_SIZE, 48)

    def test_read_one(self):
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(pack(1, 100, 0, T1, OP_WRITE, 0xdead, 0, 1))
            f.write(pack(2, 100, 0x200, T2, OP_WRITE, 0, 0, 1))
            path = f.name
        recs = read_records(Path(path))
        self.assertEqual(len(recs), 2)
        self.assertEqual(recs[0]["point_id"], T1)
        self.assertEqual(recs[1]["point_id"], T2)


class TestChains(unittest.TestCase):
    def test_clean_chain(self):
        recs = [
            {"tsc": 1, "lba": 42, "ppn": 0, "point_id": T1, "op": OP_WRITE,
             "crc": 0xaaaa, "extra": 0, "tid": 1},
            {"tsc": 2, "lba": 42, "ppn": 0, "point_id": T2, "op": OP_WRITE,
             "crc": 0, "extra": 0, "tid": 1},
            {"tsc": 3, "lba": 42, "ppn": 0x100, "point_id": T3, "op": OP_WRITE,
             "crc": 0, "extra": 0, "tid": 1},
            {"tsc": 4, "lba": 42, "ppn": 0x100, "point_id": T4, "op": OP_WRITE,
             "crc": 0xaaaa, "extra": 0, "tid": 1},
            {"tsc": 5, "lba": 0, "ppn": 0x100, "point_id": T5, "op": OP_WRITE,
             "crc": 0, "extra": 0, "tid": 1},
        ]
        chains = build_chains(recs)
        self.assertEqual(len(chains), 1)
        self.assertIsNone(first_corrupt_hop(chains[0]))

    def test_t1_t4_mismatch(self):
        recs = [
            {"tsc": 1, "lba": 42, "ppn": 0, "point_id": T1, "op": OP_WRITE,
             "crc": 0xaaaa, "extra": 0, "tid": 1},
            {"tsc": 4, "lba": 42, "ppn": 0x100, "point_id": T4, "op": OP_WRITE,
             "crc": 0xbbbb, "extra": 0, "tid": 1},
        ]
        chains = build_chains(recs)
        hop = first_corrupt_hop(chains[0])
        self.assertEqual(hop, "T1->T4")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd scripts/qemu_blackbox/phase_a && python3 -m unittest test_analyze_trace -v`
Expected: ImportError or ModuleNotFoundError.

- [ ] **Step 3: Write the implementation**

Create `scripts/qemu_blackbox/phase_a/analyze_trace.py`:

```python
#!/usr/bin/env python3
"""Analyze a trace.bin dump produced by HFSSS_DEBUG_TRACE and identify
the first corrupt hop per IO chain.

Binary record format (little-endian, 48 bytes):
    <Q Q Q I I I I I I>
    tsc, lba, ppn_or_len, point_id, op, crc32c, extra, thread_id, _pad

Chains are grouped by (op, lba) and by (op, ppn) at T4/T5.
"""
import argparse
import struct
from collections import defaultdict
from pathlib import Path
from typing import Dict, List, Optional

RECORD_FMT = "<QQQIIIIII"
RECORD_SIZE = struct.calcsize(RECORD_FMT)

OP_READ, OP_WRITE, OP_TRIM = 0, 1, 2
T1, T2, T3, T4, T5 = 1, 2, 3, 4, 5


def read_records(path: Path) -> List[Dict]:
    data = path.read_bytes()
    assert len(data) % RECORD_SIZE == 0, (
        f"file size {len(data)} not a multiple of {RECORD_SIZE}")
    out = []
    for i in range(0, len(data), RECORD_SIZE):
        (tsc, lba, ppn_or_len, pid, op, crc, extra, tid, _pad) = struct.unpack(
            RECORD_FMT, data[i:i + RECORD_SIZE])
        out.append({
            "tsc": tsc, "lba": lba, "ppn": ppn_or_len,
            "point_id": pid, "op": op, "crc": crc,
            "extra": extra, "tid": tid,
        })
    out.sort(key=lambda r: r["tsc"])
    return out


def build_chains(recs: List[Dict]) -> List[List[Dict]]:
    """Group records into per-IO chains.

    Chain identity: (op, lba) for T1/T2/T3/T4 entries, then by ppn for
    T5 (which lacks lba). For a simple analyzer we pair T4 to T5 on ppn.
    """
    by_op_lba: Dict[tuple, List[Dict]] = defaultdict(list)
    t5_by_op_ppn: Dict[tuple, List[Dict]] = defaultdict(list)
    for r in recs:
        if r["point_id"] == T5:
            t5_by_op_ppn[(r["op"], r["ppn"])].append(r)
        else:
            by_op_lba[(r["op"], r["lba"])].append(r)
    chains = []
    for (op, lba), chain_recs in by_op_lba.items():
        chain_recs.sort(key=lambda r: r["point_id"])
        t4 = next((r for r in chain_recs if r["point_id"] == T4), None)
        if t4 is not None:
            t5_cands = t5_by_op_ppn.get((op, t4["ppn"]), [])
            t5_match = next((r for r in t5_cands if r["tsc"] >= t4["tsc"]), None)
            if t5_match:
                chain_recs.append(t5_match)
        chains.append(chain_recs)
    return chains


def first_corrupt_hop(chain: List[Dict]) -> Optional[str]:
    """Return the name of the first corrupt hop in a chain, or None if clean.

    For writes: expect T1.crc == T4.crc (data intact to media submission).
    For reads: expect T4.crc == T5.crc where T4 is the prior write-of-same-lba
              at same PPN (cross-chain linkage, simplified here).
    """
    by_pid = {r["point_id"]: r for r in chain}
    op = chain[0]["op"] if chain else -1
    if op == OP_WRITE:
        t1 = by_pid.get(T1)
        t4 = by_pid.get(T4)
        if t1 and t4 and t1["crc"] != 0 and t4["crc"] != 0:
            if t1["crc"] != t4["crc"]:
                return "T1->T4"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    recs = read_records(args.trace)
    chains = build_chains(recs)
    corrupt_hops: Dict[str, int] = defaultdict(int)
    for c in chains:
        h = first_corrupt_hop(c)
        if h:
            corrupt_hops[h] += 1
    lines = ["# Trace Analysis", "",
             f"Total records: {len(recs)}",
             f"Total chains: {len(chains)}", ""]
    if not corrupt_hops:
        lines.append("No corrupt hops detected.")
    else:
        lines.append("| Hop | Count |")
        lines.append("|---|---|")
        for hop, count in sorted(corrupt_hops.items(), key=lambda x: -x[1]):
            lines.append(f"| {hop} | {count} |")
    args.out.write_text("\n".join(lines) + "\n")
    print(f"Wrote {args.out}")


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Run the tests and verify they pass**

Run: `cd scripts/qemu_blackbox/phase_a && python3 -m unittest test_analyze_trace -v`
Expected: 3 test cases, all PASS.

- [ ] **Step 5: Commit**

```bash
git add scripts/qemu_blackbox/phase_a/analyze_trace.py scripts/qemu_blackbox/phase_a/test_analyze_trace.py
git commit -m "feat: trace analyzer with first-corrupt-hop classification"
```

---

### Task 8: fio-over-NBD harness (Seg-1)

**Files:**
- Create: `scripts/qemu_blackbox/phase_a/fio_over_nbd.sh`

- [ ] **Step 1: Write the harness**

Create `scripts/qemu_blackbox/phase_a/fio_over_nbd.sh`:

```bash
#!/bin/bash
# Seg-1: fio directly against the hfsss NBD server, bypassing QEMU.
#
# Requires: running hfsss-nbd-server on HOST, reachable at 127.0.0.1:$PORT.
# Requires: fio with ioengine=nbd (built with --enable-libnbd).
#
# Usage:
#   fio_over_nbd.sh --mfc <mfc.json> --artifact-dir <dir> --port <port>
set -euo pipefail

MFC=""
ARTIFACT_DIR=""
PORT="10820"

while [ $# -gt 0 ]; do
    case "$1" in
        --mfc) MFC="$2"; shift 2;;
        --artifact-dir) ARTIFACT_DIR="$2"; shift 2;;
        --port) PORT="$2"; shift 2;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done

[ -n "$MFC" ] || { echo "--mfc required (path to MFC json from Stage W)" >&2; exit 2; }
[ -n "$ARTIFACT_DIR" ] || { echo "--artifact-dir required" >&2; exit 2; }
mkdir -p "$ARTIFACT_DIR"

# Build fio args from MFC json.
FIO_ARGS=$(python3 - "$MFC" <<'PY'
import sys, json
cfg = json.load(open(sys.argv[1]))
out = []
for k, v in cfg.items():
    out.append(f"--{k}={v}")
print(" ".join(out))
PY
)

for rep in 1 2 3; do
    stem="$ARTIFACT_DIR/seg1_rep${rep}"
    echo "[seg-1] rep=$rep"
    fio --output-format=json \
        --ioengine=nbd --uri=nbd://127.0.0.1:${PORT} \
        $FIO_ARGS > "$stem.stdout" 2> "$stem.stderr" || true
    # Extract JSON from stdout
    python3 - "$stem.stdout" "$stem.json" <<'PY' || true
import json, pathlib, sys
raw = pathlib.Path(sys.argv[1]).read_text(errors="replace")
s = raw.find("{"); e = raw.rfind("}")
if s >= 0 and e > s:
    pathlib.Path(sys.argv[2]).write_text(
        json.dumps(json.loads(raw[s:e+1]), indent=2) + "\n")
PY
done

# Quick tally using the sweep summarizer's count function
python3 - "$ARTIFACT_DIR" <<'PY'
import sys, pathlib, re
d = pathlib.Path(sys.argv[1])
RE = re.compile(r"^(verify:|fio: verify)", re.MULTILINE)
total = 0
for p in sorted(d.glob("seg1_rep*.stderr")):
    n = len(RE.findall(p.read_text(errors='replace')))
    print(f"  {p.name}: {n} verify errors")
    total += n
print(f"[seg-1] total verify errors: {total}")
PY
```

- [ ] **Step 2: Make executable**

Run: `chmod +x scripts/qemu_blackbox/phase_a/fio_over_nbd.sh`

- [ ] **Step 3: Syntax check**

Run: `bash -n scripts/qemu_blackbox/phase_a/fio_over_nbd.sh`
Expected: no output, exit 0.

- [ ] **Step 4: Commit**

```bash
git add scripts/qemu_blackbox/phase_a/fio_over_nbd.sh
git commit -m "feat: Seg-1 fio-over-NBD harness bypassing QEMU"
```

---

### Task 9: FTL direct API harness (Seg-2)

**Files:**
- Create: `tools/ftl_mfc_repro.c`
- Modify: `Makefile` (add rule for `tools/ftl_mfc_repro.c`)

- [ ] **Step 1: Write the harness**

Create `tools/ftl_mfc_repro.c`:

```c
/* Seg-2: Drive the FTL API directly with an MFC-equivalent multi-thread
 * randrw + verify workload. No NBD, no QEMU, no fio. Self-contained
 * integrity check: each writer records (lba, sha256(data)) in a shared
 * map; readers verify that reads match the recorded hash.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ftl/ftl.h"
#include "media/media.h"
#include "hal/hal_nand.h"
#include "common/log.h"
#include "common/trace.h"

/* Minimal local "sha256" → we use trace_crc32c via public trace.h header
 * because portability and speed matter more than cryptographic strength.
 * A collision on 4 KiB random data is astronomically unlikely for a
 * 2-minute run at 64 threads.
 */
#ifndef HFSSS_DEBUG_TRACE
#warning "ftl_mfc_repro requires TRACE=1 to access trace_crc32c; using a local copy"
static uint32_t local_crc32c(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0x82F63B78u & mask);
        }
    }
    return ~crc;
}
#define CRC32C(d, l) local_crc32c((d), (l))
#else
#define CRC32C(d, l) trace_crc32c((d), (l))
#endif

/* Config — parameterized from CLI to match MFC from Stage W. */
struct repro_cfg {
    uint64_t total_lbas;
    uint32_t threads;
    uint32_t rwmix_read_pct;
    uint32_t duration_sec;
    uint32_t block_size;
};

struct lba_record {
    uint32_t crc;
    bool valid;
};

struct repro_ctx {
    struct ftl_ctx *ftl;
    struct repro_cfg cfg;
    struct lba_record *map;          /* size = total_lbas */
    pthread_mutex_t *map_locks;       /* stripe locks, 1 per 1024 LBAs */
    atomic_ulong mismatch_count;
    atomic_ulong op_count;
    atomic_int stop;
};

static size_t map_stripe(uint64_t lba) { return (size_t)(lba / 1024); }

static uint32_t next_rand(uint32_t *state) {
    /* xorshift32 */
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *state = x; return x;
}

static void *worker(void *arg) {
    struct repro_ctx *ctx = (struct repro_ctx *)arg;
    uint32_t seed = (uint32_t)(uintptr_t)pthread_self();
    uint32_t bs = ctx->cfg.block_size;
    uint8_t *buf = calloc(1, bs);
    uint8_t *cmp = calloc(1, bs);
    if (!buf || !cmp) { free(buf); free(cmp); return NULL; }

    while (!atomic_load(&ctx->stop)) {
        uint64_t lba = next_rand(&seed) % ctx->cfg.total_lbas;
        bool is_read = (next_rand(&seed) % 100) < ctx->cfg.rwmix_read_pct;
        size_t stripe = map_stripe(lba);

        if (is_read) {
            int rc = ftl_read(ctx->ftl, lba, 1, cmp);
            if (rc == 0) {
                pthread_mutex_lock(&ctx->map_locks[stripe]);
                struct lba_record rec = ctx->map[lba];
                pthread_mutex_unlock(&ctx->map_locks[stripe]);
                if (rec.valid) {
                    uint32_t got = CRC32C(cmp, bs);
                    if (got != rec.crc) {
                        atomic_fetch_add(&ctx->mismatch_count, 1);
                        fprintf(stderr, "[MISMATCH] lba=%llu expected=0x%x got=0x%x\n",
                                (unsigned long long)lba, rec.crc, got);
                    }
                }
            }
        } else {
            for (size_t i = 0; i < bs; i++) buf[i] = (uint8_t)(next_rand(&seed) & 0xff);
            uint32_t crc = CRC32C(buf, bs);
            int rc = ftl_write(ctx->ftl, lba, 1, buf);
            if (rc == 0) {
                pthread_mutex_lock(&ctx->map_locks[stripe]);
                ctx->map[lba].crc = crc;
                ctx->map[lba].valid = true;
                pthread_mutex_unlock(&ctx->map_locks[stripe]);
            }
        }
        atomic_fetch_add(&ctx->op_count, 1);
    }
    free(buf); free(cmp);
    return NULL;
}

int main(int argc, char **argv) {
    struct repro_cfg cfg = {
        .total_lbas = 128 * 1024,  /* 512 MiB @ 4K LBA */
        .threads = 64,
        .rwmix_read_pct = 70,
        .duration_sec = 120,
        .block_size = 4096,
    };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--threads") && i + 1 < argc) cfg.threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rwmix") && i + 1 < argc) cfg.rwmix_read_pct = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--duration") && i + 1 < argc) cfg.duration_sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lbas") && i + 1 < argc) cfg.total_lbas = strtoull(argv[++i], NULL, 0);
    }
    printf("[ftl_mfc_repro] threads=%u rwmix=%u%% duration=%us lbas=%llu\n",
           cfg.threads, cfg.rwmix_read_pct, cfg.duration_sec,
           (unsigned long long)cfg.total_lbas);

    /* Minimal FTL init — caller must ensure test geometry is sane for the
     * cfg.total_lbas. For a 512 MiB working set with 4 channels / 4 planes
     * / 128 blocks / 64 pages this works out to ~16 GiB of raw NAND which
     * leaves enough over-provisioning for read/write mix without GC stress.
     */
    struct media_config mc = {
        .channels = 4, .dies_per_channel = 2, .planes_per_die = 4,
        .blocks_per_plane = 128, .pages_per_block = 64,
        .page_size = 4096,
    };
    struct media_ctx *media = NULL;
    if (media_init(&media, &mc) != 0) { fprintf(stderr, "media_init failed\n"); return 1; }
    struct hal_nand_ctx *hal = NULL;
    if (hal_nand_init(&hal, media) != 0) { fprintf(stderr, "hal_nand_init failed\n"); return 1; }
    struct ftl_ctx *ftl = NULL;
    struct ftl_config fc = { .total_lbas = cfg.total_lbas };
    if (ftl_init(&ftl, hal, &fc) != 0) { fprintf(stderr, "ftl_init failed\n"); return 1; }

    struct repro_ctx ctx = { .ftl = ftl, .cfg = cfg };
    ctx.map = calloc(cfg.total_lbas, sizeof(struct lba_record));
    size_t stripes = (cfg.total_lbas + 1023) / 1024;
    ctx.map_locks = calloc(stripes, sizeof(pthread_mutex_t));
    for (size_t i = 0; i < stripes; i++) pthread_mutex_init(&ctx.map_locks[i], NULL);
    atomic_store(&ctx.mismatch_count, 0);
    atomic_store(&ctx.op_count, 0);
    atomic_store(&ctx.stop, 0);

    pthread_t *threads = calloc(cfg.threads, sizeof(pthread_t));
    for (uint32_t i = 0; i < cfg.threads; i++)
        pthread_create(&threads[i], NULL, worker, &ctx);
    sleep(cfg.duration_sec);
    atomic_store(&ctx.stop, 1);
    for (uint32_t i = 0; i < cfg.threads; i++) pthread_join(threads[i], NULL);

    unsigned long ops = atomic_load(&ctx.op_count);
    unsigned long mm = atomic_load(&ctx.mismatch_count);
    printf("[ftl_mfc_repro] ops=%lu mismatches=%lu rate=%.2e\n",
           ops, mm, ops ? (double)mm / (double)ops : 0.0);

    /* Cleanup */
    for (size_t i = 0; i < stripes; i++) pthread_mutex_destroy(&ctx.map_locks[i]);
    free(ctx.map); free(ctx.map_locks); free(threads);
    return mm > 0 ? 2 : 0;
}
```

**Note:** the exact FTL/media/HAL init signatures above are a template. If the real APIs differ, adapt the init calls to match what `tests/test_ftl.c` does. The rest of the harness is unchanged.

- [ ] **Step 2: Add Makefile rule**

Append to `Makefile`, near other tool binaries (after `HFSSS_NBD` section):

```makefile
FTL_MFC_REPRO = $(BIN_DIR)/ftl_mfc_repro
$(FTL_MFC_REPRO): tools/ftl_mfc_repro.c $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@mkdir -p $(BIN_DIR)
	@$(CC) $(CFLAGS) tools/ftl_mfc_repro.c -o $@ -L$(LIB_DIR) -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common $(LDFLAGS)
```

Add `$(FTL_MFC_REPRO)` to the top-level `all:` target.

- [ ] **Step 3: Compile**

Run: `make clean && make $(FTL_MFC_REPRO) 2>&1 | tail -10`
(Or: `make all`.)
Expected: build succeeds. If FTL/media init signatures in the template don't match real APIs, compile errors will guide the adaptation.

- [ ] **Step 4: Smoke run (5 seconds)**

Run: `./build/bin/ftl_mfc_repro --threads 4 --duration 5 --lbas 4096`
Expected: prints `ops=<N> mismatches=0 rate=0.00e+00` (baseline run with a well-behaved FTL should have no mismatches under light load).

- [ ] **Step 5: Commit**

```bash
git add tools/ftl_mfc_repro.c Makefile
git commit -m "feat: Seg-2 direct FTL API repro harness"
```

---

### Task 10: Execute Stage W and produce MFC

**Artifact:** this task produces `artifacts/sweep-<ts>/matrix.md` and `artifacts/sweep-<ts>/decision.md`. No code commit.

- [ ] **Step 1: Start the QEMU environment with mt mode**

Run: `./scripts/qemu_blackbox/run.sh --mode mt --size-mb 2048 --keep-env --skip-build 2>&1 | tail -5`
Expected: environment comes up, one or two smoke cases run, QEMU stays running (keep-env).

(Alternative if running from a fresh state: drop `--skip-build`.)

- [ ] **Step 2: Run the sweep**

Run:
```bash
TS=$(date +%Y%m%d_%H%M%S)
OUT="artifacts/sweep-${TS}"
mkdir -p "$OUT"
./scripts/qemu_blackbox/sweep/fio_sweep.sh \
    --matrix scripts/qemu_blackbox/sweep/matrix.json \
    --artifact-dir "$OUT" 2>&1 | tee "$OUT/driver.log"
```
Expected: ~2 hours. Tail shows `[sweep] done`. `$OUT` contains ~45 triplets of `.stdout`/`.stderr`/`.json` files.

- [ ] **Step 3: Produce the matrix**

Run:
```bash
python3 scripts/qemu_blackbox/sweep/summarize.py \
    --artifact-dir "$OUT" \
    --matrix scripts/qemu_blackbox/sweep/matrix.json \
    --out "$OUT/matrix.md"
cat "$OUT/matrix.md"
```
Expected: markdown table printed to stdout showing per-axis point status.

- [ ] **Step 4: Decide MFC**

Inspect `$OUT/matrix.md`. Identify:
- Which axis has the cleanest PASS→FAIL transition (call this the "primary axis")
- The minimum failing point on that axis
- Whether another axis is a necessary co-factor

Write `$OUT/decision.md` with:
```markdown
# Stage W Decision

## Matrix Summary
<paste matrix.md summary>

## Minimum Failing Configuration (MFC)

| Parameter | Value |
|---|---|
| iodepth | <value> |
| rwmix   | <value> |
| ... (only parameters that matter)

## Observed error rate at MFC: <N>/1M IOs

## Next step
Seg-4 (UT regression) then Seg-1 with the above MFC.
```

Also commit an `mfc.json` to `$OUT/mfc.json` containing just the MFC parameters in a form the Seg-1 harness can consume:
```json
{
  "rw": "randrw",
  "rwmixread": "<from MFC>",
  "bs": "<from MFC>",
  "iodepth": "<from MFC>",
  "io_size": "2G",
  "verify": "crc32c",
  "verify_fatal": 0,
  "do_verify": 1,
  "direct": 1,
  "ioengine": "libaio",
  "randrepeat": 0,
  "name": "mfc_repro"
}
```

- [ ] **Step 5: Commit the artifact**

(Artifacts are `.gitignore`d, but the decision document is valuable history. Copy the MFC + decision out of `artifacts/` into the repo.)

```bash
mkdir -p docs/superpowers/artifacts
cp "$OUT/matrix.md" "docs/superpowers/artifacts/stage-w-matrix.md"
cp "$OUT/decision.md" "docs/superpowers/artifacts/stage-w-decision.md"
cp "$OUT/mfc.json" "docs/superpowers/artifacts/mfc.json"
git add docs/superpowers/artifacts/stage-w-matrix.md docs/superpowers/artifacts/stage-w-decision.md docs/superpowers/artifacts/mfc.json
git commit -m "docs: Stage W sweep matrix and MFC decision"
```

---

### Task 11: Execute Seg-4 (UT regression)

- [ ] **Step 1: Run all unit tests on current branch**

Run: `make clean && make test 2>&1 | tee artifacts/phase-a-seg4.log | tail -30`
Expected: all tests PASS, 0 failures.

- [ ] **Step 2: Write seg4-results.md**

Run:
```bash
mkdir -p artifacts/phase-a-$(date +%Y%m%d)
cat > artifacts/phase-a-$(date +%Y%m%d)/seg4-results.md <<EOF
# Seg-4: Unit Test Regression
- Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)
- Status: $(grep -c 'passed' artifacts/phase-a-seg4.log || echo 'unknown')
- Log: artifacts/phase-a-seg4.log
EOF
```

**Branch decision:** if any UT failed, STOP the plan here. File an issue/fix that first, then restart Stage P.

- [ ] **Step 3: No commit needed (artifacts are ignored)**

Proceed to Task 12.

---

### Task 12: Execute Seg-1 (fio-over-NBD)

- [ ] **Step 1: Verify fio has NBD engine**

Run: `fio --enghelp=nbd 2>&1 | head -10`
Expected: help text printed. If "unknown engine" — install `fio` with libnbd support (`brew install fio` on macOS usually has it; `apt install fio` on Linux usually has it since 3.x).

- [ ] **Step 2: Ensure hfsss-nbd-server is running on host**

Run: `pgrep -fa hfsss-nbd-server`
Expected: at least one line showing the server running on port 10820 (from Task 10 keep-env).

- [ ] **Step 3: Run fio-over-NBD with MFC**

Run:
```bash
OUT="artifacts/phase-a-$(date +%Y%m%d)"
mkdir -p "$OUT"
./scripts/qemu_blackbox/phase_a/fio_over_nbd.sh \
    --mfc docs/superpowers/artifacts/mfc.json \
    --artifact-dir "$OUT" --port 10820 2>&1 | tee "$OUT/seg1-driver.log"
```
Expected: 3 × ~2-min runs complete. Final line: `[seg-1] total verify errors: <N>`.

- [ ] **Step 4: Interpret and record**

Write `$OUT/seg1-results.md`:
```markdown
# Seg-1: fio-over-NBD (bypass QEMU)
- Date: <date>
- MFC: see docs/superpowers/artifacts/mfc.json
- Rep1 errors: <N>
- Rep2 errors: <N>
- Rep3 errors: <N>
- Total: <N>

## Interpretation
- If total == 0 across all 3 reps: failure requires QEMU/kernel path (layers 1-4).
  → Mark 014/015 as out-of-scope for hfsss-only fix. STOP.
- If total > 0 with rate comparable to 014/015: failure is in hfsss (layers 5-8).
  → Proceed to Seg-2.

## Decision
<PASS or FAIL and the branch taken>
```

- [ ] **Step 5: Commit the Seg-1 decision**

```bash
cp "$OUT/seg1-results.md" docs/superpowers/artifacts/seg1-results.md
git add docs/superpowers/artifacts/seg1-results.md
git commit -m "docs: Seg-1 fio-over-NBD result"
```

---

### Task 13: Execute Seg-2 (FTL direct harness)

**Skip this task if Seg-1 PASSED (failure is in QEMU/kernel).**

- [ ] **Step 1: Run the repro harness with MFC-equivalent parameters**

Extract threads ≈ iodepth, rwmix and duration from MFC (pin duration to 180 seconds to get enough IOs). Example for `iodepth=64, rwmix=70`:

Run:
```bash
OUT="artifacts/phase-a-$(date +%Y%m%d)"
./build/bin/ftl_mfc_repro \
    --threads 64 --rwmix 70 --duration 180 --lbas 131072 \
    2>&1 | tee "$OUT/seg2.log"
```
Expected: prints final `ops=<N> mismatches=<M> rate=<...>`.

- [ ] **Step 2: Interpret and record**

Write `$OUT/seg2-results.md`:
```markdown
# Seg-2: FTL direct API harness (bypass NBD)
- Date: <date>
- Config: threads=64 rwmix=70 duration=180s lbas=131072
- Ops: <N>
- Mismatches: <M>
- Rate: <...>

## Interpretation
- If mismatches == 0: NBD layer owns the bug. Proceed to Seg-3 with NBD focus.
- If mismatches > 0: FTL+media owns the bug. Proceed to Seg-3 with FTL-write focus.

## Decision
<NBD-focused or FTL-focused>
```

- [ ] **Step 3: Commit the Seg-2 decision**

```bash
cp "$OUT/seg2-results.md" docs/superpowers/artifacts/seg2-results.md
git add docs/superpowers/artifacts/seg2-results.md
git commit -m "docs: Seg-2 FTL direct harness result"
```

---

### Task 14: Execute Seg-3 (trace-based analysis)

- [ ] **Step 1: Rebuild with trace enabled**

Run: `make clean && TRACE=1 make all 2>&1 | tail -5`
Expected: build succeeds. The NBD server, FTL libs, and harness are all compiled with `-DHFSSS_DEBUG_TRACE=1`.

- [ ] **Step 2: Enable trace dump path and run the failing workload**

The failing workload is either (a) the 014 case via QEMU or (b) the Seg-1 fio-over-NBD harness — whichever gave the cleanest failure signal in prior segments.

The trace needs a shutdown hook. Verify the NBD server calls `trace_shutdown()` on SIGTERM — if it doesn't, add that as a small code change:

Run: `grep -n 'trace_shutdown' src/vhost/hfsss_nbd_server.c`
If no match: patch the shutdown handler to call `trace_shutdown()` after all workers have been joined. Add `trace_init("artifacts/phase-a-<date>/trace.bin")` at startup.

- [ ] **Step 3: Run the workload**

Run:
```bash
OUT="artifacts/phase-a-$(date +%Y%m%d)"
mkdir -p "$OUT"
# Assuming (a) 014 via QEMU:
./scripts/qemu_blackbox/run.sh --mode mt --size-mb 2048 --case 014_fio_pre_checkin_stress 2>&1 \
    | tee "$OUT/seg3-driver.log"
# After the case, the NBD server shutdown writes artifacts/phase-a-<date>/trace.bin.
```
Expected: fails with ~N verify errors as in prior runs.

- [ ] **Step 4: Run the analyzer**

Run:
```bash
python3 scripts/qemu_blackbox/phase_a/analyze_trace.py \
    --trace "$OUT/trace.bin" --out "$OUT/seg3-trace-analysis.md"
cat "$OUT/seg3-trace-analysis.md"
```
Expected: markdown with "Total records", "Total chains", and a hop-corruption table.

- [ ] **Step 5: Record the final decision**

Write `$OUT/final-decision.md`:
```markdown
# Final Decision (Stage P output)
- Seg-4: <pass/fail>
- Seg-1: <decision>
- Seg-2: <decision>
- Seg-3: first corrupt hop = <T1->T4 | T2->T3 | ...>
- Corrupt-hop count: <N>

## Root-cause segment
<NBD | FTL worker | TAA/PPN | HAL | media>

## Next-step
Write a follow-up plan (Task 15) targeting the root-cause segment.
```

- [ ] **Step 6: Commit**

```bash
cp "$OUT/seg3-trace-analysis.md" docs/superpowers/artifacts/seg3-trace-analysis.md
cp "$OUT/final-decision.md" docs/superpowers/artifacts/final-decision.md
git add docs/superpowers/artifacts/seg3-trace-analysis.md docs/superpowers/artifacts/final-decision.md
git commit -m "docs: Seg-3 trace analysis and final segment decision"
```

---

### Task 15: Write the follow-up root-cause fix plan

This task produces a new plan document targeting the specific failing segment identified in Task 14. It does NOT implement the fix; it plans the fix.

- [ ] **Step 1: Invoke the writing-plans skill**

Create a new plan at `docs/superpowers/plans/2026-04-16-fio-014-015-root-cause-fix.md` based on the final-decision.md content. The plan should include:
- A concrete bug hypothesis (cite the trace hop)
- A failing regression unit test (to be written first, TDD)
- The fix (targeted, focused)
- Verification: 3× clean runs of 014 at full 8 GiB io_size

- [ ] **Step 2: Commit the new plan**

```bash
git add docs/superpowers/plans/2026-04-16-fio-014-015-root-cause-fix.md
git commit -m "docs: root-cause fix plan for fio 014/015"
```

- [ ] **Step 3: Push the investigation branch and open PR**

```bash
git push -u origin chore/fio-014-015-investigation-plan
gh pr create --title "chore: fio 014/015 bisection infrastructure + stage-W/P decision" \
    --body "$(cat <<'EOF'
## Summary
- Adds spec: segmented bisection plan for fio 014/015 verify failures
- Adds Stage W tooling: sweep matrix, driver, summarizer
- Adds Stage P tooling: per-thread trace ring, instrumentation sites, analyzer, fio-over-NBD harness, FTL-direct harness
- Executes the bisection and records the final failing-segment decision in `docs/superpowers/artifacts/`
- Opens a follow-up plan for the targeted root-cause fix (next PR)

## Test plan
- [ ] `make test` all green with TRACE off (default)
- [ ] `make test` all green with TRACE=1 (trace ring exercised in test_trace)
- [ ] Stage W sweep completed, matrix captured
- [ ] Seg-1/Seg-2/Seg-3 executed, decisions recorded
- [ ] Follow-up fix plan written
EOF
)"
```

---

## Self-Review

**Spec coverage check (against `docs/superpowers/specs/2026-04-16-fio-014-015-segmented-bisection-design.md`):**

| Spec section | Plan task(s) |
|---|---|
| Stage W matrix.json | Task 2 |
| Stage W summarize.py | Task 3 |
| Stage W fio_sweep.sh | Task 4 |
| Stage W execution | Task 10 |
| Seg-1 fio-over-NBD harness | Task 8 |
| Seg-1 execution | Task 12 |
| Seg-2 tools/ftl_mfc_repro.c | Task 9 |
| Seg-2 execution | Task 13 |
| Seg-3 trace ring infrastructure | Task 5 |
| Seg-3 instrumentation sites T1-T5 | Task 6 |
| Seg-3 analyzer | Task 7 |
| Seg-3 execution | Task 14 |
| Seg-4 regression run | Task 11 |
| Fix + regression UT | Task 15 (plan only; fix deferred to next plan) |

**Placeholder scan:** All steps have concrete code, commands, and expected outputs. Tasks 10–14 contain template sections for artifact documents (decision.md, seg*-results.md) that must be filled with actual numbers from the run — this is intentional (run-time data) rather than a plan placeholder.

**Type consistency:** `trace.h` defines `enum trace_point_id`, `enum trace_op`, `struct trace_record`, `trace_emit`, `trace_init`, `trace_shutdown`, `trace_crc32c`. All task-6 call sites use `TRACE_EMIT(...)` with the correct parameter order matching the `trace_emit` signature. `struct trace_record` layout matches the Python `RECORD_FMT` `<QQQIIIIII>` (48 bytes).

**Scope check:** All 15 tasks produce independently buildable, testable, committable artifacts. No one task requires a later task's output to complete.

---

## Follow-up

The root-cause fix itself is deliberately NOT in this plan. Fix specifics depend on Task 14's trace evidence. Task 15 produces the follow-up plan that will include:
- The specific failing code location (from trace)
- A new permanent regression UT (promoted from Seg-2 harness if FTL-side, or a new `tests/test_nbd_concurrent.c` if NBD-side)
- The targeted fix commit
- Validation: 3 × full 014 runs at 8 GiB io_size clean
