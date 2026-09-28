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


def run_one(a: dict, files: list[str]) -> tuple[bool, str]:
    """Return (ok, detail).  detail is empty when ok.

    `rg_flags` carries the flags the original recipe needed.  Two of them are
    load-bearing rather than cosmetic: `--pcre2` is required by the lookaround
    patterns (rg's default Rust-regex engine cannot compile them), and `-U` is
    required by the patterns that span lines, since without it rg matches
    per-line and can never match at all.
    """
    argv = ["rg", "-q", *a.get("rg_flags", ())]
    if a.get("fixed"):
        argv.append("-F")
    argv.append(a["pattern"])
    argv.extend(files)
    if a.get("glob"):
        argv += ["-g", a["glob"]]

    r = subprocess.run(argv, cwd=REPO, check=False, capture_output=True, text=True)
    negate = bool(a.get("negate", False))

    if r.returncode == RC_ERROR:
        return False, f"rg error: {r.stderr.strip()}"

    matched = r.returncode == RC_MATCH
    if matched == negate:
        # Positive + no match, or negated + match: the assertion is violated.
        if negate:
            shown = subprocess.run(
                ["rg", "-n", *a.get("rg_flags", ()), a["pattern"], *files]
                + (["-g", a["glob"]] if a.get("glob") else []),
                cwd=REPO, check=False, capture_output=True, text=True)
            return False, (f"pattern {a['pattern']!r} must not appear in "
                           f"{', '.join(files)}\n{shown.stdout}")
        return False, f"pattern {a['pattern']!r} not found in {', '.join(files)}"
    return True, ""


def run_gate(name: str, gate: dict) -> int:
    files_cache: dict[tuple, list[str]] = {}
    failures: list[str] = []
    for a in gate["assertion"]:
        key = tuple(a["files"])
        if key not in files_cache:
            try:
                files_cache[key] = expand(a["files"])
            except SystemExit as exc:
                failures.append(str(exc))
                files_cache[key] = []
        ok, detail = run_one(a, files_cache[key])
        if not ok:
            failures.append(detail)

    total = len(gate["assertion"])
    label = gate["make_target"]
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("gate", help="gate name from tools/gates.toml, or 'host-tests'")
    ap.add_argument("--list", action="store_true", help="list gate names and exit")
    ap.add_argument("--binaries", default="",
                    help="space-separated host test binaries (host-tests only)")
    a = ap.parse_args()

    if a.gate == "host-tests":
        binaries = a.binaries.split()
        if not binaries:
            raise SystemExit("error: host-tests needs --binaries")
        return run_host_tests(binaries)

    gates = load_gates()
    if a.list:
        for name, g in gates.items():
            print(f"{name:32} {len(g['assertion']):3} assertions  ({g['make_target']})")
        return 0
    if a.gate not in gates:
        raise SystemExit(f"error: unknown gate {a.gate!r}; known: "
                         f"{', '.join(sorted(gates))}")
    return run_gate(a.gate, gates[a.gate])


if __name__ == "__main__":
    raise SystemExit(main())
