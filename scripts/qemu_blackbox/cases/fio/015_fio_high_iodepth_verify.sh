#!/bin/bash
# High queue-depth fio stress test with data verification.
#
# Pushes queue and completion behavior harder than the smoke cases by
# using elevated iodepth (64) with mixed random read/write and CRC32C
# verification.  This exercises:
#   - queue-depth pressure on the NVMe command processing path
#   - concurrent read/write completion handling
#   - data integrity under high concurrency
#
# Target: nightly CI (too heavy for PR smoke gate)
set -euo pipefail

CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../../lib/case.sh
. "$CASE_DIR/../../lib/case.sh"

hfsss_case_run_fio_json "high-iodepth" \
    "--name=high-iodepth-randrw \
     --filename=$HFSSS_GUEST_NVME_DEV \
     --rw=randrw \
     --rwmixread=70 \
     --bs=4k \
     --size=512m \
     --io_size=2g \
     --iodepth=64 \
     --numjobs=1 \
     --direct=1 \
     --verify=crc32c \
     --verify_fatal=1 \
     --ioengine=libaio \
     --norandommap"
