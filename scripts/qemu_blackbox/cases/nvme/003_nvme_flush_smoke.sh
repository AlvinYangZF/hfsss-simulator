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

hfsss_case_assert_any_file_contains \
    "records out" \
    "$HFSSS_CASE_ARTIFACT_DIR/prep-write.stdout" \
    "$HFSSS_CASE_ARTIFACT_DIR/prep-write.stderr"
hfsss_case_assert_file_contains "$HFSSS_CASE_ARTIFACT_DIR/nvme-flush.stdout" "Flush: success|NVMe Flush: success"
