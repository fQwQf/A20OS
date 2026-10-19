#!/usr/bin/env python3
"""Compile and run the production POSIX-timer table Rust tests.

`kernel/proc/posix_timer_state.rs` is the production source; this harness only
asks rustc for the `--test` build of that exact file, so the assertions below
are the same ones that ship in the kernel object.
"""

from __future__ import annotations

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "kernel/proc/posix_timer_state.rs"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="a20-posix-timer-") as tmp:
        binary = pathlib.Path(tmp) / "posix-timer-tests"
        subprocess.run(
            ["rustc", "--edition=2021", "--test", str(SOURCE), "-o", str(binary)],
            check=True,
            cwd=ROOT,
        )
        return subprocess.run([str(binary), "--nocapture"], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
