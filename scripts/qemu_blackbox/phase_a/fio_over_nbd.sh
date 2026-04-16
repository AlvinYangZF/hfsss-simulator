#!/bin/bash
# Seg-1: fio directly against the hfsss NBD server, bypassing QEMU.
#
# Requires: running hfsss-nbd-server on HOST, reachable at 127.0.0.1:$PORT.
# Requires: fio with ioengine=nbd (built with --enable-libnbd).
#
# Usage:
#   fio_over_nbd.sh --mfc <mfc.json> --artifact-dir <dir> --port <port>
set -euo pipefail

MFC=""
ARTIFACT_DIR=""
PORT="10820"

while [ $# -gt 0 ]; do
    case "$1" in
        --mfc) MFC="$2"; shift 2;;
        --artifact-dir) ARTIFACT_DIR="$2"; shift 2;;
        --port) PORT="$2"; shift 2;;
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
    out.append(f"--{k}={v}")
print(" ".join(out))
PY
)

for rep in 1 2 3; do
    stem="$ARTIFACT_DIR/seg1_rep${rep}"
    echo "[seg-1] rep=$rep"
    fio --output-format=json \
        --ioengine=nbd --uri=nbd://127.0.0.1:${PORT} \
        $FIO_ARGS > "$stem.stdout" 2> "$stem.stderr" || true
    # Extract JSON from stdout
    python3 - "$stem.stdout" "$stem.json" <<'PY' || true
import json, pathlib, sys
raw = pathlib.Path(sys.argv[1]).read_text(errors="replace")
s = raw.find("{"); e = raw.rfind("}")
if s >= 0 and e > s:
    pathlib.Path(sys.argv[2]).write_text(
        json.dumps(json.loads(raw[s:e+1]), indent=2) + "\n")
PY
done

# Quick tally using the sweep summarizer's count function
python3 - "$ARTIFACT_DIR" <<'PY'
import sys, pathlib, re
d = pathlib.Path(sys.argv[1])
RE = re.compile(r"^(verify:|fio: verify)", re.MULTILINE)
total = 0
for p in sorted(d.glob("seg1_rep*.stderr")):
    n = len(RE.findall(p.read_text(errors='replace')))
    print(f"  {p.name}: {n} verify errors")
    total += n
print(f"[seg-1] total verify errors: {total}")
PY
