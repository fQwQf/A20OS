#!/usr/bin/env python3
"""Boot an instance: the QEMU launch that used to live in run-targets.mk.

Running a guest is not compilation, so the launch itself belongs here.  What
stays in make is the *build* that has to happen first and the variable values
that describe the machine.

The argv is not written down here.  It is obtained by asking make to expand
`_qemu_argv` / `_qemu_argv_debug`, targets whose only job is to print
`$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF)`.  A second copy of QEMU_FLAGS in
Python would be a second source of truth, and it is exactly the kind that rots
silently when someone adds a device.
"""

from __future__ import annotations

import argparse
import re
import shlex
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
# QEMU's `-s` is shorthand for `-gdb tcp::1234`; the port comes from QEMU, not
# from this repository, so it is named here rather than configured twice.
GDB_PORT = 1234


def make_argv(target: str, defines: list[str]) -> list[str]:
    """Ask make to print the QEMU command line it owns.

    Run it rather than -n: the argv targets are printf recipes with no
    prerequisites, so nothing is built, and `make -n` would print the printf
    invocation instead of its output.
    """
    r = subprocess.run(["make", *defines, target], cwd=REPO,
                       check=False, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"error: 'make {target}' failed:\n{r.stderr.strip()}")
    tok = re.compile(r"(?:^|\s)((?:\S*/)?qemu-system-[A-Za-z0-9_.-]+)\s")
    for line in r.stdout.splitlines():
        m = tok.search(line)
        if m:
            return shlex.split(line[m.start(1):])
    raise SystemExit(f"error: no qemu-system command in 'make {target}' output")


def kernel_of(argv: list[str]) -> Path:
    """The -kernel argument, read from the same argv QEMU will be given.

    Taking it from argv means the emptiness check below can never inspect a
    different file than the one actually booted.
    """
    if "-kernel" not in argv:
        raise SystemExit(f"error: no -kernel in qemu argv: {shlex.join(argv)}")
    return REPO / argv[argv.index("-kernel") + 1]


def gdb_banner(elf: Path) -> str:
    return "\n".join([
        f"Waiting for GDB connection on port {GDB_PORT}...",
        "=" * 58,
        "Please run in another terminal:",
        f"  gdb-multiarch {elf}",
        f"  (gdb) target remote :{GDB_PORT}",
        "=" * 58,
    ])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("mode", choices=["run", "debug"])
    ap.add_argument("--arch", required=True)
    ap.add_argument("--bringup", default="0")
    ap.add_argument("--print-argv", action="store_true",
                    help="print the QEMU argv instead of running it")
    a = ap.parse_args()

    target = "_qemu_argv" if a.mode == "run" else "_qemu_argv_debug"
    defines = [f"ARCH={a.arch}", f"BRINGUP={a.bringup}"]
    argv = make_argv(target, defines)
    if a.print_argv:
        print(shlex.join(argv))
        return 0

    # Build first: the kernel ELF and its root image are make's job.  The debug
    # build is a different kernel, so it must not reuse the dev-build objects.
    build = ["kernel-only"] if a.bringup == "1" else ["dev-build"]
    if a.mode == "debug":
        build = [*build, "OPT=-O0 -g -DDEBUG"]
    b = subprocess.run(["make", *defines, *build], cwd=REPO, check=False)
    if b.returncode != 0:
        return b.returncode

    elf = kernel_of(argv)
    if not elf.is_file() or elf.stat().st_size == 0:
        print(f"ERROR: kernel ELF missing or empty: {elf}", file=sys.stderr)
        return 1

    if a.mode == "debug":
        print(gdb_banner(elf), flush=True)
    print("+ " + shlex.join(argv), flush=True)
    return subprocess.run(argv, cwd=REPO, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
