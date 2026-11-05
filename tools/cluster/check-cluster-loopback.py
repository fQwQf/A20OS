#!/usr/bin/env python3
"""Boot the production loopback cluster regression suite in QEMU."""
import argparse
from pathlib import Path
import re
import shlex
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent.parent
    directory = root / ".kernel-build/cluster"
    directory.mkdir(parents=True, exist_ok=True)
    make = ["make", "--no-print-directory", "ARCH=riscv64", "BRINGUP=1",
            "RAMFS_USER=1"]
    if not args.skip_build:
        with (directory / "loopback-build.log").open("w") as log:
            subprocess.run(make + ["-j8", "kernel-only"], cwd=root, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
    cmd = shlex.split(subprocess.check_output(make + ["_qemu_argv"], cwd=root,
                                               text=True))
    cmd += ["-append", "clxselftest=1"]
    path = directory / "loopback.log"
    with path.open("w") as log:
        proc = subprocess.Popen(cmd, cwd=root, stdin=subprocess.DEVNULL,
                                stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                text = path.read_text(errors="replace")
                match = re.search(r"clx-selftest: (\d+)/(\d+) checks passed", text)
                if match:
                    ok = match[1] == match[2] and int(match[2]) >= 24
                    print(f"check-cluster-loopback: {'PASS' if ok else 'FAIL'} "
                          f"({match[1]}/{match[2]})")
                    return 0 if ok else 1
                if proc.poll() is not None:
                    break
                time.sleep(0.1)
            print(f"check-cluster-loopback: FAIL (see {path})")
            print("\n".join(path.read_text(errors="replace").splitlines()[-25:]))
            return 1
        finally:
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    raise SystemExit(main())
