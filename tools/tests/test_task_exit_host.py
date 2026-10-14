#!/usr/bin/env python3
"""Compile and run the production task-exit ordering oracle."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "kernel/proc/task_exit.rs"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="a20-task-exit-") as tmp:
        binary = pathlib.Path(tmp) / "task-exit-tests"
        subprocess.run(
            ["rustc", "--edition=2021", "--test", str(SOURCE), "-o", str(binary)],
            check=True,
            cwd=ROOT,
        )
        return subprocess.run([str(binary), "--nocapture"], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
