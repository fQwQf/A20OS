#!/usr/bin/env python3
"""Compile and run the production resource-namespace Rust tests.

`kernel/proc/resource_ns_state.rs` is the production source; this harness only
asks rustc for the `--test` build of that exact file.
"""

from __future__ import annotations

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "kernel/proc/resource_ns_state.rs"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="a20-resource-ns-") as tmp:
        binary = pathlib.Path(tmp) / "resource-ns-tests"
        subprocess.run(
            ["rustc", "--edition=2021", "--test", str(SOURCE), "-o", str(binary)],
            check=True,
            cwd=ROOT,
        )
        return subprocess.run([str(binary), "--nocapture"], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
