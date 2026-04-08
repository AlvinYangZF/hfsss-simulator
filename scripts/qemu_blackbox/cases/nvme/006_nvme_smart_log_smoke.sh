#!/bin/bash
# Smoke test for NVMe SMART/Health log page (LID=2).
#
# Writes some data first to generate host_write_commands, then reads
# the SMART log and verifies it contains expected fields.
set -euo pipefail

CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../../lib/case.sh
. "$CASE_DIR/../../lib/case.sh"

hfsss_case_require_guest_tool nvme dd

# 1. Write some data to generate write statistics
hfsss_case_guest_run "prep-write" \
    "dd if=/dev/urandom of=$HFSSS_GUEST_NVME_DEV bs=4096 count=32 oflag=direct conv=fsync"

# 2. Read SMART/Health log
hfsss_case_run_nvme "smart-log" \
    "smart-log $HFSSS_GUEST_NVME_DEV --output-format=normal"

# 3. Read Error log (another log page)
hfsss_case_run_nvme "error-log" \
    "error-log $HFSSS_GUEST_NVME_DEV"

# Assertions

# SMART log must contain temperature field
hfsss_case_assert_any_file_contains \
    "temperature|Temperature" \
    "$HFSSS_CASE_ARTIFACT_DIR/smart-log.stdout" \
    "$HFSSS_CASE_ARTIFACT_DIR/smart-log.stderr"

# SMART log must contain data_units or host_writes field
hfsss_case_assert_any_file_contains \
    "data_units|host_write|Data Units|Host Write" \
    "$HFSSS_CASE_ARTIFACT_DIR/smart-log.stdout" \
    "$HFSSS_CASE_ARTIFACT_DIR/smart-log.stderr"

# dd write should succeed
hfsss_case_assert_any_file_contains \
    "records out" \
    "$HFSSS_CASE_ARTIFACT_DIR/prep-write.stdout" \
    "$HFSSS_CASE_ARTIFACT_DIR/prep-write.stderr"
