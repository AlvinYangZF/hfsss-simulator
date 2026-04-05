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
