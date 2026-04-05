#!/bin/bash

set -euo pipefail

hfsss_log() {
    printf '[hfsss-blackbox] %s\n' "$*"
}

hfsss_warn() {
    printf '[hfsss-blackbox] WARN: %s\n' "$*" >&2
}

hfsss_die() {
    printf '[hfsss-blackbox] ERROR: %s\n' "$*" >&2
    exit 1
}

hfsss_require_cmd() {
    local cmd
    for cmd in "$@"; do
        command -v "$cmd" >/dev/null 2>&1 || hfsss_die "required command not found: $cmd"
    done
}

hfsss_abs_dirname() {
    local target="$1"
    (cd "$target" && pwd)
}

hfsss_timestamp() {
    date '+%Y%m%d-%H%M%S'
}

hfsss_quote_remote() {
    printf '%q' "$1"
}
