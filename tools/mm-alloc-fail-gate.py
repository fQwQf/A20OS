#!/usr/bin/env python3
"""MM-04 allocation-failure rollback gate.

For each allocation stage the kernel can be built to fail, this gate:

  1. builds a riscv64/aarch64 dev image with ``CONFIG_MM_FAIL_STAGE=<n>``
     armed (one stage per image, never a normal build),
  2. boots it and runs ``mm_alloc_fail_test <n>``, which drives the operation
     that reaches that stage,
  3. asserts the injected failure fired, the operation reported the failure
     the real allocator would (errno, or the no-op observable for the
     best-effort swap-out path), the workload's own accounting checks passed,
     and the kernel's shutdown ``[MM-ASM]`` audit is all-zero.

The stages are:

  stage 1  an intermediate page-table node in pt_walk()/pt_map_huge(); the
           DONTUNMAP destination leaf map fails with -ENOMEM.
  stage 2  the per-page frames mm_demote_huge_page() copies into; a partial
           munmap of a huge leaf fails with -ENOMEM after freeing what it
           already allocated.
  stage 3  the swap slot get_swap_page_owned() hands out during swap-out;
           MADV_PAGEOUT must leave every page resident and consume no slot.

The gate lives here rather than in tools/smoke_cases.py because each stage is
a separate kernel build plus boot; the make target invokes it like
smoke-ext4-journal does.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

# The QEMU invocation per architecture, kept here rather than borrowed from
# the smoke cases because this gate owns its own build directory per stage.
QEMU = {
    "riscv64": lambda k, f, a: [
        "qemu-system-riscv64", "-machine", "virt", "-m", "1G", "-nographic",
        "-smp", "1", "-no-reboot", "-bios", "default",
        "-global", "virtio-mmio.force-legacy=false",
        "-drive", f"file={f},if=none,format=raw,id=x0",
        "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
        "-kernel", k,
    ] + a,
    "aarch64": lambda k, f, a: [
        "qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a57",
        "-m", "1G", "-nographic", "-smp", "1", "-no-reboot",
        "-global", "virtio-mmio.force-legacy=false",
        "-drive", f"file={f},if=none,format=raw,id=x0",
        "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
        "-kernel", k,
    ] + a,
}

# stage -> the extra facts that prove the operation really reached the site.
STAGES = {
    1: {"marker": "MM_FAILINJECT: stage=1 fired",
        "expect": [], "timeout": 90},
    2: {"marker": "MM_FAILINJECT: stage=2 fired",
        # huge_install>0 proves a huge leaf was really installed, so the
        # partial munmap reached the demotion the injection targets.
        "expect": [r"huge_install=[1-9]"], "timeout": 90},
    3: {"marker": "MM_FAILINJECT: stage=3 fired",
        "expect": [], "timeout": 150},
}

# The shutdown audit must show the per-PTE status, the VMA list and the shadow
# segments all agreeing.  Any nonzero counter is a rollback that left the
# status sidecar describing freed or absent pages.
AUDIT = (r"\[MM-ASM\].*missing_meta=0 present=0 absent=0 prot=0 cow=0 "
         r"vma=0 vmai=0 cls=0 safe=0.*\bseg_bad=0\b.*\bseg_kind=0\b.*"
         r"\bseg_diff=0\b")

BUILD_SLOT = os.environ.get("A20OS_MM_FAIL_BUILD_SLOT", "build-1")


class GateError(Exception):
    pass


def repo_root():
    out = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise GateError("not inside a git worktree")
    return out.stdout.strip()


def run_build(root, arch, stage):
    """Build one stage under the shared build-slot lease, retrying a busy slot.

    run-build.sh reports a busy slot as EX_TEMPFAIL (75); that is a deliberate
    no-op, so the gate retries instead of failing the stage.
    """
    common = subprocess.run(
        ["git", "-C", root, "rev-parse", "--path-format=absolute",
         "--git-common-dir"], capture_output=True, text=True)
    runner = os.path.join(common.stdout.strip(), "parallel-rewrite",
                          "run-build.sh")
    if not os.path.exists(runner):
        raise GateError(f"build runner not found: {runner}")
    argv = ["sh", runner, BUILD_SLOT, "make", f"ARCH={arch}", "ABI=linux",
            "BRINGUP=0", "dev-build",
            f"EXTRA_CFLAGS=-DCONFIG_MM_FAIL_STAGE={stage}", "-j8"]
    for attempt in range(40):
        proc = subprocess.run(argv, cwd=root, capture_output=True, text=True)
        if proc.returncode == 0:
            return
        if proc.returncode == 75:
            time.sleep(5)
            continue
        raise GateError(f"stage {stage} build failed (rc={proc.returncode})\n"
                        + tail(proc.stdout + proc.stderr))
    raise GateError(f"stage {stage}: build slot stayed busy")


def print_build_dir(root, arch, stage):
    proc = subprocess.run(
        ["make", "--no-print-directory", f"ARCH={arch}", "ABI=linux",
         "BRINGUP=0", f"EXTRA_CFLAGS=-DCONFIG_MM_FAIL_STAGE={stage}",
         "print-build-dir"],
        cwd=root, capture_output=True, text=True)
    if proc.returncode != 0:
        raise GateError(f"print-build-dir failed for stage {stage}\n"
                        + tail(proc.stdout + proc.stderr))
    lines = [ln.strip() for ln in proc.stdout.splitlines() if ln.strip()]
    if not lines:
        raise GateError(f"print-build-dir produced no path for stage {stage}")
    return os.path.join(root, lines[-1])


def read_log(path):
    with open(path, errors="replace") as handle:
        return handle.read()


def tail(text, lines=25):
    return "  " + "\n  ".join(text.splitlines()[-lines:])


def feed(argv, script, delay, log_path, timeout):
    """Boot the guest, type `script`, and stop.

    `timeout` is a hard wall on the whole interaction: a guest that never
    reaches poweroff (a wedged scheduler, a shell that stopped accepting input)
    would otherwise hang the gate forever.  The QEMU process is killed and the
    caller sees a failure with the partial log."""
    feeder = subprocess.Popen(["bash", "-c", f"sleep {delay}; printf '{script}'"],
                              stdout=subprocess.PIPE)
    guest = subprocess.Popen(argv, stdin=feeder.stdout,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    feeder.stdout.close()
    try:
        try:
            out, _ = guest.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            guest.kill()
            out, _ = guest.communicate()
            with open(log_path, "wb") as log:
                log.write(out or b"")
            raise GateError(f"guest did not finish within {timeout}s (killed)\n"
                            + tail((out or b"").decode("utf-8", "replace")))
        with open(log_path, "wb") as log:
            log.write(out or b"")
    finally:
        if guest.poll() is None:
            guest.kill()
            guest.wait()
        feeder.kill()
        feeder.wait()


def run_stage(root, arch, stage, log_dir, delay, keep):
    run_build(root, arch, stage)
    build_dir = print_build_dir(root, arch, stage)
    kernel = os.path.join(build_dir, "kernel.elf")
    fat32 = os.path.join(build_dir, "fat32.img")
    for path in (kernel, fat32):
        if not os.path.exists(path):
            raise GateError(f"missing build artefact: {path}")

    workdir = tempfile.mkdtemp(prefix=f"a20-mmfail-s{stage}-")
    try:
        fat_copy = os.path.join(workdir, "fat32.img")
        shutil.copyfile(fat32, fat_copy)
        log_path = os.path.join(log_dir, f"mm-alloc-fail-{arch}-stage{stage}.log")
        # The leading newline matters: QEMU serial input can lose the first
        # byte if it arrives before the shell is ready, and the loss lands
        # harmlessly on an empty line instead of eating a character off the
        # command name.
        script = f"\\nmm_alloc_fail_test {stage}\\npoweroff\\n"
        feed(QEMU[arch](kernel, fat_copy, ["-append", "console=ttyS0"]),
             script, delay, log_path, STAGES[stage]["timeout"])
        text = read_log(log_path)
    finally:
        if not keep:
            shutil.rmtree(workdir, ignore_errors=True)

    if re.search(r"PANIC|Kernel panic|LOCK-STALL", text):
        raise GateError(f"stage {stage}: guest panicked\n" + tail(text))
    if STAGES[stage]["marker"] not in text:
        raise GateError(f"stage {stage}: injection never fired\n" + tail(text))
    if f"MM_ALLOC_FAIL_TEST: stage={stage} PASS" not in text:
        raise GateError(f"stage {stage}: workload did not pass\n" + tail(text))
    if not re.search(AUDIT, text):
        raise GateError(f"stage {stage}: [MM-ASM] audit is not clean\n"
                        + tail(text))
    for pattern in STAGES[stage]["expect"]:
        if not re.search(pattern, text):
            raise GateError(f"stage {stage}: missing /{pattern}/\n" + tail(text))
    if "System is going down for power-off NOW" not in text:
        raise GateError(f"stage {stage}: no clean poweroff\n" + tail(text))
    return log_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", default="riscv64", choices=sorted(QEMU))
    parser.add_argument("--build-dir", default=None,
                        help="unused; the build directory is discovered from "
                             "the Makefile so an EXTRA_CFLAGS change cannot "
                             "make it stale")
    parser.add_argument("--log-dir", default="build/smoke-logs")
    parser.add_argument("--timeout", type=int, default=None,
                        help="override the per-stage guest wall clock")
    parser.add_argument("--delay", type=int, default=25,
                        help="seconds to wait before typing at the shell")
    parser.add_argument("--stage", type=int, action="append",
                        help="only run this stage (repeatable); default all")
    parser.add_argument("--keep", action="store_true",
                        help="keep the scratch image directory")
    args = parser.parse_args()

    root = repo_root()
    os.makedirs(os.path.join(root, args.log_dir), exist_ok=True)
    log_dir = os.path.join(root, args.log_dir)
    stages = args.stage or sorted(STAGES)

    try:
        for stage in stages:
            if stage not in STAGES:
                raise GateError(f"unknown stage {stage}")
            if args.timeout is not None:
                STAGES[stage]["timeout"] = args.timeout
            log_path = run_stage(root, args.arch, stage, log_dir, args.delay,
                                 args.keep)
            print(f"mm-alloc-fail-gate: stage {stage} PASS "
                  f"(injection fired, operation failed, accounting baseline, "
                  f"audit clean); log {os.path.relpath(log_path, root)}")
    except GateError as exc:
        print(f"mm-alloc-fail-gate: FAIL\n{exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
