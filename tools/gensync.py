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
            cmd: list[str], stale: str = "generated file is stale",
            also: list[str] | None = None) -> int:
    """Regenerate into a temp dir and diff the result against the tree.

    `also` names further generator outputs that must match as well.  A
    generator that emits more than one file needs this: comparing only the
    first leaves the others silently unverified, which is how
    kernel/fs/rootfs_overlay.c went unchecked while its header was verified
    on every commit.
    """
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
        targets = [(committed, out_name)] + [
            (p, Path(p).name) for p in (also or [])
        ]
        for target_rel, produced_name in targets:
            produced = Path(tmp) / produced_name
            target = REPO / target_rel
            if not produced.is_file():
                print(f"{label}: FAIL -- generator did not emit "
                      f"{produced_name}", file=sys.stderr)
                return 1
            if not filecmp.cmp(produced, target, shallow=False):
                print(f"{label}: {stale} ({target_rel})", file=sys.stderr)
                d = subprocess.run(
                    ["diff", "-u", str(target), str(produced)],
                    check=False, capture_output=True, text=True)
                sys.stderr.write(d.stdout)
                return 1
        print(f"{label}: PASS")
        return 0


def kallsyms(a) -> int:
    """Run gen_kallsyms.py, falling back to an empty stub if it cannot run.

    Both branches succeeded, so this never fails the build: the recipe it
    replaces was an if/else that always exited 0.  Only the generator's exit
    status was consulted -- the original never checked that the output file
    existed on the success path, so neither does this.
    """
    out = REPO / a.out
    out.parent.mkdir(parents=True, exist_ok=True)
    r = subprocess.run([sys.executable, a.generator, a.elf, str(out)],
                       cwd=REPO, check=False)
    if r.returncode == 0:
        # Print the path as given; the recipe echoed `$@`, not the resolved path.
        print(f"  KALLSYMS {a.out}")
    else:
        print("  KALLSYMS skipped (configured Python unavailable)")
        out.write_text("/* empty */\n")
    return 0


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
    ovl.add_argument("--stale", default="rootfs overlay header is out of sync with its generator")
    ovl.add_argument("--also", nargs="*", default=[],
                     help="further generated files that must match too, "
                          "relative to the repo root (e.g. the .c twin of the "
                          "header)")
    ovl.add_argument("generator", nargs=argparse.REMAINDER)

    idl = sub.add_parser("a20-idl", help="a20idl.py output must match the committed header")
    idl.add_argument("--label", default="check-a20-idl")
    idl.add_argument("--idl", required=True)
    idl.add_argument("--committed", required=True)
    idl.add_argument("--out-name", required=True)
    idl.add_argument("--stale", default="generated header is stale")
    idl.add_argument("generator", nargs=argparse.REMAINDER)

    kal = sub.add_parser("kallsyms", help="gen_kallsyms.py, stub on failure")
    kal.add_argument("--generator", default="tools/gen_kallsyms.py")
    kal.add_argument("--elf", required=True)
    kal.add_argument("--out", required=True)

    a = ap.parse_args()
    if a.cmd == "envelope-coverage":
        return in_place(a.label, a.file,
                        [sys.executable, "tools/gen_envelope_coverage.py"])
    if a.cmd == "a20-idl":
        argv = a.generator[1:] if a.generator[:1] == ["--"] else a.generator
        return to_temp(a.label, a.committed, a.out_name, argv, a.stale)
    if a.cmd == "kallsyms":
        return kallsyms(a)
    # REMAINDER keeps the `--` separator itself; passing it on would make
    # subprocess try to exec it as the program.
    argv = a.generator[1:] if a.generator[:1] == ["--"] else a.generator
    return to_temp(a.label, a.committed, a.out_name, argv,
                   stale=a.stale, also=a.also)


if __name__ == "__main__":
    raise SystemExit(main())
