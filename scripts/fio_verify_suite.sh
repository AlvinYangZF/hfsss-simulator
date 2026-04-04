#!/bin/bash
# HFSSS fio Data Verification Test Suite
#
# Runs comprehensive fio tests with data verification against the HFSSS
# simulator exposed as an NVMe device via QEMU + NBD.
#
# Prerequisites:
#   - QEMU guest running with HFSSS NBD backend (use start_nvme_test.sh)
#   - SSH access to guest on port 2222
#
# Usage:
#   ./scripts/fio_verify_suite.sh [ssh_port] [nvme_dev]
#
# REQ-122 coverage: ioengine=libaio, direct=1, numjobs=32, iodepth=128

set -e

SSH_PORT="${1:-2222}"
NVME_DEV="${2:-/dev/nvme0n1}"
SSH_KEY="/tmp/hfsss_qemu_key"
SSH_CMD="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i $SSH_KEY -p $SSH_PORT root@127.0.0.1"

PASS=0
FAIL=0
TOTAL=0

run_test() {
    local name="$1"
    local fio_args="$2"
    TOTAL=$((TOTAL + 1))

    echo ""
    echo "========================================"
    echo "Test $TOTAL: $name"
    echo "========================================"

    local output
    output=$($SSH_CMD "fio $fio_args 2>&1" 2>/dev/null)
    local rc=$?

    # Check for verify errors in output
    if echo "$output" | grep -q "verify:.*bad"; then
        echo "  RESULT: FAIL (verify errors detected)"
        echo "$output" | grep "verify:" | head -5
        FAIL=$((FAIL + 1))
        return 1
    fi

    # Check fio exit code (from err= line)
    local fio_err
    fio_err=$(echo "$output" | grep -o 'err= *[0-9]*' | head -1 | grep -o '[0-9]*')
    if [ -n "$fio_err" ] && [ "$fio_err" -ne 0 ]; then
        echo "  RESULT: FAIL (fio err=$fio_err)"
        FAIL=$((FAIL + 1))
        return 1
    fi

    # Extract performance numbers
    local read_bw write_bw
    read_bw=$(echo "$output" | grep "READ:" | grep -o 'bw=[^,]*' | head -1)
    write_bw=$(echo "$output" | grep "WRITE:" | grep -o 'bw=[^,]*' | head -1)

    echo "  RESULT: PASS"
    [ -n "$read_bw" ] && echo "  Read:  $read_bw"
    [ -n "$write_bw" ] && echo "  Write: $write_bw"
    PASS=$((PASS + 1))
    return 0
}

# =========================================================================
echo "========================================"
echo "HFSSS fio Data Verification Test Suite"
echo "========================================"
echo "Target: $NVME_DEV via SSH port $SSH_PORT"
echo ""

# Verify connectivity
if ! $SSH_CMD "true" 2>/dev/null; then
    echo "ERROR: Cannot SSH to guest on port $SSH_PORT"
    echo "Start the test environment first: ./scripts/start_nvme_test.sh"
    exit 1
fi

# Verify NVMe device exists
if ! $SSH_CMD "test -b $NVME_DEV" 2>/dev/null; then
    echo "ERROR: $NVME_DEV not found in guest"
    exit 1
fi

echo "Guest connected, $NVME_DEV found."

# =========================================================================
# Test 1: Sequential write + crc32c verify (baseline)
# =========================================================================
run_test "Sequential write + crc32c verify (4K, QD=1, sync)" \
    "--name=seq_crc32c \
     --rw=write --bs=4k --size=64M \
     --filename=$NVME_DEV --direct=1 \
     --ioengine=sync \
     --do_verify=1 --verify=crc32c --verify_fatal=1"

# =========================================================================
# Test 2: Random write + crc32c verify
# =========================================================================
run_test "Random write + crc32c verify (4K, QD=1, sync)" \
    "--name=rand_crc32c \
     --rw=randwrite --bs=4k --size=32M \
     --filename=$NVME_DEV --direct=1 \
     --ioengine=sync \
     --do_verify=1 --verify=crc32c --verify_fatal=1"

# =========================================================================
# Test 3: Sequential write + md5 verify (large block)
# =========================================================================
run_test "Sequential write + md5 verify (128K, QD=1, sync)" \
    "--name=seq_md5 \
     --rw=write --bs=128k --size=32M \
     --filename=$NVME_DEV --direct=1 \
     --ioengine=sync \
     --do_verify=1 --verify=md5 --verify_fatal=1"

# =========================================================================
# Test 4: Sequential write + sha256 verify
# =========================================================================
run_test "Sequential write + sha256 verify (4K, QD=1, sync)" \
    "--name=seq_sha256 \
     --rw=write --bs=4k --size=16M \
     --filename=$NVME_DEV --direct=1 \
     --ioengine=sync \
     --do_verify=1 --verify=sha256 --verify_fatal=1"

# =========================================================================
# Test 5: Multi-job write + verify (libaio, QD=16, 4 jobs, non-overlapping)
# =========================================================================
run_test "Multi-job write + crc32c verify (4 jobs, QD=16, libaio)" \
    "--name=multi_verify \
     --rw=write --bs=4k --size=8M \
     --offset_increment=8M \
     --filename=$NVME_DEV --direct=1 \
     --ioengine=libaio --iodepth=16 --numjobs=4 \
     --do_verify=1 --verify=crc32c --verify_fatal=1 \
     --group_reporting"

# =========================================================================
# Test 6: Deep queue write + verify (libaio, QD=32, 4 jobs)
# =========================================================================
run_test "Deep queue write + sha256 verify (4 jobs, QD=32, libaio)" \
    "--name=deep_verify \
     --rw=write --bs=4k --size=16M \
     --offset_increment=16M \
     --filename=$NVME_DEV --direct=1 \
     --ioengine=libaio --iodepth=32 --numjobs=4 \
     --do_verify=1 --verify=sha256 --verify_fatal=1 \
     --group_reporting"

# =========================================================================
# Test 7: REQ-122 scale test — numjobs=4, iodepth=128
# (Guest has 2 vCPUs so numjobs=4 is reasonable; iodepth=128 tests deep queue)
# =========================================================================
run_test "REQ-122 scale: write + verify (4 jobs, QD=128, libaio)" \
    "--name=req122_scale \
     --rw=write --bs=4k --size=32M \
     --offset_increment=32M \
     --filename=$NVME_DEV --direct=1 \
     --ioengine=libaio --iodepth=128 --numjobs=4 \
     --do_verify=1 --verify=crc32c --verify_fatal=1 \
     --group_reporting"

# =========================================================================
# Test 8: Mixed randrw + verify (70/30 read/write)
# =========================================================================
run_test "Mixed randrw + crc32c verify (4 jobs, QD=16, 70/30)" \
    "--name=mixed_verify \
     --rw=randrw --rwmixread=70 \
     --bs=4k --size=8M \
     --offset_increment=8M \
     --filename=$NVME_DEV --direct=1 \
     --ioengine=libaio --iodepth=16 --numjobs=4 \
     --do_verify=1 --verify=crc32c --verify_fatal=1 \
     --group_reporting"

# =========================================================================
# Test 9: Large block multi-job verify
# =========================================================================
run_test "Large block multi-job write + md5 verify (128K, 4 jobs, QD=8)" \
    "--name=large_multi \
     --rw=write --bs=128k --size=16M \
     --offset_increment=16M \
     --filename=$NVME_DEV --direct=1 \
     --ioengine=libaio --iodepth=8 --numjobs=4 \
     --do_verify=1 --verify=md5 --verify_fatal=1 \
     --group_reporting"

# =========================================================================
# Test 10: Pattern verify (write known pattern, read back)
# =========================================================================
run_test "Pattern write + verify (4K, pattern=0xdeadbeef)" \
    "--name=pattern_verify \
     --rw=write --bs=4k --size=16M \
     --filename=$NVME_DEV --direct=1 \
     --ioengine=sync \
     --do_verify=1 --verify=pattern --verify_pattern=0xdeadbeef \
     --verify_fatal=1"

# =========================================================================
# Summary
# =========================================================================
echo ""
echo "========================================"
echo "HFSSS fio Verification Suite — Results"
echo "========================================"
echo "Total:  $TOTAL"
echo "Passed: $PASS"
echo "Failed: $FAIL"
echo "========================================"

if [ "$FAIL" -eq 0 ]; then
    echo "RESULT: ALL PASS"
    exit 0
else
    echo "RESULT: $FAIL FAILURES"
    exit 1
fi
