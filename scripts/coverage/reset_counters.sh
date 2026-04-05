#!/bin/bash
# Delete all .gcda runtime counter files while preserving .gcno structure files.
# Used between test phases (UT vs E2E) so coverage data doesn't accumulate
# across phases when we want them captured separately.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-cov}"

if [ ! -d "$BUILD_DIR" ]; then
    echo "reset_counters: $BUILD_DIR does not exist, nothing to reset"
    exit 0
fi

count=$(find "$BUILD_DIR" -name '*.gcda' | wc -l | tr -d ' ')
find "$BUILD_DIR" -name '*.gcda' -delete
echo "reset_counters: removed $count .gcda files from $BUILD_DIR"
