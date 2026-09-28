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
import shutil
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
    if not a.build_id:
        raise SystemExit("error: user stamp needs --build-id")
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
    if not a.build_id:
        raise SystemExit("error: native stamp needs --build-id")
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


GLIBC_RISCV64 = ("ld-linux-riscv64-lp64d.so.1", "libc.so.6", "libdl.so.2",
                 "libm.so.6", "libpthread.so.0", "librt.so.1",
                 "libatomic.so.1", "libgcc_s.so.1")
LAMINA_STAMPS = {"vim": ".vim-built", "git": ".git-built",
                 "gcc": ".gcc-built", "cc": ".gcc-built",
                 "rust": ".rust-built", "rustc": ".rust-built",
                 "cargo": ".rust-built", "rustfmt": ".rust-built",
                 "lamina": ".lamina-built"}
EXTRA_SKIP_SUFFIX = (".o", ".a", ".so", ".d")


def _stat_record(tag: str, path: Path, name: str | None = None) -> str | None:
    """One `find -printf '<tag> <name> <size> <mtime>'` record, or None if gone.

    The mtime is built by integer division, not `st_mtime_ns / 1e9`: a float
    cannot hold a 10-digit epoch plus nine fractional digits without losing
    the tail.  GNU find's %T@ also appends a literal trailing '0' to the
    9-digit nanosecond field, which was measured rather than assumed (two
    probes, one with leading zeros in the fraction).
    """
    try:
        st = path.stat()
    except OSError:
        return None
    label = name if name is not None else path.name
    secs, nanos = divmod(st.st_mtime_ns, 1_000_000_000)
    return f"{tag} {label} {st.st_size} {secs}.{nanos:09d}0"


def _tree_records(tag: str, root: Path) -> list[str]:
    """`find <dir> -type f -printf '<tag> %P <size> <mtime>'` (paths relative)."""
    if not root.is_dir():
        return []
    out = []
    for dp, _dns, fns in os.walk(root):
        for f in sorted(fns):
            p = Path(dp) / f
            rec = _stat_record(tag, p, name=str(p.relative_to(root)))
            if rec:
                out.append(rec)
    return out


def extra_inputs(a) -> int:
    """Regenerate the extra-image input manifest (was $(EXTRA_IMAGE_STAMP)).

    The point of this stamp is to answer "did any input to extra.img change?"
    with a single sorted file that make can diff cheaply, instead of stat'ing
    the whole tree on every build.  The header lines live in the same sorted
    stream, so a changed variable and a changed binary show up the same way.
    """
    stamp = REPO / a.stamp
    stamp.parent.mkdir(parents=True, exist_ok=True)
    recs: list[str] = [
        f"arch={a.arch}", f"nommu={a.nommu}", f"opt={a.opt}",
        f"profile={a.profile}", f"user_variant={a.user_variant}",
        f"packages={' '.join(sorted(a.extra_packages.split()))}",
        f"image_mb={a.extra_mb}", f"image={a.extra_img}",
        f"glibc_dir={a.riscv_glibc_lib_dir}",
        f"glibc_local_dir={a.riscv_glibc_local_lib_dir}",
    ]

    # The shell glob omitted dotfiles and dropped build products.
    user_build = REPO / a.user_build_dir
    if user_build.is_dir():
        for f in sorted(user_build.iterdir()):
            if not f.is_file() or f.name.startswith("."):
                continue
            if f.name.endswith(EXTRA_SKIP_SUFFIX):
                continue
            rec = _stat_record("user", f)
            if rec:
                recs.append(rec)

    extra_dir = REPO / a.extra_dir
    wanted = set(a.extra_packages.split())
    if extra_dir.is_dir():
        for f in sorted(extra_dir.iterdir()):
            if f.is_file() and f.name in wanted:
                rec = _stat_record("extra", f)
                if rec:
                    recs.append(rec)
    if "lamina" in wanted:
        for pat in ("liblaminaCore.so*", "liblmcas.so*", "liblmmc.so*",
                    "libLammpCore.so*", "libstdc++.so*"):
            for f in sorted(extra_dir.glob(pat)):
                rec = _stat_record("extra", f)
                if rec:
                    recs.append(rec)

    for package in sorted(wanted):
        name = LAMINA_STAMPS.get(package)
        if not name:
            continue
        rec = _stat_record("stamp", extra_dir / "stamp" / name)
        if rec:
            recs.append(rec)

    if "vim" in wanted:
        recs += _tree_records("vim-runtime", REPO / a.vim_runtime)
    if "git" in wanted:
        recs += _tree_records("git-template", REPO / a.git_templates)
        for helper in ("git-remote-http", "git-remote-https"):
            rec = _stat_record("git-helper", extra_dir / helper)
            if rec:
                recs.append(rec)
        if a.ca_cert_bundle:
            # The original used `find -L`, so the bundle is recorded resolved.
            rec = _stat_record("ca-bundle", Path(a.ca_cert_bundle),
                               name=a.ca_cert_bundle)
            if rec:
                recs.append(rec)

    if a.arch == "riscv64" and ({"gcc", "cc"} & wanted):
        rec = _stat_record("gcc-musl-libc", REPO / a.riscv_gcc_musl_libc,
                           name=a.riscv_gcc_musl_libc)
        if rec:
            recs.append(rec)
    if a.arch == "riscv64" and ({"rust", "rustc", "cargo", "rustfmt"} & wanted):
        for d in (a.riscv_glibc_lib_dir, a.riscv_glibc_local_lib_dir):
            if not d:
                continue
            for name in GLIBC_RISCV64:
                rec = _stat_record("glibc", REPO / d / name, name=f"{d}/{name}")
                if rec:
                    recs.append(rec)
    # The original ended with `find Makefile user/extra.mk -maxdepth 0 -type f
    # -printf 'recipe %p ...'`, and re-running the original recipe here emits no
    # 'recipe' record at all, so the manifest must not contain one either.
    # Recorded as dead code rather than "fixed": changing what the stamp covers
    # would change when extra.img rebuilds, which is a semantic decision.

    # Byte order, not locale order: LC_ALL=C sort in the original.
    text = "\n".join(sorted(recs, key=lambda s: s.encode())) + "\n"
    try:
        current = stamp.read_text()
    except OSError:
        current = None
    if current == text:
        return 0
    tmp = stamp.with_suffix(stamp.suffix + ".tmp")
    tmp.write_text(text, encoding="utf-8")
    os.replace(tmp, stamp)
    print("[EXTRA] image inputs changed")
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
        shutil.rmtree(REPO / d, ignore_errors=True)
    for f in a.rm_f:
        (REPO / f).unlink(missing_ok=True)
    print(f"[CLEAN] {n} object file(s) removed")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("user", "native", "extra-inputs"):
        s = sub.add_parser(name)
        s.add_argument("--stamp", required=True)
        # only the user/native stamps compare a build id; extra-inputs
        # writes its own header lines and must not be forced to take one
        s.add_argument("--build-id", required=(name != "extra-inputs"))
        s.add_argument("--arch", required=True)
        s.add_argument("--nommu", default="0")
        s.add_argument("--opt", default="")
        s.add_argument("--roots", default="")
        s.add_argument("--skip", default="")
        if name == "extra-inputs":
            s.add_argument("--user-build-dir", required=True)
            s.add_argument("--extra-img", required=True)
            s.add_argument("--extra-mb", default="")
            s.add_argument("--extra-packages", default="")
            s.add_argument("--user-variant", default="")
            s.add_argument("--profile", default="")
            s.add_argument("--extra-dir", default="")
            s.add_argument("--riscv-gcc-musl-libc", default="")
            s.add_argument("--riscv-glibc-lib-dir", default="")
            s.add_argument("--riscv-glibc-local-lib-dir", default="")
            s.add_argument("--ca-cert-bundle", default="")
            s.add_argument("--vim-runtime", default="")
            s.add_argument("--git-templates", default="")
        elif name == "user":
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
    s = sub.add_parser("build-flags")
    s.add_argument("--stamp", required=True)
    s.add_argument("--build-flags-sig", required=True)
    a = ap.parse_args()
    return {"user": cmd_user, "native": cmd_native,
            "extra-inputs": extra_inputs,
            "build-flags": cmd_build_flags,
            "clean": cmd_clean}[a.cmd](a)


if __name__ == "__main__":
    raise SystemExit(main())
