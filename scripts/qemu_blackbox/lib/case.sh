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

    hfsss_case_require_guest_tool fio
    hfsss_guest_capture \
        "$HFSSS_CASE_ARTIFACT_DIR/${stem}.json" \
        "$HFSSS_CASE_ARTIFACT_DIR/${stem}.stderr" \
        "fio --output-format=json $fio_args"
}
