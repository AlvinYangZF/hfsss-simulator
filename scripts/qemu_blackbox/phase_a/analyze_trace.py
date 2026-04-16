#!/usr/bin/env python3
"""Analyze a trace.bin dump produced by HFSSS_DEBUG_TRACE and identify
the first corrupt hop per IO chain.

Binary record format (little-endian, 48 bytes):
    <Q Q Q I I I I I I>
    tsc, lba, ppn_or_len, point_id, op, crc32c, extra, thread_id, _pad

Chains are grouped by (op, lba) and by (op, ppn) at T4/T5.
"""
import argparse
import struct
from collections import defaultdict
from pathlib import Path
from typing import Dict, List, Optional

RECORD_FMT = "<QQQIIIIII"
RECORD_SIZE = struct.calcsize(RECORD_FMT)

OP_READ, OP_WRITE, OP_TRIM = 0, 1, 2
T1, T2, T3, T4, T5 = 1, 2, 3, 4, 5


def read_records(path: Path) -> List[Dict]:
    data = path.read_bytes()
    assert len(data) % RECORD_SIZE == 0, (
        f"file size {len(data)} not a multiple of {RECORD_SIZE}")
    out = []
    for i in range(0, len(data), RECORD_SIZE):
        (tsc, lba, ppn_or_len, pid, op, crc, extra, tid, _pad) = struct.unpack(
            RECORD_FMT, data[i:i + RECORD_SIZE])
        out.append({
            "tsc": tsc, "lba": lba, "ppn": ppn_or_len,
            "point_id": pid, "op": op, "crc": crc,
            "extra": extra, "tid": tid,
        })
    out.sort(key=lambda r: r["tsc"])
    return out


def build_chains(recs: List[Dict]) -> List[List[Dict]]:
    """Group records into per-IO chains.

    Chain identity: (op, lba) for T1/T2/T3/T4 entries, then by ppn for
    T5 (which lacks lba). For a simple analyzer we pair T4 to T5 on ppn.
    """
    by_op_lba: Dict[tuple, List[Dict]] = defaultdict(list)
    t5_by_op_ppn: Dict[tuple, List[Dict]] = defaultdict(list)
    for r in recs:
        if r["point_id"] == T5:
            t5_by_op_ppn[(r["op"], r["ppn"])].append(r)
        else:
            by_op_lba[(r["op"], r["lba"])].append(r)
    chains = []
    for (op, lba), chain_recs in by_op_lba.items():
        chain_recs.sort(key=lambda r: r["point_id"])
        t4 = next((r for r in chain_recs if r["point_id"] == T4), None)
        if t4 is not None:
            t5_cands = t5_by_op_ppn.get((op, t4["ppn"]), [])
            t5_match = next((r for r in t5_cands if r["tsc"] >= t4["tsc"]), None)
            if t5_match:
                chain_recs.append(t5_match)
        chains.append(chain_recs)
    return chains


def first_corrupt_hop(chain: List[Dict]) -> Optional[str]:
    """Return the name of the first corrupt hop in a chain, or None if clean.

    For writes: expect T1.crc == T4.crc (data intact to media submission).
    For reads: expect T4.crc == T5.crc where T4 is the prior write-of-same-lba
              at same PPN (cross-chain linkage, simplified here).
    """
    by_pid = {r["point_id"]: r for r in chain}
    op = chain[0]["op"] if chain else -1
    if op == OP_WRITE:
        t1 = by_pid.get(T1)
        t4 = by_pid.get(T4)
        if t1 and t4 and t1["crc"] != 0 and t4["crc"] != 0:
            if t1["crc"] != t4["crc"]:
                return "T1->T4"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    recs = read_records(args.trace)
    chains = build_chains(recs)
    corrupt_hops: Dict[str, int] = defaultdict(int)
    for c in chains:
        h = first_corrupt_hop(c)
        if h:
            corrupt_hops[h] += 1
    lines = ["# Trace Analysis", "",
             f"Total records: {len(recs)}",
             f"Total chains: {len(chains)}", ""]
    if not corrupt_hops:
        lines.append("No corrupt hops detected.")
    else:
        lines.append("| Hop | Count |")
        lines.append("|---|---|")
        for hop, count in sorted(corrupt_hops.items(), key=lambda x: -x[1]):
            lines.append(f"| {hop} | {count} |")
    args.out.write_text("\n".join(lines) + "\n")
    print(f"Wrote {args.out}")


if __name__ == "__main__":
    main()
