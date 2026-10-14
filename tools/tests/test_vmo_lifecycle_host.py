#!/usr/bin/env python3
"""Run the production VMO reference/peek Rust entrypoints with atomic stubs."""

from pathlib import Path
import subprocess
import tempfile


SOURCE = Path(__file__).resolve().parents[2] / "kernel/mm/vmo_lifecycle.rs"


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="vmo-lifecycle-host-") as tmp:
        binary = Path(tmp) / "vmo_lifecycle_host"
        subprocess.run(
            ["rustc", "--edition=2021", "--test", str(SOURCE), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary), "--test-threads=1"], check=True)


if __name__ == "__main__":
    main()
