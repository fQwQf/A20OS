#!/usr/bin/env python3
"""Compile and run the production xlator configuration Rust tests.

The configuration pass calls the template parser from its own object, so the
harness builds that production source as a library first and links it into the
test binary -- the same two objects the kernel links.
"""

from __future__ import annotations

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "kernel/proc/xlator_config.rs"
PARSER = ROOT / "kernel/proc/xlator_tmpl.rs"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="a20-xlator-config-") as tmp:
        tmpdir = pathlib.Path(tmp)
        # The parser exports plain C symbols; link its production object
        # directly, which is also how the kernel links the two objects.
        obj = tmpdir / "xlator_tmpl.o"
        subprocess.run(
            ["rustc", "--edition=2021", "--crate-type", "lib", "--emit=obj",
             str(PARSER), "-o", str(obj)],
            check=True,
            cwd=ROOT,
        )
        binary = tmpdir / "xlator-config-tests"
        subprocess.run(
            ["rustc", "--edition=2021", "--test", str(SOURCE),
             "-C", f"link-arg={obj}", "-o", str(binary)],
            check=True,
            cwd=ROOT,
        )
        return subprocess.run([str(binary), "--nocapture"], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
