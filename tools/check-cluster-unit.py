#!/usr/bin/env python3
"""Compile production CL codecs/operators/leaf and verify the gold vectors."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def run(command, root):
    result = subprocess.run(command, cwd=root, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    if result.returncode:
        print(result.stdout)
        raise subprocess.CalledProcessError(result.returncode, command)
    lines = result.stdout.strip().splitlines()
    if lines:
        print(lines[-1])


def main():
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="a20-cluster-unit-") as directory:
        compiler = shlex.split(os.environ.get("HOST_CC", "cc"))
        for name in ("test_clx_frame", "test_clx_uart", "test_clx_operator",
                     "test_mcu_leaf", "test_clx_uart_pair", "test_clx_transaction",
                     "test_clx_work", "test_clx_membership", "test_clx_job", "test_clx_epoch", "test_clx_board_uart"):
            binary = Path(directory) / name
            run(compiler + ["-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-Ikernel/arch/riscv64/include", "-Ikernel/include", "-Ikernel",
                            f"tools/tests/{name}.c", "-o", str(binary)], root)
            run([str(binary)], root)
        run([sys.executable, "tools/cluster-ref/selftest.py"], root)
        run([sys.executable, "tools/cluster-ref/check_c_side.py", "--decoder",
             str(Path(directory) / "test_clx_frame")], root)
    print("check-cluster-unit: PASS")


if __name__ == "__main__":
    main()
