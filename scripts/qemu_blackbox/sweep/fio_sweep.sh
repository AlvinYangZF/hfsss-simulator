#!/bin/bash
# Stage W fio parameter sweep driver.
#
# Requires a running QEMU + NBD environment (see scripts/qemu_blackbox/run.sh).
# Reuses the guest for the full sweep. Formats the NVMe namespace only between
# axis switches.
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

run_one() {
    local axis="$1" point="$2" rep="$3" fio_args="$4"
    local stem="$ARTIFACT_DIR/${axis}_${point}_rep${rep}"
    echo "[sweep] $axis=$point rep=$rep"
    if [ "$DRY_RUN" = "1" ]; then
        echo "DRY: fio $fio_args > $stem.stdout 2> $stem.stderr"
        return 0
    fi
    hfsss_guest_capture "$stem.stdout" "$stem.stderr" \
        "fio --output-format=json $fio_args" || true
    python3 - "$stem.stdout" "$stem.json" <<'PY' || true
import json, pathlib, sys
raw = pathlib.Path(sys.argv[1]).read_text(errors="replace")
start = raw.find("{"); end = raw.rfind("}")
if start >= 0 and end > start:
    pathlib.Path(sys.argv[2]).write_text(
        json.dumps(json.loads(raw[start:end+1]), indent=2) + "\n")
PY
}

format_ns() {
    echo "[sweep] nvme format /dev/nvme0n1"
    if [ "$DRY_RUN" = "1" ]; then
        echo "DRY: nvme format /dev/nvme0n1 -s 0 -f"
        return 0
    fi
    hfsss_guest_capture /dev/null /dev/null \
        "nvme format /dev/nvme0n1 -s 0 -f" || true
}

# Build iteration plan via Python, then execute each step in bash.
# Python emits tab-separated records: type TAB fields...
# Types: FORMAT, RUN axis point rep fio_args
# Use a temp file to avoid bash 3.2 limitations (no mapfile / process substitution
# does not keep functions in scope on macOS default bash).
_PLAN_FILE="$(mktemp /tmp/hfsss_sweep_plan.XXXXXX)"
trap 'rm -f "$_PLAN_FILE"' EXIT

python3 - "$MATRIX" "$HFSSS_GUEST_NVME_DEV" >"$_PLAN_FILE" <<'PY'
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
