#!/usr/bin/env python3
"""Unit tests for analyze_trace.py."""
import struct
import tempfile
import unittest
from pathlib import Path

from analyze_trace import (
    RECORD_FMT, RECORD_SIZE, read_records, build_chains,
    first_corrupt_hop,
)

OP_WRITE, OP_READ = 1, 0
T1, T2, T3, T4, T5 = 1, 2, 3, 4, 5


def pack(tsc, lba, ppn, pid, op, crc, extra, tid):
    return struct.pack(RECORD_FMT, tsc, lba, ppn, pid, op, crc, extra, tid, 0)


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


class TestChains(unittest.TestCase):
    def test_clean_chain(self):
        recs = [
            {"tsc": 1, "lba": 42, "ppn": 0, "point_id": T1, "op": OP_WRITE,
             "crc": 0xaaaa, "extra": 0, "tid": 1},
            {"tsc": 2, "lba": 42, "ppn": 0, "point_id": T2, "op": OP_WRITE,
             "crc": 0, "extra": 0, "tid": 1},
            {"tsc": 3, "lba": 42, "ppn": 0x100, "point_id": T3, "op": OP_WRITE,
             "crc": 0, "extra": 0, "tid": 1},
            {"tsc": 4, "lba": 42, "ppn": 0x100, "point_id": T4, "op": OP_WRITE,
             "crc": 0xaaaa, "extra": 0, "tid": 1},
            {"tsc": 5, "lba": 0, "ppn": 0x100, "point_id": T5, "op": OP_WRITE,
             "crc": 0, "extra": 0, "tid": 1},
        ]
        chains = build_chains(recs)
        self.assertEqual(len(chains), 1)
        self.assertIsNone(first_corrupt_hop(chains[0]))

    def test_t1_t4_mismatch(self):
        recs = [
            {"tsc": 1, "lba": 42, "ppn": 0, "point_id": T1, "op": OP_WRITE,
             "crc": 0xaaaa, "extra": 0, "tid": 1},
            {"tsc": 4, "lba": 42, "ppn": 0x100, "point_id": T4, "op": OP_WRITE,
             "crc": 0xbbbb, "extra": 0, "tid": 1},
        ]
        chains = build_chains(recs)
        hop = first_corrupt_hop(chains[0])
        self.assertEqual(hop, "T1->T4")


if __name__ == "__main__":
    unittest.main()
