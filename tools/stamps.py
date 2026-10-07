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
import fcntl
import fnmatch
import os
import filecmp
import shutil
import re
import subprocess
import sys
from contextlib import contextmanager
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


@contextmanager
def user_output_lock(user_build_dir: str):
    """Serialize stamp decisions and writers that share a user output tree.

    The lock is a sibling of ``build/<variant>``, not inside it: a rebuild may
    remove that directory, but must not unlink the inode another process is
    using to coordinate.  Build IDs/options are deliberately absent from the
    lock name because all configurations for one USER_VARIANT write the same
    output root.
    """
    output_dir = REPO / user_build_dir
    lock_dir = output_dir.parent
    lock_dir.mkdir(parents=True, exist_ok=True)
    lock_path = lock_dir / f".{output_dir.name}.a20-build.lock"
    with lock_path.open("a") as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)


def cmd_user(a) -> int:
    if not a.build_id:
        raise SystemExit("error: user stamp needs --build-id")
    with user_output_lock(a.user_build_dir):
        stamp = REPO / a.stamp
        stamp.parent.mkdir(parents=True, exist_ok=True)
        need_build = need_clean = False
        init = a.user_build_dir + "/init"
        mksh = a.user_build_dir + "/mksh"

        # Re-evaluate only after taking the output-root lock. Another make
        # process may have completed this exact build while we were waiting.
        if stamp_id(stamp) != a.build_id:
            need_build = need_clean = True
        elif not os.access(init, os.X_OK) or not os.access(mksh, os.X_OK):
            need_build = True
        elif any_newer(stamp, a.roots.split(), a.skip.split()):
            need_build = True

        if not need_build:
            print(f"[USER] {a.build_id} up to date")
            return 0
        # A clean user rebuild removes the whole shared output directory, and
        # even an incremental rebuild can rewrite native targets.  Invalidate
        # the sibling stamp while holding the same output-root lock so another
        # make process cannot treat deleted native artifacts as current.
        (stamp.parent / ".native-build-id").unlink(missing_ok=True)
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
    if not a.build_id:
        raise SystemExit("error: native stamp needs --build-id")
    with user_output_lock(a.stamp.rsplit("/", 1)[0]):
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


def cmd_build_flags(a) -> int:
    """Rewrite the build-flags stamp only when the signature actually changed.

    The stamp is an order-only-ish dependency of every object file, so its mtime
    is load-bearing: touching it on an unchanged signature would force a full
    kernel rebuild on every invocation.  The shell version compared the freshly
    written temp file against the live stamp and discarded the temp on a match;
    byte comparison, not mtime, decided.
    """
    stamp = REPO / a.stamp
    stamp.parent.mkdir(parents=True, exist_ok=True)
    body = a.build_flags_sig + "\n"
    tmp = stamp.with_name(stamp.name + ".tmp")
    tmp.write_text(body, encoding="utf-8")
    if stamp.is_file() and stamp.read_text(encoding="utf-8") == body:
        tmp.unlink()
        return 0
    tmp.replace(stamp)
    return 0


def cmd_clean(a) -> int:
    """Delete kernel object files and build trees.

    Replaces the `find ... -delete` / `rm -rf` pairs that `clean` and
    `_reset_obj` each carried, so the two no longer have to agree on what a
    reset means.  Sub-makes (`make -C user clean`) stay in make: they are
    recursive builds, not file surgery.
    """
    n = 0
    for d in a.find_root:
        root = REPO / d
        if not root.is_dir():
            continue
        for f in root.rglob("*.o"):
            f.unlink()
            n += 1
    for d in a.rm_rf:
        path = REPO / d
        if path.is_symlink() and d == ".kernel-build":
            # Developers may point the build root at a dedicated external
            # cache. Preserve that link, but only empty it when its immediate
            # children look like our generated build directories. This keeps
            # `make clean` from silently retaining stale objects while making
            # an unexpected symlink target a loud, non-destructive error.
            target = path.resolve(strict=True)
            if (not target.is_dir() or target == REPO or REPO in target.parents
                    or target in REPO.parents):
                raise SystemExit(f"error: refusing unsafe .kernel-build target: {target}")
            marker = target / ".a20-build-root"
            if marker.is_symlink() or not marker.is_file() or marker.read_text(encoding="utf-8").strip() != "a20-build-root-v1":
                raise SystemExit(
                    f"error: refusing unmarked .kernel-build target: {target} "
                    "(initialize a dedicated build cache with .a20-build-root containing a20-build-root-v1)"
                )
            allowed = re.compile(
                r"^(?:aarch64|arm32|armv7m|loongarch32|loongarch64|ppc64le|riscv32|riscv64|x86_64)-.*|smoke$"
            )
            children = [p for p in target.iterdir() if p != marker]
            unexpected = [p.name for p in children if not allowed.fullmatch(p.name)]
            if unexpected or any(p.is_symlink() or not p.is_dir() for p in children):
                raise SystemExit(
                    "error: refusing to clear unexpected .kernel-build contents: "
                    + ", ".join(unexpected or [p.name for p in children if p.is_symlink() or not p.is_dir()])
                )
            for child in children:
                shutil.rmtree(child)
        elif path.is_symlink():
            # rmtree does not remove directory symlinks; unlink explicitly so
            # other rm-rf callers do not leave stale links behind.
            path.unlink()
        else:
            shutil.rmtree(path, ignore_errors=True)
    for f in a.rm_f:
        (REPO / f).unlink(missing_ok=True)
    print(f"[CLEAN] {n} object file(s) removed")
    return 0


def cmd_adopt(a) -> int:
    """`cmp -s TMP TARGET && rm -f TMP || mv -f TMP TARGET`.

    Shared by the generated config headers, whose *content* is still produced
    in make but whose write-if-changed decision is not.  Byte comparison, not
    mtime, decides -- these headers are prerequisites of the objects that
    include them, so touching an unchanged header would force a rebuild.
    """
    tmp, target = REPO / a.tmp, REPO / a.target
    if target.is_file() and tmp.is_file() and \
            filecmp.cmp(tmp, target, shallow=False):
        tmp.unlink()
        return 0
    tmp.replace(target)
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
    s = sub.add_parser("clean")
    s.add_argument("--find-root", action="append", default=[])
    s.add_argument("--rm-rf", action="append", default=[])
    s.add_argument("--rm-f", action="append", default=[])
    s = sub.add_parser("adopt")
    s.add_argument("--tmp", required=True)
    s.add_argument("--target", required=True)
    s = sub.add_parser("build-flags")
    s.add_argument("--stamp", required=True)
    s.add_argument("--build-flags-sig", required=True)
    a = ap.parse_args()
    return {"user": cmd_user, "native": cmd_native,
            "build-flags": cmd_build_flags,
            "clean": cmd_clean,
            "adopt": cmd_adopt}[a.cmd](a)


if __name__ == "__main__":
    raise SystemExit(main())
