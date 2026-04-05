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
# Per-binary timeout: stress tests have hardcoded long runtimes (10-30 min).
# Cap each binary at 60s so coverage runs complete in reasonable time.
BIN_TIMEOUT=60

# Portable timeout function (macOS lacks GNU coreutils 'timeout')
run_with_timeout() {
    local timeout_s="$1"; shift
    "$@" &
    local pid=$!
    (sleep "$timeout_s" && kill "$pid" 2>/dev/null) &
    local watcher=$!
    if wait "$pid" 2>/dev/null; then
        kill "$watcher" 2>/dev/null; wait "$watcher" 2>/dev/null || true
        return 0
    else
        local rc=$?
        kill "$watcher" 2>/dev/null; wait "$watcher" 2>/dev/null || true
        return $rc
    fi
}

for bin in "$BIN_DIR"/test_* "$BIN_DIR"/systest_* "$BIN_DIR"/stress_*; do
    [ -f "$bin" ] && [ -x "$bin" ] || continue
    name=$(basename "$bin")
    echo ">>> $name"
    if run_with_timeout "$BIN_TIMEOUT" "$bin" > /tmp/cov_ut_out.log 2>&1; then
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
     --rc branch_coverage=1 \
     --ignore-errors deprecated,unsupported,inconsistent,inconsistent \
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
     --rc branch_coverage=1 \
     --ignore-errors deprecated,unsupported,inconsistent,inconsistent,unused,unused \
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
        --ignore-errors inconsistent,inconsistent,corrupt \
        --quiet

# Print summary
echo ""
echo "========================================"
echo "UT Coverage Summary"
echo "========================================"
lcov --summary "$INFO" --rc branch_coverage=1 --ignore-errors deprecated,inconsistent,inconsistent 2>&1 | grep -E 'lines|functions|branches'
echo ""
echo "HTML report: $COV_DIR/ut/index.html"
