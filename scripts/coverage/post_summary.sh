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
summary=$(lcov --summary "$INFO" --rc branch_coverage=1 --ignore-errors deprecated,inconsistent,inconsistent 2>&1)
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
## HFSSS Coverage Report (UT)

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
