#!/bin/bash

set -euo pipefail

CASE_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
. "$CASE_LIB_DIR/common.sh"
# shellcheck source=env.sh
. "$CASE_LIB_DIR/env.sh"

HFSSS_CASE_ARTIFACT_DIR="${HFSSS_CASE_ARTIFACT_DIR:-}"
[ -n "$HFSSS_CASE_ARTIFACT_DIR" ] || hfsss_die "HFSSS_CASE_ARTIFACT_DIR is not set"

hfsss_case_log() {
    printf '[case:%s] %s\n' "${HFSSS_CASE_NAME:-unknown}" "$*"
}

hfsss_case_skip() {
    hfsss_case_log "SKIP: $*"
    exit 2
}

hfsss_case_require_guest_tool() {
    local tool
    for tool in "$@"; do
        if ! hfsss_guest_has_tool "$tool"; then
            hfsss_case_skip "guest tool not installed: $tool"
        fi
    done
}

hfsss_case_guest_capture() {
    local stem="$1"
    local cmd="$2"

    hfsss_guest_capture \
        "$HFSSS_CASE_ARTIFACT_DIR/${stem}.stdout" \
        "$HFSSS_CASE_ARTIFACT_DIR/${stem}.stderr" \
        "$cmd"
}

hfsss_case_guest_run() {
    local stem="$1"
    local cmd="$2"

    hfsss_case_log "$cmd"
    hfsss_case_guest_capture "$stem" "$cmd"
}

hfsss_case_run_nvme() {
    local stem="$1"
    shift

    hfsss_case_require_guest_tool nvme
    hfsss_case_guest_run "$stem" "nvme $*"
}

hfsss_case_assert_file_contains() {
    local path="$1"
    local pattern="$2"

    if ! grep -E -q "$pattern" "$path"; then
        hfsss_case_log "ASSERT FAIL: pattern '$pattern' not found in $path"
        return 1
    fi
}

hfsss_case_require_spdk_tool() {
    local tool

    for tool in "$@"; do
        if ! hfsss_guest_has_tool "$tool"; then
            hfsss_case_skip "SPDK guest tool not installed: $tool"
        fi
    done
}

hfsss_case_run_fio_json() {
    local stem="$1"
    local fio_args="$2"
    local raw_stdout="$HFSSS_CASE_ARTIFACT_DIR/${stem}.stdout"
    local json_out="$HFSSS_CASE_ARTIFACT_DIR/${stem}.json"
    local stderr_out="$HFSSS_CASE_ARTIFACT_DIR/${stem}.stderr"

    hfsss_case_require_guest_tool fio
    hfsss_guest_capture \
        "$raw_stdout" \
        "$stderr_out" \
        "fio --output-format=json $fio_args"

    python3 - "$raw_stdout" "$json_out" <<'PY'
import json
import pathlib
import sys

raw_path = pathlib.Path(sys.argv[1])
json_path = pathlib.Path(sys.argv[2])
raw = raw_path.read_text(errors="replace")
start = raw.find("{")
end = raw.rfind("}")
if start < 0 or end < start:
    raise SystemExit(1)
data = json.loads(raw[start:end + 1])
json_path.write_text(json.dumps(data, indent=2) + "\n")

jobs = data.get("jobs", [])
if not jobs:
    raise SystemExit(1)
if any(int(job.get("error", 0)) != 0 for job in jobs):
    raise SystemExit(2)
PY
}
