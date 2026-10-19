#!/usr/bin/env python3
"""Compile and run the production core-dump layout Rust tests.

`kernel/proc/coredump_layout.rs` is the production source; this harness only
asks rustc for the `--test` build of that exact file, so the assertions below
are the same ones that ship in the kernel object.  The tests compare the wire
bytes against the ELF64 and Linux note layouts the C adapter writes.
"""

from __future__ import annotations

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "kernel/proc/coredump_layout.rs"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="a20-coredump-layout-") as tmp:
        binary = pathlib.Path(tmp) / "coredump-layout-tests"
        subprocess.run(
            ["rustc", "--edition=2021", "--test", str(SOURCE), "-o", str(binary)],
            check=True,
            cwd=ROOT,
        )
        return subprocess.run([str(binary), "--nocapture"], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
