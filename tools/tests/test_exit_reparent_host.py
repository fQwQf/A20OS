#!/usr/bin/env python3
"""Compile and run the production orphan-reparenting Rust tests."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "kernel/proc/exit_reparent.rs"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="a20-exit-reparent-") as tmp:
        binary = pathlib.Path(tmp) / "exit-reparent-tests"
        subprocess.run(
            ["rustc", "--edition=2021", "--test", str(SOURCE), "-o", str(binary)],
            check=True,
            cwd=ROOT,
        )
        return subprocess.run([str(binary), "--nocapture"], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
