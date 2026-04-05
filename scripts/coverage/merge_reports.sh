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
     --rc branch_coverage=1 \
     --ignore-errors deprecated,unsupported,inconsistent,inconsistent,unused,unused \
     --quiet

genhtml "$MERGED_INFO" --output-directory "$COV_DIR/merged" \
        --title "HFSSS Merged Coverage (UT + E2E)" \
        --branch-coverage \
        --ignore-errors inconsistent,inconsistent,corrupt \
        --quiet

echo ""
echo "========================================"
echo "Merged Coverage Summary"
echo "========================================"
lcov --summary "$MERGED_INFO" --rc branch_coverage=1 --ignore-errors deprecated,inconsistent,inconsistent 2>&1 | grep -E 'lines|functions|branches'
echo ""
echo "HTML report: $COV_DIR/merged/index.html"
