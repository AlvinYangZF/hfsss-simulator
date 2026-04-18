#!/bin/bash
# Seg-1: fio directly against the hfsss NBD server, bypassing QEMU.
#
# Requires: running hfsss-nbd-server on HOST, reachable at 127.0.0.1:$PORT.
# Requires: fio with ioengine=nbd (built with --enable-libnbd).
#
# Per run, this writes:
#   <stem>.stdout  fio raw stdout
#   <stem>.stderr  fio raw stderr
#   <stem>.json    pretty-printed JSON extracted from stdout (if parseable)
#   <stem>.exit    fio exit code
#
# Usage:
#   fio_over_nbd.sh --mfc <mfc.json> --artifact-dir <dir> --port <port>
set -euo pipefail

MFC=""
ARTIFACT_DIR=""
PORT="10820"
REPEATS=3

while [ $# -gt 0 ]; do
    case "$1" in
        --mfc) MFC="$2"; shift 2;;
        --artifact-dir) ARTIFACT_DIR="$2"; shift 2;;
        --port) PORT="$2"; shift 2;;
        --repeats) REPEATS="$2"; shift 2;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done

[ -n "$MFC" ] || { echo "--mfc required (path to MFC json from Stage W)" >&2; exit 2; }
[ -n "$ARTIFACT_DIR" ] || { echo "--artifact-dir required" >&2; exit 2; }
mkdir -p "$ARTIFACT_DIR"

# Build fio args from MFC json.
FIO_ARGS=$(python3 - "$MFC" <<'PY'
import sys, json
cfg = json.load(open(sys.argv[1]))
out = []
for k, v in cfg.items():
    if k.startswith("_"):
        continue
    out.append(f"--{k}={v}")
print(" ".join(out))
PY
)

summary_fail=0
summary_pass=0

for rep in $(seq 1 "$REPEATS"); do
    stem="$ARTIFACT_DIR/seg1_rep${rep}"
    echo "[seg-1] rep=$rep"
    fio_rc=0
    set +e
    fio --output-format=json \
        --ioengine=nbd --uri=nbd://127.0.0.1:${PORT} \
        $FIO_ARGS > "$stem.stdout" 2> "$stem.stderr"
    fio_rc=$?
    set -e
    printf '%d\n' "$fio_rc" > "$stem.exit"

    extract_rc=0
    set +e
    python3 - "$stem.stdout" "$stem.json" <<'PY'
import json, pathlib, sys
raw = pathlib.Path(sys.argv[1]).read_text(errors="replace")
s = raw.find("{"); e = raw.rfind("}")
if s < 0 or e <= s:
    sys.exit(1)
pathlib.Path(sys.argv[2]).write_text(
    json.dumps(json.loads(raw[s:e+1]), indent=2) + "\n")
PY
    extract_rc=$?
    set -e
    if [ "$extract_rc" -ne 0 ]; then
        echo "[seg-1] WARN: JSON extract failed for $stem"
    fi
done

# Tally using the same rules as the sweep summarizer.
python3 - "$ARTIFACT_DIR" "$REPEATS" <<'PY'
import sys, pathlib, re, json
artifact_dir = pathlib.Path(sys.argv[1])
repeats = int(sys.argv[2])
VERIFY_RE = re.compile(r"^(verify:|fio: verify)", re.MULTILINE)
IO_U_RE = re.compile(r"^fio: io_u error", re.MULTILINE)

total_verify = 0
total_iou = 0
failing_runs = 0
missing_runs = 0

for rep in range(1, repeats + 1):
    stem = artifact_dir / f"seg1_rep{rep}"
    stderr_p = stem.with_suffix(".stderr")
    json_p = stem.with_suffix(".json")
    exit_p = stem.with_suffix(".exit")

    if not stderr_p.exists() or not exit_p.exists():
        missing_runs += 1
        failing_runs += 1
        print(f"  rep{rep}: MISSING artifacts")
        continue

    stderr_text = stderr_p.read_text(errors="replace")
    v = len(VERIFY_RE.findall(stderr_text))
    iou = len(IO_U_RE.findall(stderr_text))
    total_verify += v
    total_iou += iou

    fio_exit = int(exit_p.read_text().strip() or "0")

    json_valid = False
    je = 0
    jerr = 0
    if json_p.exists() and json_p.stat().st_size > 0:
        try:
            d = json.loads(json_p.read_text())
            jobs = d.get("jobs", [])
            if jobs:
                je = int(jobs[0].get("total_err", 0) or 0)
                jerr = int(jobs[0].get("error", 0) or 0)
            json_valid = True
        except Exception:
            json_valid = False

    run_failing = (v or iou or not json_valid or fio_exit != 0 or je or jerr)
    if run_failing:
        failing_runs += 1
    markers = []
    if not json_valid:
        markers.append("!json")
    if fio_exit != 0:
        markers.append(f"exit={fio_exit}")
    if jerr:
        markers.append(f"json_err={jerr}")
    if je:
        markers.append(f"json_total_err={je}")
    mark_str = " ".join(markers) if markers else "-"
    print(f"  rep{rep}: verify={v} io_u={iou} {mark_str}")

print(f"[seg-1] aggregate: verify_errors={total_verify} io_u_errors={total_iou} "
      f"failing_runs={failing_runs}/{repeats} missing_runs={missing_runs}")
if failing_runs > 0:
    sys.exit(1)
PY
