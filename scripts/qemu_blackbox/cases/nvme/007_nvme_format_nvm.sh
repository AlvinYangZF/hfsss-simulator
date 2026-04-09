#!/bin/bash
# Verifies that NVMe Format NVM is handled correctly and that
# previously written data is erased after a format operation.
#
# This is a destructive admin command test. It exercises:
#   - the NVMe Format NVM admin command path
#   - device accessibility after format
#   - data erasure semantics (reads return zeros post-format)
#
# Verification: write random data, format, read back and confirm the
# region now matches the well-known MD5 of 64 KiB of zeros.
set -euo pipefail

CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../../lib/case.sh
. "$CASE_DIR/../../lib/case.sh"

hfsss_case_require_guest_tool dd nvme md5sum

# Known MD5 of 64 KiB of zero bytes
ZERO_64K_MD5="fcd6bcb56c1689fcef28b57c22475bad"

# 1. Write random data to the first 64 KiB (16 x 4 KiB blocks)
hfsss_case_guest_run "prep-write-random" \
    "dd if=/dev/urandom of=$HFSSS_GUEST_NVME_DEV bs=4096 count=16 oflag=direct conv=fsync"

# 2. Read back and checksum -- must NOT be all-zero (proves write landed)
hfsss_case_guest_run "md5-pre-format" \
    "dd if=$HFSSS_GUEST_NVME_DEV bs=4096 count=16 iflag=direct status=none | md5sum | awk '{print \$1}'"

# 3. Format namespace 1
hfsss_case_run_nvme "nvme-format" "format $HFSSS_GUEST_NVME_CTRL --namespace-id=1 --ses=1"

# 4. Verify device is still accessible after format
hfsss_case_guest_run "post-format-read" \
    "dd if=$HFSSS_GUEST_NVME_DEV bs=4096 count=16 iflag=direct status=none | md5sum | awk '{print \$1}'"

# Assertions

# dd write must have succeeded
hfsss_case_assert_file_contains \
    "$HFSSS_CASE_ARTIFACT_DIR/prep-write-random.stderr" "records out"

# Pre-format MD5 must NOT match all-zero hash (confirms write path)
if grep -Eq "^${ZERO_64K_MD5}$" "$HFSSS_CASE_ARTIFACT_DIR/md5-pre-format.stdout"; then
    hfsss_case_log "ASSERT FAIL: pre-format read matches all-zero MD5 -- write path may be broken"
    exit 1
fi

# Post-format MD5 must match all-zero hash (confirms format erased data)
hfsss_case_assert_file_contains \
    "$HFSSS_CASE_ARTIFACT_DIR/post-format-read.stdout" "^${ZERO_64K_MD5}$"
