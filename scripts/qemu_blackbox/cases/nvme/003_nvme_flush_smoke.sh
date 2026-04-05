#!/bin/bash

set -euo pipefail

CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../../lib/case.sh
. "$CASE_DIR/../../lib/case.sh"

hfsss_case_require_guest_tool dd nvme
hfsss_case_guest_run \
    "prep-write" \
    "dd if=/dev/zero of=$HFSSS_GUEST_NVME_DEV bs=4096 count=8 oflag=direct conv=fsync"
hfsss_case_run_nvme "nvme-flush" "flush $HFSSS_GUEST_NVME_CTRL --namespace-id=1"
