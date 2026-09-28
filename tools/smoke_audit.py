#!/usr/bin/env python3
"""Check that every smoke case is reachable as a make target, and vice versa.

Added after a real regression slipped through: migrating the recipes replaced
smoke-netctl with a bare `smoke-netctl: NET_HOSTFWD=` line and no recipe at
all, so `make smoke-netctl` failed with "no rule to make target".  Nothing
caught it -- check-manifests validated the case *table*, and the gate that
would have noticed was never run.

Two directions matter, because each catches a different mistake:
  * a case in the table with no reachable target  (the netctl regression)
  * a target invoking tools/smoke.py for a case that is not in the table
    (a recipe left behind by a rename, silently doing nothing)

One `make -n MAKE=true` invocation covers every target, so the whole audit
costs a single make spawn rather than one per case.  MAKE=true suppresses the
recursive build, which would otherwise bury the recipes under compile lines.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

from smoke import SUBCOMMANDS  # noqa: E402
from smoke_cases import CASES  # noqa: E402


def main() -> int:
    names = sorted(CASES)
    # Dump the whole rule database, not just the cases we know about: expanding
    # only the table's names makes the orphan direction unreachable, because a
    # target naming an unknown case is never expanded and so never seen.
    r = subprocess.run(["make", "-p", "-n", "MAKE=true"], cwd=REPO,
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("check-smoke-cases: FAIL -- make could not print its database:")
        for line in (r.stderr or r.stdout).splitlines()[:20]:
            if line.strip():
                print(f"  {line}")
        return 1

    # A case with no target has no recipe either, so it is simply absent here --
    # no separate existence probe needed, which also avoids reporting it twice.
    wired = set(re.findall(r"tools/smoke\.py (\S+)", r.stdout))
    missing = sorted(set(names) - wired)
    extra = sorted(wired - set(names) - SUBCOMMANDS)

    if missing or extra:
        if missing:
            print(f"check-smoke-cases: FAIL -- {len(missing)} case(s) have no "
                  f"reachable make target: {missing}")
        if extra:
            print(f"check-smoke-cases: FAIL -- {len(extra)} target(s) invoke "
                  f"tools/smoke.py for an unknown case: {extra}")
        return 1

    print(f"check-smoke-cases: {len(names)} case(s) reachable, "
          f"no orphans, table and makefiles agree")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
