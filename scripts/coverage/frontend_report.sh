#!/bin/bash
# Generate coverage report for front-end modules that are normally excluded
# from the standard UT/E2E reports (src/vhost/*, src/pcie/*, etc.).
#
# This is an ADDITIONAL report; it does not replace or modify existing reports.
#
# Usage:
#   bash scripts/coverage/frontend_report.sh [INFO_FILE]
#
# Arguments:
#   INFO_FILE  Path to an lcov .info file containing raw (unfiltered) coverage
#              data.  Defaults to build-cov/coverage/ut.raw.info.
#
# Outputs:
#   build-cov/coverage/frontend.info   (extracted tracefile)
#   build-cov/coverage/frontend/       (HTML report)
#   Module-level summary table on stdout
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
COV_DIR="$ROOT/build-cov/coverage"
INFO="${1:-$COV_DIR/ut.raw.info}"
FRONTEND_INFO="$COV_DIR/frontend.info"
HTML_DIR="$COV_DIR/frontend"

# Front-end modules to include in the report
FRONTEND_MODULES=(
    "*/src/vhost/hfsss_nbd_server.c"
    "*/src/vhost/nbd_async.c"
    "*/src/pcie/nvme.c"
    "*/src/pcie/nvme_uspace.c"
    "*/src/pcie/queue.c"
    "*/src/controller/shmem_if.c"
    "*/src/common/msgqueue.c"
)

command -v lcov >/dev/null || { echo "ERROR: lcov not installed"; exit 1; }
command -v genhtml >/dev/null || { echo "ERROR: genhtml not installed (bundled with lcov)"; exit 1; }
[ -f "$INFO" ] || { echo "ERROR: $INFO not found. Run 'make coverage-ut' first."; exit 1; }

mkdir -p "$COV_DIR"

# Build lcov --extract arguments for all front-end modules
extract_args=()
for mod in "${FRONTEND_MODULES[@]}"; do
    extract_args+=("$mod")
done

lcov --extract "$INFO" "${extract_args[@]}" \
     --output-file "$FRONTEND_INFO" \
     --rc branch_coverage=1 \
     --ignore-errors deprecated,unsupported,inconsistent,inconsistent,unused,unused \
     --quiet

# Verify extraction produced data
if [ ! -s "$FRONTEND_INFO" ]; then
    echo "WARNING: no coverage data found for front-end modules in $INFO"
    echo "This may happen if coverage was captured without exercising front-end code."
    echo "Try using e2e.raw.info instead: bash $0 build-cov/coverage/e2e.raw.info"
    exit 0
fi

# Generate HTML report
genhtml "$FRONTEND_INFO" --output-directory "$HTML_DIR" \
        --title "HFSSS Front-End Coverage" \
        --branch-coverage \
        --ignore-errors inconsistent,inconsistent,corrupt \
        --quiet

# Print module-level summary table
echo ""
echo "========================================"
echo "Front-End Module Coverage Summary"
echo "========================================"
printf "%-40s %10s %10s %10s\n" "Module" "Lines" "Functions" "Branches"
printf "%-40s %10s %10s %10s\n" "------" "-----" "---------" "--------"

for mod in "${FRONTEND_MODULES[@]}"; do
    # Extract per-module info into a temp file
    mod_info=$(mktemp)
    lcov --extract "$FRONTEND_INFO" "$mod" \
         --output-file "$mod_info" \
         --rc branch_coverage=1 \
         --ignore-errors deprecated,unsupported,inconsistent,inconsistent,unused,unused \
         --quiet 2>/dev/null || true

    # Parse the short module name from the glob pattern
    short_name=$(echo "$mod" | sed 's|^\*/||')

    if [ ! -s "$mod_info" ]; then
        printf "%-40s %10s %10s %10s\n" "$short_name" "n/a" "n/a" "n/a"
        rm -f "$mod_info"
        continue
    fi

    summary=$(lcov --summary "$mod_info" --rc branch_coverage=1 \
              --ignore-errors deprecated,inconsistent,inconsistent,format,empty 2>&1)

    extract_pct() {
        echo "$summary" | grep -E "^[[:space:]]+$1" | head -1 | \
            grep -oE '[0-9]+\.[0-9]+%' | head -1 || echo "n/a"
    }

    line_pct=$(extract_pct lines)
    func_pct=$(extract_pct functions)
    branch_pct=$(extract_pct branches)

    printf "%-40s %10s %10s %10s\n" "$short_name" "$line_pct" "$func_pct" "$branch_pct"
    rm -f "$mod_info"
done

# Print aggregate summary
echo ""
echo "Aggregate:"
lcov --summary "$FRONTEND_INFO" --rc branch_coverage=1 \
     --ignore-errors deprecated,inconsistent,inconsistent 2>&1 | grep -E 'lines|functions|branches'
echo ""
echo "HTML report: $HTML_DIR/index.html"
