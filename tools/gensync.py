#!/usr/bin/env python3
"""Verify that checked-in generated files match what their generator produces.

Two gates in the tree are the same shape -- snapshot, regenerate, compare -- and
both used to be shell in a recipe.  They differ in one way that matters:

  in-place   the generator rewrites the committed file, so drift is *left in
             place* for a human to review and commit.  That is the point of
             the gate: `check-envelope-coverage` fails so a newly registered
             syscall cannot be silently unclassified, and the regenerated
             document is the review artefact.
  to-temp    the generator writes into a temp dir and the committed file is
             never touched, so a failure leaves the tree alone.

Making that difference explicit here is the whole reason these moved out of
make: in shell it was invisible, one `cmp` buried in a `;`-chained recipe.
"""

from __future__ import annotations

import argparse
import filecmp
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def in_place(label: str, target: str, cmd: list[str]) -> int:
    target_path = REPO / target
    before = target_path.read_bytes()
    r = subprocess.run(cmd, cwd=REPO, check=False)
    if r.returncode == 0 and target_path.read_bytes() == before:
        print(f"{label}: PASS")
        return 0
    if r.returncode != 0:
        print(f"{label}: FAIL -- generator exited {r.returncode}", file=sys.stderr)
    else:
        print(f"{label}: FAIL -- matrix drifted; review and commit")
    return 1


def to_temp(label: str, committed: str, out_name: str,
            cmd: list[str]) -> int:
    with tempfile.TemporaryDirectory() as tmp:
        # The generator writes more than one file; hand it the temp dir and
        # let it name its own outputs, then compare just the one under test.
        argv = [x.replace("@TMP@", tmp) for x in cmd]
        r = subprocess.run(argv, cwd=REPO, check=False)
        produced = Path(tmp) / out_name
        if r.returncode != 0 or not produced.is_file():
            print(f"{label}: FAIL -- generator produced nothing "
                  f"(exit {r.returncode})", file=sys.stderr)
            return 1
        target = REPO / committed
        if filecmp.cmp(produced, target, shallow=False):
            print(f"{label}: PASS")
            return 0
        print(f"{label}: rootfs overlay header is out of sync with its generator",
              file=sys.stderr)
        d = subprocess.run(["diff", "-u", str(target), str(produced)],
                           check=False, capture_output=True, text=True)
        sys.stderr.write(d.stdout)
        return 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    env = sub.add_parser("envelope-coverage",
                         help="gen_envelope_coverage.py rewrites the doc in place")
    env.add_argument("--label", default="check-envelope-coverage")
    env.add_argument("--file", required=True)

    ovl = sub.add_parser("rootfs-overlay",
                         help="gen_rootfs_overlay.py writes into @TMP@")
    ovl.add_argument("--label", default="check-rootfs-overlay-generator")
    ovl.add_argument("--committed", required=True)
    ovl.add_argument("--out-name", default="rootfs_overlay.h")
    ovl.add_argument("generator", nargs=argparse.REMAINDER)

    a = ap.parse_args()
    if a.cmd == "envelope-coverage":
        return in_place(a.label, a.file,
                        [sys.executable, "tools/gen_envelope_coverage.py"])
    # REMAINDER keeps the `--` separator itself; passing it on would make
    # subprocess try to exec it as the program.
    argv = a.generator[1:] if a.generator[:1] == ["--"] else a.generator
    return to_temp(a.label, a.committed, a.out_name, argv)


if __name__ == "__main__":
    raise SystemExit(main())
