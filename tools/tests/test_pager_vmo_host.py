#!/usr/bin/env python3
"""Run the production pager attach/detach Rust transactions with C-style stubs."""

from pathlib import Path
import subprocess
import tempfile


SOURCE = Path(__file__).resolve().parents[2] / "kernel/ipc/pager_vmo.rs"


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="pager-vmo-host-") as tmp:
        binary = Path(tmp) / "pager_vmo_host"
        subprocess.run(
            ["rustc", "--edition=2021", "--test", str(SOURCE), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary), "--test-threads=1"], check=True)


if __name__ == "__main__":
    main()
