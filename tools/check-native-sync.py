#!/usr/bin/env python3
"""Boot the same Native application synchronization test on server ISAs.

This executes real A20OS kernels and SDK syscalls in QEMU. It does not claim
physical-board coverage. MCU SVC/fabric coverage is check-mcu-program[-rtl].
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_case(arch, args, output):
    flags = [f"ARCH={arch}", "ABI=both", "BRINGUP=0", f"NR_CPUS={args.cpus}"]
    if args.cpus > 1:
        flags.append("ALLOW_UNVERIFIED_SMP=1")
    build = ["make", *flags, f"-j{args.jobs}", "dev-build"]
    with (output / f"{arch}-build.log").open("w") as log:
        subprocess.run(build, cwd=ROOT, check=True, stdout=log, stderr=subprocess.STDOUT)
    command = shlex.split(subprocess.check_output(
        ["make", "-s", *flags, "_qemu_argv"], cwd=ROOT, text=True).strip())
    kernel = ROOT / command[command.index("-kernel") + 1]
    drive = command[command.index("-drive") + 1]
    image = ROOT / drive.split("file=", 1)[1].split(",", 1)[0]
    log = output / f"{arch}.log"
    tag = {"riscv64": "rv", "aarch64": "aarch64", "x86_64": "x86_64"}[arch]
    phase = "boot"
    thread_cpus = None
    host_terminated = False
    with log.open("wb") as stream:
        process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.PIPE,
                                   stdout=stream, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + args.timeout
        try:
            while time.monotonic() < deadline:
                content = log.read_text(errors="replace")
                assert "PANIC" not in content and "NATIVE_FUTEX: FAIL" not in content, str(log)
                if phase == "boot" and re.search(r"(?:^|[\r\n])# ", content):
                    process.stdin.write(f"/bin/native-futex-{tag}\n".encode())
                    process.stdin.flush()
                    phase = "application"
                    deadline = time.monotonic() + args.timeout
                elif phase == "application" and "NATIVE_FUTEX: PASS" in content:
                    assert all(marker in content for marker in ["NATIVE_ATOMIC32: PASS", "NATIVE_ALLOCATOR: PASS", "NATIVE_DOMAIN: PASS", "NATIVE_THREAD: PASS", "NATIVE_RUNTIME: PASS"]), str(log)
                    match = re.search(r"NATIVE_THREAD_CPUS: (\d+)->(\d+)", content)
                    assert match, str(log)
                    thread_cpus = [int(match[1]), int(match[2])]
                    assert all(cpu < args.cpus for cpu in thread_cpus), str(log)
                    assert args.cpus == 1 or thread_cpus[0] != thread_cpus[1], str(log)
                    assert f"NATIVE_DOMAIN_CPUS: {args.cpus:02d}\n" in content, str(log)
                    process.stdin.write(b"poweroff\n")
                    process.stdin.flush()
                    phase = "shutdown"
                    deadline = time.monotonic() + args.timeout
                elif phase == "shutdown" and "System is going down for power-off NOW" in content:
                    phase = "complete"
                    break
                if process.poll() is not None:
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
            process.stdin.close()
    assert phase == "complete", f"{arch} stopped at {phase}; see {log}"
    return dict(arch=arch, cpus=args.cpus, domain_online_verified=args.cpus, thread_cpus_verified=thread_cpus, passed=True, build=build, command=command,
                log=str(log.relative_to(ROOT)), kernel=str(kernel.relative_to(ROOT)),
                kernel_sha256=sha(kernel), image=str(image.relative_to(ROOT)),
                image_sha256=sha(image), returncode=process.returncode,
                termination="host SIGTERM after shutdown output; not poweroff" if host_terminated
                            else "QEMU exited naturally", physical_board=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arches", nargs="+", default=["riscv64", "aarch64", "x86_64"],
                        choices=["riscv64", "aarch64", "x86_64"])
    parser.add_argument("--cpus", type=int, default=2, choices=[1, 2, 4])
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--output", type=Path,
                        default=Path(".kernel-build/cluster/native-sync"))
    args = parser.parse_args()
    if args.jobs < 1 or args.timeout <= 0:
        parser.error("jobs and timeout must be positive")
    output = (ROOT / args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    cases = []
    report = output / "report.json"
    # Publish an incomplete report first, so a failed rerun cannot leave a
    # previous all-green report looking like evidence for this invocation.
    report.write_text(json.dumps(dict(passed=False, cases=cases), indent=2) + "\n")
    for arch in args.arches:
        cases.append(run_case(arch, args, output))
        report.write_text(json.dumps(dict(passed=False, cases=cases), indent=2) + "\n")
        print("PASS Native futex/atomic32:", arch, f"cpus={args.cpus}", flush=True)
    report.write_text(json.dumps(dict(passed=True, cases=cases), indent=2) + "\n")


if __name__ == "__main__":
    main()
