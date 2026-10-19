#!/usr/bin/env python3
"""Compile and run the production resource-limit policy Rust tests."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "kernel/abi/linux/rlimit_policy.rs"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="a20-rlimit-policy-") as tmp:
        binary = pathlib.Path(tmp) / "rlimit-policy-tests"
        subprocess.run(
            ["rustc", "--edition=2021", "--test", str(SOURCE), "-o", str(binary)],
            check=True,
            cwd=ROOT,
        )
        return subprocess.run([str(binary), "--nocapture"], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
