#!/usr/bin/env python3
"""clang-format drift gate: compare first-party sources against a ratchet baseline.

`.clang-format` describes a style, not the tree's history.  Measured over the
996 first-party `.c`/`.h` files when this gate landed, 844 of them did not
conform to it -- the single largest cause being function-opening braces on
their own line where `.clang-format` says `BreakBeforeBraces: Attach`.  So
the honest formulation of "stop the style from drifting further" is a ratchet,
not a clean-tree assertion: every file that was already non-conforming is
grandfathered in tools/clang-format-baseline.txt, and the gate fails only on a
file that is *not* in that baseline.  New drift is caught; nobody is asked to
reformat 844 files to get a green build.

The ratchet is set-based rather than whole-file based (unlike the
snapshot/regenerate/compare shape in tools/gensync.py) because the interesting
comparison is "which files are dirty", not "does this file match".  A baseline
entry that has since become clean is reported as a `note:` and does not fail:
shrinking the baseline is a courtesy the gate asks for, not a chore it
compels, because a file can stop being dirty through a legitimate change that
has nothing to do with formatting.

Scope is first-party sources only.  The vendored trees (kernel/external/lwip,
user/external/*) and the build output directories are excluded: reformatting
upstream code is not this project's decision to make, and they are large enough
that scanning them would dominate the gate's runtime.  docs/ carries no C, but
it is excluded by the same list so the scope stays a single readable constant.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BASELINE = Path(__file__).resolve().parent / "clang-format-baseline.txt"

# First-party roots, in the order their contents are reported.
SOURCE_ROOTS = ("kernel", "user", "tools")

# Path prefixes, relative to the repo root, that are never this project's to
# reformat.  Keep in sync with the `vendored` / `generated` sections of
# .editorconfig, which already exempts the same trees from whitespace tooling.
EXCLUDED_PREFIXES = (
    "kernel/external/",  # vendored lwIP
    "user/external/",    # vendored musl / sbase / mksh, fastfetch submodule
    "user/build/",       # checked-in userspace build output
    "build/",            # repo-root build output
    ".kernel-build/",    # per-variant kernel build dir
    "docs/",             # LaTeX and Markdown; no C, excluded to keep scope explicit
)

# Whole generated files that are checked in.  .editorconfig already marks
# rootfs_overlay.c as generated build output; reformatting it would only be
# undone by the next generator run.
EXCLUDED_FILES = ("kernel/fs/rootfs_overlay.c",)

SOURCE_SUFFIXES = (".c", ".h")

# `clang-format --dry-run --Werror` reports one diagnostic per offending
# region, all shaped `path:line:col: error: ... [-Wclang-format-violations]`.
# Older releases spell the same diagnostic slightly differently but always keep
# the `path:line:col:` prefix, so anchor on that and ignore the message text.
DIAG = re.compile(r"^(?P<path>[^\s:][^:]*):\d+:\d+: (?:error|warning): ")

BASELINE_HEADER = """\
# A20OS clang-format ratchet baseline -- consumed by `make check-format`.
#
# Every path below is a first-party .c/.h that did NOT conform to .clang-format
# when the gate landed.  They are grandfathered so the gate can detect *new*
# drift without demanding a tree-wide reformat; a file that is not listed here
# must conform, and a listed file that has since become clean is reported as a
# note so this list can shrink.
#
# Rules for this file:
#   - Only ever shrink.  A path is added by fixing the file, never by
#     re-recording it (see `make format-baseline`, which exists so the shrink
#     is a reviewable diff rather than a silent widening of the exemption).
#   - Generated and vendored trees are out of scope entirely, so a path from
#     kernel/external/ or user/external/ here is a bug in the gate.
#
# Recorded with clang-format {version}.  The set of offending files was measured
# to be byte-identical under clang-format 19.1.7 and 23.1.2, so this baseline is
# not tied to one release; a future version that disagrees shows up as a FAIL
# on files listed here, which is the intended fail-closed direction.
"""


def source_files() -> list[str]:
    """First-party .c/.h, repo-relative, sorted, with the vendored trees cut."""
    out: list[str] = []
    for root in SOURCE_ROOTS:
        base = REPO / root
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            rel = path.relative_to(REPO).as_posix()
            if rel in EXCLUDED_FILES or rel.startswith(EXCLUDED_PREFIXES):
                continue
            out.append(rel)
    return sorted(out)


def clang_format_version(binary: str) -> str:
    r = subprocess.run([binary, "--version"], check=False,
                       capture_output=True, text=True)
    m = re.search(r"\d+\.\d+\.\d+", r.stdout)
    return m.group(0) if m else (r.stdout.strip() or "unknown")


def dirty_files(binary: str, files: list[str]) -> list[str]:
    """Files clang-format would rewrite.

    Uses `--dry-run --Werror`, so clang-format itself decides pass/fail and the
    gate only has to collect the names.  A chunked invocation keeps the argv
    under ARG_MAX on a 996-file tree without a shell.
    """
    dirty: set[str] = set()
    chunk = 200
    for i in range(0, len(files), chunk):
        argv = [binary, "--dry-run", "--Werror", *files[i:i + chunk]]
        r = subprocess.run(argv, cwd=REPO, check=False,
                           capture_output=True, text=True)
        # rc 0 = clean, 1 = violations found, anything else is clang-format
        # failing to run at all (bad binary, unreadable file).  Do not treat
        # that as "clean": a gate that cannot run must say so, not pass.
        if r.returncode not in (0, 1):
            sys.stderr.write(f"error: clang-format exited {r.returncode}\n")
            sys.stderr.write(r.stderr)
            return None
        for line in r.stderr.splitlines():
            m = DIAG.match(line)
            if m:
                dirty.add(m.group("path"))
    return sorted(dirty)


def read_baseline() -> set[str]:
    if not BASELINE.is_file():
        sys.stderr.write(
            f"error: ratchet baseline {BASELINE.relative_to(REPO)} is missing; "
            "regenerate it with `make format-baseline`\n")
        return None
    return {ln.strip() for ln in BASELINE.read_text(encoding="utf-8").splitlines()
            if ln.strip() and not ln.startswith("#")}


def write_baseline(files: list[str], version: str) -> int:
    body = "".join(f"{f}\n" for f in files)
    BASELINE.write_text(BASELINE_HEADER.format(version=version) + body,
                        encoding="utf-8")
    print(f"format-baseline: recorded {len(files)} grandfathered file(s) in "
          f"{BASELINE.relative_to(REPO)} (clang-format {version})")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--clang-format", required=True,
                    help="path to the clang-format binary")
    ap.add_argument("--write", action="store_true",
                    help="re-record the baseline instead of checking it")
    ap.add_argument("--list", action="store_true",
                    help="print the first-party source list and exit")
    a = ap.parse_args()

    files = source_files()
    if a.list:
        print("\n".join(files))
        return 0

    version = clang_format_version(a.clang_format)
    dirty = dirty_files(a.clang_format, files)
    if dirty is None:
        return 1

    if a.write:
        return write_baseline(dirty, version)

    baseline = read_baseline()
    if baseline is None:
        return 1

    in_scope = set(files)
    drifted = [f for f in dirty if f not in baseline]
    # A baseline path that no longer exists, or that fell out of the first-party
    # set, means the exemption is stale in the cheapest possible way to notice.
    stale_missing = sorted(baseline - in_scope)
    stale_clean = sorted((baseline & in_scope) - set(dirty))

    if stale_missing:
        print(f"note: {len(stale_missing)} baseline path(s) no longer exist or "
              "left the first-party set; drop them from "
              f"{BASELINE.relative_to(REPO)}", file=sys.stderr)
        for f in stale_missing:
            print(f"  gone: {f}", file=sys.stderr)
    if stale_clean:
        print(f"note: {len(stale_clean)} baseline file(s) now conform to "
              ".clang-format; drop them from the baseline "
              "(`make format-baseline` rewrites it)", file=sys.stderr)
        for f in stale_clean:
            print(f"  clean: {f}", file=sys.stderr)

    if drifted:
        print(f"check-format: FAIL -- {len(drifted)} file(s) are not in "
              f"{BASELINE.relative_to(REPO)} and do not conform to "
              ".clang-format:", file=sys.stderr)
        for f in drifted:
            print(f"  {f}", file=sys.stderr)
        print(f"\nFix with: clang-format -i {' '.join(drifted[:5])}"
              f"{' ...' if len(drifted) > 5 else ''}\n"
              f"(or re-record deliberately with: make format-baseline)",
              file=sys.stderr)
        return 1

    print(f"check-format: PASS -- {len(files)} first-party source(s) checked "
          f"({len(baseline)} grandfathered), clang-format {version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
