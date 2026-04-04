#!/usr/bin/env python3
"""Fill HLD/LLD Reference columns for all requirements in REQUIREMENTS_MATRIX_EN.csv."""

import csv
import sys
from pathlib import Path

# Column indices (0-based)
COL_MODULE = 1
COL_SUBMODULE = 2
COL_HLD = 11
COL_LLD = 12

MODULE_MAP = {
    "PCIe/NVMe Device Emulation Module":                              ("HLD_01", "LLD_01"),
    "Controller Thread Module":                                        ("HLD_02", "LLD_02"),
    "Media Thread Module":                                             ("HLD_03", "LLD_03"),
    "Hardware Abstraction Layer":                                      ("HLD_04", "LLD_04"),
    "Common Platform Layer":                                           ("HLD_05", "LLD_05"),
    "Algorithm Task Layer":                                            ("HLD_06", "LLD_06"),
    "Performance Requirements":                                        ("HLD_06", "LLD_10"),
    "Product Interface":                                               ("HLD_05", "LLD_07"),
    "Fault Injection Framework":                                       ("HLD_05", "LLD_08"),
    "System Reliability and Stability":                                ("HLD_05", "LLD_11"),
    "UPLP (Unclean Power Loss Protection)":                           ("HLD_05", "LLD_17"),
    "QoS (Quality of Service)":                                       ("HLD_02", "LLD_18"),
    "T10 DIF/PI (Data Integrity Field / Protection Information)":      ("HLD_06", "LLD_11"),
    "Security":                                                        ("HLD_02", "LLD_19"),
    "Multi-Namespace Management":                                      ("HLD_06", "LLD_06"),
    "Thermal Management and Telemetry":                               ("HLD_05", "LLD_12"),
}

# Sub-module refinements for Common Platform Layer
# Key: substring to match in sub-module name, Value: refined LLD
CPL_SUBMODULE_LLD_MAP = [
    ("Out-of-Band",   "LLD_07"),
    ("OOB",           "LLD_07"),
    ("Fault",         "LLD_08"),
    ("Panic",         "LLD_08"),
    ("Boot",          "LLD_09"),
    ("Real-Time",     "LLD_12"),
    ("NOR",           "LLD_14"),
    ("Persistence",   "LLD_15"),
]


def get_hld_lld(module: str, submodule: str) -> tuple[str, str]:
    """Return (HLD, LLD) for a given module and sub-module."""
    if module not in MODULE_MAP:
        return ("", "")

    hld, lld = MODULE_MAP[module]

    # Refine LLD for Common Platform Layer based on sub-module name
    if module == "Common Platform Layer":
        for keyword, refined_lld in CPL_SUBMODULE_LLD_MAP:
            if keyword.lower() in submodule.lower():
                lld = refined_lld
                break

    return (hld, lld)


def main() -> int:
    repo_root = Path(__file__).parent.parent
    csv_path = repo_root / "REQUIREMENTS_MATRIX_EN.csv"

    if not csv_path.exists():
        print(f"ERROR: CSV not found at {csv_path}", file=sys.stderr)
        return 1

    # Read all rows
    with open(csv_path, newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        rows = list(reader)

    if not rows:
        print("ERROR: CSV is empty", file=sys.stderr)
        return 1

    header = rows[0]
    data_rows = rows[1:]

    total = len(data_rows)
    filled = 0
    unresolved = []

    updated_rows = [header]
    for row in data_rows:
        # Pad row to at least 13 columns
        while len(row) < 13:
            row.append("")

        module = row[COL_MODULE].strip()
        submodule = row[COL_SUBMODULE].strip()

        hld, lld = get_hld_lld(module, submodule)

        if hld and lld:
            row[COL_HLD] = hld
            row[COL_LLD] = lld
            filled += 1
        else:
            unresolved.append((row[0], module, submodule))

        updated_rows.append(row)

    # Write back
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerows(updated_rows)

    print(f"Filled {filled}/{total} rows.")

    if unresolved:
        print(f"\nWARNING: {len(unresolved)} rows could not be resolved:")
        for req_id, mod, sub in unresolved:
            print(f"  {req_id}: module='{mod}' submodule='{sub}'")

    # Verification: check no empty HLD or LLD cells remain
    with open(csv_path, newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        verify_rows = list(reader)

    empty_cells = []
    for i, row in enumerate(verify_rows[1:], start=2):
        while len(row) < 13:
            row.append("")
        if not row[COL_HLD].strip() or not row[COL_LLD].strip():
            empty_cells.append((i, row[0], row[COL_HLD], row[COL_LLD]))

    if empty_cells:
        print(f"\nVERIFICATION FAILED: {len(empty_cells)} rows still have empty HLD/LLD:")
        for line, req_id, hld_val, lld_val in empty_cells:
            print(f"  Line {line}: {req_id} HLD='{hld_val}' LLD='{lld_val}'")
        return 1
    else:
        print(f"\nVERIFICATION PASSED: All {total} rows have non-empty HLD and LLD references.")
        return 0


if __name__ == "__main__":
    sys.exit(main())
