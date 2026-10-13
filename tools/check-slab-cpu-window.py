#!/usr/bin/env python3
"""Exercise production CPU-local slab array slow paths with IRQ reentry."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=root / "kernel/mm/slab.c")
    args = parser.parse_args()
    source = args.source.read_text()
    fixture = (root / "tools/tests/slab_cpu_window_fixture.c.in").read_text()
    for name in ("kmalloc_flags", "kfree"):
        match = re.search(r"^void\s*\*?\s*" + name + r"\([^)]*\)\s*\{.*?^}\n", source, re.M | re.S)
        marker = "/* PRODUCTION_" + name + " */"
        if not match or fixture.count(marker) != 1:
            raise RuntimeError("missing production function or splice: " + name)
        fixture = fixture.replace(marker, match[0])
    directory = root / "build/tmp"
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="slab-cpu-window-", dir=directory) as tmp:
        test = Path(tmp) / "test.c"
        binary = Path(tmp) / "test"
        test.write_text(fixture)
        compiler = shlex.split(os.environ.get("HOST_CC", "cc"))
        subprocess.run(compiler + ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                                   str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=60)
    print("check-slab-cpu-window: PASS (production functions, IRQ reentry/migration, ASan/UBSan)")


if __name__ == "__main__":
    main()
