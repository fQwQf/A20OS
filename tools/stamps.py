#!/usr/bin/env python3
"""Incremental-build stamps for the user and native trees (was targets-dev.mk).

Both rules were the same hand-rolled staleness decision in shell: compare the
build id in a stamp file, then either the expected binaries are missing or some
source file is newer than the stamp, in which case rebuild and rewrite the stamp.

The `find ... -newer` half is the part worth having in Python.  Written as shell
it was a `find` with a prune list, `-print -quit` and a `grep -q` in a pipeline,
where the interaction between the prune expression and the `-quit` is easy to get
subtly wrong and impossible to see.  Here it is a walk with an explicit skip set.

Only the *decision* and the orchestration move; the sub-makes are still make, so
nothing about how the trees are compiled changes.
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def stamp_id(stamp: Path) -> str:
    try:
        return stamp.read_text().strip()
    except OSError:
        return ""


def any_newer(stamp: Path, roots: list[str], skip: list[str]) -> bool:
    """True if any file under `roots` is newer than the stamp.

    Mirrors `find <roots> -prune... -o -type f -newer <stamp> -print -quit`:
    skip whole directories, and stop at the first hit.  Everything is compared
    as an absolute path, because mixing os.path.relpath against absolute skip
    entries silently never matches once the tree is outside the repo.
    """
    if not stamp.exists():
        return False
    ref = stamp.stat().st_mtime
    # find's -path takes a glob, so these are matched with fnmatch against the
    # absolute directory.  Three kinds of entry appear: '*/.git' must stay
    # unanchored to match at any depth, a relative literal or relative glob is
    # anchored to the repo (os.path.join keeps the wildcard intact), and an
    # absolute path is already anchored.  abspath() on any of them would freeze
    # a wildcard into a literal and silently stop pruning.
    def _pat(s: str) -> str:
        if s.startswith("*") or os.path.isabs(s):
            return s
        return os.path.join(str(REPO), s)

    skip_pats = [_pat(s) for s in skip]

    def skipped(path: str) -> bool:
        ap = os.path.abspath(path)
        return any(fnmatch.fnmatch(ap, p) for p in skip_pats)

    for root in roots:
        base = Path(os.path.abspath(REPO / root))
        if base.is_file():
            if base.stat().st_mtime > ref:
                return True
            continue
        if not base.exists():
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            if skipped(dirpath):
                dirnames[:] = []
                continue
            dirnames[:] = [d for d in dirnames
                           if not skipped(os.path.join(dirpath, d))]
            for name in filenames:
                try:
                    if (Path(dirpath) / name).stat().st_mtime > ref:
                        return True
                except OSError:
                    continue
    return False


def submake(args: list[str]) -> int:
    r = subprocess.run(["make", *args], cwd=REPO, check=False)
    return r.returncode


def cmd_user(a) -> int:
    stamp = REPO / a.stamp
    stamp.parent.mkdir(parents=True, exist_ok=True)
    need_build = need_clean = False
    init = a.user_build_dir + "/init"
    mksh = a.user_build_dir + "/mksh"

    # The shell tested these as an elif chain, so a changed build id short
    # circuits the two later checks rather than also running them.
    if stamp_id(stamp) != a.build_id:
        need_build = need_clean = True
    elif not os.access(init, os.X_OK) or not os.access(mksh, os.X_OK):
        need_build = True
    elif any_newer(stamp, a.roots.split(), a.skip.split()):
        need_build = True

    if not need_build:
        print(f"[USER] {a.build_id} up to date")
        return 0
    common = [f"ARCH={a.arch}", f"NOMMU={a.nommu}",
              f"OPT={a.user_opt}", f"PROFILE={a.profile}",
              f"BUILD_DIR=build/{a.user_variant}"]
    if need_clean:
        rc = submake(["-C", "user", *common, "clean"])
        if rc != 0:
            return rc
    rc = submake(["-C", "user", *common])
    if rc != 0:
        return rc
    stamp.write_text(a.build_id + "\n")
    return 0


def cmd_native(a) -> int:
    stamp = REPO / a.stamp
    stamp.parent.mkdir(parents=True, exist_ok=True)
    need_build = False
    if stamp_id(stamp) != a.build_id:
        need_build = True
    elif any(not os.access(b, os.X_OK) for b in a.binaries.split()):
        need_build = True
    elif any_newer(stamp, a.roots.split(), []):
        need_build = True

    if not need_build:
        print(f"[NATIVE] {a.build_id} up to date")
        return 0
    rc = submake([f"ARCH={a.arch}", f"NOMMU={a.nommu}", f"OPT={a.opt}",
                  "native-programs"])
    if rc != 0:
        return rc
    stamp.write_text(a.build_id + "\n")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("user", "native"):
        s = sub.add_parser(name)
        s.add_argument("--stamp", required=True)
        s.add_argument("--build-id", required=True)
        s.add_argument("--arch", required=True)
        s.add_argument("--nommu", default="0")
        s.add_argument("--opt", default="")
        s.add_argument("--roots", default="")
        s.add_argument("--skip", default="")
        if name == "user":
            s.add_argument("--user-build-dir", required=True)
            s.add_argument("--user-opt", default="")
            s.add_argument("--profile", default="")
            s.add_argument("--user-variant", required=True)
        else:
            s.add_argument("--binaries", default="")
    a = ap.parse_args()
    return {"user": cmd_user, "native": cmd_native}[a.cmd](a)


if __name__ == "__main__":
    raise SystemExit(main())
