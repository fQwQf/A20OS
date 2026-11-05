#!/usr/bin/env python3
"""Run the final Signal guest cases on RV64 SMP2 and bind their boot inputs."""
from pathlib import Path
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parent.parent.parent
CASES = (
    ("smoke-sandbox", None),
    ("smoke-signalfd-stress", None),
    ("smoke-native-signal", "riscv64"),
)


def main() -> int:
    for case, arch in CASES:
        command = [sys.executable, "tools/smoke.py", case, "--nr-cpus", "2",
                   "--bind-dir", "out/smoke-bind"]
        if arch:
            command += ["--arch", arch]
        print("RUN:", " ".join(command), flush=True)
        for attempt in range(1, 4):
            result = subprocess.run(command, cwd=REPO, check=False)
            if result.returncode != 75 or attempt == 3:
                break
            print(f"{case}: QEMU slot busy; retrying after 45 seconds ({attempt}/3)", flush=True)
            time.sleep(45)
        if result.returncode != 0:
            print(f"{case}: failed with status {result.returncode}; no final binding accepted",
                  file=sys.stderr)
            return result.returncode or 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
