#!/bin/bash
# Pre-check-in mixed read/write stress with end-to-end data verification.
#
# This is the heaviest fio case in the blackbox suite and the canonical
# gate case for the `make pre-checkin` standard. Any change that claims
# to preserve I/O correctness must pass this. Change parameters only
# with owner sign-off — they are calibrated for the pre-checkin time
# budget.
#
# Profile:
#   - Random 70/30 read/write mix
#   - 4 KiB block size at iodepth=64, single job (clean verify semantics)
#   - 1 GiB working set, 8 GiB cumulative I/O (~8x working-set churn)
#   - CRC32C block-level verification, fatal on mismatch
#   - direct=1, libaio — bypasses guest page cache, exercises NBD path
#
# Concurrency choice: numjobs=1 on purpose. With numjobs>1 and verify=1,
# fio emits a "multiple writers may overwrite blocks that belong to
# other jobs" warning and can abort the verify phase. Per-job concurrency
# comes from iodepth=64 and the NBD server's async (-a) mode instead.
#
# Space requirements: minimum 1 GiB of namespace. The pre-checkin
# Makefile target sets --size-mb 2048 for headroom.
#
# Pass criteria:
#   - fio exits 0
#   - no "verify:" error lines in output
#   - 8 GiB cumulative I/O completes without underrun
set -euo pipefail

CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../../lib/case.sh
. "$CASE_DIR/../../lib/case.sh"

hfsss_case_run_fio_json "fio-pre-checkin-stress" \
    "--name=pre_checkin_stress \
     --filename=$HFSSS_GUEST_NVME_DEV \
     --rw=randrw --rwmixread=70 --bs=4k --direct=1 \
     --ioengine=libaio --iodepth=64 --numjobs=1 \
     --size=1G --io_size=8G \
     --verify=crc32c --verify_fatal=1 --verify_async=4 --do_verify=1 \
     --randrepeat=0"
