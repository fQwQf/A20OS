#!/usr/bin/env python3
"""No-journal ext4 crash-recovery gate.

Boots the plain (no-journal) ext4 image, leaves an orphan behind by opening a
file and unlinking it while the descriptor is still open, kills the machine
instead of letting it shut down, then reboots the same image.

Three things have to hold, and they are the whole point of the gate:

  * the remount *admits* the image.  A no-journal filesystem has no replay
    path, and refusing every image whose state word is not VALID makes any
    crash leave it permanently unmountable;
  * the interrupted unlink is finished, so the orphan inode is freed rather
    than left allocated and unreachable;
  * e2fsck -fn accepts the image both after the crash and after recovery.

The inode check is separate from the e2fsck check on purpose: an image can pass
e2fsck with the inode still sitting on the orphan chain, and that is not the
same thing as having reclaimed it.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

# Open a file, unlink it while the descriptor stays open, then idle so the
# machine has to be killed with the orphan still on disk.
ORPHAN_SCRIPT = (
    "mkdir -p /extra/rcgate\n"
    "exec 3>/extra/rcgate/orphan.txt\n"
    "echo orphan-payload >&3\n"
    "rm /extra/rcgate/orphan.txt\n"
    "echo GATE-ORPHAN-READY\n"
    "sleep 300\n"
)

REMOUNT_SCRIPT = (
    "ls /extra/rcgate\n"
    "echo GATE-REMOUNT-OK\n"
    "poweroff\n"
)


class GateError(Exception):
    pass


def qemu_argv(kernel, fat, ext):
    return [
        "qemu-system-riscv64", "-machine", "virt", "-m", "1G", "-nographic",
        "-smp", "1", "-no-reboot", "-bios", "default",
        "-global", "virtio-mmio.force-legacy=false",
        "-drive", f"file={fat},if=none,format=raw,id=xf",
        "-device", "virtio-blk-device,drive=xf,bus=virtio-mmio-bus.0",
        "-drive", f"file={ext},if=none,format=raw,id=xe",
        "-device", "virtio-blk-device,drive=xe,bus=virtio-mmio-bus.1",
        "-kernel", kernel,
    ]


def feed(argv, script, delay, log_path, timeout):
    feeder = subprocess.Popen(["bash", "-c", f"sleep {delay}; printf '{script}'"],
                              stdout=subprocess.PIPE)
    guest = subprocess.Popen(argv, stdin=feeder.stdout,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    feeder.stdout.close()
    try:
        out, _ = guest.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        guest.kill()
        out, _ = guest.communicate()
    finally:
        if guest.poll() is None:
            guest.kill()
            guest.wait()
        feeder.kill()
        feeder.wait()
    with open(log_path, "wb") as log:
        log.write(out or b"")
    return (out or b"").decode("utf-8", "replace")


def fsck_clean(image, e2fsck):
    """e2fsck -fn must find nothing to fix (never -fy: fixing is not passing)."""
    proc = subprocess.run([e2fsck, "-fn", image], capture_output=True, text=True)
    bad = [line.strip() for line in proc.stdout.splitlines()
           if "Fix?" in line or "WARNING" in line or "UNEXPECTED" in line]
    if proc.returncode != 0 or bad:
        raise GateError("e2fsck rejects the image:\n  " + "\n  ".join(bad or
                        [proc.stdout.strip()]))


def inode_state(image, ino, debugfs):
    proc = subprocess.run([debugfs, "-R", f"stat <{ino}>", image],
                          capture_output=True, text=True)
    return proc.stdout + proc.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--log-dir", required=True)
    ap.add_argument("--delay", type=int, default=10)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--crash-timeout", type=int, default=90)
    a = ap.parse_args()

    e2fsck = shutil.which("e2fsck") or "/usr/sbin/e2fsck"
    debugfs = shutil.which("debugfs") or "/usr/sbin/debugfs"
    kernel = os.path.join(a.build_dir, "kernel.elf")
    os.makedirs(a.log_dir, exist_ok=True)

    work = tempfile.mkdtemp(prefix="a20-ext4-recovery-")
    fat = os.path.join(work, "fat32.img")
    ext = os.path.join(work, "ext4.img")
    shutil.copyfile(os.path.join(a.build_dir, "fat32.img"), fat)
    shutil.copyfile(os.path.join(a.build_dir, "ext4.img"), ext)

    try:
        text = feed(qemu_argv(kernel, fat, ext), ORPHAN_SCRIPT, a.delay,
                    os.path.join(a.log_dir, "ext4-recovery-crash.log"),
                    a.crash_timeout)
        if "GATE-ORPHAN-READY" not in text:
            raise GateError("the guest never reached the orphan setup\n"
                            + tail(text))
        m = re.search(r"unlink orphan ino=(\d+)", text)
        if not m:
            raise GateError("the unlink did not go through the orphan chain\n"
                            + tail(text))
        ino = m.group(1)
        fsck_clean(ext, e2fsck)

        text = feed(qemu_argv(kernel, fat, ext), REMOUNT_SCRIPT, a.delay,
                    os.path.join(a.log_dir, "ext4-recovery-remount.log"),
                    a.timeout)
        if "GATE-REMOUNT-OK" not in text:
            raise GateError("the image did not come back after the crash\n"
                            + tail(text))
        if "refusing mount" in text:
            raise GateError("recovery refused the image it should have fixed\n"
                            + tail(text))
        fsck_clean(ext, e2fsck)

        state = inode_state(ext, ino, debugfs)
        if f"Inode: {ino}" not in state:
            raise GateError(f"inode {ino} vanished from the inode table\n"
                            + tail(state, 6))
        if "Links: 0" not in state or "Type: bad type" not in state:
            raise GateError(
                f"orphan inode {ino} is still allocated after recovery; the "
                f"unlink was not finished\n" + tail(state, 6))
        print(f"ext4-recovery-gate: PASS (inode {ino} reclaimed, e2fsck clean "
              f"after crash and after recovery)")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def tail(text, lines=25):
    return "  " + "\n  ".join(text.splitlines()[-lines:])


if __name__ == "__main__":
    try:
        main()
    except GateError as exc:
        print(f"ext4-recovery-gate: FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
