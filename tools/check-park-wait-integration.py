#!/usr/bin/env python3
"""Run real timer-capacity rollback and cross-CPU park wake on RV64 QEMU."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import time


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build-root", type=Path,
                    default=Path(".kernel-build/park-wait-integration"))
parser.add_argument("--timeout", type=float, default=40)
parser.add_argument("--jobs", type=int, default=2)
parser.add_argument("--report", type=Path)
args = parser.parse_args()

root = args.build_root.resolve()
root.mkdir(parents=True, exist_ok=True)
report_path = (args.report or root / "report.json").resolve()
common = [
    "ARCH=riscv64", "BOARD=qemu-virt-riscv64", "ABI=both", "BRINGUP=1",
    "NR_CPUS=2", "SMP_SELFTEST=1", "WAIT_TIMER_HEAP_MAX=1",
    "ALLOW_UNVERIFIED_SMP=1", "CONFIG_KERNEL_PREEMPT=1",
    "KERNEL_WERROR=1", "WAIT_TIMER_HEAP_TEST_ARRAY_BOUNDS=1",
    "USER_COPY_IMPL=c",
]
markers = [
    "[PARK_WAIT_TIMER_FULL: PASS capacity=1 entries=1 failures=",
    "[PARK_WAIT_CROSS_CPU_WAKE: PASS waiter_cpu=1 affinity_cpu=1 waker_cpu=0 timer_cancelled=1]",
    "[PARK_WAIT_TIMER_REAP: PASS entries=0 heap_violations=0]",
    "[SMP-TEST] PASS cpus=2 mask=0x3",
    "[SMP-TEST] CLEANUP PASS",
    "System is going down for power-off NOW",
]
runs = []

for impl in ("c", "rust"):
    build_dir = root / impl
    build_dir.mkdir(parents=True, exist_ok=True)
    flags = [*common, f"PARK_WAIT_IMPL={impl}", f"BUILD_DIR={build_dir}"]
    build_log = root / f"{impl}-build.log"
    with build_log.open("wb") as output:
        built = subprocess.run(["make", *flags, f"-j{args.jobs}", "kernel-only"],
                               stdout=output, stderr=subprocess.STDOUT)
    if built.returncode != 0:
        raise SystemExit(f"{impl} kernel build failed; see {build_log}")
    build_text = build_log.read_text(errors="replace")
    bounds_suppression = [
        line for line in build_text.splitlines()
        if "-Wno-array-bounds" in line and " -c kernel/" in line
    ]
    if (len(bounds_suppression) != 1 or
            "-c kernel/proc/timer_heap.c " not in bounds_suppression[0]):
        raise SystemExit(
            f"{impl} build did not confine array-bounds suppression to timer_heap.c; "
            f"see {build_log}"
        )

    argv_text = subprocess.check_output(["make", "-s", *flags, "_qemu_argv"],
                                        text=True).strip()
    qemu_argv = shlex.split(argv_text)
    log_path = root / f"{impl}-qemu.log"
    started = time.monotonic()
    with log_path.open("wb") as output:
        process = subprocess.Popen(qemu_argv, stdin=subprocess.DEVNULL,
                                   stdout=output, stderr=subprocess.STDOUT)
        passed = False
        while time.monotonic() - started < args.timeout:
            content = log_path.read_text(errors="replace")
            if any(word in content for word in ("PANIC", "FATAL", "LOCKSTALL")):
                break
            if all(marker in content for marker in markers):
                passed = True
                break
            if process.poll() is not None:
                break
            time.sleep(0.05)
        stopped_by = "guest_poweroff"
        if process.poll() is None and passed:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                stopped_by = "host_terminate_after_pass"
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    stopped_by = "host_kill_after_pass"
                    process.kill()
                    process.wait()
        elif process.poll() is None:
            stopped_by = "host_terminate_after_failure"
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                stopped_by = "host_kill_after_failure"
                process.kill()
                process.wait()
    content = log_path.read_text(errors="replace")
    fatal_markers = {word: content.count(word)
                     for word in ("PANIC", "FATAL", "LOCKSTALL")}
    if not passed or process.returncode != 0 or any(fatal_markers.values()):
        raise SystemExit(f"{impl} QEMU case failed or timed out; see {log_path}")

    kernel = build_dir / "kernel.elf"
    runs.append({
        "impl": impl,
        "build_command": ["make", *flags, f"-j{args.jobs}", "kernel-only"],
        "build_exit_status": built.returncode,
        "build_log": str(build_log),
        "build_log_sha256": hashlib.sha256(build_log.read_bytes()).hexdigest(),
        "qemu_argv": qemu_argv,
        "qemu_returncode": process.returncode,
        "qemu_stopped_by": stopped_by,
        "kernel": str(kernel),
        "kernel_sha256": hashlib.sha256(kernel.read_bytes()).hexdigest(),
        "selftest_source_sha256": hashlib.sha256(
            Path("kernel/core/smp_selftest.c").read_bytes()).hexdigest(),
        "timer_heap_source_sha256": hashlib.sha256(
            Path("kernel/proc/timer_heap.c").read_bytes()).hexdigest(),
        "alarm_timer_heap_capacity": 1,
        "array_bounds_suppression_scope": "kernel/proc/timer_heap.c only",
        "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "log": str(log_path),
        "log_sha256": hashlib.sha256(log_path.read_bytes()).hexdigest(),
        "required_markers": markers,
        "fatal_markers": fatal_markers,
    })
    print(f"PASS {impl}: real timeout-capacity rollback and CPU1-to-CPU0 wake",
          flush=True)

report_path.parent.mkdir(parents=True, exist_ok=True)
report_path.write_text(json.dumps({"schema": 1, "runs": runs}, indent=2) + "\n")
print(f"report: {report_path}")
