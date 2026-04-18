#!/usr/bin/env python3
"""Analyze a trace.bin dump produced by HFSSS_DEBUG_TRACE and identify
the first corrupt hop per IO chain.

Binary record format (little-endian, 48 bytes):
    <Q Q Q I I I I I I>
    tsc, lba, ppn_or_len, point_id, op, crc32c, extra, thread_id, _pad

Per-chain semantics:
    Write chains (grouped by (op=WRITE, lba)):
        T1 sets crc = hash(caller buffer).
        T2, T3 are routing markers (crc = 0).
        T4 re-hashes the buffer just before HAL submission.
        T5 (matched by (op, ppn) to T4) re-hashes after HAL returns.
        FAIL if T1.crc != T4.crc or T4.crc != T5.crc (both non-zero).

    Read chains (grouped by (op=READ, lba)):
        T1..T4 carry crc = 0 (buffer is empty before the read).
        T5 (matched by (op=READ, ppn) to T4) carries the post-HAL
        read-back hash.
        FAIL if T5_read.crc != most-recent T4_write.crc for the same lba
        (cross-op match — the data we read back should equal what the last
        write to this lba put on the media).
"""
import argparse
import struct
from collections import defaultdict
from pathlib import Path
from typing import Dict, List, Optional, Tuple

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

    Chains are grown left-to-right in tsc order:
      - A T1 record for (op, lba) opens a new chain and replaces any
        previously-open chain for that (op, lba). Subsequent T2/T3/T4
        records for the same (op, lba) attach to the most recent open
        chain.
      - A T5 record is attached to the open chain whose T4.ppn matches
        the T5.ppn, choosing the earliest matching chain whose T4.tsc is
        before the T5.tsc. After attachment the chain is considered
        closed.
      - Chains without a T1 are emitted in order of their earliest tsc so
        analyzers can still reason about partial chains.
    """
    records = sorted(recs, key=lambda r: r["tsc"])

    chains: List[List[Dict]] = []
    open_by_op_lba: Dict[Tuple[int, int], List[Dict]] = {}
    closed_pending_t5: List[List[Dict]] = []
    # Index closed chains awaiting a T5 by (op, ppn) for fast lookup.
    closed_by_op_ppn: Dict[Tuple[int, int], List[List[Dict]]] = defaultdict(list)

    def close_and_stash(chain: List[Dict]) -> None:
        chains.append(chain)
        t4 = next((r for r in chain if r["point_id"] == T4), None)
        if t4 is not None:
            closed_by_op_ppn[(chain[0]["op"], t4["ppn"])].append(chain)
            closed_pending_t5.append(chain)

    for r in records:
        if r["point_id"] == T5:
            bucket = closed_by_op_ppn.get((r["op"], r["ppn"]), [])
            chosen = None
            for chain in bucket:
                # Must not already have a T5 and must be time-ordered.
                has_t5 = any(x["point_id"] == T5 for x in chain)
                if has_t5:
                    continue
                t4 = next((x for x in chain if x["point_id"] == T4), None)
                if t4 and t4["tsc"] <= r["tsc"]:
                    chosen = chain
                    break
            if chosen is not None:
                chosen.append(r)
            else:
                # Orphan T5 — emit as its own single-record chain so the
                # analyzer does not silently drop evidence.
                chains.append([r])
            continue

        key = (r["op"], r["lba"])
        if r["point_id"] == T1:
            prev = open_by_op_lba.get(key)
            if prev is not None:
                close_and_stash(prev)
            open_by_op_lba[key] = [r]
            continue

        # T2, T3, T4 attach to currently open chain for (op, lba).
        open_chain = open_by_op_lba.get(key)
        if open_chain is not None:
            open_chain.append(r)
            if r["point_id"] == T4:
                # Chain waits for T5 but is no longer extensible from
                # future T2/T3/T4; close it (stash for T5 lookup), and
                # stop treating it as the "open" one for new records.
                close_and_stash(open_chain)
                del open_by_op_lba[key]
        else:
            # No T1 seen — orphan record. Emit as its own chain so a
            # missing T1 is visible (not lost).
            chains.append([r])

    # Any chains that never saw a T4 (and therefore never got close_and_stash)
    # still need to land in the result.
    for chain in open_by_op_lba.values():
        chains.append(chain)

    # Sort individual chains by point_id first, then tsc, for display.
    for chain in chains:
        chain.sort(key=lambda r: (r["point_id"], r["tsc"]))
    return chains


def _chain_tsc_anchor(chain: List[Dict]) -> int:
    """Smallest tsc in a chain — used for cross-op temporal ordering."""
    return min((r["tsc"] for r in chain), default=0)


def first_corrupt_hop_write(chain: List[Dict]) -> Optional[str]:
    """Classify a write chain's first corrupt hop, or None if clean.

    Checks:
      - T1.crc == T4.crc (data unchanged between NBD entry and HAL submit)
      - T4.crc == T5.crc (HAL returned the bytes we submitted — program
        surface write-through sanity)
    """
    by_pid = {r["point_id"]: r for r in chain if r["point_id"] != T5}
    t5 = next((r for r in chain if r["point_id"] == T5), None)
    t1 = by_pid.get(T1)
    t4 = by_pid.get(T4)
    if t1 and t4 and t1["crc"] != 0 and t4["crc"] != 0:
        if t1["crc"] != t4["crc"]:
            return "T1->T4"
    if t4 and t5 and t4["crc"] != 0 and t5["crc"] != 0:
        if t4["crc"] != t5["crc"]:
            return "T4->T5"
    return None


def first_corrupt_hop_read(chain: List[Dict],
                           last_write_crc_for_lba: Dict[int, int]
                           ) -> Optional[str]:
    """Classify a read chain's first corrupt hop, or None if clean.

    Read chains expect the T5 post-HAL crc to equal the most-recent prior
    write's T4 crc for the same lba. A mismatch here means the media
    returned bytes different from what the last write put there (i.e. the
    integrity bug 014/015 reports).
    """
    if not chain:
        return None
    lba = chain[0]["lba"]
    t5 = next((r for r in chain if r["point_id"] == T5), None)
    if t5 is None or t5["crc"] == 0:
        # No post-HAL evidence — not enough data to judge; skip.
        return None
    expected = last_write_crc_for_lba.get(lba)
    if expected is None:
        # No prior write observed — the read's expected value is not
        # knowable from this trace; cannot classify.
        return None
    if expected != t5["crc"]:
        return "readback-vs-last-write"
    return None


def first_corrupt_hop(chain: List[Dict],
                      last_write_crc_for_lba: Optional[Dict[int, int]] = None
                      ) -> Optional[str]:
    """Unified dispatcher used by tests and by main()."""
    if not chain:
        return None
    op = chain[0]["op"]
    if op == OP_WRITE:
        return first_corrupt_hop_write(chain)
    if op == OP_READ:
        ctx = last_write_crc_for_lba if last_write_crc_for_lba is not None else {}
        return first_corrupt_hop_read(chain, ctx)
    return None


def analyze(chains: List[List[Dict]]) -> Dict[str, int]:
    """Walk chains in temporal order; maintain per-lba last-write-crc.

    Returns a {hop_label: count} dict. Hop labels are the ones produced by
    first_corrupt_hop_* above.
    """
    # Sort chains by their earliest tsc so the "last write" table is built
    # in correct temporal order before the corresponding reads land.
    chains_sorted = sorted(chains, key=_chain_tsc_anchor)
    last_write_crc: Dict[int, int] = {}
    hops: Dict[str, int] = defaultdict(int)
    for chain in chains_sorted:
        if not chain:
            continue
        op = chain[0]["op"]
        if op == OP_WRITE:
            hop = first_corrupt_hop_write(chain)
            if hop:
                hops[hop] += 1
            # Track the last T4.crc for this lba (the known-good bytes
            # submitted to HAL). We use T4 rather than T5 so a failed-
            # HAL-write does not update the expectation (T5 is skipped or
            # crc=0 on failure).
            t4 = next((r for r in chain if r["point_id"] == T4), None)
            if t4 and t4["crc"] != 0:
                last_write_crc[chain[0]["lba"]] = t4["crc"]
        elif op == OP_READ:
            hop = first_corrupt_hop_read(chain, last_write_crc)
            if hop:
                hops[hop] += 1
    return hops


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    recs = read_records(args.trace)
    chains = build_chains(recs)
    hops = analyze(chains)

    # Split chain counts by op for context in the markdown output.
    total_write_chains = sum(1 for c in chains if c and c[0]["op"] == OP_WRITE)
    total_read_chains = sum(1 for c in chains if c and c[0]["op"] == OP_READ)

    lines = [
        "# Trace Analysis",
        "",
        f"Total records: {len(recs)}",
        f"Total chains:  {len(chains)} "
        f"(write={total_write_chains}, read={total_read_chains})",
        "",
    ]
    if not hops:
        lines.append("No corrupt hops detected.")
    else:
        lines.append("| Hop | Count |")
        lines.append("|---|---|")
        for hop, count in sorted(hops.items(), key=lambda x: -x[1]):
            lines.append(f"| {hop} | {count} |")
    args.out.write_text("\n".join(lines) + "\n")
    print(f"Wrote {args.out}")


if __name__ == "__main__":
    main()
