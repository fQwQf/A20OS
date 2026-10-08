#!/usr/bin/env python3
"""Reject x86 kernel driver modules that can clobber unsaved user FP state."""

from __future__ import annotations

import re
import subprocess
import sys


FP_REG = re.compile(r"%(?:xmm\d+|ymm\d+|zmm\d+|mm[0-7]|st(?:\(\d+\))?)\b")
FP_MNEMONIC = re.compile(
    r"\b(?:vzeroupper|emms|ldmxcsr|stmxcsr|fxsave\w*|fxrstor\w*|"
    r"xsave\w*|xrstor\w*|f[a-z]{2,})\b"
)
INSTRUCTION = re.compile(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2}\s+)+([a-z][a-z0-9.]*)\b")


def run(*argv: str) -> str:
    return subprocess.check_output(argv, text=True, stderr=subprocess.STDOUT)


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: check_x86_drvmod_isa.py MODULE...", file=sys.stderr)
        return 2
    failures = []
    for path in sys.argv[1:]:
        header = run("x86_64-linux-gnu-readelf", "-h", path)
        if not re.search(r"Type:\s+REL \(Relocatable file\)", header):
            failures.append(f"{path}: expected an ET_REL driver object")
            continue
        disassembly = run("x86_64-linux-gnu-objdump", "-d", path)
        regs = sorted(set(FP_REG.findall(disassembly)))
        mnemonics = sorted({
            match.group(1)
            for line in disassembly.splitlines()
            if (match := INSTRUCTION.match(line))
            and FP_MNEMONIC.fullmatch(match.group(1))
        })
        if regs or mnemonics:
            bad = regs + mnemonics
            failures.append(f"{path}: forbidden FP/SIMD ISA: {', '.join(bad)}")
    if failures:
        print("x86 drvmod ISA check: FAIL")
        print("\n".join(f"  {item}" for item in failures))
        return 1
    print(f"x86 drvmod ISA check: PASS ({len(sys.argv) - 1} ET_REL modules, no FP/SIMD)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
