#!/usr/bin/env python3
"""Execute the production TX completion function with synthetic device entries."""
import argparse
from pathlib import Path
import re
import shlex
import os
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("kernel/drivers/net/virtio_net.c"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    source = (root / args.source).read_text()
    match = re.search(r"static void virtio_net_complete_tx_locked\([^)]*\)\s*\{.*?\n}\n", source, re.S)
    if not match:
        raise RuntimeError("production completion function not found")
    fixture = (root / "tools/tests/virtio_net_completion_fixture.c.in").read_text()
    if fixture.count("/* PRODUCTION_COMPLETION */") != 1:
        raise RuntimeError("invalid fixture splice")
    fixture = fixture.replace("/* PRODUCTION_COMPLETION */", match[0])
    directory = root / "build/tmp"
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="virtio-net-completion-", dir=directory) as tmp:
        test = Path(tmp) / "completion.c"
        test.write_text(fixture)
        binary = Path(tmp) / "completion"
        command = shlex.split(os.environ.get("HOST_CC", "cc"))
        subprocess.run(command + ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                                 str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)
    print("check-virtio-net-completion: PASS (production function, ASan/UBSan)")


if __name__ == "__main__":
    main()
