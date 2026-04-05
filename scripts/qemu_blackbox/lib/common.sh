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

hfsss_run_with_timeout() {
    local timeout_s="$1"
    shift

    python3 - "$timeout_s" "$@" <<'PY'
import subprocess
import sys

timeout_s = float(sys.argv[1])
cmd = sys.argv[2:]

proc = subprocess.Popen(cmd)
try:
    sys.exit(proc.wait(timeout=timeout_s))
except subprocess.TimeoutExpired:
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    sys.exit(124)
PY
}
