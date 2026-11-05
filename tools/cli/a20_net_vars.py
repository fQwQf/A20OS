#!/usr/bin/env python3
"""Print make variables derived from an instance TOML.

This is the plain-`make` counterpart to `tools/a20 build <instance>`: it lets

    make NET_INSTANCE=xfce-x86_64 kernel-only

select the protocol stack, lane count, resource tier and role from
instances/xfce-x86_64.toml instead of spelling NET_STACK=... by hand.

Only build-shaping fields are emitted.  QEMU device flags stay in [net] and
are consumed by the runner, not by the kernel build.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from a20_derive import derive_make_vars  # noqa: E402
from a20_instance import InstanceError, parse_instance  # noqa: E402

_KEYS = ("ARCH", "BOARD", "ABI", "PROFILE", "NOMMU", "NET_STACK", "NET_LANES",
         "NET_PROFILE", "NET_ROLE", "NET_CONNTRACK_ENTRIES")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} <instance.toml>", file=sys.stderr)
        return 2
    path = Path(argv[1])
    if not path.is_absolute():
        path = Path.cwd() / path
    try:
        inst = parse_instance(path)
    except InstanceError as err:
        print(f"{path}: {err}", file=sys.stderr)
        return 1
    out: list[str] = []
    for assignment in derive_make_vars(inst):
        key = assignment.split("=", 1)[0]
        if key not in _KEYS:
            continue
        # The output is evaluated word by word by the Makefile.  A value with
        # whitespace would silently split into two assignments, so reject it
        # here instead of quoting it into something make cannot parse.
        if any(c.isspace() for c in assignment):
            print(f"{path}: net/instance field for {key} contains whitespace",
                  file=sys.stderr)
            return 1
        out.append(assignment)
    print(" ".join(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
