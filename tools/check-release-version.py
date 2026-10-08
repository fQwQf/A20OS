#!/usr/bin/env python3
"""Check the kernel's native/Linux version pair and an optional release tag."""

import argparse
from pathlib import Path
import re
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag", nargs="?", help="release tag, e.g. v0.17")
    parser.add_argument(
        "--header", type=Path,
        default=Path(__file__).resolve().parent.parent / "kernel/include/core/version.h",
    )
    args = parser.parse_args()
    try:
        source = args.header.read_text()
        values = {}
        for name in ("A20OS_VERSION", "A20OS_RELEASE"):
            matches = re.findall(
                rf'^\s*#define\s+{name}\s+"([^"\n]+)"\s*$', source, re.MULTILINE,
            )
            if len(matches) != 1:
                raise ValueError(f"expected one quoted {name} definition")
            values[name] = matches[0]
        version = values["A20OS_VERSION"]
        if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", version):
            raise ValueError(f"invalid A20OS_VERSION: {version}")
        expected_release = "20." + version
        if values["A20OS_RELEASE"] != expected_release:
            raise ValueError(f"A20OS_RELEASE must be {expected_release}")
        if args.tag is not None and args.tag != "v" + version:
            raise ValueError(f"tag {args.tag!r} does not match kernel version v{version}")
    except (OSError, ValueError) as exc:
        print(f"release-version: FAIL: {exc}", file=sys.stderr)
        return 1
    print(f"release-version: PASS: v{version} (Linux release {expected_release})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
