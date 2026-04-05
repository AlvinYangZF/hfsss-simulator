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
