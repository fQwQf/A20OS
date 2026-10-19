#!/usr/bin/env python3
"""Compile and run the production fork/clone-transaction Rust tests.

`kernel/proc/clone_txn.rs` is the production source; this harness only asks
rustc for the `--test` build of that exact file, so the assertions below are
the same ones that ship in the kernel object.  The C bridges are replaced by
the fixture stubs inside the same file, which also stand in for the link-time
symbols the production object takes from kernel/proc/fork.c.
"""

from __future__ import annotations

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "kernel/proc/clone_txn.rs"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="a20-clone-txn-") as tmp:
        binary = pathlib.Path(tmp) / "clone-txn-tests"
        subprocess.run(
            ["rustc", "--edition=2021", "--test", str(SOURCE), "-o", str(binary)],
            check=True,
            cwd=ROOT,
        )
        return subprocess.run([str(binary), "--nocapture"], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
