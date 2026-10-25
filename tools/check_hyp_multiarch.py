#!/usr/bin/env python3
"""Run the opt-in real hardware hypervisor smoke using a freshly built kernel.

Examples:
  make ARCH=aarch64 BRINGUP=1 kernel-only -j8
  python3 tools/check_hyp_multiarch.py --arch aarch64 --kernel PATH/kernel.bin
  python3 tools/check_hyp_multiarch.py --arch x86_64 --kernel PATH/kernel.elf

The disabled-extension mode must SKIP cleanly and still shut the host down.
"""
import argparse
import re
from pathlib import Path
import subprocess
import sys


def command(arch, kernel, disabled=False, accel="tcg"):
    common = ["-m", "1G", "-nographic", "-smp", "1", "-kernel", str(kernel),
              "-append", "a20.hyp_test=1"]
    if arch == "aarch64":
        return ["qemu-system-aarch64", "-machine",
                "virt,virtualization=" + ("off" if disabled else "on"),
                "-cpu", "cortex-a57", "-accel", "tcg", *common]
    if arch == "x86_64":
        cpu = "host" if accel == "kvm" else "max"
        if disabled:
            cpu += ",-svm"
        return ["qemu-system-x86_64", "-machine", "pc", "-accel", accel,
                "-cpu", cpu, "-no-reboot", "-fw_cfg",
                "name=opt/x86/cmdline,string=a20.hyp_test=1", *common]
    return ["qemu-system-riscv64", "-machine", "virt", "-cpu",
            "rv64,h=" + ("false" if disabled else "true"), "-bios", "default",
            "-accel", "tcg", *common]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--arch", choices=("aarch64", "x86_64", "riscv64"), required=True)
    source = p.add_mutually_exclusive_group(required=True)
    source.add_argument("--kernel", type=Path)
    source.add_argument("--build", action="store_true", help="build this worktree with BRINGUP=1 first")
    p.add_argument("--jobs", type=int, default=8)
    p.add_argument("--without-virtualization", action="store_true")
    p.add_argument("--accel", choices=("tcg", "kvm"), default="tcg")
    p.add_argument("--timeout", type=float, default=90)
    p.add_argument("--log", type=Path)
    args = p.parse_args()
    if args.build:
        build_log = Path(".kernel-build/smoke") / ("hyp-" + args.arch + "-build.log")
        build_log.parent.mkdir(parents=True, exist_ok=True)
        with build_log.open("w") as output:
            result = subprocess.run(["make", f"ARCH={args.arch}", "BRINGUP=1", "kernel-only",
                                     f"-j{max(1, args.jobs)}"], stdout=output, stderr=subprocess.STDOUT)
        build_text = build_log.read_text(errors="replace")
        paths = re.findall(r"Kernel-only build complete: (\S+)", build_text)
        if result.returncode or not paths:
            print(f"FAIL: kernel build; log: {build_log}\n{build_text[-4000:]}", file=sys.stderr)
            return 1
        args.kernel = Path(paths[-1]).with_suffix(".bin" if args.arch == "aarch64" else ".elf")
    if not args.kernel.is_file():
        p.error("kernel does not exist; build ARCH=<arch> BRINGUP=1 kernel-only first")
    if args.arch == "aarch64" and args.kernel.suffix != ".bin":
        p.error("AArch64 needs kernel.bin so QEMU supplies the DTB and boot arguments")
    if args.accel == "kvm" and args.arch != "x86_64":
        p.error("this smoke supports KVM only for x86_64")
    log = args.log or Path(".kernel-build/smoke") / (
        "hyp-" + args.arch + ("-disabled-" + args.accel if args.without_virtualization else "-" + args.accel) + ".log")
    log.parent.mkdir(parents=True, exist_ok=True)
    argv = command(args.arch, args.kernel.resolve(), args.without_virtualization, args.accel)
    try:
        with log.open("w") as output:
            result = subprocess.run(argv, stdout=output, stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        print(f"FAIL: QEMU timed out; log: {log}", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"FAIL: {exc}; log: {log}", file=sys.stderr)
        return 1
    text = log.read_text(errors="replace")
    backend = {"aarch64": "aarch64-el2", "x86_64": "x86-svm", "riscv64": "riscv-h"}[args.arch]
    required = ["part ok", "System is going down for power-off NOW"]
    if args.without_virtualization:
        required += ["HYP_ARCH_TEST: SKIP (backend unavailable)"]
    else:
        required += ["HYP_SELFTEST: PASS", f"HYP_ARCH_TEST: PASS backend={backend} rounds=3"]
        if args.arch != "riscv64":
            required += ["HYP_ARCH_TEST: preemption+stop PASS"]
    forbidden = ["HYP_ARCH_TEST: FAIL", "HYP_SELFTEST: FAIL", "PANIC", "Kernel Panic"]
    if args.without_virtualization:
        forbidden += ["HYP_ARCH_TEST: PASS"]
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
