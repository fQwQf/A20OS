#!/usr/bin/env python3
"""Run the opt-in real hardware hypervisor smoke using a freshly built kernel.

Examples:
  make ARCH=aarch64 BRINGUP=1 kernel-only -j8
  python3 tools/qemu/check_hyp_multiarch.py --arch aarch64 --kernel PATH/kernel.bin
  python3 tools/qemu/check_hyp_multiarch.py --arch x86_64 --kernel PATH/kernel.elf

The disabled-extension mode must SKIP cleanly and still shut the host down.
"""
import argparse
import re
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]


def command(arch, kernel, disabled=False, accel="tcg", cpus=1):
    bootargs = "a20.hyp_test=1" + (" a20.hyp_smp=1" if cpus > 1 else "")
    common = ["-m", "1G", "-nographic", "-smp", str(cpus), "-kernel", str(kernel),
              "-append", bootargs]
    if arch in ("aarch64", "arm32"):
        return ["qemu-system-aarch64" if arch == "aarch64" else "qemu-system-arm", "-machine",
                "virt,virtualization=" + ("off" if disabled else "on"),
                "-cpu", "cortex-a57" if arch == "aarch64" else "cortex-a15", "-accel", "tcg", *common]
    if arch == "x86_64":
        cpu = "host" if accel == "kvm" else "max"
        if disabled:
            cpu += ",-svm,-vmx"
        return ["qemu-system-x86_64", "-machine", "pc", "-accel", accel,
                "-cpu", cpu, "-no-reboot", "-fw_cfg",
                "name=opt/x86/cmdline,string=" + bootargs, *common]
    if arch == "loongarch64":
        return ["qemu-system-loongarch64", "-machine", "virt", "-cpu", "max", "-accel", "tcg", *common]
    return ["qemu-system-" + arch, "-machine", "virt", "-cpu",
            ("rv32,h=" if arch == "riscv32" else "rv64,h=") + ("false" if disabled else "true"), "-bios", "default",
            "-accel", "tcg", *common]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--arch", choices=("aarch64", "arm32", "x86_64", "riscv64", "riscv32", "loongarch64"), required=True)
    source = p.add_mutually_exclusive_group(required=True)
    source.add_argument("--kernel", type=Path)
    source.add_argument("--build", action="store_true", help="build this worktree with BRINGUP=1 first")
    p.add_argument("--cpus", type=int, choices=(1, 2), default=1, help="2 enables parallel vCPU rendezvous and teardown")
    p.add_argument("--jobs", type=int, default=8)
    p.add_argument("--without-virtualization", action="store_true")
    p.add_argument("--accel", choices=("tcg", "kvm"), default="tcg")
    p.add_argument("--timeout", type=float, default=90)
    p.add_argument("--log", type=Path)
    args = p.parse_args()
    if args.arch == "loongarch64" and not args.without_virtualization:
        p.error("LoongArch TCG lacks LVZ; only the unavailable-backend negative control can run locally")
    if args.build:
        build_log = ROOT / ".kernel-build/smoke" / ("hyp-" + args.arch + "-build.log")
        build_log.parent.mkdir(parents=True, exist_ok=True)
        with build_log.open("w") as output:
            result = subprocess.run(["make", "-C", str(ROOT), f"ARCH={args.arch}", "BRINGUP=1", f"NR_CPUS={args.cpus}", "kernel-only",
                                     f"-j{max(1, args.jobs)}"] + (["UART_CMDLINE=y"] if args.arch == "loongarch64" else []), cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
        build_text = build_log.read_text(errors="replace")
        paths = re.findall(r"Kernel-only build complete: (\S+)", build_text)
        if result.returncode or not paths:
            print(f"FAIL: kernel build; log: {build_log}\n{build_text[-4000:]}", file=sys.stderr)
            return 1
        args.kernel = (ROOT / paths[-1]).with_suffix(".bin" if args.arch in ("aarch64", "arm32") else ".elf")
    if not args.kernel.is_file():
        p.error("kernel does not exist; build ARCH=<arch> BRINGUP=1 kernel-only first")
    if args.arch in ("aarch64", "arm32") and args.kernel.suffix != ".bin":
        p.error("Arm needs kernel.bin so QEMU supplies the DTB and boot arguments")
    if args.accel == "kvm" and args.arch != "x86_64":
        p.error("this smoke supports KVM only for x86_64")
    log = args.log or Path(".kernel-build/smoke") / (
        "hyp-" + args.arch + ("-smp" if args.cpus > 1 else "") + ("-disabled-" + args.accel if args.without_virtualization else "-" + args.accel) + ".log")
    log.parent.mkdir(parents=True, exist_ok=True)
    argv = command(args.arch, args.kernel.resolve(), args.without_virtualization, args.accel, args.cpus)
    try:
        with log.open("w") as output:
            if args.arch == "loongarch64":
                result = subprocess.Popen(argv, stdout=output, stderr=subprocess.STDOUT,
                                          stdin=subprocess.PIPE)
                deadline = time.monotonic() + args.timeout
                try:
                    while "none\n[UARTCMD] " not in log.read_text(errors="replace"):
                        if result.poll() is not None or time.monotonic() >= deadline:
                            raise subprocess.TimeoutExpired(argv, args.timeout)
                        time.sleep(0.01)
                    result.stdin.write(b"a20.hyp_test=1 a20.lvz_experimental=1\n")
                    result.stdin.close()
                    result.wait(timeout=max(0.1, deadline - time.monotonic()))
                except subprocess.TimeoutExpired:
                    result.kill()
                    result.wait()
                    raise
            else:
                result = subprocess.run(argv, stdout=output, stderr=subprocess.STDOUT,
                                        stdin=subprocess.DEVNULL, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        print(f"FAIL: QEMU timed out; log: {log}", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"FAIL: {exc}; log: {log}", file=sys.stderr)
        return 1
    text = log.read_text(errors="replace")
    backend = {"aarch64": "aarch64-el2", "x86_64": "x86-svm", "riscv64": "riscv-h", "riscv32": "riscv32-h", "arm32": "arm32-hyp", "loongarch64": "loongarch-lvz-experimental"}[args.arch]
    required = ["part ok", "System is going down for power-off NOW"]
    if args.without_virtualization:
        required += ["HYP_ARCH_TEST: SKIP (backend unavailable)"]
    else:
        required += ["HYP_ARCH_TEST: NX+RO PASS", "HYP_SELFTEST: PASS", f"HYP_ARCH_TEST: PASS backend={backend} rounds=3"]
        if args.arch in ("riscv32", "riscv64"):
            required += ["HYP_SBI_TEST: PASS"]
        if args.arch != "riscv64":
            required += ["HYP_ARCH_TEST: preemption+stop PASS"]
        if args.cpus > 1 and args.arch in ("riscv32", "riscv64"):
            required += ["HYP_SBI_SMP_TEST: PASS remote-ipi+rfence+hsm"]
        if args.cpus > 1:
            required += [f"HYP_SMP_TEST: PASS backend={backend} cpus=0,1 peak=2 step=stopped"]
    forbidden = ["HYP_SBI_TEST: FAIL", "HYP_SMP_TEST: FAIL", "HYP_ARCH_TEST: FAIL", "HYP_SELFTEST: FAIL", "PANIC", "Kernel Panic"]
    if args.without_virtualization:
        forbidden += ["HYP_ARCH_TEST: PASS", "HYP_SBI_TEST: PASS", "HYP_SBI_SMP_TEST: PASS"]
    missing = [marker for marker in required if marker not in text]
    found = [marker for marker in forbidden if marker in text]
    if result.returncode or missing or found:
        print(f"FAIL: exit={result.returncode} missing={missing} forbidden={found}; log: {log}", file=sys.stderr)
        print(text[-4000:], file=sys.stderr)
        return 1
    print(f"PASS: {args.arch} {'unavailable' if args.without_virtualization else backend}; log: {log}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
