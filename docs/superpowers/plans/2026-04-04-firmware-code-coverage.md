# HFSSS Firmware Code Coverage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add parallel coverage build variant (`build-cov/`) with gcov+lcov, producing three HTML reports (UT, E2E, merged) scoped to HFSSS firmware only, enforced in CI via ratchet floor.

**Architecture:** A `COVERAGE=1` Makefile switch activates `--coverage -O0` instrumentation and redirects output to `build-cov/`. Shell scripts under `scripts/coverage/` drive UT/E2E capture, filter out non-firmware paths (`src/vhost`, `src/kernel`, `src/tools`, `tests/`) via `lcov --remove`, and generate HTML via `genhtml`. A JSON baseline (`.coverage-baseline.json`) holds the ratchet reference; CI fails any PR that drops any metric > 2% below baseline.

**Tech Stack:** gcc `--coverage` (gcov), lcov + genhtml, bash scripts, GNU Make, GitHub Actions, `gh` CLI for PR comments.

**Spec reference:** `docs/superpowers/specs/2026-04-04-firmware-code-coverage-design.md`

---

## File Structure

**Files to create:**
- `scripts/coverage/reset_counters.sh` — deletes stale `.gcda` files
- `scripts/coverage/run_ut_coverage.sh` — runs UT suite, captures lcov, outputs HTML
- `scripts/coverage/run_e2e_coverage.sh` — runs QEMU+fio E2E, captures lcov, outputs HTML
- `scripts/coverage/merge_reports.sh` — merges ut.info + e2e.info → merged.info + HTML
- `scripts/coverage/ratchet_check.sh` — compares lcov summary against baseline JSON
- `scripts/coverage/post_summary.sh` — posts/updates PR comment with coverage metrics
- `scripts/coverage/tests/test_exclusion.sh` — grep assertions on filtered .info
- `scripts/coverage/tests/test_merge.sh` — synthetic union-merge test
- `scripts/coverage/tests/test_ratchet.sh` — ratchet pass/fail matrix
- `scripts/coverage/tests/test_reset.sh` — reset_counters.sh behavior
- `scripts/coverage/tests/test_build.sh` — coverage-build smoke test
- `.github/workflows/coverage.yml` — CI workflow
- `docs/coverage.md` — user-facing usage + troubleshooting

**Files to modify:**
- `Makefile` — add `COVERAGE=1` switch and 6 new targets
- `.gitignore` — add `build-cov/`, `.coverage-baseline.json.bak`

**Files that will be created at runtime (not shipped):**
- `.coverage-baseline.json` — written by first ratchet run; committed by user after review
- `build-cov/**` — instrumented build output

---

## Task 1: Makefile COVERAGE=1 switch + coverage-build target

**Files:**
- Modify: `Makefile:1-30, Makefile:126`
- Modify: `.gitignore`
- Test: `scripts/coverage/tests/test_build.sh`

- [ ] **Step 1: Write the failing test**

Create `scripts/coverage/tests/test_build.sh`:

```bash
#!/bin/bash
# Smoke test: coverage-build produces .gcno files under build-cov/
set -euo pipefail

cd "$(dirname "$0")/../../.."

# Clean any prior state
rm -rf build-cov
find . -name '*.gcda' -delete 2>/dev/null || true

# Build instrumented
make coverage-build > /tmp/covbuild.log 2>&1 || {
    echo "FAIL: make coverage-build exited non-zero"
    tail -40 /tmp/covbuild.log
    exit 1
}

# Assert build-cov/ exists
[ -d build-cov ] || { echo "FAIL: build-cov/ not created"; exit 1; }

# Assert at least one .gcno file was emitted (indicates --coverage was applied)
gcno_count=$(find build-cov -name '*.gcno' | wc -l | tr -d ' ')
if [ "$gcno_count" -lt 10 ]; then
    echo "FAIL: expected >= 10 .gcno files, got $gcno_count"
    exit 1
fi

# Assert existing build/ directory was NOT touched (parallel variant)
# (only check this if build/ existed before — skip if fresh clone)

# Assert a specific firmware source produced a .gcno
[ -f build-cov/ftl/block.gcno ] || { echo "FAIL: build-cov/ftl/block.gcno missing"; exit 1; }
[ -f build-cov/sssim.gcno ] || { echo "FAIL: build-cov/sssim.gcno missing"; exit 1; }

# Assert at least one test binary was built
[ -x build-cov/bin/test_ftl ] || { echo "FAIL: build-cov/bin/test_ftl not built"; exit 1; }

echo "PASS: coverage-build produced $gcno_count .gcno files"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `chmod +x scripts/coverage/tests/test_build.sh && bash scripts/coverage/tests/test_build.sh`
Expected: FAIL — `make: *** No rule to make target 'coverage-build'`

- [ ] **Step 3: Add COVERAGE switch to Makefile**

Modify `Makefile` — insert this block after line 13 (after the `LDFLAGS` conditional, before `# Directories` at line 15):

```make

# Coverage build variant (GCC --coverage instrumentation)
COVERAGE ?= 0
ifeq ($(COVERAGE),1)
    CFLAGS := $(filter-out -O2, $(CFLAGS)) --coverage -O0
    LDFLAGS += --coverage
    BUILD_DIR = build-cov
else
    BUILD_DIR = build
endif
```

Then **delete** the existing line 25 `BUILD_DIR = build` since it's now set conditionally above. The lines `LIB_DIR = $(BUILD_DIR)/lib` and `BIN_DIR = $(BUILD_DIR)/bin` remain as-is.

Then modify the `.PHONY` line at approximately line 126. The existing line is:
```make
.PHONY: all clean directories test systest stress-long help
```
Change it to:
```make
.PHONY: all clean directories test systest stress-long help coverage-build coverage-clean coverage-ut coverage-e2e coverage-merge coverage coverage-selftest
```

Then add these targets at the end of the Makefile, after the `help:` block:

```make

# Coverage targets
coverage-build:
	@$(MAKE) COVERAGE=1 all

coverage-clean:
	@echo "  CLEAN   build-cov/"
	@rm -rf build-cov
	@find . -name '*.gcda' -delete 2>/dev/null || true
	@echo "Coverage clean complete!"
```

- [ ] **Step 4: Update .gitignore**

Add these lines to the end of `.gitignore`:

```gitignore

# Coverage build output
build-cov/
*.gcno
*.gcda
.coverage-baseline.json.bak
```

- [ ] **Step 5: Run test to verify it passes**

Run: `bash scripts/coverage/tests/test_build.sh`
Expected: `PASS: coverage-build produced <N> .gcno files` (N should be >= 50)

- [ ] **Step 6: Verify existing build still works**

Run: `make clean && make all 2>&1 | tail -5`
Expected: Normal build succeeds, binaries in `build/bin/`, no `--coverage` flag visible.

Run: `file build/bin/test_ftl`
Expected: Shows it's an executable, no coverage symbols (check with `nm build/bin/test_ftl | grep -c gcov` which should output 0).

- [ ] **Step 7: Commit**

```bash
git add Makefile .gitignore scripts/coverage/tests/test_build.sh
git commit -m "feat: add COVERAGE=1 Makefile switch and coverage-build target

Adds parallel build-cov/ variant with --coverage -O0 instrumentation.
Existing 'make all' and 'make test' flows remain -O2 optimized and
untouched. build-cov/ and .gcda/.gcno files are gitignored."
```

---

## Task 2: reset_counters.sh

**Files:**
- Create: `scripts/coverage/reset_counters.sh`
- Test: `scripts/coverage/tests/test_reset.sh`

- [ ] **Step 1: Write the failing test**

Create `scripts/coverage/tests/test_reset.sh`:

```bash
#!/bin/bash
# Verify reset_counters.sh deletes .gcda but keeps .gcno
set -euo pipefail

cd "$(dirname "$0")/../../.."

# Setup: ensure coverage build exists
[ -d build-cov ] || make coverage-build > /dev/null 2>&1

# Plant some fake .gcda files
touch build-cov/ftl/block.gcda
touch build-cov/sssim.gcda
touch build-cov/common/log.gcda

# Also record .gcno count before
gcno_before=$(find build-cov -name '*.gcno' | wc -l | tr -d ' ')
gcda_before=$(find build-cov -name '*.gcda' | wc -l | tr -d ' ')

[ "$gcda_before" -ge 3 ] || { echo "FAIL: setup — expected >= 3 .gcda, got $gcda_before"; exit 1; }

# Run reset
bash scripts/coverage/reset_counters.sh

# Assert .gcda are gone
gcda_after=$(find build-cov -name '*.gcda' | wc -l | tr -d ' ')
[ "$gcda_after" -eq 0 ] || { echo "FAIL: $gcda_after .gcda remaining after reset"; exit 1; }

# Assert .gcno count unchanged
gcno_after=$(find build-cov -name '*.gcno' | wc -l | tr -d ' ')
[ "$gcno_before" -eq "$gcno_after" ] || { echo "FAIL: .gcno count changed: $gcno_before → $gcno_after"; exit 1; }

echo "PASS: reset_counters removed $gcda_before .gcda files, preserved $gcno_after .gcno files"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `chmod +x scripts/coverage/tests/test_reset.sh && bash scripts/coverage/tests/test_reset.sh`
Expected: FAIL — `bash: scripts/coverage/reset_counters.sh: No such file or directory`

- [ ] **Step 3: Implement reset_counters.sh**

Create `scripts/coverage/reset_counters.sh`:

```bash
#!/bin/bash
# Delete all .gcda runtime counter files while preserving .gcno structure files.
# Used between test phases (UT vs E2E) so coverage data doesn't accumulate
# across phases when we want them captured separately.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-cov}"

if [ ! -d "$BUILD_DIR" ]; then
    echo "reset_counters: $BUILD_DIR does not exist, nothing to reset"
    exit 0
fi

count=$(find "$BUILD_DIR" -name '*.gcda' | wc -l | tr -d ' ')
find "$BUILD_DIR" -name '*.gcda' -delete
echo "reset_counters: removed $count .gcda files from $BUILD_DIR"
```

Then make it executable:

```bash
chmod +x scripts/coverage/reset_counters.sh
```

- [ ] **Step 4: Run test to verify it passes**

Run: `bash scripts/coverage/tests/test_reset.sh`
Expected: `PASS: reset_counters removed 3 .gcda files, preserved <N> .gcno files`

- [ ] **Step 5: Commit**

```bash
git add scripts/coverage/reset_counters.sh scripts/coverage/tests/test_reset.sh
git commit -m "feat: add reset_counters.sh to clear .gcda between test phases"
```

---

## Task 3: UT coverage capture script

**Files:**
- Create: `scripts/coverage/run_ut_coverage.sh`

- [ ] **Step 1: Write a smoke test inline (no separate test file yet)**

Plan verification approach: after running `run_ut_coverage.sh`, we assert that `build-cov/coverage/ut.info` and `build-cov/coverage/ut/index.html` exist. We use a simple inline assertion block at the end of `run_ut_coverage.sh` itself (self-check-on-success pattern). Separate exclusion assertions come in Task 4.

- [ ] **Step 2: Implement run_ut_coverage.sh**

Create `scripts/coverage/run_ut_coverage.sh`:

```bash
#!/bin/bash
# Run the UT suite against the instrumented build and capture lcov coverage.
#
# Inputs:  build-cov/bin/{test_*,systest_*,stress_*} (built via make coverage-build)
# Outputs: build-cov/coverage/ut.info     (cleaned lcov tracefile)
#          build-cov/coverage/ut/         (HTML report)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT/build-cov"
BIN_DIR="$BUILD_DIR/bin"
COV_DIR="$BUILD_DIR/coverage"
INFO="$COV_DIR/ut.info"
INFO_RAW="$COV_DIR/ut.raw.info"

mkdir -p "$COV_DIR"

# Check lcov is installed
command -v lcov >/dev/null || { echo "ERROR: lcov not installed. On Ubuntu: sudo apt-get install lcov"; exit 1; }
command -v genhtml >/dev/null || { echo "ERROR: genhtml not installed (bundled with lcov)"; exit 1; }

# Reset counters
bash "$ROOT/scripts/coverage/reset_counters.sh"

# Run every test binary, ignoring individual failures (coverage keeps going)
echo "========================================"
echo "Running UT suite under coverage..."
echo "========================================"

pass=0; fail=0
export STRESS_DURATION=5

for bin in "$BIN_DIR"/test_* "$BIN_DIR"/systest_* "$BIN_DIR"/stress_*; do
    [ -x "$bin" ] || continue
    name=$(basename "$bin")
    echo ">>> $name"
    if "$bin" > /tmp/cov_ut_out.log 2>&1; then
        pass=$((pass + 1))
    else
        echo "    WARN: $name exited non-zero (coverage data still captured)"
        fail=$((fail + 1))
    fi
done

echo "UT pass: $pass, fail: $fail"

# Capture raw lcov data
echo "========================================"
echo "Capturing lcov data..."
echo "========================================"
lcov --capture --directory "$BUILD_DIR" --output-file "$INFO_RAW" \
     --rc lcov_branch_coverage=1 \
     --quiet

# Remove non-firmware paths
lcov --remove "$INFO_RAW" \
     "*/src/vhost/*" \
     "*/src/kernel/*" \
     "*/src/tools/*" \
     "*/tests/*" \
     "/usr/include/*" \
     "/usr/lib/*" \
     "*/guest/*" \
     --output-file "$INFO" \
     --rc lcov_branch_coverage=1 \
     --quiet

# Self-check: verify exclusions worked (Task 4 expands this into full test)
if grep -q 'SF:.*src/vhost/' "$INFO" 2>/dev/null; then
    echo "ERROR: src/vhost/ not excluded from $INFO"
    exit 1
fi

# Generate HTML
echo "========================================"
echo "Generating HTML report..."
echo "========================================"
genhtml "$INFO" --output-directory "$COV_DIR/ut" \
        --title "HFSSS UT Coverage" \
        --branch-coverage \
        --quiet

# Print summary
echo ""
echo "========================================"
echo "UT Coverage Summary"
echo "========================================"
lcov --summary "$INFO" --rc lcov_branch_coverage=1 2>&1 | grep -E 'lines|functions|branches'
echo ""
echo "HTML report: $COV_DIR/ut/index.html"
```

Then make it executable:

```bash
chmod +x scripts/coverage/run_ut_coverage.sh
```

- [ ] **Step 3: Install lcov if missing**

On macOS:
```bash
brew install lcov
```

On Ubuntu/Debian:
```bash
sudo apt-get install -y lcov
```

- [ ] **Step 4: Run the script**

Run: `bash scripts/coverage/run_ut_coverage.sh`
Expected: script completes, prints `HTML report: .../ut/index.html`, and the summary line shows non-zero line/function/branch percentages. Expected to take 1-3 minutes.

- [ ] **Step 5: Verify outputs exist**

Run:
```bash
ls -la build-cov/coverage/ut.info build-cov/coverage/ut/index.html
```
Expected: both files exist, `ut.info` is > 10 KB, `ut/index.html` is a valid HTML file.

- [ ] **Step 6: Open the report in a browser for visual check**

Run: `open build-cov/coverage/ut/index.html` (macOS) or `xdg-open` (Linux)
Expected: Shows HTML report with file tree, coverage percentages, browsable source files.

- [ ] **Step 7: Commit**

```bash
git add scripts/coverage/run_ut_coverage.sh
git commit -m "feat: add UT coverage capture script (lcov + genhtml)"
```

---

## Task 4: Exclusion validation test

**Files:**
- Create: `scripts/coverage/tests/test_exclusion.sh`

- [ ] **Step 1: Write the test**

Create `scripts/coverage/tests/test_exclusion.sh`:

```bash
#!/bin/bash
# Verify that non-firmware paths are excluded from the UT .info file,
# and that firmware paths ARE present.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
INFO="$ROOT/build-cov/coverage/ut.info"

[ -f "$INFO" ] || { echo "FAIL: $INFO does not exist. Run run_ut_coverage.sh first."; exit 1; }

# Assert EXCLUSIONS (these paths must NOT appear in the info file)
for excluded in "src/vhost/" "src/kernel/" "src/tools/" "/tests/" "/usr/include/" "/usr/lib/"; do
    if grep -q "SF:.*${excluded}" "$INFO"; then
        echo "FAIL: excluded path still present in $INFO: $excluded"
        grep "SF:.*${excluded}" "$INFO" | head -3
        exit 1
    fi
done

# Assert INCLUSIONS (these firmware paths MUST appear)
for included in "src/ftl/" "src/common/" "src/controller/" "src/hal/" "src/media/" "src/pcie/" "src/perf/" "sssim.c"; do
    if ! grep -q "SF:.*${included}" "$INFO"; then
        echo "FAIL: expected firmware path missing from $INFO: $included"
        exit 1
    fi
done

# Count source files included
sf_count=$(grep -c '^SF:' "$INFO")
echo "PASS: exclusion test — $sf_count firmware source files in coverage scope"
```

- [ ] **Step 2: Run test to verify it passes**

Run: `chmod +x scripts/coverage/tests/test_exclusion.sh && bash scripts/coverage/tests/test_exclusion.sh`
Expected: `PASS: exclusion test — <N> firmware source files in coverage scope` (N ~= 60-80).

- [ ] **Step 3: Negative test — simulate a regression**

Manually corrupt the info file to include a vhost line, run the test, confirm it fails:

```bash
cp build-cov/coverage/ut.info /tmp/ut.info.bak
echo "SF:/fake/path/src/vhost/nbd_async.c" >> build-cov/coverage/ut.info
bash scripts/coverage/tests/test_exclusion.sh && echo "BUG: test should have failed"
cp /tmp/ut.info.bak build-cov/coverage/ut.info
```
Expected: The test prints `FAIL: excluded path still present in ...: src/vhost/` and exits non-zero. Then restore works.

- [ ] **Step 4: Commit**

```bash
git add scripts/coverage/tests/test_exclusion.sh
git commit -m "test: verify vhost/kernel/tools/tests excluded from UT coverage"
```

---

## Task 5: E2E coverage capture script

**Files:**
- Create: `scripts/coverage/run_e2e_coverage.sh`

- [ ] **Step 1: Review existing E2E entry points**

Open and read these files to understand the E2E startup flow (do not modify):
- `scripts/start_nvme_test.sh` — how NBD server + QEMU are launched
- `scripts/fio_verify_suite.sh` — how fio tests are run over SSH

Expected: You'll see that `start_nvme_test.sh` launches `hfsss-nbd-server` in background and starts QEMU pointing at it. SSH key is at `/tmp/hfsss_qemu_key`, SSH port 2222.

- [ ] **Step 2: Implement run_e2e_coverage.sh**

Create `scripts/coverage/run_e2e_coverage.sh`:

```bash
#!/bin/bash
# Run end-to-end fio tests through QEMU guest against the instrumented
# HFSSS NBD server, capturing coverage data from the firmware.
#
# Requires: QEMU installed, port 10809/2222 free, lcov.
# Outputs: build-cov/coverage/e2e.info and build-cov/coverage/e2e/
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT/build-cov"
BIN_DIR="$BUILD_DIR/bin"
COV_DIR="$BUILD_DIR/coverage"
INFO="$COV_DIR/e2e.info"
INFO_RAW="$COV_DIR/e2e.raw.info"
NBD_PID_FILE="/tmp/hfsss_nbd_cov.pid"
QEMU_PID_FILE="/tmp/hfsss_qemu_cov.pid"

mkdir -p "$COV_DIR"

# Preconditions
command -v lcov >/dev/null || { echo "ERROR: lcov not installed"; exit 1; }
[ -x "$BIN_DIR/hfsss-nbd-server" ] || { echo "ERROR: $BIN_DIR/hfsss-nbd-server not built. Run make coverage-build."; exit 1; }
command -v qemu-system-x86_64 >/dev/null || { echo "ERROR: qemu-system-x86_64 not installed"; exit 1; }

cleanup() {
    echo "Cleaning up E2E processes..."
    # Graceful NBD shutdown (needs SIGTERM so .gcda flushes)
    if [ -f "$NBD_PID_FILE" ]; then
        pid=$(cat "$NBD_PID_FILE")
        if kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
            # Wait up to 10s for clean exit
            for _ in $(seq 1 20); do
                kill -0 "$pid" 2>/dev/null || break
                sleep 0.5
            done
            kill -0 "$pid" 2>/dev/null && { echo "WARN: NBD server didn't exit, sending SIGKILL (will lose coverage)"; kill -KILL "$pid" || true; }
        fi
        rm -f "$NBD_PID_FILE"
    fi
    # Stop QEMU
    if [ -f "$QEMU_PID_FILE" ]; then
        qpid=$(cat "$QEMU_PID_FILE")
        kill "$qpid" 2>/dev/null || true
        rm -f "$QEMU_PID_FILE"
    fi
}
trap cleanup EXIT INT TERM

# Reset coverage counters
bash "$ROOT/scripts/coverage/reset_counters.sh"

# Start instrumented NBD server in background
echo "Starting instrumented hfsss-nbd-server..."
cd "$ROOT"
"$BIN_DIR/hfsss-nbd-server" -a > /tmp/cov_nbd.log 2>&1 &
echo $! > "$NBD_PID_FILE"
sleep 2

# Sanity: NBD server listening?
if ! kill -0 "$(cat $NBD_PID_FILE)" 2>/dev/null; then
    echo "ERROR: NBD server died on startup"
    cat /tmp/cov_nbd.log
    exit 1
fi

# Start QEMU (reuse existing launcher but direct its QEMU PID into our file)
echo "Starting QEMU guest..."
bash "$ROOT/scripts/start_nvme_test.sh" > /tmp/cov_qemu.log 2>&1 &
echo $! > "$QEMU_PID_FILE"

# Wait for SSH to be ready (up to 90s for guest boot)
echo "Waiting for SSH in guest..."
for i in $(seq 1 180); do
    if ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
           -i /tmp/hfsss_qemu_key -p 2222 -o ConnectTimeout=2 \
           root@127.0.0.1 'true' 2>/dev/null; then
        echo "SSH ready after ${i}s"
        break
    fi
    sleep 1
    if [ "$i" -eq 180 ]; then
        echo "ERROR: SSH not ready after 180s"
        exit 1
    fi
done

# Run fio verification suite
echo "========================================"
echo "Running fio verification suite..."
echo "========================================"
bash "$ROOT/scripts/fio_verify_suite.sh" 2222 /dev/nvme0n1 || {
    echo "WARN: fio suite reported failures (coverage data still captured)"
}

# Graceful shutdown (trap will handle NBD SIGTERM and flush .gcda)
echo "Shutting down..."
cleanup
trap - EXIT INT TERM

# Give FS time to sync .gcda to disk
sleep 1

# Capture lcov
echo "========================================"
echo "Capturing lcov data..."
echo "========================================"
lcov --capture --directory "$BUILD_DIR" --output-file "$INFO_RAW" \
     --rc lcov_branch_coverage=1 --quiet

# Apply identical exclusions as UT
lcov --remove "$INFO_RAW" \
     "*/src/vhost/*" \
     "*/src/kernel/*" \
     "*/src/tools/*" \
     "*/tests/*" \
     "/usr/include/*" \
     "/usr/lib/*" \
     "*/guest/*" \
     --output-file "$INFO" \
     --rc lcov_branch_coverage=1 --quiet

# Self-check exclusions
if grep -q 'SF:.*src/vhost/' "$INFO" 2>/dev/null; then
    echo "ERROR: src/vhost/ not excluded from $INFO"; exit 1
fi

# Generate HTML
genhtml "$INFO" --output-directory "$COV_DIR/e2e" \
        --title "HFSSS E2E Coverage (QEMU+fio)" \
        --branch-coverage --quiet

echo ""
echo "========================================"
echo "E2E Coverage Summary"
echo "========================================"
lcov --summary "$INFO" --rc lcov_branch_coverage=1 2>&1 | grep -E 'lines|functions|branches'
echo ""
echo "HTML report: $COV_DIR/e2e/index.html"
```

Then make it executable:

```bash
chmod +x scripts/coverage/run_e2e_coverage.sh
```

- [ ] **Step 3: Verify prerequisites**

Check required tools and existing test infra:
```bash
command -v qemu-system-x86_64
ls /tmp/hfsss_qemu_key
ls scripts/start_nvme_test.sh scripts/fio_verify_suite.sh
```
Expected: all present. If `/tmp/hfsss_qemu_key` missing, the QEMU image first-run setup is needed (see user's qemu_nvme_setup memory).

- [ ] **Step 4: Run the E2E capture**

Run: `bash scripts/coverage/run_e2e_coverage.sh`
Expected: Takes 5-15 minutes. Prints progression through SSH ready → fio tests → shutdown → lcov capture → HTML. Final "HTML report: .../e2e/index.html" line.

- [ ] **Step 5: Verify outputs**

Run:
```bash
ls -la build-cov/coverage/e2e.info build-cov/coverage/e2e/index.html
bash scripts/coverage/tests/test_exclusion.sh || echo "Reuse UT test manually for e2e.info below"
INFO=build-cov/coverage/e2e.info bash -c '
  grep -c SF: $INFO
  ! grep -q "SF:.*src/vhost/" $INFO && echo "no vhost leak"
  grep -q "SF:.*src/ftl/" $INFO && echo "ftl present"
'
```
Expected: e2e.info exists with SF entries, vhost not leaked, ftl present.

- [ ] **Step 6: Commit**

```bash
git add scripts/coverage/run_e2e_coverage.sh
git commit -m "feat: add E2E coverage capture via QEMU+fio

Launches instrumented hfsss-nbd-server, starts QEMU, runs fio
verification suite over SSH, gracefully shuts down NBD to flush
.gcda files, then captures lcov with same exclusion filters as UT."
```

---

## Task 6: Merge reports script

**Files:**
- Create: `scripts/coverage/merge_reports.sh`
- Create: `scripts/coverage/tests/test_merge.sh`

- [ ] **Step 1: Write the failing test**

Create `scripts/coverage/tests/test_merge.sh`:

```bash
#!/bin/bash
# Verify that merge_reports.sh produces union coverage from two .info files.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# Craft two synthetic .info files that exercise union semantics:
#   file A covers lines 1,2,3 of fake.c (line 3 hit twice)
#   file B covers lines 3,4,5 of fake.c (line 3 hit once)
# Union should show lines 1-5 covered with line 3 hit 3 times.

cat > "$TMP/a.info" <<'EOF'
TN:
SF:/fake/src/fake.c
DA:1,1
DA:2,1
DA:3,2
DA:4,0
DA:5,0
LH:3
LF:5
end_of_record
EOF

cat > "$TMP/b.info" <<'EOF'
TN:
SF:/fake/src/fake.c
DA:1,0
DA:2,0
DA:3,1
DA:4,1
DA:5,1
LH:3
LF:5
end_of_record
EOF

# Bypass real merge_reports.sh (which reads fixed paths) by invoking lcov directly
# using the same command the script uses
lcov -a "$TMP/a.info" -a "$TMP/b.info" -o "$TMP/merged.info" --quiet

# Assert merged covers all 5 lines
line_hit=$(grep '^LH:' "$TMP/merged.info" | head -1 | cut -d: -f2)
[ "$line_hit" = "5" ] || { echo "FAIL: expected LH:5 in merge, got LH:$line_hit"; cat "$TMP/merged.info"; exit 1; }

# Assert line 3 count is 3 (summed)
da3=$(grep '^DA:3,' "$TMP/merged.info" | head -1 | cut -d, -f2)
[ "$da3" = "3" ] || { echo "FAIL: expected DA:3,3, got DA:3,$da3"; exit 1; }

# Assert full merge_reports.sh runs if real inputs exist
if [ -f "$ROOT/build-cov/coverage/ut.info" ] && [ -f "$ROOT/build-cov/coverage/e2e.info" ]; then
    bash "$ROOT/scripts/coverage/merge_reports.sh"
    [ -f "$ROOT/build-cov/coverage/merged.info" ] || { echo "FAIL: merged.info not created"; exit 1; }
    [ -f "$ROOT/build-cov/coverage/merged/index.html" ] || { echo "FAIL: merged/index.html not created"; exit 1; }
    echo "PASS: union-merge semantics correct AND real merge_reports.sh produces merged/ report"
else
    echo "PASS: union-merge semantics correct (real ut/e2e not available, full script skipped)"
fi
```

- [ ] **Step 2: Run test to verify it fails**

Run: `chmod +x scripts/coverage/tests/test_merge.sh && bash scripts/coverage/tests/test_merge.sh`
Expected: First half passes (direct lcov call), second half fails with `bash: .../merge_reports.sh: No such file or directory` IF both ut.info and e2e.info exist. If one is missing, first half passes and second half skips.

- [ ] **Step 3: Implement merge_reports.sh**

Create `scripts/coverage/merge_reports.sh`:

```bash
#!/bin/bash
# Merge UT and E2E coverage .info files into a union coverage report.
#
# Inputs:  build-cov/coverage/{ut,e2e}.info
# Outputs: build-cov/coverage/merged.info, build-cov/coverage/merged/
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
COV_DIR="$ROOT/build-cov/coverage"
UT_INFO="$COV_DIR/ut.info"
E2E_INFO="$COV_DIR/e2e.info"
MERGED_INFO="$COV_DIR/merged.info"

command -v lcov >/dev/null || { echo "ERROR: lcov not installed"; exit 1; }
[ -f "$UT_INFO" ] || { echo "ERROR: $UT_INFO not found. Run scripts/coverage/run_ut_coverage.sh first."; exit 1; }
[ -f "$E2E_INFO" ] || { echo "ERROR: $E2E_INFO not found. Run scripts/coverage/run_e2e_coverage.sh first."; exit 1; }

lcov --add-tracefile "$UT_INFO" \
     --add-tracefile "$E2E_INFO" \
     --output-file "$MERGED_INFO" \
     --rc lcov_branch_coverage=1 \
     --quiet

genhtml "$MERGED_INFO" --output-directory "$COV_DIR/merged" \
        --title "HFSSS Merged Coverage (UT + E2E)" \
        --branch-coverage --quiet

echo ""
echo "========================================"
echo "Merged Coverage Summary"
echo "========================================"
lcov --summary "$MERGED_INFO" --rc lcov_branch_coverage=1 2>&1 | grep -E 'lines|functions|branches'
echo ""
echo "HTML report: $COV_DIR/merged/index.html"
```

Then make it executable: `chmod +x scripts/coverage/merge_reports.sh`

- [ ] **Step 4: Run test to verify it passes**

Run: `bash scripts/coverage/tests/test_merge.sh`
Expected: If both real ut.info and e2e.info exist, `PASS: union-merge semantics correct AND real merge_reports.sh produces merged/ report`. Otherwise the first-half PASS message.

- [ ] **Step 5: Commit**

```bash
git add scripts/coverage/merge_reports.sh scripts/coverage/tests/test_merge.sh
git commit -m "feat: add merge_reports.sh for UT+E2E union coverage"
```

---

## Task 7: Ratchet check — parse lcov summary

**Files:**
- Create: `scripts/coverage/ratchet_check.sh` (initial — parse only, no baseline yet)
- Create: `scripts/coverage/tests/test_ratchet.sh`

- [ ] **Step 1: Understand lcov --summary output format**

Run this to see the format:
```bash
lcov --summary build-cov/coverage/ut.info --rc lcov_branch_coverage=1 2>&1
```

Expected output contains lines like:
```
  lines......: 62.5% (1234 of 1975 lines)
  functions..: 78.1% (245 of 314 functions)
  branches...: 48.3% (987 of 2043 branches)
```

Note the three percentages we need to extract.

- [ ] **Step 2: Write the failing test**

Create `scripts/coverage/tests/test_ratchet.sh`:

```bash
#!/bin/bash
# Test ratchet_check.sh: parsing, bootstrap, pass/fail logic.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SCRIPT="$ROOT/scripts/coverage/ratchet_check.sh"
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# Craft a fake .info summary by creating a minimal .info file
cat > "$TMP/fake.info" <<'EOF'
TN:
SF:/fake/a.c
DA:1,1
DA:2,1
DA:3,0
DA:4,0
LH:2
LF:4
FNF:2
FNH:1
BRF:4
BRH:2
end_of_record
EOF
# This fake.info: lines 50%, functions 50%, branches 50%

# === Test 1: Bootstrap (no baseline exists) ===
BASELINE="$TMP/baseline.json"
rm -f "$BASELINE"
output=$(INFO="$TMP/fake.info" BASELINE="$BASELINE" bash "$SCRIPT" 2>&1)
echo "$output" | grep -q "bootstrap" || { echo "FAIL: bootstrap message missing"; echo "$output"; exit 1; }
[ -f "$BASELINE" ] || { echo "FAIL: baseline not created on bootstrap"; exit 1; }

# === Test 2: Pass — current meets floor ===
# fake.info is 50/50/50; baseline is now 50/50/50; floor = 48/48/48; should pass
output=$(INFO="$TMP/fake.info" BASELINE="$BASELINE" bash "$SCRIPT" 2>&1) || { echo "FAIL: should pass but exited non-zero"; echo "$output"; exit 1; }
echo "$output" | grep -qi 'pass\|OK' || { echo "FAIL: no pass message"; echo "$output"; exit 1; }

# === Test 3: Fail — coverage dropped > 2% ===
# Craft an info with 40% line coverage
cat > "$TMP/low.info" <<'EOF'
TN:
SF:/fake/a.c
DA:1,1
DA:2,0
DA:3,0
DA:4,0
DA:5,0
LH:1
LF:5
FNF:2
FNH:1
BRF:4
BRH:2
end_of_record
EOF
# low.info: lines 20%, functions 50%, branches 50% — line dropped 30%, should FAIL

if INFO="$TMP/low.info" BASELINE="$BASELINE" bash "$SCRIPT" > "$TMP/out3.log" 2>&1; then
    echo "FAIL: ratchet should reject 30% drop"; cat "$TMP/out3.log"; exit 1
fi
grep -qi 'fail\|below' "$TMP/out3.log" || { echo "FAIL: no fail message"; cat "$TMP/out3.log"; exit 1; }

# === Test 4: --update-baseline explicitly rewrites baseline ===
INFO="$TMP/low.info" BASELINE="$BASELINE" bash "$SCRIPT" --update-baseline > "$TMP/out4.log" 2>&1
grep -qi 'updated' "$TMP/out4.log" || { echo "FAIL: no updated message"; cat "$TMP/out4.log"; exit 1; }
# Now low.info should pass (new baseline is 20/50/50)
INFO="$TMP/low.info" BASELINE="$BASELINE" bash "$SCRIPT" > "$TMP/out5.log" 2>&1

echo "PASS: ratchet bootstrap/pass/fail/update-baseline all working"
```

- [ ] **Step 3: Run test to verify it fails**

Run: `chmod +x scripts/coverage/tests/test_ratchet.sh && bash scripts/coverage/tests/test_ratchet.sh`
Expected: FAIL — `bash: .../ratchet_check.sh: No such file or directory`

- [ ] **Step 4: Implement ratchet_check.sh**

Create `scripts/coverage/ratchet_check.sh`:

```bash
#!/bin/bash
# Compare current coverage against baseline; fail if any metric drops > FLOOR_DELTA%.
#
# Env vars:
#   INFO         - path to lcov .info file (default: build-cov/coverage/ut.info)
#   BASELINE     - path to baseline JSON (default: .coverage-baseline.json)
#   FLOOR_DELTA  - ratchet delta in percentage points (default: 2.0)
#
# Flags:
#   --update-baseline  Force-rewrite baseline from current metrics
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
INFO="${INFO:-$ROOT/build-cov/coverage/ut.info}"
BASELINE="${BASELINE:-$ROOT/.coverage-baseline.json}"
FLOOR_DELTA="${FLOOR_DELTA:-2.0}"

UPDATE=0
if [ "${1:-}" = "--update-baseline" ]; then
    UPDATE=1
fi

[ -f "$INFO" ] || { echo "ERROR: $INFO not found"; exit 1; }
command -v lcov >/dev/null || { echo "ERROR: lcov not installed"; exit 1; }

# Extract line/function/branch % from lcov --summary
summary=$(lcov --summary "$INFO" --rc lcov_branch_coverage=1 2>&1)

extract_pct() {
    local kind="$1"
    # Match lines like "  lines......: 62.5% (1234 of 1975 lines)"
    echo "$summary" | grep -E "^[[:space:]]+${kind}" | head -1 | \
        grep -oE '[0-9]+\.[0-9]+%' | head -1 | tr -d '%'
}

cur_line=$(extract_pct lines)
cur_func=$(extract_pct functions)
cur_branch=$(extract_pct branches)

# Default to 0.0 if any metric missing (e.g., no branches compiled)
cur_line="${cur_line:-0.0}"
cur_func="${cur_func:-0.0}"
cur_branch="${cur_branch:-0.0}"

echo "Current coverage: lines=${cur_line}% functions=${cur_func}% branches=${cur_branch}%"

# Bootstrap: if baseline doesn't exist OR --update-baseline requested, write current
if [ ! -f "$BASELINE" ] || [ "$UPDATE" -eq 1 ]; then
    mode=$([ "$UPDATE" -eq 1 ] && echo "updated" || echo "bootstrap")
    commit=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo "unknown")
    ts=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

    floor_line=$(awk -v a="$cur_line" -v d="$FLOOR_DELTA" 'BEGIN{printf "%.1f", a-d}')
    floor_func=$(awk -v a="$cur_func" -v d="$FLOOR_DELTA" 'BEGIN{printf "%.1f", a-d}')
    floor_branch=$(awk -v a="$cur_branch" -v d="$FLOOR_DELTA" 'BEGIN{printf "%.1f", a-d}')

    cat > "$BASELINE" <<EOF
{
  "created_at": "$ts",
  "commit": "$commit",
  "floor_delta_pct": $FLOOR_DELTA,
  "metrics": {
    "line":     { "baseline": $cur_line, "floor": $floor_line },
    "function": { "baseline": $cur_func, "floor": $floor_func },
    "branch":   { "baseline": $cur_branch, "floor": $floor_branch }
  }
}
EOF

    if [ "$UPDATE" -eq 1 ]; then
        echo "Baseline updated: $BASELINE"
        echo "  line baseline=${cur_line}% floor=${floor_line}%"
        echo "  function baseline=${cur_func}% floor=${floor_func}%"
        echo "  branch baseline=${cur_branch}% floor=${floor_branch}%"
    else
        echo "Baseline bootstrap: wrote $BASELINE"
        echo "  line baseline=${cur_line}% floor=${floor_line}%"
        echo "  function baseline=${cur_func}% floor=${floor_func}%"
        echo "  branch baseline=${cur_branch}% floor=${floor_branch}%"
        echo ""
        echo "NOTE: commit this file to git: git add $BASELINE"
    fi
    exit 0
fi

# Compare against baseline floors
floor_line=$(grep -A1 '"line"' "$BASELINE" | grep '"floor"' | grep -oE '[0-9]+\.[0-9]+' | head -1)
floor_func=$(grep -A1 '"function"' "$BASELINE" | grep '"floor"' | grep -oE '[0-9]+\.[0-9]+' | head -1)
floor_branch=$(grep -A1 '"branch"' "$BASELINE" | grep '"floor"' | grep -oE '[0-9]+\.[0-9]+' | head -1)

fail=0
compare() {
    local kind="$1" cur="$2" floor="$3"
    local below=$(awk -v c="$cur" -v f="$floor" 'BEGIN{print (c < f) ? 1 : 0}')
    if [ "$below" -eq 1 ]; then
        echo "  FAIL: $kind coverage ${cur}% is below floor ${floor}%"
        fail=1
    else
        echo "  OK  : $kind coverage ${cur}% >= floor ${floor}%"
    fi
}

echo "Ratchet check against $BASELINE:"
compare line "$cur_line" "$floor_line"
compare function "$cur_func" "$floor_func"
compare branch "$cur_branch" "$floor_branch"

if [ $fail -eq 1 ]; then
    echo ""
    echo "RATCHET FAIL: coverage regressed > ${FLOOR_DELTA}% below baseline."
    echo "  If this regression is intentional, run:"
    echo "    bash scripts/coverage/ratchet_check.sh --update-baseline"
    echo "  and commit the updated $BASELINE."
    exit 1
fi

echo ""
echo "RATCHET PASS: all metrics at or above floor."
```

Then make it executable: `chmod +x scripts/coverage/ratchet_check.sh`

- [ ] **Step 5: Run test to verify it passes**

Run: `bash scripts/coverage/tests/test_ratchet.sh`
Expected: `PASS: ratchet bootstrap/pass/fail/update-baseline all working`

- [ ] **Step 6: Run against real UT coverage (bootstrap)**

Run: `bash scripts/coverage/ratchet_check.sh`
Expected: Prints "Baseline bootstrap: wrote .../.coverage-baseline.json" and creates the file. This is the first-ever run, creating the baseline.

Check the baseline:
```bash
cat .coverage-baseline.json
```
Expected: JSON with line/function/branch baseline & floor percentages.

- [ ] **Step 7: Run again (should pass)**

Run: `bash scripts/coverage/ratchet_check.sh`
Expected: `RATCHET PASS: all metrics at or above floor.`

- [ ] **Step 8: Commit**

```bash
git add scripts/coverage/ratchet_check.sh scripts/coverage/tests/test_ratchet.sh
git commit -m "feat: add ratchet_check.sh for coverage floor enforcement

Parses lcov --summary output, compares against .coverage-baseline.json.
Bootstrap on first run (writes baseline), --update-baseline to
rewrite, fails if any line/function/branch metric drops >2% below
floor."
```

Do **not** commit `.coverage-baseline.json` yet — that happens after the CI workflow exists and we've run it once (Task 10+).

---

## Task 8: PR summary poster

**Files:**
- Create: `scripts/coverage/post_summary.sh`

- [ ] **Step 1: Implement post_summary.sh**

Create `scripts/coverage/post_summary.sh`:

```bash
#!/bin/bash
# Post or update a PR comment with coverage summary.
# Expects env: GITHUB_REPOSITORY, GITHUB_EVENT_NAME, GITHUB_EVENT_PATH, GH_TOKEN
# Only runs when GITHUB_EVENT_NAME=pull_request.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
INFO="${INFO:-$ROOT/build-cov/coverage/ut.info}"
BASELINE="${BASELINE:-$ROOT/.coverage-baseline.json}"
MARKER="<!-- coverage-bot -->"

if [ "${GITHUB_EVENT_NAME:-}" != "pull_request" ]; then
    echo "Not a pull_request event; skipping PR comment"
    exit 0
fi

command -v gh >/dev/null || { echo "ERROR: gh CLI not installed"; exit 1; }
[ -f "$INFO" ] || { echo "ERROR: $INFO not found"; exit 1; }

# Extract current metrics
summary=$(lcov --summary "$INFO" --rc lcov_branch_coverage=1 2>&1)
extract() { echo "$summary" | grep -E "^[[:space:]]+$1" | head -1 | grep -oE '[0-9]+\.[0-9]+%' | head -1; }
cur_line=$(extract lines)
cur_func=$(extract functions)
cur_branch=$(extract branches)

# Extract baseline if present
if [ -f "$BASELINE" ]; then
    bl_line=$(grep -A1 '"line"' "$BASELINE" | grep '"baseline"' | grep -oE '[0-9]+\.[0-9]+' | head -1)
    bl_func=$(grep -A1 '"function"' "$BASELINE" | grep '"baseline"' | grep -oE '[0-9]+\.[0-9]+' | head -1)
    bl_branch=$(grep -A1 '"branch"' "$BASELINE" | grep '"baseline"' | grep -oE '[0-9]+\.[0-9]+' | head -1)
    delta() { awk -v c="$1" -v b="$2" 'BEGIN{d=c-b; printf "%+.1f", d}'; }
    # shellcheck disable=SC2001
    d_line=$(delta "$(echo $cur_line | tr -d %)" "$bl_line")
    d_func=$(delta "$(echo $cur_func | tr -d %)" "$bl_func")
    d_branch=$(delta "$(echo $cur_branch | tr -d %)" "$bl_branch")
    baseline_section="Baseline: lines=${bl_line}% functions=${bl_func}% branches=${bl_branch}%"
    delta_section="Delta:    lines=${d_line} functions=${d_func} branches=${d_branch}"
else
    baseline_section="(no baseline yet — first run)"
    delta_section=""
fi

# Build comment body
body=$(cat <<EOF
$MARKER
## 📊 HFSSS Coverage Report (UT)

| Metric | Current | Baseline | Delta |
|--------|---------|----------|-------|
| Lines | ${cur_line} | ${bl_line:-n/a}% | ${d_line:-n/a} |
| Functions | ${cur_func} | ${bl_func:-n/a}% | ${d_func:-n/a} |
| Branches | ${cur_branch} | ${bl_branch:-n/a}% | ${d_branch:-n/a} |

$baseline_section
$delta_section

_UT-only coverage. E2E runs locally via \`make coverage-e2e\`._
EOF
)

# Find PR number from event payload
PR_NUM=$(jq -r .pull_request.number "$GITHUB_EVENT_PATH")

# Check for existing bot comment
existing=$(gh api "repos/$GITHUB_REPOSITORY/issues/$PR_NUM/comments" --jq ".[] | select(.body | contains(\"$MARKER\")) | .id" | head -1)

if [ -n "$existing" ]; then
    echo "Updating existing comment id=$existing"
    gh api -X PATCH "repos/$GITHUB_REPOSITORY/issues/comments/$existing" -f body="$body" > /dev/null
else
    echo "Creating new comment on PR #$PR_NUM"
    gh api -X POST "repos/$GITHUB_REPOSITORY/issues/$PR_NUM/comments" -f body="$body" > /dev/null
fi

echo "PR comment posted/updated."
```

Then make it executable: `chmod +x scripts/coverage/post_summary.sh`

- [ ] **Step 2: Syntax check**

Run: `bash -n scripts/coverage/post_summary.sh`
Expected: No output (syntax OK).

- [ ] **Step 3: Dry-run test (no actual API call)**

Run: `GITHUB_EVENT_NAME=push bash scripts/coverage/post_summary.sh`
Expected: `Not a pull_request event; skipping PR comment`

- [ ] **Step 4: Commit**

```bash
git add scripts/coverage/post_summary.sh
git commit -m "feat: add post_summary.sh to update PR coverage comment

Uses gh CLI with <!-- coverage-bot --> marker for idempotent updates.
Falls back to create-if-absent. No-op outside pull_request events."
```

---

## Task 9: Wire Makefile coverage targets

**Files:**
- Modify: `Makefile` (append targets at end)

- [ ] **Step 1: Add the remaining coverage targets to Makefile**

Append to the end of `Makefile` (after the `coverage-clean:` target added in Task 1):

```make

coverage-ut: coverage-build
	@bash scripts/coverage/run_ut_coverage.sh

coverage-e2e: coverage-build
	@bash scripts/coverage/run_e2e_coverage.sh

coverage-merge:
	@bash scripts/coverage/merge_reports.sh

# Full local flow: UT + E2E + merge
coverage: coverage-ut coverage-e2e coverage-merge

coverage-selftest:
	@echo "Running coverage self-tests..."
	@bash scripts/coverage/tests/test_build.sh
	@bash scripts/coverage/tests/test_reset.sh
	@bash scripts/coverage/tests/test_exclusion.sh
	@bash scripts/coverage/tests/test_merge.sh
	@bash scripts/coverage/tests/test_ratchet.sh
	@echo "All coverage self-tests passed!"
```

- [ ] **Step 2: Verify each new target resolves**

Run: `make -n coverage-ut coverage-e2e coverage-merge coverage coverage-selftest 2>&1 | head -20`
Expected: Shows the commands that would be executed. No "No rule to make target" errors.

- [ ] **Step 3: Run the self-test suite**

Run: `make coverage-selftest`
Expected: Each test script runs and prints `PASS: ...`. Final line: `All coverage self-tests passed!`

- [ ] **Step 4: Commit**

```bash
git add Makefile
git commit -m "feat: wire coverage-ut/e2e/merge/selftest Makefile targets"
```

---

## Task 10: GitHub Actions CI workflow

**Files:**
- Create: `.github/workflows/coverage.yml`

- [ ] **Step 1: Check if .github/workflows exists**

Run: `ls .github/workflows 2>/dev/null || echo "missing"`
If "missing", create it: `mkdir -p .github/workflows`

- [ ] **Step 2: Implement coverage.yml**

Create `.github/workflows/coverage.yml`:

```yaml
name: Coverage

on:
  pull_request:
    branches: [master]
  push:
    branches: [master]

jobs:
  ut-coverage:
    runs-on: ubuntu-latest
    permissions:
      contents: read
      pull-requests: write
    steps:
      - uses: actions/checkout@v4
        with:
          fetch-depth: 0

      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y lcov build-essential jq

      - name: Build instrumented firmware
        run: make coverage-build

      - name: Run UT coverage
        run: make coverage-ut

      - name: Ratchet check
        run: bash scripts/coverage/ratchet_check.sh

      - name: Upload HTML report artifact
        uses: actions/upload-artifact@v4
        if: always()
        with:
          name: coverage-ut-html
          path: build-cov/coverage/ut/
          retention-days: 14

      - name: Upload .info tracefile
        uses: actions/upload-artifact@v4
        if: always()
        with:
          name: coverage-ut-info
          path: build-cov/coverage/ut.info
          retention-days: 14

      - name: Post PR comment
        if: github.event_name == 'pull_request'
        env:
          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: bash scripts/coverage/post_summary.sh
```

- [ ] **Step 3: Validate YAML syntax**

Run: `python3 -c "import yaml; yaml.safe_load(open('.github/workflows/coverage.yml'))" && echo OK`
Expected: `OK`

- [ ] **Step 4: Check that lcov is invoked correctly inside the workflow locally**

Simulate the CI steps:
```bash
make coverage-clean
make coverage-build
make coverage-ut
bash scripts/coverage/ratchet_check.sh
```
Expected: All steps succeed. `.coverage-baseline.json` exists (created in Task 7 step 6); ratchet passes (or bootstraps if somehow missing).

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/coverage.yml
git commit -m "ci: add coverage.yml GitHub Actions workflow

Runs make coverage-ut + ratchet_check on every PR, uploads HTML
artifact, posts/updates PR comment with metrics."
```

---

## Task 11: User documentation

**Files:**
- Create: `docs/coverage.md`

- [ ] **Step 1: Write docs/coverage.md**

Create `docs/coverage.md`:

```markdown
# HFSSS Code Coverage

## Quick Start

```bash
# Install lcov (one-time)
brew install lcov         # macOS
sudo apt install lcov     # Linux

# UT coverage only (fast — 1-3 min)
make coverage-ut
open build-cov/coverage/ut/index.html

# Full flow: UT + E2E + merged report (slow — 10-20 min)
# Requires QEMU image set up per scripts/start_nvme_test.sh prerequisites.
make coverage

# Run the coverage infra self-tests
make coverage-selftest
```

## Scope

**Included** (measured):
- `src/{common,media,hal,ftl,controller,pcie,perf}/*.c` + `src/sssim.c`
- inline functions in `include/`

**Excluded** (filtered via `lcov --remove`):
- `src/vhost/*` — NBD / vhost-user transport layer
- `src/kernel/*` — Linux kernel module (host driver)
- `src/tools/*` — CLI utilities
- `tests/*` — the tests themselves
- system headers

## Reports

After `make coverage`, three HTML reports are produced:

| Report | Path | What it shows |
|--------|------|--------------|
| UT | `build-cov/coverage/ut/index.html` | Coverage from `make test` + systests + short stress |
| E2E | `build-cov/coverage/e2e/index.html` | Coverage from fio running in QEMU guest through NBD |
| Merged | `build-cov/coverage/merged/index.html` | Union of UT + E2E |

**Use the merged report** to see overall coverage; use **UT vs E2E comparison** to find test blind spots (code only covered by one path).

## CI Ratchet

A `.coverage-baseline.json` tracks the reference coverage.
Every PR runs `make coverage-ut` in CI and **fails** if any metric
(line / function / branch) drops more than 2 percentage points below
the baseline.

### Raising the baseline (after improving coverage)

1. Merge your PR to master.
2. Pull latest master locally.
3. Run:
   ```bash
   make coverage-ut
   bash scripts/coverage/ratchet_check.sh --update-baseline
   git add .coverage-baseline.json
   git commit -m "chore: update coverage baseline"
   git push
   ```

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `lcov: ERROR: stamp mismatch` | Source changed but old `.gcno` remains | `make coverage-clean && make coverage-build` |
| Coverage shows 0% after run | Test binary crashed before flush, OR `.gcda` not written | Check stderr of failing binary; check disk space; rerun |
| `ut/index.html` shows `src/vhost/` | Exclusion filter missed | Check lcov version ≥ 1.14; path separator `/src/vhost/` |
| E2E hangs at "Waiting for SSH" | QEMU guest not booting | Check `/tmp/cov_qemu.log` and `scripts/start_nvme_test.sh` prerequisites |
| Ratchet fails on first PR | No baseline yet | First run bootstraps; commit `.coverage-baseline.json` |
| Branch coverage is noisy | `-O0` exposes compiler-generated branches | Accept for now; documented in spec Open Questions |

## Design

See `docs/superpowers/specs/2026-04-04-firmware-code-coverage-design.md`
```

- [ ] **Step 2: Commit**

```bash
git add docs/coverage.md
git commit -m "docs: add coverage.md with quick start + troubleshooting"
```

---

## Task 12: Commit the baseline + verify end-to-end

**Files:**
- Create: `.coverage-baseline.json` (by running ratchet_check)

- [ ] **Step 1: Generate a fresh baseline from clean state**

Run:
```bash
make coverage-clean
make coverage-build
make coverage-ut
rm -f .coverage-baseline.json
bash scripts/coverage/ratchet_check.sh
```
Expected: "Baseline bootstrap: wrote .../.coverage-baseline.json" with the current coverage percentages.

- [ ] **Step 2: Inspect the baseline**

Run: `cat .coverage-baseline.json`
Expected: JSON with realistic baselines (not 0%) for line/function/branch, each floor = baseline - 2.0.

- [ ] **Step 3: Verify ratchet passes on second run**

Run: `bash scripts/coverage/ratchet_check.sh`
Expected: `RATCHET PASS: all metrics at or above floor.`

- [ ] **Step 4: Run the complete self-test suite one more time**

Run: `make coverage-selftest`
Expected: All 5 tests print `PASS: ...`, final "All coverage self-tests passed!".

- [ ] **Step 5: Verify existing flows still work**

Run:
```bash
make clean
make all
make test 2>&1 | tail -10
```
Expected: Normal `-O2` build succeeds, `make test` runs existing UT suite, no regressions.

- [ ] **Step 6: Commit the baseline**

```bash
git add .coverage-baseline.json
git commit -m "chore: add initial coverage baseline from master

Created via 'bash scripts/coverage/ratchet_check.sh' after running
make coverage-ut on master. Floor is baseline - 2.0pp per metric."
```

---

## Verification checklist (run at end)

- [ ] `make all` still works (existing -O2 build unchanged)
- [ ] `make test` still works
- [ ] `make coverage-build` produces `build-cov/**/*.gcno`
- [ ] `make coverage-ut` produces `build-cov/coverage/ut/index.html`
- [ ] `make coverage-e2e` produces `build-cov/coverage/e2e/index.html` (local QEMU env)
- [ ] `make coverage-merge` produces `build-cov/coverage/merged/index.html`
- [ ] `make coverage-selftest` passes all 5 tests
- [ ] `.coverage-baseline.json` exists and committed
- [ ] `.github/workflows/coverage.yml` passes YAML validation
- [ ] `docs/coverage.md` exists with troubleshooting
- [ ] No `.gcda` / `.gcno` / `build-cov/` files in `git status` (gitignored)
- [ ] `grep SF: build-cov/coverage/ut.info` contains `src/ftl/`, `src/common/`, etc., but **not** `src/vhost/`, `src/kernel/`, `src/tools/`, `tests/`
