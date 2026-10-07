#!/usr/bin/env python3
"""Fail closed on `file.c:123` citations in docs/ that no longer resolve.

A line-number citation is a promise: "the thing I am describing is here".  The
tree has no gate for that promise.  `check-doc-drift` resolves cited *paths* for
a keyword gate but never a line number, so a doc can point a reader at
`lock.h:62` for years after the code it names has moved to `lock.h:71` and every
gate still passes -- which is exactly what happened: 353 line citations across
145 files, with drifted ones no tool could see.

This checks the mechanically decidable half of that promise.  For every
`path:NNN[-MMM]` in a markdown file:

  * the path resolves to a file in this tree  -> the cited range must be inside
    that file's current line count.  A file that grew keeps an old citation
    valid-looking; a file that shrank, or a function that moved, invalidates it.
  * the basename exists nowhere in the tree    -> the citation points outside
    A20OS (upstream mesa, lkml, wlroots).  Reported, never failed: an external
    reference is a legitimate thing for a doc to cite and this gate has no
    network and no upstream checkouts.

What it deliberately does NOT check is whether the cited line still says what
the sentence claims.  That needs a reader, not a regex; pretending otherwise
would produce a gate that cries wolf and gets ignored.  Range validity is the
part that can be decided exactly, so only that part is a gate.

See docs/CONTRIBUTING.md for how to run it.
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Directories never worth searching for a cited basename: build output, the
# unpacked qemu source, and VCS metadata.  Descending into them is slow and any
# match found there is noise, not a documentation target.
SKIP_DIRS = {".git", ".kernel-build", "qemu-10.0.13+ds", "node_modules",
             "user/build", "__pycache__"}

# Source extensions a citation may legitimately point at.
SRC_EXT = {".c", ".h", ".S", ".ld", ".mk", ".py", ".rs", ".toml", ".sh",
           ".inc", ".ld.S", ".md"}

# Some upstream trees use distinctive internal roots that may collide with a
# first-party basename.  In particular QEMU's include/standard-headers tree is
# not mirrored at the repository root; `virtio_net.h` there must not fall
# through to the unrelated A20OS driver header with the same basename.  Exact
# in-tree paths and the conventional kernel-relative paths above are resolved
# before this check, so a real tracked target still gets line-count checking.
EXTERNAL_PATH_MARKERS = (("standard-headers", "linux"),)

CITE = re.compile(
    r'(?P<path>(?:[\w./-]+/)?[\w.-]+\.(?:c|h|S|ld|mk|py|rs|toml|sh|inc))'
    r':(?P<a>\d+)(?:-(?P<b>\d+))?'
)

# Docs whose line citations cannot be checked yet, with the reason.  A line
# citation is only stable while its target is; these point into a subtree that
# another branch is actively rewriting, so every number here is expected to move
# and "fixing" it now would just be wrong again on merge day.
#
# Each entry is removed when the branch it names lands -- that is the moment the
# numbers stop moving and the doc can be checked like any other.
KNOWN_MOVING: dict[str, str] = {}


def find_all(named):
    """Every path in the tree whose basename is `named`."""
    hits = []
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            if fn == named:
                hits.append(os.path.relpath(os.path.join(dirpath, fn), ROOT))
    return hits


def resolve(path):
    """Map a cited path onto a tree-relative path.

    Returns (target, ambiguous).  `target` is None when nothing in this tree
    carries that basename -- the citation points outside A20OS and there is
    nothing to check.  `ambiguous` is True when several files share the
    basename and no path prefix in the citation picks one; a bare `trap.c` next
    to a sentence about `user_trap_handler` says nothing to distinguish
    kernel/core/trap.c from eight arch trap.c files, and guessing would make
    this gate fail on correct citations.

    Docs write citations the way a person says them: `sched.c:1790` for a file
    they are already talking about, `x86_64/trap/irqchip.c` for an arch
    subdirectory, `mm/vm.c` for a kernel subsystem.  Trying those shapes in
    order beats reporting a false external reference for a file that is right
    there under `kernel/`.
    """
    if os.path.isfile(os.path.join(ROOT, path)):
        return path, False

    candidates = [
        os.path.join("kernel", path),           # mm/vm.c, proc/sched.c
        os.path.join("kernel/arch", path),       # x86_64/trap/irqchip.c
        os.path.join("kernel/include", path),    # core/lock.h
        os.path.join("kernel/fs", path),
        os.path.join("user/svc", path),
    ]
    for cand in candidates:
        if os.path.isfile(os.path.join(ROOT, cand)):
            return cand, False

    parts = path.split("/")
    if any(tuple(parts[i:i + len(marker)]) == marker
           for marker in EXTERNAL_PATH_MARKERS
           for i in range(len(parts) - len(marker) + 1)):
        return None, False

    hits = find_all(os.path.basename(path))
    if not hits:
        return None, False
    if len(hits) == 1:
        return hits[0], False

    def rank(h):
        vendored = 1 if h.startswith("user/external/") else 0
        if h.startswith("kernel/"):
            tier = 0
        elif h.startswith(("user/svc/", "user/cmds/", "user/libs/")):
            tier = 1
        else:
            tier = 2
        common = len(os.path.commonprefix([h, path]))
        return (vendored, tier, -common)

    ordered = sorted(hits, key=rank)
    # rank() puts vendored trees last and first-party kernel/user code first, so
    # a citation like `mmap.c:300` in a doc talking about the kernel means
    # kernel/mm/mmap.c even though user/external/musl/src/mman/mmap.c exists.
    # Docs describe this tree, not the vendored copies inside it, so that is not
    # a guess -- it is the only reading the sentence can have.  Settle it, or
    # else every such citation goes unchecked forever.
    if rank(ordered[0])[:2] < rank(ordered[1])[:2]:
        return ordered[0], False
    # Genuinely ambiguous: same tier, e.g. bare `trap.c` beside prose about
    # user_trap_handler, where kernel/core/trap.c and eight arch trap.c files
    # all match equally.  Skip rather than guess.
    return ordered[0], True


def line_count(rel):
    try:
        with open(os.path.join(ROOT, rel), "rb") as fh:
            return sum(1 for _ in fh)
    except OSError:
        return None


def markdown_files():
    for sub in ("docs", "kernel", "user", "tools"):
        base = os.path.join(ROOT, sub)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for fn in sorted(filenames):
                if fn.endswith(".md"):
                    full = os.path.join(dirpath, fn)
                    yield os.path.relpath(full, ROOT), full


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--verbose", action="store_true",
                    help="also list the external references that were skipped")
    args = ap.parse_args()

    checked = 0
    external = []
    ambiguous_hits = []
    log_lines = []
    failures = []
    skipped_docs = []

    for rel, full in markdown_files():
        if rel in KNOWN_MOVING:
            skipped_docs.append(rel)
            continue
        with open(full, encoding="utf-8", errors="replace") as fh:
            for lineno, text in enumerate(fh, 1):
                for m in CITE.finditer(text):
                    cited = m.group("path")
                    if os.path.splitext(cited)[1] not in SRC_EXT:
                        continue
                    # `[foo.c:122] some message` is a quoted program log line
                    # with the emitting program's own path in the prefix --
                    # swaybg, labwc, ALSA, wlroots.  Those name files that are
                    # not in this tree and never were; they are evidence, not
                    # citations, and must not be "fixed" to point somewhere.
                    if m.start() > 0 and text[m.start() - 1] == "[":
                        log_lines.append((rel, lineno, m.group(0)))
                        continue
                    first = int(m.group("a"))
                    last = int(m.group("b")) if m.group("b") else first
                    if first < 1 or last < first:
                        failures.append(
                            f"{rel}:{lineno}: malformed range `{m.group(0)}`")
                        continue

                    target, ambiguous = resolve(cited)
                    if target is None:
                        external.append((rel, lineno, m.group(0)))
                        continue
                    if ambiguous:
                        ambiguous_hits.append((rel, lineno, m.group(0)))
                        continue

                    total = line_count(target)
                    if total is None:
                        continue
                    checked += 1
                    if last > total:
                        failures.append(
                            f"{rel}:{lineno}: `{m.group(0)}` points past the end "
                            f"of {target} ({total} lines)")

    for doc in sorted(skipped_docs):
        print(f"  SKIP {doc}: {KNOWN_MOVING[doc]}")

    if args.verbose:
        for rel, lineno, raw in external:
            print(f"  external {rel}:{lineno}: {raw} (not in this tree)")
        for rel, lineno, raw in log_lines:
            print(f"  log-line {rel}:{lineno}: {raw} (quoted program output)")
        for rel, lineno, raw in ambiguous_hits:
            print(f"  ambiguous {rel}:{lineno}: {raw} (basename not unique)")

    print(f"check-doc-citations: {checked} in-tree line citations checked, "
          f"{len(external)} external references skipped, "
          f"{len(log_lines)} quoted log lines skipped, "
          f"{len(ambiguous_hits)} ambiguous basenames skipped, "
          f"{len(skipped_docs)} docs skipped as in-flight")

    if failures:
        print()
        print("check-doc-citations: FAIL -- "
              f"{len(failures)} citation(s) no longer resolve:", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        print("\n  Re-point each at the line the symbol is on now, or drop the\n"
              "  line number if the file is quoted rather than cited.  A\n"
              "  citation that points past the end of a file is a reader\n"
              "  landing on nothing.", file=sys.stderr)
        return 1

    print("check-doc-citations: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
