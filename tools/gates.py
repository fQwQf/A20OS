#!/usr/bin/env python3
"""Run the source-level contract assertions in tools/gates.toml.

These checks used to be ~97 inline `rg -q` recipes in tools/targets-gates.mk.
They are assertions about what appears in which file, so the natural shape is a
table (tools/gates.toml) plus one runner, not 97 shell lines.

`rg -q` is silent on success and on failure alike, which makes a failing gate
unhelpful: you learn that `rg` returned 1 but not what it found.  So a failing
assertion is re-run without `-q` to print the matching lines, and the gate names
the assertion that broke.  The pass/fail decision is identical to the recipes.
"""

from __future__ import annotations

import argparse
import glob as globlib
import os
import re
import subprocess
import sys
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TOML = Path(__file__).resolve().parent / "gates.toml"

# rg exit codes: 0 = match, 1 = no match, 2 = error (missing file, bad regex).
RC_MATCH, RC_NOMATCH, RC_ERROR = 0, 1, 2


def load_gates() -> dict[str, dict]:
    doc = tomllib.loads(TOML.read_text(encoding="utf-8"))
    return {g["name"]: g for g in doc["gate"]}


def expand(files: list[str]) -> list[str]:
    """Expand `*` in a path, the way the shell did when these were recipes.

    Kept as expansion rather than a baked file list so that dropping a new
    source file into a checked directory is covered without editing the table.
    A glob that matches nothing is an error, mirroring the shell passing the
    unexpanded word to rg, which then failed.
    """
    out: list[str] = []
    for f in files:
        if any(c in f for c in "*?["):
            hits = sorted(globlib.glob(f, root_dir=REPO))
            if not hits:
                raise SystemExit(f"error: pattern {f!r} matched no files")
            out.extend(hits)
        else:
            out.append(f)
    return out


def _run_build_must_fail(a: dict) -> tuple[bool, str]:
    """Assert that `make ARCH=<arch> NOMMU=1 <target>` is REJECTED.

    The polarity is inverted on purpose: these architectures must refuse a
    NOMMU build, so a zero exit status is the failure.  make rejects them in its
    variable-validation stage (Makefile:115), before invoking a compiler, so no
    cross toolchain is needed to run this.
    """
    spec = a["build_must_fail"]
    r = subprocess.run(["make", "-s", f"ARCH={spec['arch']}", "NOMMU=1",
                        spec["target"]], cwd=REPO, check=False,
                       capture_output=True, text=True)
    if r.returncode == 0:
        return False, (f"unsupported NOMMU build accepted for {spec['arch']}")
    return True, ""


def run_one(a: dict, files: list[str]) -> tuple[bool, str]:
    """Return (ok, detail).  detail is empty when ok.

    `rg_flags` carries the flags the original recipe needed.  Two of them are
    load-bearing rather than cosmetic: `--pcre2` is required by the lookaround
    patterns (rg's default Rust-regex engine cannot compile them), and `-U` is
    required by the patterns that span lines, since without it rg matches
    per-line and can never match at all.
    """
    if a.get("exists"):
        return _run_exists(a)
    if a.get("build_must_fail"):
        return _run_build_must_fail(a)

    argv = ["rg", "-q", *a.get("rg_flags", ())]
    if a.get("fixed"):
        argv.append("-F")
    # Globs must precede the path: rg treats a `--glob` that appears after a
    # positional argument as another path, silently dropping the filter
    # (measured: 23 matches with the glob last, 17 with it first).
    for g in a.get("globs", [a["glob"]] if a.get("glob") else []):
        argv += ["--glob", g]
    # A pattern may start with '-' (e.g. '->state'); rg then needs `--` or it
    # reads the pattern as a flag.
    if a.get("ddash"):
        argv.append("--")
    argv.append(a["pattern"])
    argv.extend(files)

    if a.get("post_filter"):
        return _run_post_filtered(a, argv)

    r = subprocess.run(argv, cwd=REPO, check=False, capture_output=True, text=True)
    negate = bool(a.get("negate", False))

    if r.returncode == RC_ERROR:
        return False, f"rg error: {r.stderr.strip()}"

    matched = r.returncode == RC_MATCH
    if matched == negate:
        # Positive + no match, or negated + match: the assertion is violated.
        if negate:
            show = ["rg", "-n", *a.get("rg_flags", ())]
            for g in a.get("globs", [a["glob"]] if a.get("glob") else []):
                show += ["--glob", g]
            if a.get("ddash"):
                show.append("--")
            show += [a["pattern"], *files]
            shown = subprocess.run(show, cwd=REPO, check=False,
                                   capture_output=True, text=True)
            return False, (f"pattern {a['pattern']!r} must not appear in "
                           f"{', '.join(files)}\n{shown.stdout}")
        return False, f"pattern {a['pattern']!r} not found in {', '.join(files)}"
    return True, ""


def _run_exists(a: dict) -> tuple[bool, str]:
    """`test -r <file> && ...`: every listed path must be a readable file.

    Kept as a table entry rather than left in the recipe so the gate can stay a
    single delegation: the original interleaves these existence guards between
    rg assertions, and moving only the rg lines would reorder the checks.
    """
    missing = [f for f in a["exists"] if not os.access(REPO / f, os.R_OK)]
    if missing:
        return False, "missing required file(s): " + ", ".join(missing)
    return True, ""


def _run_post_filtered(a: dict, argv: list[str]) -> tuple[bool, str]:
    """`bad=$(rg ... | rg -v WHITELIST)`; require nothing to survive the filter.

    These assertions cannot be expressed as an rg exit code: the whitelist drops
    the files allowed to contain the pattern, so the verdict depends on the
    surviving *output*, which has to be captured and filtered here.
    """
    r = subprocess.run(["rg", "-n", *argv[2:]], cwd=REPO, check=False,
                       capture_output=True, text=True)
    if r.returncode not in (0, 1):
        return False, f"rg error: {r.stderr.strip()}"
    keep = [ln for ln in r.stdout.splitlines()
            if not re.search(a["post_filter"], ln)]
    if keep:
        return False, (f"pattern {a['pattern']!r} appears outside the allowed "
                       f"files:\n" + "\n".join(keep[:20]))
    return True, ""


def run_gate(name: str, gate: dict, segment: int | None = None) -> int:
    files_cache: dict[tuple, list[str]] = {}
    failures: list[str] = []
    nseg = gate.get("segments", 1)
    items = gate["assertion"]
    if segment is not None:
        # A gate whose assertions straddle a codegen step is split into
        # segments so make can run the generator between two calls and the
        # original check order is preserved.
        if not 0 <= segment < nseg:
            raise SystemExit(f"error: segment {segment} out of range (0..{nseg - 1})")
        items = [a for a in items if a.get("segment", 0) == segment]
    for a in items:
        # `exists` assertions name their paths in `exists`, never `files`.
        key = tuple(a.get("files", ()))
        if key and key not in files_cache:
            try:
                files_cache[key] = expand(a["files"])
            except SystemExit as exc:
                failures.append(str(exc))
                files_cache[key] = []
        ok, detail = run_one(a, files_cache.get(key, []))
        if not ok:
            failures.append(detail)

    total = len(items)
    label = gate["make_target"]
    if segment is not None:
        label = f"{label}[{segment}]"
    if failures:
        print(f"{label}: {len(failures)}/{total} assertions FAILED", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1
    print(f"{label}: {total} assertions PASS")
    return 0


def run_host_tests(binaries: list[str]) -> int:
    """Run each host-compiled unit test binary, stopping at the first failure.

    make owns the compilation (HOST_CC, HOST_CFLAGS, the pattern rule); running
    the results is orchestration, so it lives here.  The binaries come from
    make as arguments rather than being re-globbed here, so the set that runs is
    exactly the set make decided was out of date.
    """
    for t in binaries:
        print(f"== {t} ==", flush=True)
        r = subprocess.run([t], cwd=REPO, check=False)
        if r.returncode != 0:
            print(f"host-tests: {t} failed with status {r.returncode}",
                  file=sys.stderr)
            return r.returncode
    print("host-tests: PASS")
    return 0


def run_whitespace(label: str) -> int:
    """`git diff --check`: fail on whitespace errors in the working diff.

    The six check-proc-step*-local targets each ran this after their source
    assertions, so it is a real gate rather than build noise.
    """
    r = subprocess.run(["git", "diff", "--check"], cwd=REPO, check=False,
                       capture_output=True, text=True)
    if r.returncode != 0 or r.stdout.strip():
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        print(f"{label}: whitespace errors in the working diff", file=sys.stderr)
        return 1
    print(f"{label}: PASS")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("gate",
                    help="gate name from tools/gates.toml, 'host-tests' or "
                         "'whitespace'")
    ap.add_argument("--list", action="store_true", help="list gate names and exit")
    ap.add_argument("--binaries", default="",
                    help="space-separated host test binaries (host-tests only)")
    ap.add_argument("--segment", type=int, default=None,
                    help="run only this contiguous run of a split gate")
    ap.add_argument("--label", default="whitespace",
                    help="label for the PASS line (whitespace only)")
    a = ap.parse_args()

    if a.gate == "host-tests":
        binaries = a.binaries.split()
        if not binaries:
            raise SystemExit("error: host-tests needs --binaries")
        return run_host_tests(binaries)
    if a.gate == "whitespace":
        return run_whitespace(a.label)

    gates = load_gates()
    if a.list:
        for name, g in gates.items():
            print(f"{name:32} {len(g['assertion']):3} assertions  ({g['make_target']})")
        return 0
    if a.gate not in gates:
        raise SystemExit(f"error: unknown gate {a.gate!r}; known: "
                         f"{', '.join(sorted(gates))}")
    return run_gate(a.gate, gates[a.gate], a.segment)


if __name__ == "__main__":
    raise SystemExit(main())
