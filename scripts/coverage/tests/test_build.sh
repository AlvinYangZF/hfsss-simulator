#!/bin/bash
# Smoke test: coverage-build produces .gcno files under build-cov/
set -euo pipefail

cd "$(dirname "$0")/../../.."

# Clean any prior state
rm -rf build-cov
find . -name '*.gcda' -delete 2>/dev/null || true

# Record pre-build state of build/ (for parallel-variant isolation check)
sentinel=$(mktemp)
build_existed=0
if [ -d build ]; then
    build_existed=1
    # Touch the sentinel AFTER a tiny sleep so its mtime is strictly after
    # any real pre-existing file in build/
    sleep 1
    touch "$sentinel"
fi

# Build instrumented
make coverage-build > /tmp/covbuild.log 2>&1 || {
    echo "FAIL: make coverage-build exited non-zero"
    tail -40 /tmp/covbuild.log
    rm -f "$sentinel"
    exit 1
}

# Assert build-cov/ exists
[ -d build-cov ] || { echo "FAIL: build-cov/ not created"; exit 1; }

# Assert at least one .gcno file was emitted (indicates --coverage was applied)
gcno_count=$(find build-cov -name '*.gcno' | wc -l | tr -d ' ')
if [ "$gcno_count" -lt 10 ]; then
    echo "FAIL: expected >= 10 .gcno files, got $gcno_count"
    rm -f "$sentinel"
    exit 1
fi

# Assert existing build/ directory was NOT touched (parallel variant)
if [ "$build_existed" = "1" ]; then
    touched=$(find build -newer "$sentinel" -type f 2>/dev/null | head -5)
    if [ -n "$touched" ]; then
        echo "FAIL: coverage-build modified files in build/ (parallel variant violated):"
        echo "$touched"
        rm -f "$sentinel"
        exit 1
    fi
fi
rm -f "$sentinel"

# Assert a specific firmware source produced a .gcno
[ -f build-cov/ftl/block.gcno ] || { echo "FAIL: build-cov/ftl/block.gcno missing"; exit 1; }
[ -f build-cov/sssim.gcno ] || { echo "FAIL: build-cov/sssim.gcno missing"; exit 1; }

# Assert at least one test binary was built
[ -x build-cov/bin/test_ftl ] || { echo "FAIL: build-cov/bin/test_ftl not built"; exit 1; }

echo "PASS: coverage-build produced $gcno_count .gcno files"
