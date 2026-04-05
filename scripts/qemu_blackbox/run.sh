#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib/env.sh
. "$SCRIPT_DIR/lib/env.sh"

usage() {
    cat <<'EOF'
Usage: scripts/run_qemu_blackbox_tests.sh [options]

Options:
  --list                 List available test cases
  --case NAME            Run only the named case (repeatable)
  --guest-dir DIR        Directory holding guest qcow2/iso/OVMF assets
  --artifacts-dir DIR    Store suite logs and per-case output here
  --mode MODE            NBD mode: sync, mt, async (default: mt)
  --size-mb N            Exported NBD size in MiB (default: 512)
  --nbd-port PORT        Host NBD port (default: 10820)
  --ssh-port PORT        Guest SSH forward port (default: 10022)
  --reuse-env            Reuse an already running QEMU/NBD environment
  --keep-env             Keep QEMU/NBD running after suite exit
  --build                Rebuild hfsss-nbd-server before running
  --skip-build           Reuse existing hfsss-nbd-server binary
  --help                 Show this help
EOF
}

hfsss_blackbox_init_defaults

declare -a SELECTED_CASES=()
LIST_ONLY=0

while [ $# -gt 0 ]; do
    case "$1" in
        --list)
            LIST_ONLY=1
            shift
            ;;
        --case)
            [ $# -ge 2 ] || hfsss_die "--case requires a value"
            SELECTED_CASES+=("$2")
            shift 2
            ;;
        --guest-dir)
            [ $# -ge 2 ] || hfsss_die "--guest-dir requires a value"
            HFSSS_GUEST_DIR="$2"
            shift 2
            ;;
        --artifacts-dir)
            [ $# -ge 2 ] || hfsss_die "--artifacts-dir requires a value"
            HFSSS_ARTIFACT_ROOT="$2"
            HFSSS_NBD_LOG="$HFSSS_ARTIFACT_ROOT/nbd-server.log"
            HFSSS_QEMU_CONSOLE_LOG="$HFSSS_ARTIFACT_ROOT/qemu-console.log"
            shift 2
            ;;
        --mode)
            [ $# -ge 2 ] || hfsss_die "--mode requires a value"
            HFSSS_NBD_MODE="$2"
            shift 2
            ;;
        --size-mb)
            [ $# -ge 2 ] || hfsss_die "--size-mb requires a value"
            HFSSS_NBD_SIZE_MB="$2"
            shift 2
            ;;
        --nbd-port)
            [ $# -ge 2 ] || hfsss_die "--nbd-port requires a value"
            HFSSS_NBD_PORT="$2"
            shift 2
            ;;
        --ssh-port)
            [ $# -ge 2 ] || hfsss_die "--ssh-port requires a value"
            HFSSS_SSH_PORT="$2"
            shift 2
            ;;
        --reuse-env)
            HFSSS_REUSE_ENV=1
            shift
            ;;
        --keep-env)
            HFSSS_KEEP_ENV=1
            shift
            ;;
        --build)
            HFSSS_BUILD_NBD=1
            shift
            ;;
        --skip-build)
            HFSSS_BUILD_NBD=0
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            hfsss_die "unknown option: $1"
            ;;
    esac
done

if [ "$LIST_ONLY" = "1" ]; then
    hfsss_blackbox_list_cases
    exit 0
fi

declare -a CASE_PATHS=()
declare -a CASE_IDS=()
declare -a CASE_STATUSES=()
case_path=""

resolve_case_path() {
    local requested="$1"
    local candidate="$HFSSS_BLACKBOX_ROOT/cases/$requested"

    if [ -f "$candidate" ]; then
        printf '%s\n' "$candidate"
        return 0
    fi

    if [ -f "$requested" ]; then
        printf '%s\n' "$requested"
        return 0
    fi

    candidate="$(find "$HFSSS_BLACKBOX_ROOT/cases" -type f -name "$requested" | sort | head -1)"
    if [ -n "$candidate" ]; then
        printf '%s\n' "$candidate"
        return 0
    fi

    return 1
}

if [ "${#SELECTED_CASES[@]}" -eq 0 ]; then
    while IFS= read -r case_path; do
        [ -f "$case_path" ] || continue
        CASE_PATHS+=("$case_path")
    done < <(find "$HFSSS_BLACKBOX_ROOT/cases" -type f -name '*.sh' | sort)
else
    for case_path in "${SELECTED_CASES[@]}"; do
        resolved_case="$(resolve_case_path "$case_path")" || {
            hfsss_die "case not found: $case_path"
        }
        CASE_PATHS+=("$resolved_case")
    done
fi

[ "${#CASE_PATHS[@]}" -gt 0 ] || hfsss_die "no test cases selected"

hfsss_blackbox_prepare_dirs
trap hfsss_blackbox_cleanup EXIT

SUMMARY_FILE="$HFSSS_ARTIFACT_ROOT/summary.tsv"
SUMMARY_JSON="$HFSSS_ARTIFACT_ROOT/summary.json"
JUNIT_XML="$HFSSS_ARTIFACT_ROOT/junit.xml"
printf 'case\tstatus\n' >"$SUMMARY_FILE"

PASS=0
FAIL=0
SKIP=0

if [ "$HFSSS_REUSE_ENV" = "1" ]; then
    hfsss_blackbox_verify_existing_env
else
    hfsss_blackbox_start_env
fi

for case_path in "${CASE_PATHS[@]}"; do
    case_id="${case_path#"$HFSSS_BLACKBOX_ROOT/cases/"}"
    case_name="$(basename "$case_path" .sh)"
    case_dir="$HFSSS_ARTIFACT_ROOT/$case_name"
    mkdir -p "$case_dir"

    export HFSSS_CASE_NAME="$case_name"
    export HFSSS_CASE_ARTIFACT_DIR="$case_dir"
    export HFSSS_PROJECT_DIR
    export HFSSS_BLACKBOX_ROOT
    export HFSSS_SSH_PORT
    export HFSSS_SSH_KEY
    export HFSSS_GUEST_NVME_DEV
    export HFSSS_GUEST_NVME_CTRL
    export HFSSS_ARTIFACT_ROOT

    hfsss_log "running $case_name"
    hfsss_guest_run "dmesg -C || true" >/dev/null 2>&1 || true

    if "$case_path" >"$case_dir/case.stdout.log" 2>"$case_dir/case.stderr.log"; then
        status="PASS"
        PASS=$((PASS + 1))
    else
        rc=$?
        if [ "$rc" -eq 2 ]; then
            status="SKIP"
            SKIP=$((SKIP + 1))
        else
            status="FAIL"
            FAIL=$((FAIL + 1))
        fi
    fi

    hfsss_blackbox_capture_guest_state "$case_dir"
    printf '%s\t%s\n' "$case_id" "$status" >>"$SUMMARY_FILE"
    CASE_IDS+=("$case_id")
    CASE_STATUSES+=("$status")
    hfsss_log "$case_name -> $status"
done

hfsss_log "summary: pass=$PASS fail=$FAIL skip=$SKIP"
hfsss_log "artifacts: $HFSSS_ARTIFACT_ROOT"

{
    printf '{\n'
    printf '  "suite": "qemu-blackbox",\n'
    printf '  "artifacts_dir": "%s",\n' "$HFSSS_ARTIFACT_ROOT"
    printf '  "counts": {\n'
    printf '    "pass": %d,\n' "$PASS"
    printf '    "fail": %d,\n' "$FAIL"
    printf '    "skip": %d\n' "$SKIP"
    printf '  },\n'
    printf '  "cases": [\n'
    for i in "${!CASE_IDS[@]}"; do
        sep=","
        if [ "$i" -eq $((${#CASE_IDS[@]} - 1)) ]; then
            sep=""
        fi
        printf '    {"name": "%s", "status": "%s"}%s\n' "${CASE_IDS[$i]}" "${CASE_STATUSES[$i]}" "$sep"
    done
    printf '  ]\n'
    printf '}\n'
} >"$SUMMARY_JSON"

{
    printf '<?xml version="1.0" encoding="UTF-8"?>\n'
    printf '<testsuite name="qemu-blackbox" tests="%d" failures="%d" skipped="%d">\n' "${#CASE_IDS[@]}" "$FAIL" "$SKIP"
    for i in "${!CASE_IDS[@]}"; do
        printf '  <testcase name="%s">' "${CASE_IDS[$i]}"
        case "${CASE_STATUSES[$i]}" in
            PASS)
                printf '</testcase>\n'
                ;;
            SKIP)
                printf '<skipped message="case skipped"/></testcase>\n'
                ;;
            *)
                printf '<failure message="case failed"/></testcase>\n'
                ;;
        esac
    done
    printf '</testsuite>\n'
} >"$JUNIT_XML"

if [ "$FAIL" -ne 0 ]; then
    exit 1
fi
