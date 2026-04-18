#!/usr/bin/env python3
"""Unit tests for analyze_trace.py."""
import struct
import tempfile
import unittest
from pathlib import Path

from analyze_trace import (
    RECORD_FMT, RECORD_SIZE, read_records, build_chains,
    first_corrupt_hop, first_corrupt_hop_write, first_corrupt_hop_read,
    analyze,
)

OP_READ, OP_WRITE = 0, 1
T1, T2, T3, T4, T5 = 1, 2, 3, 4, 5


def pack(tsc, lba, ppn, pid, op, crc, extra, tid):
    return struct.pack(RECORD_FMT, tsc, lba, ppn, pid, op, crc, extra, tid, 0)


def mk_rec(tsc, lba, ppn, pid, op, crc=0, extra=0, tid=1):
    return {"tsc": tsc, "lba": lba, "ppn": ppn, "point_id": pid,
            "op": op, "crc": crc, "extra": extra, "tid": tid}


class TestReadRecords(unittest.TestCase):
    def test_record_size(self):
        self.assertEqual(RECORD_SIZE, 48)

    def test_read_one(self):
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(pack(1, 100, 0, T1, OP_WRITE, 0xdead, 0, 1))
            f.write(pack(2, 100, 0x200, T2, OP_WRITE, 0, 0, 1))
            path = f.name
        recs = read_records(Path(path))
        self.assertEqual(len(recs), 2)
        self.assertEqual(recs[0]["point_id"], T1)
        self.assertEqual(recs[1]["point_id"], T2)


class TestWriteChain(unittest.TestCase):
    def _clean_write(self):
        return [
            mk_rec(1, 42, 0, T1, OP_WRITE, crc=0xAAAA),
            mk_rec(2, 42, 0, T2, OP_WRITE),
            mk_rec(3, 42, 0x100, T3, OP_WRITE),
            mk_rec(4, 42, 0x100, T4, OP_WRITE, crc=0xAAAA),
            mk_rec(5, 0, 0x100, T5, OP_WRITE, crc=0xAAAA),
        ]

    def test_clean_write_chain(self):
        chains = build_chains(self._clean_write())
        self.assertEqual(len(chains), 1)
        self.assertIsNone(first_corrupt_hop(chains[0]))

    def test_t1_t4_mismatch(self):
        recs = self._clean_write()
        recs[3]["crc"] = 0xBBBB  # T4 drift
        chains = build_chains(recs)
        self.assertEqual(first_corrupt_hop_write(chains[0]), "T1->T4")

    def test_t4_t5_mismatch(self):
        recs = self._clean_write()
        recs[4]["crc"] = 0xCCCC  # T5 drift
        chains = build_chains(recs)
        self.assertEqual(first_corrupt_hop_write(chains[0]), "T4->T5")


class TestReadChain(unittest.TestCase):
    def test_clean_read_matches_prior_write(self):
        # A write landed bytes with crc 0xAAAA at ppn 0x100. A later read
        # returns the same bytes.
        write_chain = [
            mk_rec(1, 42, 0, T1, OP_WRITE, crc=0xAAAA),
            mk_rec(4, 42, 0x100, T4, OP_WRITE, crc=0xAAAA),
            mk_rec(5, 0, 0x100, T5, OP_WRITE, crc=0xAAAA),
        ]
        read_chain = [
            mk_rec(10, 42, 0, T1, OP_READ),
            mk_rec(11, 42, 0x100, T3, OP_READ),
            mk_rec(12, 42, 0x100, T4, OP_READ),
            mk_rec(13, 0, 0x100, T5, OP_READ, crc=0xAAAA),
        ]
        hops = analyze(build_chains(write_chain + read_chain))
        self.assertEqual(hops, {})

    def test_corrupt_read_mismatches_prior_write(self):
        write_chain = [
            mk_rec(1, 42, 0, T1, OP_WRITE, crc=0xAAAA),
            mk_rec(4, 42, 0x100, T4, OP_WRITE, crc=0xAAAA),
            mk_rec(5, 0, 0x100, T5, OP_WRITE, crc=0xAAAA),
        ]
        # Read returns bytes with a different crc than the last write.
        read_chain = [
            mk_rec(10, 42, 0, T1, OP_READ),
            mk_rec(12, 42, 0x100, T4, OP_READ),
            mk_rec(13, 0, 0x100, T5, OP_READ, crc=0xBEEF),
        ]
        hops = analyze(build_chains(write_chain + read_chain))
        self.assertEqual(hops.get("readback-vs-last-write"), 1)

    def test_read_without_prior_write_is_skipped(self):
        # Only a read chain in the trace — no known-good expectation.
        read_chain = [
            mk_rec(10, 42, 0, T1, OP_READ),
            mk_rec(12, 42, 0x100, T4, OP_READ),
            mk_rec(13, 0, 0x100, T5, OP_READ, crc=0xBEEF),
        ]
        hops = analyze(build_chains(read_chain))
        self.assertEqual(hops, {})

    def test_reads_use_most_recent_write(self):
        # Write, overwrite, then read — read must match the overwrite crc.
        trace = [
            mk_rec(1, 42, 0, T1, OP_WRITE, crc=0xAAAA),
            mk_rec(2, 42, 0x100, T4, OP_WRITE, crc=0xAAAA),
            mk_rec(3, 0, 0x100, T5, OP_WRITE, crc=0xAAAA),
            # Overwrite to a different PPN
            mk_rec(10, 42, 0, T1, OP_WRITE, crc=0xBBBB),
            mk_rec(11, 42, 0x200, T4, OP_WRITE, crc=0xBBBB),
            mk_rec(12, 0, 0x200, T5, OP_WRITE, crc=0xBBBB),
            # Read now: should return 0xBBBB (most recent write), not 0xAAAA
            mk_rec(20, 42, 0, T1, OP_READ),
            mk_rec(21, 42, 0x200, T4, OP_READ),
            mk_rec(22, 0, 0x200, T5, OP_READ, crc=0xBBBB),
        ]
        hops = analyze(build_chains(trace))
        self.assertEqual(hops, {})

    def test_reads_after_overwrite_return_stale_is_flagged(self):
        trace = [
            mk_rec(1, 42, 0, T1, OP_WRITE, crc=0xAAAA),
            mk_rec(2, 42, 0x100, T4, OP_WRITE, crc=0xAAAA),
            mk_rec(3, 0, 0x100, T5, OP_WRITE, crc=0xAAAA),
            mk_rec(10, 42, 0, T1, OP_WRITE, crc=0xBBBB),
            mk_rec(11, 42, 0x200, T4, OP_WRITE, crc=0xBBBB),
            mk_rec(12, 0, 0x200, T5, OP_WRITE, crc=0xBBBB),
            # Read returns stale bytes (0xAAAA) — corrupt
            mk_rec(20, 42, 0, T1, OP_READ),
            mk_rec(21, 42, 0x200, T4, OP_READ),
            mk_rec(22, 0, 0x200, T5, OP_READ, crc=0xAAAA),
        ]
        hops = analyze(build_chains(trace))
        self.assertEqual(hops.get("readback-vs-last-write"), 1)


class TestAnalyzeCombined(unittest.TestCase):
    def test_mix_of_clean_and_corrupt(self):
        # One clean write, one corrupt write (T1/T4), one clean read, one
        # corrupt read. Expected: 2 hops counted.
        trace = [
            # Clean write to lba=1
            mk_rec(1, 1, 0, T1, OP_WRITE, crc=0x1111),
            mk_rec(2, 1, 0x10, T4, OP_WRITE, crc=0x1111),
            mk_rec(3, 0, 0x10, T5, OP_WRITE, crc=0x1111),
            # Corrupt write to lba=2 (T1/T4 mismatch)
            mk_rec(4, 2, 0, T1, OP_WRITE, crc=0x2222),
            mk_rec(5, 2, 0x20, T4, OP_WRITE, crc=0x9999),
            mk_rec(6, 0, 0x20, T5, OP_WRITE, crc=0x9999),
            # Clean read of lba=1
            mk_rec(10, 1, 0, T1, OP_READ),
            mk_rec(11, 1, 0x10, T4, OP_READ),
            mk_rec(12, 0, 0x10, T5, OP_READ, crc=0x1111),
            # Corrupt read of lba=2 (reading back different crc than T4)
            mk_rec(20, 2, 0, T1, OP_READ),
            mk_rec(21, 2, 0x20, T4, OP_READ),
            mk_rec(22, 0, 0x20, T5, OP_READ, crc=0x3333),
        ]
        hops = analyze(build_chains(trace))
        self.assertEqual(hops.get("T1->T4"), 1)
        self.assertEqual(hops.get("readback-vs-last-write"), 1)


if __name__ == "__main__":
    unittest.main()
