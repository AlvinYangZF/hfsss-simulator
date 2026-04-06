#!/bin/bash
# Verify that COVERAGE_UT_BINS stays aligned with the runnable
# test_/systest_/stress_ binary target set defined in the Makefile.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MAKEFILE="$ROOT/Makefile"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT INT TERM

expected="$tmpdir/expected.txt"
actual="$tmpdir/actual.txt"
missing="$tmpdir/missing.txt"
extra="$tmpdir/extra.txt"
db="$tmpdir/make-db.txt"

# Expected: every runnable test_/systest_/stress_ binary target declared
# in the Makefile, excluding the known out-of-scope vhost protocol test.
awk '/^(TEST|STRESS|SYSTEST)_[A-Z0-9_]+[[:space:]]*=/{print $3}' "$MAKEFILE" \
  | grep '^\$(BIN_DIR)/' \
  | grep -v 'test_vhost_proto' \
  | sed 's#$(BIN_DIR)#build-cov/bin#g' \
  | sort -u > "$expected"

# Actual: the expanded COVERAGE_UT_BINS make variable as coverage-build uses it.
make -s -pn -f "$MAKEFILE" help > "$db"
awk -F' = ' '/^COVERAGE_UT_BINS[[:space:]]*=/{print $2; exit}' "$db" \
  | tr ' ' '\n' \
  | sed '/^$/d' \
  | sed 's#$(COVERAGE_BIN_DIR)#build-cov/bin#g' \
  | sort -u > "$actual"

comm -23 "$expected" "$actual" > "$missing"
comm -13 "$expected" "$actual" > "$extra"

if [ -s "$missing" ] || [ -s "$extra" ]; then
    echo "ERROR: COVERAGE_UT_BINS is out of sync with runnable test targets."
    if [ -s "$missing" ]; then
        echo ""
        echo "Missing from COVERAGE_UT_BINS:"
        sed 's/^/  - /' "$missing"
    fi
    if [ -s "$extra" ]; then
        echo ""
        echo "Extra entries in COVERAGE_UT_BINS:"
        sed 's/^/  - /' "$extra"
    fi
    exit 1
fi

echo "coverage target self-check: OK"
