#!/bin/bash
# Test ratchet_check.sh: parsing, bootstrap, pass/fail logic.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SCRIPT="$ROOT/scripts/coverage/ratchet_check.sh"
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# Craft a fake .info summary by creating a minimal .info file
cat > "$TMP/fake.info" <<'EOF'
TN:
SF:/fake/a.c
DA:1,1
DA:2,1
DA:3,0
DA:4,0
LH:2
LF:4
FNF:2
FNH:1
BRF:4
BRH:2
end_of_record
EOF
# This fake.info: lines 50%, functions 50%, branches 50%

# === Test 1: Bootstrap (no baseline exists) ===
BASELINE="$TMP/baseline.json"
rm -f "$BASELINE"
output=$(INFO="$TMP/fake.info" BASELINE="$BASELINE" bash "$SCRIPT" 2>&1)
echo "$output" | grep -q "bootstrap" || { echo "FAIL: bootstrap message missing"; echo "$output"; exit 1; }
[ -f "$BASELINE" ] || { echo "FAIL: baseline not created on bootstrap"; exit 1; }

# === Test 2: Pass — current meets floor ===
# fake.info is 50/50/50; baseline is now 50/50/50; floor = 48/48/48; should pass
output=$(INFO="$TMP/fake.info" BASELINE="$BASELINE" bash "$SCRIPT" 2>&1) || { echo "FAIL: should pass but exited non-zero"; echo "$output"; exit 1; }
echo "$output" | grep -qi 'pass\|OK' || { echo "FAIL: no pass message"; echo "$output"; exit 1; }

# === Test 3: Fail — coverage dropped > 2% ===
# Craft an info with 20% line coverage
cat > "$TMP/low.info" <<'EOF'
TN:
SF:/fake/a.c
DA:1,1
DA:2,0
DA:3,0
DA:4,0
DA:5,0
LH:1
LF:5
FNF:2
FNH:1
BRF:4
BRH:2
end_of_record
EOF
# low.info: lines 20%, functions 50%, branches 50% — line dropped 30%, should FAIL

if INFO="$TMP/low.info" BASELINE="$BASELINE" bash "$SCRIPT" > "$TMP/out3.log" 2>&1; then
    echo "FAIL: ratchet should reject 30% drop"; cat "$TMP/out3.log"; exit 1
fi
grep -qi 'fail\|below' "$TMP/out3.log" || { echo "FAIL: no fail message"; cat "$TMP/out3.log"; exit 1; }

# === Test 4: --update-baseline explicitly rewrites baseline ===
INFO="$TMP/low.info" BASELINE="$BASELINE" bash "$SCRIPT" --update-baseline > "$TMP/out4.log" 2>&1
grep -qi 'updated' "$TMP/out4.log" || { echo "FAIL: no updated message"; cat "$TMP/out4.log"; exit 1; }
# Now low.info should pass (new baseline is 20/50/50)
INFO="$TMP/low.info" BASELINE="$BASELINE" bash "$SCRIPT" > "$TMP/out5.log" 2>&1

echo "PASS: ratchet bootstrap/pass/fail/update-baseline all working"
