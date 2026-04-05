#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
ARTIFACT_DIR="${HFSSS_SYSTEST_ARTIFACT_DIR:-$PROJECT_DIR/build/ci/systest}"
LOG_FILE="$ARTIFACT_DIR/systest.log"
SUMMARY_JSON="$ARTIFACT_DIR/summary.json"

mkdir -p "$ARTIFACT_DIR"

status="pass"
started_at="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
git_sha="$(git -C "$PROJECT_DIR" rev-parse HEAD 2>/dev/null || printf 'unknown')"

cd "$PROJECT_DIR"

if ! make systest 2>&1 | tee "$LOG_FILE"; then
    status="fail"
fi

finished_at="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

cat >"$SUMMARY_JSON" <<EOF
{
  "suite": "systest",
  "status": "$status",
  "git_sha": "$git_sha",
  "log_file": "systest.log",
  "started_at": "$started_at",
  "finished_at": "$finished_at"
}
EOF

if [ "$status" != "pass" ]; then
    exit 1
fi
