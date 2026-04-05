#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ARTIFACT_DIR="${HFSSS_CI_ARTIFACT_DIR:-$PROJECT_DIR/build/blackbox-tests/latest}"

mkdir -p "$ARTIFACT_DIR"

exec "$SCRIPT_DIR/run_qemu_blackbox_tests.sh" \
  --artifacts-dir "$ARTIFACT_DIR" \
  "$@"
