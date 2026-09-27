#!/usr/bin/env python3
"""Run a smoke/gate case described in tools/smoke_cases.py.

The case table used to live as ~1900 lines of shell embedded in the .mk files.
That shell was unreviewable, unlintable, and its failure modes were invisible:
a target that printed nothing looked exactly like a target that passed.  Each
case is now data here, and this module is the single interpreter for it.

Order of operations per case mirrors what the make recipe did:
  1. resource gate  (tools/a20_resource.py) -- wait rather than risk the OOM killer
  2. pre-build commands (some gates delete a driver so it rebuilds with a
     smoke-test define)
  3. recursive build
  4. QEMU under tools/run_with_timeout.py, log to file
  5. every pass pattern must appear in the log, else dump the tail and fail

`--print-argv` exists so the invocation can be diffed against make; it is what
makes a future edit to this file falsifiable.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

from smoke_cases import CASES  # noqa: E402

TIMEOUT_HELPER = "tools/run_with_timeout.py"
GATE = "tools/a20_resource.py"
# CI must fail rather than hang when the host is genuinely out of room; an
# interactive `a20 run` waits forever, so the ceiling is here instead.
GATE_WAIT_S = os.environ.get("A20_WAIT_TIMEOUT", "900")


def sh(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=REPO, check=False, **kw)


def run_gate(case: dict) -> None:
    g = case.get("gate")
    if not g:
        return
    env = dict(os.environ, A20_WAIT_TIMEOUT=GATE_WAIT_S)
    r = sh([sys.executable, GATE, "-m", g["mem"], "-c", str(g["cpus"])], env=env)
    if r.returncode != 0:
        raise SystemExit(r.returncode)


def run_build(case: dict) -> None:
    for cmd in case.get("pre") or []:
        if sh(["bash", "-c", cmd]).returncode != 0:
            raise SystemExit(f"pre-build command failed: {cmd}")
    b = case.get("build")
    if not b:
        return
    if sh(["make", *b["vars"], b["target"]]).returncode != 0:
        raise SystemExit(f"build failed: make {' '.join(b['vars'])} {b['target']}")


def qemu_argv(case: dict) -> list[str]:
    """The full QEMU command line, as the make recipe invoked it."""
    argv = [sys.executable, TIMEOUT_HELPER]
    stdin = case.get("stdin")
    if stdin and stdin["kind"] == "sendline":
        if stdin.get("expect"):
            argv += ["--expect", stdin["expect"]]
        for ln in stdin["lines"]:
            argv += ["--send-line", ln]
    argv.append(case["timeout"])
    argv += case["argv"]
    return argv


def run_qemu(case: dict) -> int:
    log = Path(case["log"])
    log.parent.mkdir(parents=True, exist_ok=True)
    argv = qemu_argv(case)
    stdin = case.get("stdin")
    with log.open("wb") as fh:
        if stdin and stdin["kind"] == "pipe":
            payload = "".join(f"{ln}\n" for ln in stdin["lines"]).encode()
            proc = subprocess.Popen(argv, cwd=REPO, stdin=subprocess.PIPE,
                                    stdout=fh, stderr=subprocess.STDOUT)
            time.sleep(stdin["delay"])
            try:
                proc.stdin.write(payload)
                proc.stdin.flush()
                proc.stdin.close()
            except (BrokenPipeError, ValueError):
                pass
            return proc.wait()
        return sh(argv, stdout=fh, stderr=subprocess.STDOUT).returncode


def grep_matches(pattern: str, text: str) -> bool:
    """`grep -q PATTERN` semantics, not substring containment.

    The recipes used `grep -q`, so these are regular expressions: 57 of the 146
    patterns rely on that (escaped brackets such as \\[HDA\\], wildcards like
    FAKELD:.*ok, and line anchors like ^valid_pages:).  A substring test fails
    every one of them.  MULTILINE reproduces grep's per-line matching so ^ and
    $ anchor to line boundaries.
    """
    return re.search(pattern, text, re.MULTILINE) is not None


def report(name: str, case: dict, status: int) -> int:
    log = Path(case["log"])
    text = log.read_text(encoding="utf-8", errors="replace")
    missing = [p for p in case["expect"] if not grep_matches(p, text)]
    if not missing:
        # pass_msg was captured from the make recipe, where $log was a shell
        # variable; render it with this case's real path.
        print(case["pass_msg"].replace("$log", str(log)))
        return 0
    print(f"{name}: failed with status {status}; missing "
          f"{missing!r}; tail of {log}:")
    print("\n".join(text.splitlines()[-80:]))
    return 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("case")
    ap.add_argument("--print-argv", action="store_true",
                    help="print the QEMU argv and exit (for diffing against make)")
    ap.add_argument("--list", action="store_true", help="list case names and exit")
    a = ap.parse_args(argv)
    if a.list:
        print("\n".join(sorted(CASES)))
        return 0
    if a.case not in CASES:
        print(f"unknown case {a.case!r}; try --list", file=sys.stderr)
        return 2
    case = CASES[a.case]
    if a.print_argv:
        print(" ".join(qemu_argv(case)))
        return 0
    run_gate(case)
    run_build(case)
    status = run_qemu(case)
    return report(a.case, case, status)


if __name__ == "__main__":
    raise SystemExit(main())
