#!/usr/bin/env python3
"""Summarize fio verify-error sweep artifacts into a markdown report."""

import argparse
import json
import re
from pathlib import Path

VERIFY_RE = re.compile(r"^(verify:|fio: verify)", re.MULTILINE)


def count_verify_errors(stderr_text: str) -> int:
    """Return the number of fio verify-error lines in stderr_text."""
    return len(VERIFY_RE.findall(stderr_text))


def classify_point(err_counts: list) -> str:
    """Classify a sweep point given per-repeat error counts.

    0 non-zero repeats  -> PASS
    1 non-zero repeat   -> FLAKY
    2 non-zero repeats  -> SUSPECT
    3+ non-zero repeats -> FAIL
    """
    nonzero = sum(1 for c in err_counts if c != 0)
    if nonzero == 0:
        return "PASS"
    if nonzero == 1:
        return "FLAKY"
    if nonzero == 2:
        return "SUSPECT"
    return "FAIL"


def summarize_run(stem: Path) -> dict:
    """Read <stem>.stderr and <stem>.json, return a summary dict.

    Returns zero values for missing files or JSON decode errors.
    """
    err_stderr = 0
    err_json = 0
    json_error_code = 0

    stderr_path = stem.parent / (stem.name + ".stderr")
    if stderr_path.exists():
        err_stderr = count_verify_errors(stderr_path.read_text())

    json_path = stem.parent / (stem.name + ".json")
    if json_path.exists():
        try:
            data = json.loads(json_path.read_text())
            jobs = data.get("jobs", [])
            if jobs:
                err_json = jobs[0].get("total_err", 0)
                json_error_code = jobs[0].get("error", 0)
        except (json.JSONDecodeError, KeyError, TypeError):
            pass

    return {
        "stem": str(stem),
        "err_stderr": err_stderr,
        "err_json": err_json,
        "json_error_code": json_error_code,
    }


def build_matrix(artifact_dir: Path, matrix_json: Path) -> str:
    """Build a markdown summary table from a matrix.json and artifact directory."""
    with matrix_json.open() as fh:
        matrix = json.load(fh)

    top_repeats = int(matrix.get("repeats", 3))
    lines = []
    for axis_entry in matrix.get("axes", []):
        axis_name = axis_entry["name"]
        points = axis_entry.get("points", [])
        repeats = int(axis_entry.get("repeats", top_repeats))
        lines.append(f"## {axis_name}")
        lines.append("")
        lines.append("| point | rep0 | rep1 | rep2 | status |")
        lines.append("|-------|------|------|------|--------|")
        for point in points:
            counts = []
            for rep in range(repeats):
                stem_name = f"{axis_name}_{point}_rep{rep}"
                result = summarize_run(artifact_dir / stem_name)
                counts.append(result["err_stderr"])
            status = classify_point(counts)
            row = f"| {point} | " + " | ".join(str(c) for c in counts) + f" | {status} |"
            lines.append(row)
        lines.append("")

    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description="Summarize sweep artifacts.")
    parser.add_argument("--artifact-dir", required=True, type=Path,
                        help="Directory containing <stem>.stderr and <stem>.json files.")
    parser.add_argument("--matrix", required=True, type=Path,
                        help="Path to matrix.json describing the sweep axes.")
    parser.add_argument("--out", required=True, type=Path,
                        help="Output markdown file path.")
    args = parser.parse_args()

    report = build_matrix(args.artifact_dir, args.matrix)
    args.out.write_text(report)
    print(f"Report written to {args.out}")


if __name__ == "__main__":
    main()
