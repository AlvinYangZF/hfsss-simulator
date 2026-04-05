#!/bin/bash
# Run isolation primitives for concurrent CI execution.
#
# Multiple HFSSS CI runs (E2E coverage, blackbox suite, ad-hoc QEMU tests)
# must coexist on the same host without interfering. This library provides:
#
#   - Stable, unique RUN_ID per invocation
#   - Per-run workspace directory under build/runs/$RUN_ID/
#   - OS-allocated free ports (best-effort — see TOCTOU note on alloc_port)
#   - Unique QEMU process name for safe filtering
#   - PID-based cleanup (no pkill broadcast)
#
# Source this file and call hfsss_run_init at the top of your script.
# After that, the following variables are exported and ready to use:
#
#   HFSSS_RUN_ID              - unique identifier for this run
#   HFSSS_RUN_WORKSPACE       - absolute path to per-run workspace dir
#   HFSSS_RUN_NBD_PID_FILE    - path to write NBD server PID
#   HFSSS_RUN_QEMU_PID_FILE   - path to write QEMU PID
#   HFSSS_RUN_NBD_LOG         - NBD stderr/stdout log
#   HFSSS_RUN_QEMU_LOG        - QEMU console log
#   HFSSS_RUN_OVMF_VARS       - per-run copy of UEFI vars
#   HFSSS_RUN_QEMU_NAME       - unique QEMU -name tag
#
# Usage pattern:
#
#   . scripts/lib/run_isolation.sh
#   hfsss_run_init
#   hfsss_run_alloc_port nbd_port           # sets $nbd_port to a free port
#   hfsss_run_alloc_port ssh_port
#   trap 'hfsss_run_cleanup' EXIT INT TERM
#   ... launch NBD ($HFSSS_RUN_NBD_PID_FILE) + QEMU ($HFSSS_RUN_QEMU_PID_FILE) ...

set -euo pipefail

_hfsss_run_isolation_loaded=1

hfsss_run_generate_id() {
    if [ -n "${GITHUB_RUN_ID:-}" ] && [ -n "${GITHUB_RUN_ATTEMPT:-}" ]; then
        printf 'gh-%s-%s' "$GITHUB_RUN_ID" "$GITHUB_RUN_ATTEMPT"
    elif [ -n "${GITHUB_RUN_ID:-}" ]; then
        printf 'gh-%s' "$GITHUB_RUN_ID"
    else
        printf '%s-%s-%s' "$(hostname -s 2>/dev/null || echo host)" "$$" "$(date +%s)"
    fi
}

hfsss_run_init() {
    HFSSS_RUN_ID="${HFSSS_RUN_ID:-$(hfsss_run_generate_id)}"

    # Anchor workspace to project root unless caller overrides
    local project_root
    if [ -n "${HFSSS_PROJECT_DIR:-}" ]; then
        project_root="$HFSSS_PROJECT_DIR"
    else
        project_root="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
    fi

    HFSSS_RUN_WORKSPACE="${HFSSS_RUN_WORKSPACE:-$project_root/build/runs/$HFSSS_RUN_ID}"
    mkdir -p "$HFSSS_RUN_WORKSPACE"

    HFSSS_RUN_NBD_PID_FILE="$HFSSS_RUN_WORKSPACE/nbd.pid"
    HFSSS_RUN_QEMU_PID_FILE="$HFSSS_RUN_WORKSPACE/qemu.pid"
    HFSSS_RUN_NBD_LOG="$HFSSS_RUN_WORKSPACE/nbd-server.log"
    HFSSS_RUN_QEMU_LOG="$HFSSS_RUN_WORKSPACE/qemu-console.log"
    HFSSS_RUN_OVMF_VARS="$HFSSS_RUN_WORKSPACE/ovmf_vars.fd"
    HFSSS_RUN_QEMU_NAME="hfsss-${HFSSS_RUN_ID}"

    export HFSSS_RUN_ID HFSSS_RUN_WORKSPACE
    export HFSSS_RUN_NBD_PID_FILE HFSSS_RUN_QEMU_PID_FILE
    export HFSSS_RUN_NBD_LOG HFSSS_RUN_QEMU_LOG HFSSS_RUN_OVMF_VARS HFSSS_RUN_QEMU_NAME

    # Emit a manifest so operators can trace a run
    cat > "$HFSSS_RUN_WORKSPACE/run.manifest" <<EOF
run_id=$HFSSS_RUN_ID
workspace=$HFSSS_RUN_WORKSPACE
started_at=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
commit=$(git -C "$project_root" rev-parse HEAD 2>/dev/null || echo unknown)
pid=$$
EOF
}

# Allocate a free TCP port using the OS (BEST-EFFORT, NOT ATOMIC).
#
# This asks the OS for an unused port via socket.bind(('127.0.0.1', 0)),
# reads the port number, closes the socket, and returns. There is a brief
# TOCTOU window between close and the downstream service's bind during
# which another process on the host may claim the same port.
#
# This is a real improvement over hardcoded 10809/2222 (which guaranteed
# collision under concurrency), but callers still need to handle bind
# failure. See scripts/coverage/run_e2e_coverage.sh (start_nbd_with_retry)
# for the recommended retry pattern: allocate → start service → if it dies
# within 2s, reallocate and retry up to N times.
#
# Usage: hfsss_run_alloc_port <var_name>
#        sets $var_name to an available port (at the time of call)
hfsss_run_alloc_port() {
    local var_name="$1"
    local port
    port="$(python3 -c 'import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()' 2>/dev/null)"
    if [ -z "$port" ]; then
        echo "ERROR: hfsss_run_alloc_port: python3 not available or port allocation failed" >&2
        return 1
    fi
    printf -v "$var_name" '%s' "$port"
    export "$var_name"
}

# Check if a TCP port is currently bindable on 127.0.0.1.
# Returns 0 if bindable, non-zero otherwise.
hfsss_run_port_is_free() {
    local port="$1"
    python3 -c "
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
try:
    s.bind(('127.0.0.1', int('$port')))
    s.close()
    sys.exit(0)
except OSError:
    sys.exit(1)
" 2>/dev/null
}

# Kill a process whose PID is stored in a file.
# Sends SIGTERM, waits up to $timeout_s seconds, escalates to SIGKILL.
# Usage: hfsss_run_kill_pidfile <pid_file> [timeout_s]
hfsss_run_kill_pidfile() {
    local pid_file="$1"
    local timeout_s="${2:-10}"
    local pid

    [ -f "$pid_file" ] || return 0
    pid="$(cat "$pid_file" 2>/dev/null || true)"
    [ -n "$pid" ] || { rm -f "$pid_file"; return 0; }

    if kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        local i=0
        while kill -0 "$pid" 2>/dev/null && [ "$i" -lt $((timeout_s * 2)) ]; do
            sleep 0.5
            i=$((i + 1))
        done
        if kill -0 "$pid" 2>/dev/null; then
            echo "WARN: PID $pid did not exit within ${timeout_s}s after SIGTERM, sending SIGKILL" >&2
            kill -KILL "$pid" 2>/dev/null || true
        fi
    fi
    rm -f "$pid_file"
}

# Default cleanup — kill QEMU first (so guest TCP closes and any NBD
# async threads drain), then NBD server. Safe to call multiple times.
hfsss_run_cleanup() {
    hfsss_run_kill_pidfile "${HFSSS_RUN_QEMU_PID_FILE:-}" 5
    # Give NBD a moment to notice client disconnect and drain async threads
    [ -f "${HFSSS_RUN_NBD_PID_FILE:-}" ] && sleep 1
    hfsss_run_kill_pidfile "${HFSSS_RUN_NBD_PID_FILE:-}" 10
}

# Assert a process matching $HFSSS_RUN_QEMU_NAME does NOT exist yet
# (guards against workspace reuse without cleanup).
hfsss_run_assert_no_qemu() {
    if pgrep -f "$HFSSS_RUN_QEMU_NAME" >/dev/null 2>&1; then
        echo "ERROR: a QEMU process tagged '$HFSSS_RUN_QEMU_NAME' is already running" >&2
        echo "       (workspace: $HFSSS_RUN_WORKSPACE)" >&2
        return 1
    fi
}
