#!/usr/bin/env python3
"""Run the real A20OS Image on QEMU raspi4b with explicit firmware fixtures.

Requires a BRINGUP=1, ABI=both, NR_CPUS=4 Raspberry Pi 4 kernel. QEMU always
models four physical CPUs; disabled firmware CPUs must remain parked. This
is a native kernel instruction test, not physical board/JVM/network evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

from raspberrypi4_fixture import board_dtb

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--kernel", type=Path, required=True)
parser.add_argument("--report", type=Path,
                    default=Path(".kernel-build/cluster/raspberrypi4/report.json"))
parser.add_argument("--qemu", default="qemu-system-aarch64")
parser.add_argument("--timeout", type=float, default=20)
args = parser.parse_args()
args.report.parent.mkdir(parents=True, exist_ok=True)
version = subprocess.check_output([args.qemu, "--version"], text=True).splitlines()[0]
cases = [
    ("ordered", {}, 4, [(0, 0), (1, 1), (2, 2), (3, 3)], None),
    ("reordered", {"order": (3, 1, 0, 2)}, 4,
     [(0, 0), (1, 3), (2, 1), (3, 2)], None),
    ("sparse", {"order": (3, 2, 0, 1), "disabled": (1, 2)}, 2,
     [(0, 0), (1, 3)], None),
    ("reserve-map", {"reservations": ((0, 0x1000), (0x4000000, 0x1000))}, 4,
     None, "allocator RAM=0..0x4000000"),
    ("reserved-memory", {"reserved_memory": (0x4000000, 0x1000)}, 4,
     None, "allocator RAM=0..0x4000000"),
    ("reject-soc", {"compatible": "unsupported,soc"}, 0, None,
     "DTB must describe BCM2711"),
    ("reject-release", {"release_delta": 8}, 0, None, "unsupported CPU topology"),
    ("reject-duplicate", {"order": (0, 1, 1, 3)}, 0, None, "duplicate CPU ID"),
    ("reject-method", {"method": "psci"}, 0, None, "unsupported CPU topology"),
    ("reject-overlap", {"reservations": ((0x80000, 0x1000),)}, 0, None,
     "firmware reservation overlaps kernel"),
]
results = []
for name, settings, count, topology, diagnostic in cases:
    dtb = args.report.parent / (name + ".dtb")
    dtb.write_bytes(board_dtb(**settings))
    log = args.report.parent / (name + ".log")
    command = [args.qemu, "-machine", "raspi4b", "-m", "2G", "-smp", "4",
               "-display", "none", "-serial", "stdio", "-monitor", "none",
               "-kernel", str(args.kernel), "-dtb", str(dtb)]
    start = time.monotonic()
    # Stop only after native kernel output reaches its expected terminal state.
    # No GDB task/register mutation or host-supplied scheduler progress.
    with log.open("wb") as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
        reached = False
        try:
            while time.monotonic() - start < args.timeout:
                content = log.read_text(errors="replace")
                reached = ("System is going down for power-off NOW" in content if count
                           else "KERNEL PANIC" in content or "panic:" in content.lower())
                if reached or process.poll() is not None:
                    break
                time.sleep(0.05)
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    content = log.read_text(errors="replace")
    if count:
        expected = (f"[SMP-TEST] PASS cpus={count} mask=0x{(1 << count)-1:x}")
        assert reached and expected in content, f"{name}: no functional PASS, see {log}"
        assert "timer-irqs=all-online ipi=acked exit=0" in content
        assert "[SMP-TEST] CLEANUP PASS" in content, str(log)
        assert "PANIC" not in content and "TRAP" not in content, str(log)
        for logical, hardware in topology or []:
            assert f"[SMP-TEST] cpu={logical} hw={hardware}" in content, str(log)
    else:
        assert reached and "[SMP-TEST] PASS" not in content, str(log)
    if diagnostic:
        assert diagnostic in content, str(log)
    match = re.search(r"counter=(\d+) Hz", content)
    results.append({"case": name, "passed": True, "online": count,
                    "counter_hz": int(match[1]) if match else None,
                    "dtb_sha256": hashlib.sha256(dtb.read_bytes()).hexdigest(),
                    "log": str(log), "command": command,
                    "termination": "host SIGTERM after native terminal output; not poweroff"})
    print("PASS", name, flush=True)
args.report.write_text(json.dumps({"qemu": version, "kernel": str(args.kernel),
    "kernel_sha256": hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
    "cases": results, "physical_board_tested": False}, indent=2) + "\n")
print("PASS Raspberry Pi 4 native functional matrix:", args.report)
