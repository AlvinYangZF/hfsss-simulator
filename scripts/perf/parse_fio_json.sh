#!/bin/bash
# Parse fio JSON output and extract key performance metrics.
# Usage: parse_fio_json.sh <fio-output.json>
#
# Outputs a compact JSON summary to stdout with per-job:
#   read/write IOPS, mean latency (us), p99 latency (us)
#
# Requires: jq

set -euo pipefail

if [ $# -lt 1 ] || [ ! -f "$1" ]; then
    echo "Usage: $0 <fio-output.json>" >&2
    echo "ERROR: input file not found: ${1:-<none>}" >&2
    exit 1
fi

INPUT="$1"

command -v jq >/dev/null || { echo "ERROR: jq not installed" >&2; exit 1; }

# Validate it's parseable JSON with a "jobs" array
jq -e '.jobs' "$INPUT" >/dev/null 2>&1 || {
    echo "ERROR: $INPUT is not valid fio JSON (missing .jobs array)" >&2
    exit 1
}

jq --arg src "$(basename "$INPUT")" \
   --arg parse_time "$(date -u +"%Y-%m-%dT%H:%M:%SZ")" \
'{
  source: $src,
  fio_timestamp: (.timestamp // 0 | todate),
  parsed_at: $parse_time,
  jobs: [.jobs[] | {
    jobname: .jobname,
    read_iops: (.read.iops // 0),
    write_iops: (.write.iops // 0),
    read_lat_mean_us: ((.read.lat_ns.mean // 0) / 1000),
    write_lat_mean_us: ((.write.lat_ns.mean // 0) / 1000),
    read_lat_p99_us: ((.read.clat_ns.percentile["99.000000"] // 0) / 1000),
    write_lat_p99_us: ((.write.clat_ns.percentile["99.000000"] // 0) / 1000)
  }]
}' "$INPUT"
