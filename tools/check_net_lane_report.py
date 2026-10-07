#!/usr/bin/env python3
"""Validate the optional per-lane rows rendered by /proc/a20/netmem."""

import argparse
import re
import sys
from pathlib import Path


def read_report(path: Path, lanes: int) -> list[str]:
    lines = [line.strip() for line in path.read_text(
        encoding="utf-8", errors="replace").splitlines()]
    expected_headers = ("memp lane alloc freed", "rx lane rx drop")
    present = [header in " ".join(line.split()) for line in lines
               for header in expected_headers]
    if lanes == 1:
        if any(present) or any(line.startswith("rx staged") for line in lines):
            raise ValueError("one-lane report unexpectedly contains per-lane sections")
        return lines

    for header in expected_headers:
        if sum(" ".join(line.split()) == header for line in lines) != 1:
            raise ValueError(f"expected exactly one `{header}` header")

    for header, next_header, label in (
        (expected_headers[0], expected_headers[1], "memp"),
        (expected_headers[1], "rx staged", "rx"),
    ):
        start = next(i for i, line in enumerate(lines)
                     if " ".join(line.split()) == header) + 1
        stop = next((i for i in range(start, len(lines))
                     if (" ".join(lines[i].split()) == next_header
                         if label == "memp"
                         else lines[i].startswith(next_header))), len(lines))
        rows = []
        for line in lines[start:stop]:
            fields = line.split()
            if fields and fields[0].isdigit():
                if len(fields) != 3 or not all(re.fullmatch(r"\d+", x)
                                               for x in fields):
                    raise ValueError(f"malformed {label} lane row: {line!r}")
                rows.append(tuple(map(int, fields)))
        if len(rows) != lanes or [row[0] for row in rows] != list(range(lanes)):
            raise ValueError(f"expected lane rows 0..{lanes - 1} in {label} section; got {rows}")
        if label == "memp" and (sum(row[1] for row in rows) == 0 or
                                 sum(row[2] for row in rows) == 0):
            raise ValueError("memp lane counters did not record allocations and frees")

    staged = [line for line in lines if line.startswith("rx staged")]
    if len(staged) != 1 or not re.search(r":\s*\d+$", staged[0]):
        raise ValueError("missing or malformed rx staged counter")
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--lanes", type=int, required=True)
    args = parser.parse_args()
    if args.lanes < 1:
        parser.error("--lanes must be positive")
    try:
        read_report(args.log, args.lanes)
    except (OSError, ValueError) as exc:
        print(f"net-lane-report: FAIL: {exc}", file=sys.stderr)
        return 1
    print(f"net-lane-report: PASS ({args.lanes}-lane report shape and counters)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
