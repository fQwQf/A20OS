#!/usr/bin/env python3
"""Write tools/smoke_cases.py from the extractor's JSON.

Kept as a script rather than an inline `python -c` in the make target: the
target previously only printed the case count and never wrote the table, so
"regen" silently regenerated nothing.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

FIELDS = ("gate", "pre", "build", "log", "stdin", "timeout", "qemu",
          "argv", "expect", "forbid", "timeout_msg", "pass_msg")

HEADER = '''"""Smoke/gate case table -- GENERATED, do not edit by hand.

Produced by tools/smoke_extract.py and verified against make's own expansion
by tools/smoke_verify_extract.py: every argv, log path, timeout, pass pattern,
forbidden pattern and timeout flag matched exactly at the time of generation.

The extraction source is pinned in smoke_extract.py (SOURCE_REV) because these
recipes have been replaced in the .mk files; regenerate from there.

Semantics worth knowing when editing a case:
  expect      every pattern must match the log (grep -q, i.e. a regex)
  forbid      no pattern may match -- e.g. smoke-socket-stress passes only
              when SOCKET_STRESS: PASS is present AND no [LOCK] line is
  timeout_msg distinguish exit 124 (the QEMU timeout) in the failure message
"""

from __future__ import annotations

CASES: dict[str, dict] = {'''


def render(cases: list[dict]) -> str:
    lines = [HEADER]
    for c in sorted(cases, key=lambda c: c["name"]):
        lines.append(f"    {c['name']!r}: {{")
        for f in FIELDS:
            lines.append(f"        {f!r}: {c.get(f)!r},")
        lines.append("    },")
    lines.append("}")
    return "\n".join(lines) + "\n"


def main() -> int:
    src = Path(sys.argv[1])
    dst = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).with_name("smoke_cases.py")
    cases = json.loads(src.read_text())
    dst.write_text(render(cases), encoding="utf-8")
    print(f"smoke_gen_cases: wrote {dst} ({len(cases)} cases)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
