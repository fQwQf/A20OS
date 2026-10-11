#!/usr/bin/env python3
"""Execute production RX enqueue/wait code at each wake/park race boundary."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parent.parent
    directory = root / "build/tmp"
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="clx-wakeup-", dir=directory) as tmp:
        compiler = shlex.split(os.environ.get("HOST_CC", "cc"))
        cases = (("transport.c", "clx_wakeup_fixture.c.in", ("a20_clx_rx_frame", "clx_rx_wait")),
                 ("udp.c", "clx_udp_wakeup_fixture.c.in", ("a20_clx_udp_wait",)))
        for source_name, fixture_name, functions in cases:
            source = (root / "kernel/cluster" / source_name).read_text()
            fixture = (root / "tools/tests" / fixture_name).read_text()
            for name in functions:
                match = re.search(r"(?:static )?void " + name + r"\([^)]*\)\s*\{.*?\n}\n", source, re.S)
                marker = "/* PRODUCTION_" + name + " */"
                if not match or fixture.count(marker) != 1:
                    raise RuntimeError("missing production function or fixture splice: " + name)
                fixture = fixture.replace(marker, match[0])
            test = Path(tmp) / (source_name + "-test.c")
            test.write_text(fixture)
            binary = Path(tmp) / (source_name + "-test")
            subprocess.run(compiler + ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                                       str(test), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)
    print("check-cluster-wakeup: PASS (production RX functions, race boundaries, ASan/UBSan)")


if __name__ == "__main__":
    main()
