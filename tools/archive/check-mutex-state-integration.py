#!/usr/bin/env python3
"""Build and boot C/Rust mutex-state profiles on the RV64 SMP selftest."""

import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import time


source_root = Path(os.environ.get("A20_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
evidence = Path(os.environ.get(
    "A20_EVIDENCE_DIR", source_root / "build/evidence/mutex-state-rv64"))
evidence.mkdir(parents=True, exist_ok=True)
common = [
    "ARCH=riscv64", "BOARD=qemu-virt-riscv64", "ABI=both", "BRINGUP=1",
    "NR_CPUS=2", "SMP_SELFTEST=1", "WAIT_TIMER_HEAP_MAX=1",
    "ALLOW_UNVERIFIED_SMP=1", "CONFIG_KERNEL_PREEMPT=1", "KERNEL_WERROR=1",
    "WAIT_TIMER_HEAP_TEST_ARRAY_BOUNDS=1", "USER_COPY_IMPL=c",
    "PROCESS_VM_IMPL=c", "PARK_WAIT_IMPL=rust", "WAIT_TIMER_IMPL=rust",
    "WAIT_QUEUE_IMPL=rust", "RW_MUTEX_IMPL=rust", "COMPLETION_IMPL=rust",
]
markers = [
    "[PARK_WAIT_TIMER_FULL: PASS capacity=1 entries=1 failures=",
    "[PARK_WAIT_CROSS_CPU_WAKE: PASS waiter_cpu=1 affinity_cpu=1 waker_cpu=0 timer_cancelled=1]",
    "[WAIT_QUEUE_OWNER_WAKE: PASS waiter_cpu=1 waker_cpu=0 owner_cleared=1 entries=",
    "[RW_MUTEX_WRITER_PREFERENCE: PASS writer_first=1 blocked_reader=1 cpu=1 refs=stable]",
    "[MUTEX_OWNER_WAIT: PASS recursive_depth=2 waiter_cpu=1 wake=1 refs=stable]",
    "[PARK_WAIT_TIMER_REAP: PASS entries=0 heap_violations=0]",
    "[WAIT_TIMER_CANCEL_EXPIRE_RACE: PASS waiter_cpu=1 waker_cpu=1 expirer_cpu=0 reason=event timeout_lost=1 entries=0 refs=stable]",
    "[SMP-TEST] PASS cpus=2 mask=0x3",
    "[SMP-TEST] CLEANUP PASS",
    "System is going down for power-off NOW",
]
runs = []

for impl in ("c", "rust"):
    build_dir = source_root / "build/isolated" / f"mutex-state-{impl}-rv64"
    flags = [*common, f"MUTEX_STATE_IMPL={impl}", f"BUILD_DIR={build_dir}"]
    build_log = evidence / f"{impl}-build.log"
    with build_log.open("wb") as output:
        built = subprocess.run(
            ["make", "-o", "user/build/riscv64/.build-id", *flags,
             "kernel-only", "-j2"], cwd=source_root,
            stdout=output, stderr=subprocess.STDOUT)
    if built.returncode:
        raise SystemExit(f"{impl} kernel build failed; see {build_log}")

    argv = shlex.split(subprocess.check_output(
        ["make", "-s", "-o", "user/build/riscv64/.build-id", *flags,
         "_qemu_argv"], cwd=source_root, text=True).strip())
    log = evidence / f"{impl}-qemu.log"
    started = time.monotonic()
    with log.open("wb") as output:
        process = subprocess.Popen(argv, cwd=source_root, stdin=subprocess.DEVNULL,
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
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=3)
    contents = log.read_text(errors="replace")
    if (not passed or process.returncode != 0 or
            any(fatal in contents for fatal in ("PANIC", "FATAL", "LOCKSTALL"))):
        raise SystemExit(f"{impl} QEMU case failed; see {log}")
    kernel = build_dir / "kernel.elf"
    runs.append({
        "implementation": impl,
        "build_command": ["make", "-o", "user/build/riscv64/.build-id", *flags,
                          "kernel-only", "-j2"],
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
        path: hashlib.sha256((source_root / path).read_bytes()).hexdigest()
        for path in ("kernel/core/sync.c", "kernel/core/mutex_state.rs",
                     "kernel/include/core/sync.h", "kernel/core/smp_selftest.c",
                     "Makefile", "tools/mk/rust-kernel.mk", __file__)
    },
}
report_path = evidence / "report.json"
report_path.write_text(json.dumps(report, indent=2) + "\n")
print(report_path)
