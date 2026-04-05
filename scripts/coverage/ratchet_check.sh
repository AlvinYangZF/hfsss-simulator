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
summary=$(lcov --summary "$INFO" --rc branch_coverage=1 --ignore-errors deprecated,inconsistent,inconsistent,format,empty 2>&1)

extract_pct() {
    local kind="$1"
    # Match lines like "  lines......: 62.5% (1234 of 1975 lines)"
    # Use || true so grep returning non-zero (no match) doesn't abort under set -e
    echo "$summary" | grep -E "^[[:space:]]+${kind}" | head -1 | \
        grep -oE '[0-9]+\.[0-9]+%' | head -1 | tr -d '%' || true
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
