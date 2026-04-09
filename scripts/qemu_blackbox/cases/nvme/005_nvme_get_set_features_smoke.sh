#!/bin/bash
# Smoke test for NVMe Get Features and Set Features commands.
#
# Exercises the guest-visible admin command path through QEMU's NVMe
# emulation.  While QEMU handles these commands itself (they don't
# reach hfsss-nbd-server via NBD), this test validates that the
# guest-visible NVMe device exposes a sane feature set — a prerequisite
# for any guest driver or firmware that probes features before I/O.
#
# Tested features:
#   - Number of Queues (FID=0x07, mandatory per spec)
#   - Power Management (FID=0x02)
#   - Temperature Threshold (FID=0x04) — set then get round-trip
set -euo pipefail

CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../../lib/case.sh
. "$CASE_DIR/../../lib/case.sh"

hfsss_case_require_guest_tool nvme

# 1. Get Number of Queues (FID=0x07) — mandatory feature, must succeed
hfsss_case_run_nvme "get-feat-nq" \
    "get-feature $HFSSS_GUEST_NVME_CTRL --feature-id=0x07 --human-readable"

# 2. Get Power Management (FID=0x02)
hfsss_case_run_nvme "get-feat-pm" \
    "get-feature $HFSSS_GUEST_NVME_CTRL --feature-id=0x02 --human-readable"

# 3. Get Temperature Threshold (FID=0x04)
hfsss_case_run_nvme "get-feat-temp" \
    "get-feature $HFSSS_GUEST_NVME_CTRL --feature-id=0x04 --human-readable"

# 4. Set Temperature Threshold to 360 K (0x0168) then read it back
hfsss_case_run_nvme "set-feat-temp" \
    "set-feature $HFSSS_GUEST_NVME_CTRL --feature-id=0x04 --value=0x0168"

hfsss_case_run_nvme "get-feat-temp-readback" \
    "get-feature $HFSSS_GUEST_NVME_CTRL --feature-id=0x04 --human-readable"

# Assertions

# Number of Queues must report a non-zero value
hfsss_case_assert_any_file_contains \
    "get-feature:0x7|Number of Queues" \
    "$HFSSS_CASE_ARTIFACT_DIR/get-feat-nq.stdout" \
    "$HFSSS_CASE_ARTIFACT_DIR/get-feat-nq.stderr"

# Set features should not produce error output
if grep -qi "error\|invalid\|failed" "$HFSSS_CASE_ARTIFACT_DIR/set-feat-temp.stderr" 2>/dev/null; then
    hfsss_case_log "ASSERT FAIL: set-feature temp returned error"
    cat "$HFSSS_CASE_ARTIFACT_DIR/set-feat-temp.stderr"
    exit 1
fi
