#!/bin/bash

set -euo pipefail

CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../../lib/case.sh
. "$CASE_DIR/../../lib/case.sh"

hfsss_case_run_fio_json "fio-pr-smoke-randwrite-verify" \
    "--name=pr_smoke_randwrite_verify \
     --filename=$HFSSS_GUEST_NVME_DEV \
     --rw=randwrite --bs=4k --direct=1 \
     --ioengine=libaio --iodepth=16 \
     --size=1G --io_size=1G \
     --verify=crc32c --verify_fatal=1 --verify_async=2 --do_verify=1 \
     --randrepeat=0"
