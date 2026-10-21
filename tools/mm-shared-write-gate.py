#!/usr/bin/env python3
"""MAP_SHARED dirty-state two-boot gate (MM-03).

Boots a guest on an ext4 image, writes a file through a MAP_SHARED mapping in
several shapes, then does a clean ``sync`` + ``poweroff``.  It copies the image
aside and cold-boots the *same* copy a second time, reads the file back and
compares every byte against the pattern the first boot wrote, and runs the
host's own ``e2fsck -fn`` on the image.  Nothing in the guest ever calls
``msync``: the property under test is that the shutdown path flushes the dirty
state, so flushing it by hand would make the gate vacuous.

Each shape is a different way a live mapping can be left behind at shutdown:

  mprotect   the mapping is downgraded to PROT_READ while still mapped
  munmap     the mapping is dropped before the shutdown
  live       the mapping is still fully live at shutdown

A missing marker for a shape fails the gate: the shape has to have actually run
for its PASS to mean anything.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

SHAPES = ["mprotect", "munmap", "live"]

WROTE_MARKER = "MM_SHARED_WRITE: WROTE "
PASS_MARKER = "MM_SHARED_WRITE: PASS "
FAIL_MARKER = "MM_SHARED_WRITE: FAIL"
VERIFY_DONE = "MM_SHARED_WRITE: VERIFY PASS"
VERIFY_EXIT = "MM_SHARED_WRITE_EXIT=0"
SHUTDOWN_MARKER = "System is going down for power-off NOW."

# The test forks a holder for each shape whose mapping must stay live until
# poweroff, so the write command returns before shutdown; the shell then syncs
# and powers off.  sync is the whole flush path -- the guest never msyncs.
WRITE_SCRIPT = "mm_shared_write_test write\nsync\npoweroff\n"

# Capture the exit status too: a byte mismatch makes verify exit non-zero, and
# that has to be asserted on the host rather than trusted to the log alone.
VERIFY_SCRIPT = ("mm_shared_write_test verify\n"
                 "echo MM_SHARED_WRITE_EXIT=$?\n"
                 "poweroff\n")

# How the repository boots each architecture under QEMU.  Two drives: the FAT32
# image is the root filesystem, the ext4 one is the filesystem under test and
# is mounted at /extra.  Copied from tools/ext4_journal_gate.py so this gate
# needs no new machine knowledge.
QEMU = {
    "riscv64": lambda k, f, e, a: [
        "qemu-system-riscv64", "-machine", "virt", "-m", "1G", "-nographic",
        "-smp", "1", "-no-reboot", "-bios", "default",
        "-global", "virtio-mmio.force-legacy=false",
        "-drive", f"file={f},if=none,format=raw,id=xf",
        "-device", "virtio-blk-device,drive=xf,bus=virtio-mmio-bus.0",
        "-drive", f"file={e},if=none,format=raw,id=xe",
        "-device", "virtio-blk-device,drive=xe,bus=virtio-mmio-bus.1",
        "-kernel", k,
    ] + a,
    "aarch64": lambda k, f, e, a: [
        "qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a57",
        "-m", "1G", "-nographic", "-smp", "1", "-no-reboot",
        "-global", "virtio-mmio.force-legacy=false",
        "-drive", f"file={f},if=none,format=raw,id=xf",
        "-device", "virtio-blk-device,drive=xf,bus=virtio-mmio-bus.0",
        "-drive", f"file={e},if=none,format=raw,id=xe",
        "-device", "virtio-blk-device,drive=xe,bus=virtio-mmio-bus.1",
        "-kernel", k,
    ] + a,
}


class GateError(Exception):
    pass


def read_log(path):
    with open(path, errors="replace") as handle:
        return handle.read()


def fsck_clean(image, e2fsck):
    """e2fsck -fn must find nothing to fix.

    -n keeps it read-only, so a filesystem that still needs work is reported
    rather than repaired; each string below names a real inconsistency.
    """
    proc = subprocess.run([e2fsck, "-fn", image], capture_output=True, text=True)
    out = proc.stdout + proc.stderr
    bad = [line for line in out.splitlines()
           if any(marker in line for marker in (
               "count wrong", "does not match checksum", "ERROR",
               "MULTIPLE", "UNEXPECTED", "missing", "orphan",
               "FILE SYSTEM WAS MODIFIED", "UNDELETE"))]
    if bad:
        raise GateError("e2fsck found problems:\n  " + "\n  ".join(bad))
    return out


def feed(argv, script, delay, log_path, timeout):
    """Boot the guest, type `script` at the shell, and stop.

    `timeout` is a hard wall on the whole interaction: a guest that never
    reaches poweroff would otherwise hang the gate forever instead of failing
    it.  The QEMU process is killed and the caller sees a non-zero return so the
    case is reported as a failure with the partial log."""
    feeder = subprocess.Popen(
        ["bash", "-c", f"sleep {delay}; printf '{script}'"],
        stdout=subprocess.PIPE,
    )
    guest = subprocess.Popen(
        argv, stdin=feeder.stdout,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    feeder.stdout.close()
    try:
        try:
            out, _ = guest.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            guest.kill()
            out, _ = guest.communicate()
            with open(log_path, "wb") as log:
                log.write(out or b"")
            raise GateError(
                f"guest did not finish within {timeout}s (killed); the "
                f"interaction hung rather than completing\n"
                + tail((out or b"").decode("utf-8", "replace")))
        with open(log_path, "wb") as log:
            log.write(out or b"")
    finally:
        if guest.poll() is None:
            guest.kill()
            guest.wait()
        feeder.kill()
        feeder.wait()


def tail(text, lines=25):
    return "  " + "\n  ".join(text.splitlines()[-lines:])


def require(condition, message, text):
    if not condition:
        raise GateError(message + "\n" + tail(text))


def run_arch(arch, build_dir, workdir, log_dir, delay, e2fsck, timeout):
    fat32 = os.path.join(build_dir, "fat32.img")
    ext4 = os.path.join(build_dir, "ext4.img")
    kernel = os.path.join(build_dir, "kernel.elf")
    for path in (fat32, ext4, kernel):
        if not os.path.exists(path):
            raise GateError(f"missing build artefact: {path} "
                            f"(run 'make ARCH={arch} dev-build' first)")

    fat_copy = os.path.join(workdir, "fat32.img")
    ext_copy = os.path.join(workdir, "ext4.img")
    shutil.copyfile(fat32, fat_copy)
    shutil.copyfile(ext4, ext_copy)

    # Boot 1: write every shape, sync, and power off cleanly.
    log1 = os.path.join(log_dir, f"mm-shared-write-{arch}-write.log")
    feed(QEMU[arch](kernel, fat_copy, ext_copy, ["-append", "console=ttyS0"]),
         WRITE_SCRIPT, delay, log1, timeout)
    text1 = read_log(log1)
    require("[EXT4] Mounted" in text1,
            f"{arch}: /extra (ext4) did not mount, so nothing was written",
            text1)
    for shape in SHAPES:
        require(f"{WROTE_MARKER}{shape}" in text1,
                f"{arch}: shape {shape} never ran in boot 1 (missing marker)",
                text1)
    require(FAIL_MARKER not in text1, f"{arch}: a write failed", text1)
    require("[PANIC]" not in text1, f"{arch}: boot 1 panicked", text1)
    require(SHUTDOWN_MARKER in text1,
            f"{arch}: boot 1 did not reach a clean shutdown", text1)
    fsck_clean(ext_copy, e2fsck)

    # Boot 2: cold boot the SAME image and read every shape back.
    log2 = os.path.join(log_dir, f"mm-shared-write-{arch}-verify.log")
    feed(QEMU[arch](kernel, fat_copy, ext_copy, ["-append", "console=ttyS0"]),
         VERIFY_SCRIPT, delay, log2, timeout)
    text2 = read_log(log2)
    require("[EXT4] Mounted" in text2,
            f"{arch}: /extra (ext4) did not mount on the verify boot", text2)
    for shape in SHAPES:
        require(f"{PASS_MARKER}{shape}" in text2,
                f"{arch}: shape {shape} did not read back correctly", text2)
    require(FAIL_MARKER not in text2, f"{arch}: a verify failed", text2)
    require(VERIFY_DONE in text2,
            f"{arch}: verify did not report an overall PASS", text2)
    require(VERIFY_EXIT in text2,
            f"{arch}: verify exited non-zero (see log)", text2)
    require("[PANIC]" not in text2, f"{arch}: boot 2 panicked", text2)
    require(SHUTDOWN_MARKER in text2,
            f"{arch}: boot 2 did not reach a clean shutdown", text2)
    fsck_clean(ext_copy, e2fsck)


def default_build_dir(arch):
    base = ".kernel-build"
    try:
        names = os.listdir(base)
    except OSError:
        return None
    prefix = f"{arch}-qemu-virt-{arch}-"
    candidates = sorted(
        name for name in names
        if name.startswith(prefix) and not name.endswith("-ramfs-user")
        and os.path.isfile(os.path.join(base, name, "kernel.elf")))
    return os.path.join(base, candidates[0]) if candidates else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", default="riscv64", choices=sorted(QEMU))
    parser.add_argument("--build-dir",
                        help="defaults to the .kernel-build/<arch>-qemu-virt-* "
                             "directory holding kernel.elf")
    parser.add_argument("--log-dir", default="build/smoke-logs")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--delay", type=int, default=22,
                        help="seconds to wait before typing at the shell")
    parser.add_argument("--e2fsck", default=shutil.which("e2fsck") or "e2fsck")
    parser.add_argument("--keep", action="store_true",
                        help="keep the scratch image directory")
    args = parser.parse_args()

    if shutil.which(args.e2fsck) is None and not os.path.exists(args.e2fsck):
        print(f"mm-shared-write-gate: {args.e2fsck} not found; cannot verify "
              f"the images", file=sys.stderr)
        return 2

    build_dir = args.build_dir or default_build_dir(args.arch)
    if not build_dir:
        print(f"mm-shared-write-gate: no build directory for {args.arch}; "
              f"pass --build-dir or run 'make ARCH={args.arch} dev-build'",
              file=sys.stderr)
        return 2
    os.makedirs(args.log_dir, exist_ok=True)

    workdir = tempfile.mkdtemp(prefix="a20-mmshared-")
    try:
        run_arch(args.arch, build_dir, workdir, args.log_dir, args.delay,
                 args.e2fsck, args.timeout)
        print(f"mm-shared-write-gate: {args.arch} PASS "
              f"({'/'.join(SHAPES)} survived a cold boot; e2fsck clean)")
    except GateError as exc:
        print(f"mm-shared-write-gate: FAIL\n{exc}", file=sys.stderr)
        return 1
    finally:
        if not args.keep:
            shutil.rmtree(workdir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
