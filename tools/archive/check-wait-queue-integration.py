#!/usr/bin/env python3
"""Run C/Rust wait-queue owner wake coverage on a real RV64 SMP guest."""
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import time


evidence = Path("/media/fqwqf/Windows-SSD/a20os-park-wait-20261014/wait-queue-integration-20261014")
evidence.mkdir(parents=True, exist_ok=True)
common = [
    "ARCH=riscv64", "BOARD=qemu-virt-riscv64", "ABI=both", "BRINGUP=1",
    "NR_CPUS=2", "SMP_SELFTEST=1", "WAIT_TIMER_HEAP_MAX=1",
    "ALLOW_UNVERIFIED_SMP=1", "CONFIG_KERNEL_PREEMPT=1", "KERNEL_WERROR=1",
    "WAIT_TIMER_HEAP_TEST_ARRAY_BOUNDS=1", "USER_COPY_IMPL=c",
    "PROCESS_VM_IMPL=c", "PARK_WAIT_IMPL=rust",
]
markers = [
    "[PARK_WAIT_TIMER_FULL: PASS capacity=1 entries=1 failures=",
    "[PARK_WAIT_CROSS_CPU_WAKE: PASS waiter_cpu=1 affinity_cpu=1 waker_cpu=0 timer_cancelled=1]",
    "[WAIT_QUEUE_OWNER_WAKE: PASS waiter_cpu=1 waker_cpu=0 owner_cleared=1 entries=",
    "[PARK_WAIT_TIMER_REAP: PASS entries=0 heap_violations=0]",
    "[WAIT_TIMER_CANCEL_EXPIRE_RACE: PASS waiter_cpu=1 waker_cpu=1 expirer_cpu=0 reason=event timeout_lost=1 entries=0 refs=stable]",
    "[SMP-TEST] PASS cpus=2 mask=0x3",
    "[SMP-TEST] CLEANUP PASS",
    "System is going down for power-off NOW",
]
runs = []

for impl in ("c", "rust"):
    build_dir = Path("/tmp") / f"a20-wait-queue-{impl}-20261014"
    build_dir.mkdir(parents=True, exist_ok=True)
    flags = [*common, f"WAIT_TIMER_IMPL={impl}", f"WAIT_QUEUE_IMPL={impl}",
             f"BUILD_DIR={build_dir}"]
    build_log = evidence / f"{impl}-build.log"
    with build_log.open("wb") as output:
        built = subprocess.run(["make", *flags, "-j2", "kernel-only"],
                               stdout=output, stderr=subprocess.STDOUT)
    if built.returncode:
        raise SystemExit(f"{impl} kernel build failed; see {build_log}")

    argv = shlex.split(subprocess.check_output(
        ["make", "-s", *flags, "_qemu_argv"], text=True).strip())
    log = evidence / f"{impl}-qemu.log"
    started = time.monotonic()
    with log.open("wb") as output:
        process = subprocess.Popen(argv, stdin=subprocess.DEVNULL,
                                   stdout=output, stderr=subprocess.STDOUT)
        passed = False
        while time.monotonic() - started < 60:
            contents = log.read_text(errors="replace")
            if any(fatal in contents for fatal in ("PANIC", "FATAL", "LOCKSTALL")):
                break
            if all(marker in contents for marker in markers):
                passed = True
                break
            if process.poll() is not None:
                break
            time.sleep(0.05)
        if process.poll() is None:
            if passed:
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=3)
            else:
                process.terminate()
                process.wait(timeout=3)
    contents = log.read_text(errors="replace")
    if (not passed or process.returncode != 0 or
            any(fatal in contents for fatal in ("PANIC", "FATAL", "LOCKSTALL"))):
        raise SystemExit(f"{impl} QEMU case failed; see {log}")
    kernel = build_dir / "kernel.elf"
    runs.append({
        "implementation": impl,
        "build_command": ["make", *flags, "-j2", "kernel-only"],
        "build_log": str(build_log),
        "build_log_sha256": hashlib.sha256(build_log.read_bytes()).hexdigest(),
        "kernel": str(kernel),
        "kernel_sha256": hashlib.sha256(kernel.read_bytes()).hexdigest(),
        "qemu_argv": argv,
        "qemu_returncode": process.returncode,
        "qemu_log": str(log),
        "qemu_log_sha256": hashlib.sha256(log.read_bytes()).hexdigest(),
        "markers": markers,
    })

report = {
    "runs": runs,
    "sources": {
        path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
        for path in ("kernel/core/sync.c", "kernel/core/wait_queue_state.rs",
                     "kernel/core/smp_selftest.c", __file__)
    },
}
report_path = evidence / "report.json"
report_path.write_text(json.dumps(report, indent=2) + "\n")
print(report_path)
