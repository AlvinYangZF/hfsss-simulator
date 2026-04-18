#!/usr/bin/env python3
"""Summarize fio sweep artifacts into a markdown report.

A run is considered FAILED when any of the following is true:
  - one or more `verify:` / `fio: verify` lines appear in stderr
  - one or more `io_u error` lines appear in stderr
  - the JSON artifact is missing, empty, or fails to parse
  - the captured fio exit code (`<stem>.exit`) is non-zero
  - the JSON reports a non-zero `jobs[0].error` or `jobs[0].total_err`

Silent PASS from a missing / broken artifact is explicitly disallowed.
"""

import argparse
import json
import re
from pathlib import Path

VERIFY_RE = re.compile(r"^(verify:|fio: verify)", re.MULTILINE)
IO_U_ERR_RE = re.compile(r"^fio: io_u error", re.MULTILINE)


def count_verify_errors(stderr_text: str) -> int:
    """Return the number of fio verify-error lines in stderr_text."""
    return len(VERIFY_RE.findall(stderr_text))


def count_io_u_errors(stderr_text: str) -> int:
    """Return the number of `fio: io_u error` lines in stderr_text."""
    return len(IO_U_ERR_RE.findall(stderr_text))


def _is_run_failing(result: dict) -> bool:
    """A run fails if any error signal is non-zero or any artifact is missing.

    Missing `.exit` is itself a failure: the driver is contracted to write
    this file for every run, so its absence means we cannot distinguish
    "clean zero-exit" from "driver skipped / crashed mid-run". Silent PASS
    on missing artifacts is explicitly disallowed.
    """
    missing = result.get("artifacts_missing", [])
    if missing:
        return True
    if result.get("err_stderr", 0) != 0:
        return True
    if result.get("io_u_errors", 0) != 0:
        return True
    if not result.get("json_valid", False):
        return True
    fio_exit = result.get("fio_exit", None)
    # None means the .exit file was not captured at all — a missing-
    # artifact failure. A captured non-zero fio exit is likewise a failure.
    if fio_exit is None or fio_exit != 0:
        return True
    if result.get("json_error_code", 0) != 0:
        return True
    if result.get("err_json", 0) != 0:
        return True
    return False


def classify_point(results: list) -> str:
    """Classify a sweep point given a list of per-repeat `summarize_run` dicts.

    Accepts either dicts (new, preferred) or plain ints (for backward compat
    with stderr-only counts). 0 failing reps -> PASS, 1 -> FLAKY,
    2 -> SUSPECT, 3+ -> FAIL.
    """
    failing = 0
    for r in results:
        if isinstance(r, dict):
            if _is_run_failing(r):
                failing += 1
        else:
            if int(r) != 0:
                failing += 1
    if failing == 0:
        return "PASS"
    if failing == 1:
        return "FLAKY"
    if failing == 2:
        return "SUSPECT"
    return "FAIL"


def summarize_run(stem: Path) -> dict:
    """Read <stem>.stderr, <stem>.json, <stem>.exit, return a summary dict.

    - `err_stderr`: count of `verify:` / `fio: verify` lines in stderr
    - `io_u_errors`: count of `fio: io_u error` lines in stderr
    - `err_json`: JSON `jobs[0].total_err`
    - `json_error_code`: JSON `jobs[0].error` (non-zero = fio flagged error)
    - `fio_exit`: captured exit code of the fio invocation; None if not recorded
    - `json_valid`: True iff the JSON file was present, non-empty, and decoded OK
    - `artifacts_missing`: list of expected artifact kinds missing on disk
    """
    err_stderr = 0
    io_u_errors = 0
    err_json = 0
    json_error_code = 0
    json_valid = False
    fio_exit = None
    artifacts_missing = []

    stderr_path = stem.parent / (stem.name + ".stderr")
    if stderr_path.exists():
        text = stderr_path.read_text(errors="replace")
        err_stderr = count_verify_errors(text)
        io_u_errors = count_io_u_errors(text)
    else:
        artifacts_missing.append("stderr")

    json_path = stem.parent / (stem.name + ".json")
    if json_path.exists() and json_path.stat().st_size > 0:
        try:
            data = json.loads(json_path.read_text())
            jobs = data.get("jobs", [])
            if jobs:
                err_json = int(jobs[0].get("total_err", 0) or 0)
                json_error_code = int(jobs[0].get("error", 0) or 0)
            json_valid = True
        except (json.JSONDecodeError, KeyError, TypeError, ValueError):
            json_valid = False
    else:
        artifacts_missing.append("json")

    exit_path = stem.parent / (stem.name + ".exit")
    if exit_path.exists():
        try:
            fio_exit = int(exit_path.read_text().strip() or "0")
        except ValueError:
            fio_exit = None
            # Present but unreadable — record as an explicit missing-ish signal.
            artifacts_missing.append("exit")
    else:
        artifacts_missing.append("exit")

    return {
        "stem": str(stem),
        "err_stderr": err_stderr,
        "io_u_errors": io_u_errors,
        "err_json": err_json,
        "json_error_code": json_error_code,
        "json_valid": json_valid,
        "fio_exit": fio_exit,
        "artifacts_missing": artifacts_missing,
    }


def _format_cell(result: dict) -> str:
    """Render a per-repeat cell as `<verify>v/<iou>i` (plus markers)."""
    v = result["err_stderr"]
    iou = result["io_u_errors"]
    markers = []
    missing = result.get("artifacts_missing", [])
    for m in missing:
        markers.append(f"!{m}")
    if not result["json_valid"]:
        markers.append("!json") if "json" not in missing else None
    exit_code = result.get("fio_exit", None)
    if exit_code is not None and exit_code != 0:
        markers.append(f"exit={exit_code}")
    if result["err_json"]:
        markers.append(f"je={result['err_json']}")
    core = f"{v}v/{iou}i"
    if markers:
        return core + " " + " ".join(m for m in markers if m is not None)
    return core


def build_matrix(artifact_dir: Path, matrix_json: Path) -> str:
    """Build a markdown summary table from a matrix.json and artifact directory."""
    with matrix_json.open() as fh:
        matrix = json.load(fh)

    top_repeats = int(matrix.get("repeats", 3))
    lines = [
        "# Stage W Sweep Matrix",
        "",
        "Cell format: `<verify>v/<iou>i`. Additional markers: `!json` (missing/broken JSON), "
        "`exit=N` (fio non-zero exit), `je=N` (JSON total_err > 0). Any marker or non-zero "
        "counter makes the repeat a failure for status classification.",
        "",
    ]
    for axis_entry in matrix.get("axes", []):
        axis_name = axis_entry["name"]
        points = axis_entry.get("points", [])
        repeats = int(axis_entry.get("repeats", top_repeats))
        lines.append(f"## {axis_name}")
        lines.append("")
        rep_headers = " | ".join(f"rep{i+1}" for i in range(repeats))
        lines.append("| point | " + rep_headers + " | status |")
        lines.append("|-------|" + "------|" * repeats + "--------|")
        for point in points:
            results = []
            for rep in range(1, repeats + 1):
                stem_name = f"{axis_name}_{point}_rep{rep}"
                results.append(summarize_run(artifact_dir / stem_name))
            status = classify_point(results)
            cells = [_format_cell(r) for r in results]
            row = f"| {point} | " + " | ".join(cells) + f" | {status} |"
            lines.append(row)
        lines.append("")

    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description="Summarize sweep artifacts.")
    parser.add_argument("--artifact-dir", required=True, type=Path,
                        help="Directory containing <stem>.stderr/.json/.exit files.")
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
