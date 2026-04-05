#!/bin/bash
# Verifies that reading a trimmed LBA range returns all zeros per the
# dlfeat=1 semantics advertised by HFSSS's NVMe namespace.
#
# This is a behavioral counterpart to the config-line check (QEMU -drive
# must have discard=unmap). It catches regressions in:
#   - the QEMU NBD drive's discard passthrough
#   - the NBD server's NBD_CMD_TRIM handler
#   - the simulator's DSM deallocate / FTL unmap path
#   - the READ path returning zeros for unmapped LBAs
#
# Verification uses MD5 of the 64 KiB region since the MD5 of 64 KiB of
# zeros is a well-known constant: fcd6bcb56c1689fcef28b57c22475bad.
set -euo pipefail

CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../../lib/case.sh
. "$CASE_DIR/../../lib/case.sh"

hfsss_case_require_guest_tool dd blkdiscard md5sum

# Known md5 of 64 KiB of zero bytes (head -c 65536 /dev/zero | md5sum)
ZERO_64K_MD5="fcd6bcb56c1689fcef28b57c22475bad"
REGION_BYTES=65536  # 64 KiB = 16 * 4 KiB blocks

# 1. Write random data to the first 64 KiB (16 × 4 KiB blocks at offset 0)
hfsss_case_guest_run "prep-write-random" \
    "dd if=/dev/urandom of=$HFSSS_GUEST_NVME_DEV bs=4096 count=16 oflag=direct conv=fsync"

# 2. Read back, checksum — must NOT be the all-zero MD5 (proves write path)
hfsss_case_guest_run "md5-pre-trim" \
    "dd if=$HFSSS_GUEST_NVME_DEV bs=4096 count=16 iflag=direct status=none | md5sum | awk '{print \$1}'"

# 3. Trim the same 64 KiB region
hfsss_case_guest_run "blkdiscard-region" \
    "blkdiscard -o 0 -l $REGION_BYTES $HFSSS_GUEST_NVME_DEV"

# 4. Read back again, checksum — MUST equal the all-zero MD5 (proves trim path)
hfsss_case_guest_run "md5-post-trim" \
    "dd if=$HFSSS_GUEST_NVME_DEV bs=4096 count=16 iflag=direct status=none | md5sum | awk '{print \$1}'"

# Assertions

# prep-write-random should emit "records out" (dd success marker).
# dd writes its "N+0 records in / N+0 records out" summary to stderr,
# not stdout — matches the fix in 003_nvme_flush_smoke.sh for the same
# class of assertion. The capture helper splits the two streams into
# .stdout and .stderr files, so "records out" lives in .stderr.
hfsss_case_assert_file_contains "$HFSSS_CASE_ARTIFACT_DIR/prep-write-random.stderr" "records out"

# Pre-trim MD5 must NOT match the all-zero hash — confirms write actually
# landed (without this, the post-trim zero check could trivially pass because
# reads were already returning zeros for some unrelated reason).
if grep -Eq "^${ZERO_64K_MD5}$" "$HFSSS_CASE_ARTIFACT_DIR/md5-pre-trim.stdout"; then
    hfsss_case_log "ASSERT FAIL: pre-trim read matches all-zero MD5 — write path may be broken"
    exit 1
fi

# Post-trim MD5 must exactly match the all-zero hash
hfsss_case_assert_file_contains "$HFSSS_CASE_ARTIFACT_DIR/md5-post-trim.stdout" "^${ZERO_64K_MD5}$"
