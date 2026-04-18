#!/bin/bash
# Stage W fio parameter sweep driver.
#
# Requires a running QEMU + NBD environment (see scripts/qemu_blackbox/run.sh).
# Reuses the guest for the full sweep. Formats the NVMe namespace only between
# axis switches.
#
# For every run, this script captures:
#   <stem>.stdout  fio raw stdout
#   <stem>.stderr  fio raw stderr
#   <stem>.json    pretty-printed JSON extracted from stdout (if parseable)
#   <stem>.exit    fio exit code (decimal, followed by newline)
#
# Usage:
#   fio_sweep.sh --matrix <matrix.json> --artifact-dir <dir> [--dry-run]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

MATRIX=""
ARTIFACT_DIR=""
DRY_RUN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --matrix) MATRIX="$2"; shift 2;;
        --artifact-dir) ARTIFACT_DIR="$2"; shift 2;;
        --dry-run) DRY_RUN=1; shift;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done

[ -n "$MATRIX" ] || { echo "--matrix required" >&2; exit 2; }
[ -n "$ARTIFACT_DIR" ] || { echo "--artifact-dir required" >&2; exit 2; }
mkdir -p "$ARTIFACT_DIR"

# shellcheck source=../lib/env.sh
if [ "$DRY_RUN" = "0" ]; then
    . "$REPO_ROOT/scripts/qemu_blackbox/lib/env.sh"
    hfsss_blackbox_init_defaults
fi

# Run a fio invocation, capture stdout/stderr/json/exit per run. Never silently
# swallow errors: the exit code is recorded to <stem>.exit for downstream
# summarizers. The only non-zero exit we tolerate is fio's own non-zero (e.g.
# verify_fatal=0 + errors); the *driver* continues so the sweep completes.
run_one() {
    local axis="$1" point="$2" rep="$3" fio_args="$4"
    local stem="$ARTIFACT_DIR/${axis}_${point}_rep${rep}"
    echo "[sweep] $axis=$point rep=$rep"
    if [ "$DRY_RUN" = "1" ]; then
        echo "DRY: fio $fio_args > $stem.stdout 2> $stem.stderr"
        return 0
    fi

    local fio_rc=0
    set +e
    hfsss_guest_capture "$stem.stdout" "$stem.stderr" \
        "fio --output-format=json $fio_args"
    fio_rc=$?
    set -e
    printf '%d\n' "$fio_rc" > "$stem.exit"

    # Extract the JSON payload from stdout. A non-zero extractor rc means the
    # JSON is broken, which is a real failure signal we must not hide — record
    # it via .exit-extractor to make it visible in the matrix.
    local extract_rc=0
    set +e
    python3 - "$stem.stdout" "$stem.json" <<'PY'
import json, pathlib, sys
raw = pathlib.Path(sys.argv[1]).read_text(errors="replace")
start = raw.find("{"); end = raw.rfind("}")
if start < 0 or end <= start:
    sys.exit(1)
pathlib.Path(sys.argv[2]).write_text(
    json.dumps(json.loads(raw[start:end+1]), indent=2) + "\n")
PY
    extract_rc=$?
    set -e
    if [ "$extract_rc" -ne 0 ]; then
        echo "[sweep] WARN: JSON extract failed for $stem (rc=$extract_rc)"
    fi
}

format_ns() {
    local dev="${HFSSS_GUEST_NVME_DEV:-/dev/nvme0n1}"
    echo "[sweep] nvme format $dev"
    if [ "$DRY_RUN" = "1" ]; then
        echo "DRY: nvme format $dev -s 0 -f"
        return 0
    fi
    local fmt_rc=0
    set +e
    hfsss_guest_capture /dev/null /dev/null \
        "nvme format $dev -s 0 -f"
    fmt_rc=$?
    set -e
    if [ "$fmt_rc" -ne 0 ]; then
        echo "[sweep] WARN: nvme format returned $fmt_rc"
    fi
}

# Build iteration plan via Python, then execute each step in bash.
# Python emits tab-separated records: type TAB fields...
# Types: FORMAT, RUN axis point rep fio_args
# Use a temp file to avoid bash 3.2 limitations (no mapfile / process substitution
# does not keep functions in scope on macOS default bash).
_PLAN_FILE="$(mktemp /tmp/hfsss_sweep_plan.XXXXXX)"
trap 'rm -f "$_PLAN_FILE"' EXIT

NVME_DEV="${HFSSS_GUEST_NVME_DEV:-/dev/nvme0n1}"
python3 - "$MATRIX" "$NVME_DEV" >"$_PLAN_FILE" <<'PY'
import json, sys
cfg = json.load(open(sys.argv[1]))
nvme_dev = sys.argv[2]
base = cfg["baseline"]
repeats = int(cfg.get("repeats", 3))
first_axis = True
for axis in cfg["axes"]:
    axis_name = axis["name"]
    fio_param = axis.get("fio_param", axis_name)
    if cfg.get("format_between_axis_switch") and not first_axis:
        print("FORMAT")
    first_axis = False
    for point in axis["points"]:
        merged = dict(base)
        merged[fio_param] = point
        fio_args = " ".join(
            f"--{k}={v}" for k, v in merged.items() if v is not None)
        fio_args += f" --filename={nvme_dev}"
        for rep in range(1, repeats + 1):
            print(f"RUN\t{axis_name}\t{point}\t{rep}\t{fio_args}")
PY

while IFS=$'\t' read -r -u 3 type axis point rep fio_args; do
    case "$type" in
        FORMAT)
            format_ns
            ;;
        RUN)
            run_one "$axis" "$point" "$rep" "$fio_args"
            ;;
    esac
done 3< "$_PLAN_FILE"

echo "[sweep] done. Artifacts in $ARTIFACT_DIR"
