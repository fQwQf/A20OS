#!/usr/bin/env python3
"""Compare ABI_BEHAVIOR guest lines against the independent expectation.

The fixtures (user/cmds/core/abi_behavior.c and
user/tests/test_native_abi_behavior.c) print normalized `ABI_BEHAVIOR:
key=value` lines.  Neither implementation generates the expectation: this
script compares their output with tools/tests/abi_behavior_expected.txt, and
in `both` mode also asserts the Linux and Native blocks are byte-identical
apart from their `run=` tag.

Modes:
  linux  one block from an ABI=linux or ABI=both kernel
  both   a `run=linux` block and a `run=native` block from one ABI=both boot
"""
import argparse
import sys
from pathlib import Path

PREFIX = "ABI_BEHAVIOR: "


def blocks(text: str) -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    current = None
    for raw in text.splitlines():
        if not raw.startswith(PREFIX):
            continue
        line = raw[len(PREFIX):]
        if line.startswith("run="):
            current = line[len("run="):]
            out[current] = []
            continue
        if current is None:
            return {}
        out[current].append(line)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--log", required=True, type=Path)
    ap.add_argument("--expected", required=True, type=Path)
    ap.add_argument("--mode", choices=("linux", "both"), default="both")
    args = ap.parse_args()

    expected = [ln.strip() for ln in args.expected.read_text().splitlines()
                if ln.strip() and not ln.lstrip().startswith("#")]
    found = blocks(args.log.read_text(encoding="utf-8", errors="replace"))
    if "linux" not in found:
        print("abi-behavior: no run=linux block in the guest log", file=sys.stderr)
        return 1
    if expected and found["linux"] != expected:
        print("abi-behavior: Linux block differs from the expectation",
              file=sys.stderr)
        for got, want in zip(found["linux"], expected):
            if got != want:
                print(f"  got  {got}\n  want {want}", file=sys.stderr)
        if len(found["linux"]) != len(expected):
            print(f"  {len(found['linux'])} lines, expected {len(expected)}",
                  file=sys.stderr)
        return 1

    if args.mode == "both":
        if "native" not in found:
            print("abi-behavior: no run=native block in the guest log",
                  file=sys.stderr)
            return 1
        if found["linux"] != found["native"]:
            print("abi-behavior: Linux and Native personalities diverge",
                  file=sys.stderr)
            for got, want in zip(found["linux"], found["native"]):
                if got != want:
                    print(f"  linux  {got}\n  native {want}", file=sys.stderr)
            if len(found["linux"]) != len(found["native"]):
                print(f"  linux {len(found['linux'])} lines, "
                      f"native {len(found['native'])} lines", file=sys.stderr)
            return 1
        print(f"abi-behavior: both personalities match the expectation "
              f"({len(expected)} lines)")
    else:
        print(f"abi-behavior: linux personality matches the expectation "
              f"({len(expected)} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
