#!/usr/bin/env python3
"""ext4 JBD2 crash-consistency gate.

Boots a journalled ext4 image, halts the machine at a chosen point in the
commit sequence (armed through ``/proc/a20/journal``), reboots the *same*
image and checks three things:

  * the writes the kernel had promised are still there,
  * the writes it had not promised are gone rather than half-there,
  * the host's own e2fsck agrees the filesystem is consistent, both on the
    image the crash left behind and on the one recovery finished with.

The last check is the one that matters most.  A replay that "restores" the
file but writes a journal checksum into a block bitmap's hashed tail leaves an
image e2fsck calls corrupt, and no amount of reading the file back would say so.

Each crash point is a different promise boundary, so each has its own expected
outcome:

  post-recover-flag  the needs_recovery bit is set, nothing has been logged
  post-journal       descriptor and data blocks are logged, no commit block
  post-commit        the commit block is durable: the transaction must replay
  post-checkpoint    metadata has reached its home location; replay is a no-op
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import time
import shutil
import subprocess
import sys
import tempfile

# (point, must the written file survive the crash?)
CRASH_POINTS = [
    ("post-recover-flag", False),
    ("post-journal", False),
    ("post-commit", True),
    ("post-checkpoint", True),
]

# The crash point goes in through /proc/a20/journal rather than a20.journal_crash=
# on the command line: QEMU's LoongArch virt board creates /chosen without a
# bootargs property and drops -append, so a gate that selected its injection
# point that way would pass on four architectures and prove nothing on the
# fifth.  Nothing else about the gate depends on bootargs either way.
def write_script(point=None):
    lines = ["mkdir /extra/j",
             "cat /proc/version > /extra/j/f.txt"]
    if point:
        lines.append(f"echo {point} > /proc/a20/journal")
    lines += ["sync", "poweroff"]
    return "\n".join(lines) + "\n"

READ_SCRIPT = "cat /extra/j/f.txt\npoweroff\n"

# How the repository boots each architecture under QEMU.  Kept here rather than
# borrowed from the smoke cases because this gate needs two drives (the FAT32
# image carries /bin/init, the ext4 one is the filesystem under test) and its
# own -append, neither of which any existing instance spells out.
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
    "x86_64": lambda k, f, e, a: [
        "qemu-system-x86_64", "-machine", "q35", "-m", "1G", "-nographic",
        "-smp", "1", "-no-reboot",
        "-drive", f"file={f},if=none,format=raw,id=xf",
        "-device", "virtio-blk-pci,drive=xf",
        "-drive", f"file={e},if=none,format=raw,id=xe",
        "-device", "virtio-blk-pci,drive=xe",
        "-kernel", k,
    ] + a,
    "loongarch64": lambda k, f, e, a: [
        "qemu-system-loongarch64", "-machine", "virt", "-m", "1G", "-nographic",
        "-smp", "1", "-no-reboot",
        "-drive", f"file={f},if=none,format=raw,id=xf",
        "-device", "virtio-blk-pci,drive=xf",
        "-drive", f"file={e},if=none,format=raw,id=xe",
        "-device", "virtio-blk-pci,drive=xe",
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

    -n keeps it read-only, so a journal that still needs replaying is reported
    as a warning rather than repaired; the gate treats any of the strings below
    as a failure, because each one names a real inconsistency.
    """
    proc = subprocess.run([e2fsck, "-fn", image], capture_output=True, text=True)
    out = proc.stdout + proc.stderr
    bad = [line for line in out.splitlines()
           if any(marker in line for marker in (
               "count wrong", "does not match checksum", "ERROR",
               "MULTIPLE", "UNEXPECTED", "missing", "orphan",
               "FILE SYSTEM WAS MODIFIED", "UNDELETE"))]
    if proc.returncode or bad:
        raise GateError(f"e2fsck exited {proc.returncode}:\n" + out)
    return out


def run_point(arch, build_dir, workdir, log_dir, delay, e2fsck,
              point, survives, timeout):
    fat32 = os.path.join(build_dir, "fat32.img")
    ext4 = os.path.join(build_dir, "ext4-journal.img")
    kernel = os.path.join(build_dir, "kernel.elf")
    for path in (fat32, ext4, kernel):
        if not os.path.exists(path):
            raise GateError(f"missing build artefact: {path} "
                            f"(run 'make ARCH={arch} dev-build' first)")

    fat_copy = os.path.join(workdir, "fat32.img")
    ext_copy = os.path.join(workdir, "ext4.img")
    shutil.copyfile(fat32, fat_copy)
    shutil.copyfile(ext4, ext_copy)

    make_argv = QEMU[arch](kernel, fat_copy, ext_copy, ["-append", "console=ttyS0"])

    # Boot 1: write, sync, and die at the chosen point.
    log1 = os.path.join(log_dir, f"ext4-journal-{arch}-{point}-crash.log")
    feed(make_argv, write_script(point), delay, log1, timeout)
    text1 = read_log(log1)
    marker = f"CRASH-INJECT {point}"
    if marker not in text1:
        raise GateError(f"crash point {point} never fired\n"
                        f"  tail of {log1}:\n" + tail(text1))
    if "[PANIC]" not in text1:
        raise GateError(f"{point}: injected crash did not panic\n" + tail(text1))
    Path(log1 + ".fsck").write_text(fsck_clean(ext_copy, e2fsck))

    # Boot 2: same image, no injection.  Recovery has to run on its own.
    log2 = os.path.join(log_dir, f"ext4-journal-{arch}-{point}-recover.log")
    feed(QEMU[arch](kernel, fat_copy, ext_copy, []), READ_SCRIPT, delay,
         log2, timeout)
    text2 = read_log(log2)
    if "Refusing mount" in text2 or "[EXT4] Mounted" not in text2:
        raise GateError(f"{point}: filesystem did not mount after the crash\n"
                        + tail(text2))
    if "[PANIC]" in text2:
        raise GateError(f"{point}: recovery boot panicked\n" + tail(text2))

    got_file = "A20OS version" in text2
    replayed = "replay complete" in text2
    if survives:
        if not got_file:
            raise GateError(f"{point}: committed transaction was lost -- "
                            f"replayed={replayed}\n" + tail(text2))
        if not replayed:
            raise GateError(f"{point}: data survived without a reported replay\n"
                            + tail(text2))
    elif got_file:
        raise GateError(f"{point}: an uncommitted transaction came back\n"
                        + tail(text2))
    Path(log2 + ".fsck").write_text(fsck_clean(ext_copy, e2fsck))
    return replayed


def run_clean(arch, build_dir, workdir, log_dir, delay, e2fsck, timeout):
    """Control: no crash at all.  Nothing may be lost and nothing may leak."""
    fat32 = os.path.join(build_dir, "fat32.img")
    ext4 = os.path.join(build_dir, "ext4-journal.img")
    kernel = os.path.join(build_dir, "kernel.elf")
    fat_copy = os.path.join(workdir, "fat32.img")
    ext_copy = os.path.join(workdir, "ext4.img")
    shutil.copyfile(fat32, fat_copy)
    shutil.copyfile(ext4, ext_copy)

    log1 = os.path.join(log_dir, f"ext4-journal-{arch}-clean-write.log")
    feed(QEMU[arch](kernel, fat_copy, ext_copy, []), write_script(), delay,
         log1, timeout)
    text1 = read_log(log1)
    if "recovery start=" in text1:
        raise GateError("a clean run replayed a journal: the previous boot's "
                        "checkpoint did not finish\n" + tail(text1))
    Path(log1 + ".fsck").write_text(fsck_clean(ext_copy, e2fsck))

    log2 = os.path.join(log_dir, f"ext4-journal-{arch}-clean-read.log")
    feed(QEMU[arch](kernel, fat_copy, ext_copy, []), READ_SCRIPT, delay,
         log2, timeout)
    text2 = read_log(log2)
    if "A20OS version" not in text2:
        raise GateError("clean run lost a synced file across a reboot\n"
                        + tail(text2))
    if "recovery start=" in text2:
        raise GateError("clean reboot replayed a journal it should not have\n"
                        + tail(text2))
    Path(log2 + ".fsck").write_text(fsck_clean(ext_copy, e2fsck))


def feed(argv, script, delay, log_path, timeout):
    """Require a natural exit; feed each command only after a fresh shell prompt.

    delay remains accepted for callers of the older fixed-delay gate. Terminal
    cursor queries are answered so BusyBox line editing works over pipes.
    """
    del delay
    output = bytearray()
    pending = None if callable(script) else script.splitlines()
    cursor = queries = 0
    last_output = time.monotonic()
    with subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, bufsize=0) as guest:
        with selectors.DefaultSelector() as poller:
            poller.register(guest.stdout, selectors.EVENT_READ)
            deadline = time.monotonic() + timeout
            try:
                while guest.poll() is None and time.monotonic() < deadline:
                    for key, _ in poller.select(.03):
                        data = os.read(key.fd, 65536)
                        if data:
                            output.extend(data)
                            last_output = time.monotonic()
                        else:
                            poller.unregister(key.fileobj)
                    seen = output.count(b"\x1b[6n")
                    if guest.poll() is None and seen > queries:
                        guest.stdin.write(b"\x1b[1;1R" * (seen - queries))
                        guest.stdin.flush()
                        queries = seen
                    if pending is None and re.search(rb"(?:^|[\r\n])# ", output):
                        pending = script(bytes(output)).splitlines()
                    if pending and guest.poll() is None and time.monotonic() - last_output >= .05:
                        if re.search(rb"(?:^|[\r\n])# ", output[cursor:]):
                            cursor = len(output)
                            guest.stdin.write((pending.pop(0) + "\n").encode())
                            guest.stdin.flush()
                if guest.poll() is None:
                    raise GateError(f"guest did not finish within {timeout}s (killed)")
                output.extend(guest.stdout.read() or b"")
                if guest.returncode:
                    raise GateError(f"guest exited with status {guest.returncode}")
                if re.search(rb"UBSAN:|Kernel BUG|LOCK-STALL", output):
                    raise GateError("guest kernel diagnostic failed")
                if b"CRASH-INJECT " not in output and b"[SD] POWER_OFF entry" not in output:
                    raise GateError("guest exited without crash injection or completed shutdown")
            finally:
                if guest.poll() is None:
                    guest.kill(); guest.wait()
                Path(log_path).write_bytes(output)


def tail(text, lines=25):
    return "  " + "\n  ".join(text.splitlines()[-lines:])


def run_main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", default="x86_64", choices=sorted(QEMU))
    parser.add_argument("--build-dir",
                        help="defaults to .kernel-build/<arch>-qemu-virt-<arch>-both-dev")
    parser.add_argument("--log-dir", default="build/smoke-logs")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--delay", type=int, default=22,
                        help="seconds to wait before typing at the shell")
    parser.add_argument("--e2fsck", default=shutil.which("e2fsck") or "e2fsck")
    parser.add_argument("--cpus", type=int, default=1, help="configured guest CPU count")
    parser.add_argument("--keep", action="store_true",
                        help="keep the scratch image directory")
    args = parser.parse_args()

    if args.cpus < 1:
        parser.error("--cpus must be positive")
    original_qemu = QEMU[args.arch]
    def configured_qemu(*values):
        argv = original_qemu(*values)
        argv[argv.index("-smp") + 1] = str(args.cpus)
        return argv
    QEMU[args.arch] = configured_qemu

    if shutil.which(args.e2fsck) is None and not os.path.exists(args.e2fsck):
        print(f"ext4-journal-gate: {args.e2fsck} not found; cannot verify "
              f"the images", file=sys.stderr)
        return 2

    build_dir = args.build_dir or os.path.join(
        ".kernel-build", f"{args.arch}-qemu-virt-{args.arch}-both-dev")
    os.makedirs(args.log_dir, exist_ok=True)

    control = tempfile.mkdtemp(prefix="a20-journal-")
    workdir = None
    report = dict(passed=False, arch=args.arch, cpus=args.cpus, cases=[],
                  kernel_sha256=hashlib.sha256(Path(build_dir, "kernel.elf").read_bytes()).hexdigest(),
                  image_sha256=hashlib.sha256(Path(build_dir, "ext4-journal.img").read_bytes()).hexdigest())
    def record(name, directory):
        report["cases"].append(dict(name=name, image=str(Path(directory, "ext4.img")),
            postimage_sha256=hashlib.sha256(Path(directory, "ext4.img").read_bytes()).hexdigest()))
        Path(args.log_dir, "report.json").write_text(json.dumps(report, indent=2) + "\n")
    try:
        run_clean(args.arch, build_dir, control, args.log_dir, args.delay,
                  args.e2fsck, args.timeout)
        record("clean", control)
        print("ext4-journal-gate: clean run PASS (no replay, file intact, "
              "e2fsck clean)")
        for point, survives in CRASH_POINTS:
            workdir = tempfile.mkdtemp(prefix="a20-journal-")
            replayed = run_point(args.arch, build_dir, workdir, args.log_dir,
                                 args.delay, args.e2fsck, point, survives,
                                 args.timeout)
            record(point, workdir)
            state = "replayed" if replayed else "nothing to replay"
            expect = "survives" if survives else "correctly lost"
            print(f"ext4-journal-gate: {point} PASS "
                  f"({expect}, {state}, e2fsck clean)")
            if not args.keep:
                shutil.rmtree(workdir, ignore_errors=True)
                workdir = None
        if not args.keep:
            shutil.rmtree(control, ignore_errors=True)
            control = None
        report["passed"] = True
    except GateError as exc:
        report["error"] = str(exc)
        print(f"ext4-journal-gate: FAIL\n{exc}", file=sys.stderr)
        return 1
    finally:
        Path(args.log_dir, "report.json").write_text(json.dumps(report, indent=2) + "\n")
        # The per-crash-point workdir and the control dir each hold a full ext4
        # image (hundreds of MB), so a failure or a killed run must reclaim them
        # too -- previously only the success path did, leaking one image per
        # aborted run into TMPDIR.
        if not args.keep:
            for leftover in (workdir, control):
                if leftover is not None:
                    shutil.rmtree(leftover, ignore_errors=True)
    return 0


def main():
    root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(root / "tools"))
    sys.path.insert(0, str(root / "tools/cli"))
    from a20_resource import Policy, Requirement, gate
    from smoke import guest_slot_locks, _release_guest_slot_locks
    state, handles = guest_slot_locks()
    if state == "busy":
        print("ext4-journal-gate: shared QEMU slot busy"); return 75
    try:
        policy = Policy.from_env()
        gate(Requirement(1024 + policy.reserve_mem_mb, 2, policy.min_disk_mb),
             policy, root, wait=False, label="ext4 journal crash recovery")
        return run_main()
    finally:
        if handles is not None: _release_guest_slot_locks(handles)


if __name__ == "__main__":
    sys.exit(main())
