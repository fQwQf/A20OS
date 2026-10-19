#!/usr/bin/env python3
"""Compile and run the production prctl/setns policy Rust tests."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "kernel/abi/linux/sys_policy.rs"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="a20-sys-policy-") as tmp:
        binary = pathlib.Path(tmp) / "sys-policy-tests"
        subprocess.run(
            ["rustc", "--edition=2021", "--test", str(SOURCE), "-o", str(binary)],
            check=True,
            cwd=ROOT,
        )
        return subprocess.run([str(binary), "--nocapture"], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
