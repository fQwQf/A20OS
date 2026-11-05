#!/usr/bin/env python3
"""Build and execute one common SMP functional selftest on server ISAs.

SMP_SELFTEST=1 selects separate bring-up artifacts. The test uses actual
kernel threads, task migration, timer IRQs, IPIs, allocation and exit/reap.
AArch64 virt's existing ELF handoff does not carry a DTB command line, so the
compile-time opt-in is used consistently on every architecture.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--arches", nargs="+", default=["aarch64", "riscv64", "x86_64"],
                    choices=["aarch64", "riscv64", "x86_64"])
parser.add_argument("--report", type=Path,
                    default=ROOT / ".kernel-build/cluster/smp-native/report.json")
parser.add_argument("--jobs", type=int, default=8)
parser.add_argument("--timeout", type=float, default=30)
args = parser.parse_args()
args.report.parent.mkdir(parents=True, exist_ok=True)
results = []
for arch in args.arches:
    flags = [f"ARCH={arch}", "ABI=both", "BRINGUP=1", "NR_CPUS=2",
             "ALLOW_UNVERIFIED_SMP=1", "SMP_SELFTEST=1"]
    build = ["make", "-C", str(ROOT), *flags, f"-j{args.jobs}", "kernel-only"]
    build_log = args.report.parent / (arch + "-build.log")
    with build_log.open("w") as output:
        subprocess.run(build, cwd=ROOT, check=True, stdout=output, stderr=subprocess.STDOUT)
    command = shlex.split(subprocess.check_output(
        ["make", "-C", str(ROOT), "-s", *flags, "_qemu_argv"], cwd=ROOT, text=True).strip())
    kernel = Path(command[command.index("-kernel") + 1])
    log = args.report.parent / (arch + ".log")
    start = time.monotonic()
    host_terminated = False
    with log.open("wb") as output:
        process = subprocess.Popen(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL)
        try:
            while time.monotonic() - start < args.timeout:
                content = log.read_text(errors="replace")
                if "System is going down for power-off NOW" in content or process.poll() is not None:
                    break
                time.sleep(0.05)
        finally:
            if process.poll() is None:
                try:
                    process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    host_terminated = True
                    process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    content = log.read_text(errors="replace")
    assert "[SMP-TEST] PASS cpus=2 mask=0x3" in content, str(log)
    assert "timer-irqs=all-online ipi=acked exit=0" in content, str(log)
    assert "[SMP-TEST] CLEANUP PASS" in content, str(log)
    assert "System is going down for power-off NOW" in content, str(log)
    assert "PANIC" not in content, str(log)
    results.append({"arch": arch, "passed": True, "build": build,
                    "command": command, "log": str(log), "kernel": str(kernel),
                    "kernel_sha256": hashlib.sha256(kernel.read_bytes()).hexdigest(),
                    "returncode": process.returncode,
                    "termination": "host SIGTERM after shutdown output; not poweroff" if host_terminated
                                   else "QEMU exited naturally"})
    print("PASS common SMP:", arch, "returncode", process.returncode,
          "termination=" + ("host-SIGTERM" if host_terminated else "natural"), flush=True)
args.report.write_text(json.dumps({"passed": True, "cases": results}, indent=2) + "\n")
