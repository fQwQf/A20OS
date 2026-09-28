"""Verify the extracted smoke cases against make's own expansion.

For every migrated target this re-derives the QEMU invocation the way make
would print it (`make -n <target> MAKE=true`, which expands every variable but
suppresses the recursive build) and compares it token-for-token with what
tools/smoke_extract.py captured.  The two come from different paths -- make's
own expansion versus our table plus parser -- so a wrong variable value, a bad
substitution or a mis-parse shows up as a diff instead of silently becoming a
changed test gate.

Exits non-zero on any mismatch.
"""

from __future__ import annotations

import json
import re
import shlex
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Same pinned pre-migration source the extractor reads; the live .mk files no
# longer contain these recipes.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from smoke_extract import SOURCE_REV  # noqa: E402

WORKTREE = REPO.parent / (REPO.name + "-verify")


def source_text() -> str:
    return "\n".join(
        subprocess.run(["git", "show", f"{SOURCE_REV}:{f}"], cwd=REPO,
                       capture_output=True, text=True, check=True).stdout
        for f in MK_FILES)


def from_make(target: str) -> dict:
    """make's own expansion, taken in a worktree at the pinned revision.

    Feeding the sources over stdin does not work: these .mk files depend on the
    surrounding Makefile, so they must be expanded in a faithful tree.
    """
    out = subprocess.run(["make", "-n", target, "MAKE=true"], cwd=WORKTREE,
                         capture_output=True, text=True, check=True).stdout
    text = re.sub(r"\\\n\s*", " ", out)
    lg = re.search(r'log="([^"]+)"', text)
    q = re.search(r"(qemu-system-\S+)", text)
    if not q:
        raise SystemExit(f"{target}: no qemu binary in make output")
    to = re.search(r"run_with_timeout\.py\s+(?:--expect '[^']*'\s+)?"
                   r"(?:--send-line '[^']*'\s+)*(\S+)\s+qemu-system", text)
    start = text.index(q.group(1))
    end = text.index('> "$log"', start)
    cond = re.search(r"if (grep .*?); then", text)
    expect: list[str] = []
    forbid: list[str] = []
    if cond:
        for part in cond.group(1).split("&&"):
            m2 = re.search(r"grep -q '([^']*)'", part)
            if m2:
                (forbid if part.lstrip().startswith("!") else expect).append(m2.group(1))
    return {
        "log": lg.group(1) if lg else None,
        "timeout": to.group(1) if to else None,
        "argv": shlex.split(text[start:end].strip()),
        "expect": expect,
        "forbid": forbid,
        "timeout_msg": bool(re.search(r'\[ "?\$status"? -eq 124 \]', text)),
    }


def ensure_worktree() -> None:
    """Create the pinned-revision worktree if it is not already there."""
    if WORKTREE.exists():
        return
    subprocess.run(["git", "worktree", "add", "-q", "--detach", str(WORKTREE),
                    SOURCE_REV], cwd=REPO, check=True)


def main() -> int:
    ensure_worktree()
    cases = json.loads(Path(sys.argv[1]).read_text())
    bad = 0
    for c in cases:
        m = from_make(c["name"])
        for field in ("log", "timeout", "argv", "expect", "forbid", "timeout_msg"):
            if c[field] != m[field]:
                bad += 1
                print(f"MISMATCH {c['name']} [{field}]")
                print(f"  extracted: {c[field]!r}")
                print(f"  from make: {m[field]!r}")
    print(f"\n{len(cases) - bad}/{len(cases)} targets match make exactly "
          f"({bad} field mismatches)")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
