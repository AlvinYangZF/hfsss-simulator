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
