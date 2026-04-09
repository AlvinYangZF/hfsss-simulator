#!/bin/bash
# Generate coverage report for front-end modules (src/vhost/*, src/pcie/*, etc.)
# with per-module delta comparison against a stored baseline.
#
# Usage:  bash scripts/coverage/frontend_report.sh [--update-baseline] [INFO_FILE]
#
# Outputs:
#   build-cov/coverage/frontend.info   (extracted tracefile)
#   build-cov/coverage/frontend/       (HTML report)
#   Module-level summary table with delta column on stdout
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
COV_DIR="$ROOT/build-cov/coverage"
BASELINE_FILE="$ROOT/.coverage-frontend-baseline.json"
UPDATE_BASELINE=false

for arg in "$@"; do
    [ "$arg" = "--update-baseline" ] && { UPDATE_BASELINE=true; shift; break; }
done

INFO="${1:-$COV_DIR/ut.raw.info}"
FRONTEND_INFO="$COV_DIR/frontend.info"
HTML_DIR="$COV_DIR/frontend"

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

extract_args=()
for mod in "${FRONTEND_MODULES[@]}"; do extract_args+=("$mod"); done

lcov --extract "$INFO" "${extract_args[@]}" \
     --output-file "$FRONTEND_INFO" --rc branch_coverage=1 \
     --ignore-errors deprecated,unsupported,inconsistent,inconsistent,unused,unused,empty,empty --quiet || true

if [ ! -s "$FRONTEND_INFO" ]; then
    echo "WARNING: no coverage data found for front-end modules in $INFO"
    echo "Try using e2e.raw.info instead: bash $0 build-cov/coverage/e2e.raw.info"
    exit 0
fi

genhtml "$FRONTEND_INFO" --output-directory "$HTML_DIR" \
        --title "HFSSS Front-End Coverage" --branch-coverage \
        --ignore-errors inconsistent,inconsistent,corrupt --quiet

# Load baseline into associative array via python3
declare -A BASELINES
while IFS= read -r line; do
    BASELINES["${line%%:*}"]="${line#*:}"
done < <(python3 -c "
import json,os,sys
p=sys.argv[1]
if os.path.isfile(p):
    d=json.load(open(p))
    for m,v in d.get('modules',{}).items():
        print(f\"{m}:{v.get('lines',0)}:{v.get('functions',0)}:{v.get('branches',0)}\")
" "$BASELINE_FILE")

fmt_delta() {
    [ "$1" = "n/a" ] && { echo "n/a"; return; }
    python3 -c "d=${1%%%*}-$2;print(f'+{d:.1f}' if d>=0 else f'{d:.1f}')"
}

echo ""
echo "========================================"
echo "Front-End Module Coverage Summary"
echo "========================================"
printf "%-40s %10s %10s %10s %10s\n" "Module" "Lines" "Functions" "Branches" "Delta(L)"
printf "%-40s %10s %10s %10s %10s\n" "------" "-----" "---------" "--------" "--------"

declare -A CURRENT_VALS
for mod in "${FRONTEND_MODULES[@]}"; do
    mod_info=$(mktemp)
    lcov --extract "$FRONTEND_INFO" "$mod" --output-file "$mod_info" \
         --rc branch_coverage=1 \
         --ignore-errors deprecated,unsupported,inconsistent,inconsistent,unused,unused \
         --quiet 2>/dev/null || true

    short_name=$(echo "$mod" | sed 's|^\*/||')
    if [ ! -s "$mod_info" ]; then
        printf "%-40s %10s %10s %10s %10s\n" "$short_name" "n/a" "n/a" "n/a" "n/a"
        rm -f "$mod_info"; continue
    fi

    summary=$(lcov --summary "$mod_info" --rc branch_coverage=1 \
              --ignore-errors deprecated,inconsistent,inconsistent,format,empty 2>&1)
    extract_pct() {
        echo "$summary" | grep -E "^[[:space:]]+$1" | head -1 | \
            grep -oE '[0-9]+\.[0-9]+%' | head -1 || echo "n/a"
    }
    line_pct=$(extract_pct lines); func_pct=$(extract_pct functions); branch_pct=$(extract_pct branches)

    base="${BASELINES[$short_name]:-0.0:0.0:0.0}"
    delta=$(fmt_delta "$line_pct" "${base%%:*}")
    printf "%-40s %10s %10s %10s %10s\n" "$short_name" "$line_pct" "$func_pct" "$branch_pct" "$delta"
    CURRENT_VALS["$short_name"]="${line_pct}:${func_pct}:${branch_pct}"
    rm -f "$mod_info"
done

echo ""
echo "Aggregate:"
lcov --summary "$FRONTEND_INFO" --rc branch_coverage=1 \
     --ignore-errors deprecated,inconsistent,inconsistent 2>&1 | grep -E 'lines|functions|branches'
echo ""
echo "HTML report: $HTML_DIR/index.html"

if [ "$UPDATE_BASELINE" = true ]; then
    python3 -c "
import json,sys,datetime
current={}
for pair in sys.argv[1:]:
    m,v=pair.split('=',1); l,f,b=v.split(':')
    s=lambda x: float(x.rstrip('%')) if x!='n/a' else 0.0
    current[m]={'lines':s(l),'functions':s(f),'branches':s(b)}
print(json.dumps({'created_at':datetime.datetime.now(datetime.UTC).strftime('%Y-%m-%dT%H:%M:%SZ'),'modules':current},indent=2))
" $(for k in "${!CURRENT_VALS[@]}"; do echo "$k=${CURRENT_VALS[$k]}"; done) > "$BASELINE_FILE"
    echo "Baseline updated: $BASELINE_FILE"
fi
