"""Extract the smoke/gate recipes from the makefiles into a structured form.

This is a one-shot migration aid, not runtime code: it reads the .mk sources,
substitutes the make variables (values fetched in one round trip so we never
re-derive them here) and prints a JSON description of every QEMU-launching
target.  tools/smoke_cases.py is generated from that JSON.

Kept in-tree so the extraction is reproducible and reviewable rather than a
one-off script that lived in /tmp and was lost.
"""

from __future__ import annotations

import json
import re
import shlex
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MK_FILES = ["tools/targets-smoke.mk", "tools/targets-native-smoke.mk",
            "tools/targets-steps.mk", "tools/driver-modules.mk",
            "tools/targets-mlibc.mk"]

# Targets whose QEMU invocation must not be rewritten: they are launched by
# something other than this extractor, or they carry semantics the schema below
# does not model.
# run-stm32f103-qemu-impl: an MCU emulation (stm32vldiscovery) with no -m/-smp.
# smoke-arch-mmu-matrix: runs several QEMUs in a loop over arch/MMU combinations,
#   so it has no single argv, log or pass pattern and is out of this schema.
# Both stay in make.  Keeping them out of the parse is what lets a genuine parse
# failure remain a meaningful signal instead of a known-exception to ignore.
SKIP = {"run-stm32f103-qemu-impl", "smoke-arch-mmu-matrix"}

# Failure logic richer than "every pass pattern is present -> PASS".  These keep
# their make recipes:
#   * eight of them branch on the timeout status to print a different message
#     (`elif [ "$status" -eq 124 ]`), and
#   * smoke-socket-stress passes only when SOCKET_STRESS: PASS is present AND
#     no [LOCK] line is.  A pattern-membership check cannot express that
#     negation, so migrating it would turn a LOCK warning into a silent pass --
#     strictly weaker than the gate is today.
RICH_FAILURE = {
    "smoke-smp-bringup", "smoke-a20-channel", "smoke-ptrace", "smoke-network-suite",
    "smoke-netctl", "smoke-network-suite-aarch64", "smoke-io-event",
    "smoke-driver-lifecycle", "smoke-socket-stress",
}


def make_vars(names: list[str]) -> dict[str, str]:
    # Values may span several lines (PROTOCOLS_LINES and friends), so a bare
    # newline-per-variable split silently misaligns.  Bracket each name and
    # pair by name instead of by position.
    recipe = ("a20q:;@"
              + "".join(f"printf '@@{n}@%s\\n' '$({n})'; " for n in names))
    out = subprocess.run(["make", "-s", f"--eval={recipe}", "a20q"],
                         cwd=REPO, capture_output=True, text=True, check=True)
    vals: dict[str, str] = {}
    cur: str | None = None
    buf: list[str] = []
    for line in out.stdout.splitlines():
        m = re.fullmatch(r"@@(\w+)@(.*)", line)
        if m:
            if cur is not None:
                vals[cur] = "\n".join(buf)
            cur, buf = m.group(1), [m.group(2)]
        elif cur is not None:
            buf.append(line)
    if cur is not None:
        vals[cur] = "\n".join(buf)
    missing = [n for n in names if n not in vals]
    if missing:
        raise SystemExit(f"make did not report: {missing}")
    return vals


def join(body: str) -> str:
    """Collapse make/shell line continuations into one logical line."""
    return re.sub(r"\\\n\s*", " ", body)


def parse_target(name: str, body: str, V: dict[str, str]) -> dict:
    text = join(body)

    # $(MAKE) is the recursive build invocation we want to capture as data, not
    # a variable to expand, so it is rewritten to its literal value first.
    text = text.replace("$(MAKE)", "make")

    # The gate is a macro call in the source; only make expands it.  Matching
    # the expanded form instead silently yields gate=None for every target, and
    # migrating on that would strip the resource preflight from all of them.
    g = re.search(r"\$\(call smoke-gate,([^,]+),([^)]+)\)", text)
    case: dict = {"name": name,
                  "gate": {"mem": g.group(1), "cpus": g.group(2)} if g else None}

    # Commands issued before the recursive build: some gates must delete a
    # driver package so it is rebuilt with a smoke-test define, and dropping
    # those would leave the gate testing a stale binary.
    pre: list[str] = []
    head = text[:text.index("make ")] if "make " in text else ""
    for ln in head.split("\n"):
        t = ln.strip()
        if not t or t.startswith("@mkdir") or t.startswith("set -e") \
                or "smoke-gate" in t or t.endswith("status=0;"):
            continue
        pre.append(t)
    case["pre"] = pre

    b = re.search(r"\bmake\s+(.*?)\b(dev-build|kernel-only|image-world)\b", text)
    if b:
        # Keep only VAR=VALUE tokens; make flags such as -j1 belong to the
        # invocation, not to the case description.
        bvars = [w for w in b.group(1).split() if re.fullmatch(r"[A-Z_][A-Z_0-9]*=\S+", w)]
    else:
        bvars = []
    case["build"] = {"vars": bvars, "target": b.group(2)} if b else None

    def sub(m: str) -> str:
        key = m.group(1)
        if key not in V:
            raise KeyError(key)
        return V[key]

    text = re.sub(r"\$\((\w+)\)", sub, text)
    # In a recipe "$$" is make's escape for a literal shell "$".  We capture
    # data rather than shell text, so unescape it wholesale -- doing it only for
    # "$$log" left $$wav / $$monsock / $$image wrong in the captured argv.
    text = text.replace("$$", "$")

    lg = re.search(r'log="([^"]+)"', text)
    case["log"] = lg.group(1) if lg else None

    if "{ sleep" in text and "printf" in text:
        pl = re.search(r"printf '([^']*)'", text)
        lines = [x for x in pl.group(1).split("\\n") if x]
        case["stdin"] = {"kind": "pipe", "delay": int(V["SMOKE_INPUT_DELAY"]),
                         "lines": lines}
    elif "--send-line" in text:
        ex = re.search(r"--expect '([^']*)'", text)
        sl = re.findall(r"--send-line '([^']*)'", text)
        case["stdin"] = {"kind": "sendline",
                         "expect": ex.group(1) if ex else None, "lines": sl}
    else:
        case["stdin"] = None

    to = re.search(r"run_with_timeout\.py\s+(?:--expect '[^']*'\s+)?"
                   r"(?:--send-line '[^']*'\s+)*(\S+)\s+(qemu-system-\S+)", text)
    if not to:
        raise ValueError("no timeout/qemu pair found")
    case["timeout"] = to.group(1)
    case["qemu"] = to.group(2)

    start = text.index(case["qemu"])
    stop = text.index('> "$log"', start)
    case["argv"] = shlex.split(text[start:stop].strip())

    pats = re.findall(r"grep -q '([^']*)' \"\$log\"", text)
    if not pats:
        raise ValueError("no pass patterns found")
    case["expect"] = pats

    pm = re.search(r'echo "([^"]*PASS[^"]*)"', text)
    case["pass_msg"] = pm.group(1) if pm else f"{name}: PASS; log saved to $log"
    return case


# The migration replaced these recipes in the working tree, so the QEMU command
# lines no longer exist there -- extraction from the live .mk files is circular.
# Read the pinned pre-migration revision instead, which also makes the generated
# table reproducible no matter how the .mk files change afterwards.
SOURCE_REV = "363f5ec9061f0269995e0f2d58ec6ca7e96a6de2"


def main() -> int:
    src = "\n".join(
        subprocess.run(["git", "show", f"{SOURCE_REV}:{f}"], cwd=REPO,
                       capture_output=True, text=True, check=True).stdout
        for f in MK_FILES)
    # Fetch every variable the sources reference rather than an allowlist: an
    # allowlist silently drops the first target that needs a variable nobody
    # thought of (it did: FAT32_IMG, PYTHON, UFS/UBD_SCRATCH_IMG).  One make
    # round trip answers them all, and it keeps the "ask make, never re-derive"
    # rule that a20_make.query_make already documents.
    used = sorted(set(re.findall(r"\$\((\w+)\)", src)) - {"MAKE", "PYTHON"})
    V = make_vars(used)
    V["PYTHON"] = subprocess.run(["make", "-s", "--eval=a20p:;@echo $(PYTHON)", "a20p"],
                                 cwd=REPO, capture_output=True, text=True,
                                 check=True).stdout.strip()

    cases, failed = [], []
    for m in re.finditer(r"^([a-zA-Z][\w.-]*):\n((?:\t.*(?:\n|\\\n))*)", src, re.M):
        name, body = m.group(1), m.group(2)
        if "qemu-system-" not in body or name in SKIP or name in RICH_FAILURE:
            continue
        try:
            cases.append(parse_target(name, body, V))
        except Exception as exc:  # noqa: BLE001 - report, do not abort the sweep
            failed.append((name, f"{type(exc).__name__}: {exc}"))

    total = len(cases) + len(failed)
    print(f"parsed {len(cases)}/{total}", file=sys.stderr)
    for n, e in failed:
        print(f"  FAILED {n}: {e}", file=sys.stderr)
    Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/smoke_cases.json").write_text(
        json.dumps(cases, indent=1))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
