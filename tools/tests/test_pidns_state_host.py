#!/usr/bin/env python3
"""Compile and run the production PID-namespace Rust tests.

`kernel/proc/pidns_state.rs` is the production source; this harness only asks
rustc for the `--test` build of that exact file.
"""

from __future__ import annotations

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "kernel/proc/pidns_state.rs"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="a20-pidns-") as tmp:
        binary = pathlib.Path(tmp) / "pidns-tests"
        subprocess.run(
            ["rustc", "--edition=2021", "--test", str(SOURCE), "-o", str(binary)],
            check=True,
            cwd=ROOT,
        )
        return subprocess.run([str(binary), "--nocapture"], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
