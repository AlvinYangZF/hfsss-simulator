#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=qemu_blackbox/lib/env.sh
. "$SCRIPT_DIR/qemu_blackbox/lib/env.sh"

usage() {
    cat <<'EOF'
Usage: scripts/run_qemu_blackbox_soak.sh [options]

Options:
  --rounds N             Number of suite rounds to run (default: 10)
  --case NAME            Run only the named case in each round (repeatable)
  --guest-dir DIR        Directory holding guest qcow2/iso/OVMF assets
  --artifacts-dir DIR    Store soak logs and per-round output here
  --mode MODE            NBD mode: sync, mt, async (default: mt)
  --size-mb N            Exported NBD size in MiB (default: 512)
  --nbd-port PORT        Host NBD port (default: 10820, auto-adjusted if busy)
  --ssh-port PORT        Guest SSH forward port (default: 10022, auto-adjusted if busy)
  --build                Rebuild hfsss-nbd-server before running
  --skip-build           Reuse existing hfsss-nbd-server binary
  --keep-env             Keep QEMU/NBD running after soak exit
  --continue-on-fail     Continue remaining rounds after a failed round
  --help                 Show this help

Default case set:
  nvme/001_nvme_cli_smoke.sh
  nvme/002_nvme_namespace_info.sh
  nvme/003_nvme_flush_smoke.sh
  fio/010_fio_randwrite_verify.sh
  fio/011_fio_randrw_verify.sh
  fio/012_fio_seqwrite_verify.sh
  fio/013_fio_trim_verify.sh
EOF
}

hfsss_blackbox_init_defaults

ROUNDS=10
CONTINUE_ON_FAIL=0
declare -a SELECTED_CASES=()

while [ $# -gt 0 ]; do
    case "$1" in
        --rounds)
            [ $# -ge 2 ] || hfsss_die "--rounds requires a value"
            ROUNDS="$2"
            shift 2
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
        --build)
            HFSSS_BUILD_NBD=1
            shift
            ;;
        --skip-build)
            HFSSS_BUILD_NBD=0
            shift
            ;;
        --keep-env)
            HFSSS_KEEP_ENV=1
            shift
            ;;
        --continue-on-fail)
            CONTINUE_ON_FAIL=1
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

case "$ROUNDS" in
    ''|*[!0-9]*)
        hfsss_die "--rounds must be a positive integer"
        ;;
esac
[ "$ROUNDS" -gt 0 ] || hfsss_die "--rounds must be greater than zero"

if [ "${#SELECTED_CASES[@]}" -eq 0 ]; then
    SELECTED_CASES=(
        "nvme/001_nvme_cli_smoke.sh"
        "nvme/002_nvme_namespace_info.sh"
        "nvme/003_nvme_flush_smoke.sh"
        "fio/010_fio_randwrite_verify.sh"
        "fio/011_fio_randrw_verify.sh"
        "fio/012_fio_seqwrite_verify.sh"
        "fio/013_fio_trim_verify.sh"
    )
fi

hfsss_blackbox_prepare_dirs
trap hfsss_blackbox_cleanup EXIT

hfsss_log "starting soak environment"
hfsss_blackbox_start_env
hfsss_blackbox_write_manifest

ROUND_SUMMARY="$HFSSS_ARTIFACT_ROOT/rounds.tsv"
printf 'round\tstatus\tartifacts\n' >"$ROUND_SUMMARY"

PASS_ROUNDS=0
FAIL_ROUNDS=0

for round in $(seq 1 "$ROUNDS"); do
    round_dir="$HFSSS_ARTIFACT_ROOT/round-$(printf '%03d' "$round")"
    cmd=(
        "$SCRIPT_DIR/run_qemu_blackbox_tests.sh"
        "--reuse-env"
        "--ssh-port" "$HFSSS_SSH_PORT"
        "--artifacts-dir" "$round_dir"
    )

    for selected in "${SELECTED_CASES[@]}"; do
        cmd+=("--case" "$selected")
    done

    hfsss_log "starting round $round/$ROUNDS"
    if "${cmd[@]}"; then
        status="PASS"
        PASS_ROUNDS=$((PASS_ROUNDS + 1))
    else
        status="FAIL"
        FAIL_ROUNDS=$((FAIL_ROUNDS + 1))
    fi

    printf '%s\t%s\t%s\n' "$round" "$status" "$round_dir" >>"$ROUND_SUMMARY"

    if [ "$status" = "FAIL" ] && [ "$CONTINUE_ON_FAIL" != "1" ]; then
        hfsss_warn "stopping after failed round $round"
        break
    fi
done

{
    printf 'rounds=%s\n' "$ROUNDS"
    printf 'pass_rounds=%s\n' "$PASS_ROUNDS"
    printf 'fail_rounds=%s\n' "$FAIL_ROUNDS"
} >"$HFSSS_ARTIFACT_ROOT/soak-summary.txt"

hfsss_log "soak summary: pass_rounds=$PASS_ROUNDS fail_rounds=$FAIL_ROUNDS"
hfsss_log "soak artifacts: $HFSSS_ARTIFACT_ROOT"

if [ "$FAIL_ROUNDS" -ne 0 ]; then
    exit 1
fi
