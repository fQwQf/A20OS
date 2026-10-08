#!/usr/bin/env python3
"""Check evidence that host-driven TCP input overlapped across lwIP lanes."""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

CORE = re.compile(
    r"^core_parallel: peak_active_lanes=(\d+) probe_hits=(\d+) "
    r"inputs:\s+((?:\d+\s+){3}\d+) timers:\s+((?:\d+\s+){3}\d+)$"
)


def parse_core_parallel(text: str) -> tuple[int, int, list[int], list[int]]:
    rows = [line.strip() for line in text.splitlines()
            if line.strip().startswith("core_parallel:")]
    if len(rows) != 1:
        raise ValueError(f"expected one core_parallel row, found {len(rows)}")
    match = CORE.fullmatch(rows[0])
    if not match:
        raise ValueError(f"malformed core_parallel row: {rows[0]!r}")
    peak, hits = map(int, match.group(1, 2))
    inputs = list(map(int, match.group(3).split()))
    timers = list(map(int, match.group(4).split()))
    if peak < 2:
        raise ValueError(f"only {peak} lane(s) were observed active concurrently")
    if hits <= 0:
        raise ValueError("TCP parallel probe recorded no hot input calls")
    if len(inputs) != 4 or not all(inputs):
        raise ValueError(f"TCP hot input did not reach all four lanes: {inputs}")
    if len(timers) != 4 or not all(timers):
        raise ValueError(f"TCP timer did not advance on all four lanes: {timers}")
    return peak, hits, inputs, timers


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    try:
        peak, hits, inputs, timers = parse_core_parallel(
            args.log.read_text(encoding="utf-8", errors="replace"))
    except (OSError, ValueError) as exc:
        print(f"net-lane-parallel: FAIL: {exc}", file=sys.stderr)
        return 1
    print(f"net-lane-parallel: PASS (peak={peak}, probe_hits={hits}, "
          f"inputs={inputs}, timers={timers})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
