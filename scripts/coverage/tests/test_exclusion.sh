#!/bin/bash
# Verify that non-firmware paths are excluded from the UT .info file,
# and that firmware paths ARE present.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
INFO="$ROOT/build-cov/coverage/ut.info"

[ -f "$INFO" ] || { echo "FAIL: $INFO does not exist. Run run_ut_coverage.sh first."; exit 1; }

# Assert EXCLUSIONS (these paths must NOT appear in the info file)
for excluded in "src/vhost/" "src/kernel/" "src/tools/" "/tests/" "/usr/include/" "/usr/lib/"; do
    if grep -q "SF:.*${excluded}" "$INFO"; then
        echo "FAIL: excluded path still present in $INFO: $excluded"
        grep "SF:.*${excluded}" "$INFO" | head -3
        exit 1
    fi
done

# Assert INCLUSIONS (these firmware paths MUST appear)
for included in "src/ftl/" "src/common/" "src/controller/" "src/hal/" "src/media/" "src/pcie/" "src/perf/" "sssim.c"; do
    if ! grep -q "SF:.*${included}" "$INFO"; then
        echo "FAIL: expected firmware path missing from $INFO: $included"
        exit 1
    fi
done

# Count source files included
sf_count=$(grep -c '^SF:' "$INFO")
echo "PASS: exclusion test — $sf_count firmware source files in coverage scope"
