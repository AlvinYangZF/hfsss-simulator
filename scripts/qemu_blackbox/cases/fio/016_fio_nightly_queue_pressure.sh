#!/bin/bash
# Nightly queue-pressure stress test with mixed random I/O.
#
# Sustains elevated queue depth (32) with a balanced 50/50 read/write
# mix for 60 seconds. This exercises:
#   - sustained queue-depth pressure on the NVMe submission/completion path
#   - balanced read/write concurrency (unlike the 70/30 in 015)
#   - data integrity under prolonged mixed-workload stress
#   - time-based sustained load (vs fixed io_size in other cases)
#
# Nightly-only: too heavy for PR smoke gate (~60s runtime).
set -euo pipefail

CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../../lib/case.sh
. "$CASE_DIR/../../lib/case.sh"

hfsss_case_run_fio_json "nightly-queue-pressure" \
    "--name=nightly_queue_pressure_randrw \
     --filename=$HFSSS_GUEST_NVME_DEV \
     --rw=randrw \
     --rwmixread=50 \
     --bs=4k \
     --size=1g \
     --iodepth=32 \
     --numjobs=1 \
     --direct=1 \
     --time_based \
     --runtime=60 \
     --verify=crc32c \
     --verify_fatal=1 \
     --ioengine=libaio"
