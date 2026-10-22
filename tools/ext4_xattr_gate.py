#!/usr/bin/env python3
"""ext4 extended-attribute persistence gate.

Boots the unjournaled ext4 image, writes user.* attributes and a POSIX ACL
blob on a file, fsyncs, and powers off.  Then the host reads the attributes
straight out of the image with debugfs and runs e2fsck, and a second boot
reads them back through the kernel.

Checking the image with debugfs matters separately from the reboot: a driver
that kept the attributes in the VFS's volatile table would pass a same-boot
read but leave nothing on disk, and one that laid the inode-body area out
differently from e2fsprogs would still pass its own reads while e2fsck
rejects the inodes.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

WRITE_SCRIPT = "ext4_xattr_test write\npoweroff\n"
VERIFY_SCRIPT = "ext4_xattr_test verify\npoweroff\n"


class GateError(Exception):
    pass


def qemu_argv(kernel, fat, ext):
    return [
        "qemu-system-riscv64", "-machine", "virt", "-m", "1G", "-nographic",
        "-smp", "1", "-no-reboot", "-bios", "default",
        "-global", "virtio-mmio.force-legacy=false",
        "-drive", f"file={fat},if=none,format=raw,id=x0",
        "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
        "-drive", f"file={ext},if=none,format=raw,id=x1",
        "-device", "virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1",
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
    proc = subprocess.run([e2fsck, "-fn", image], capture_output=True, text=True)
    bad = [l.strip() for l in proc.stdout.splitlines()
           if "Fix?" in l or "WARNING" in l]
    if proc.returncode != 0 or bad:
        raise GateError("e2fsck rejects the image:\n  " + "\n  ".join(bad))


def ea_list(image, path, debugfs):
    proc = subprocess.run([debugfs, "-R", f"ea_list {path}", image],
                          capture_output=True, text=True)
    return proc.stdout + proc.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--log-dir", required=True)
    ap.add_argument("--delay", type=int, default=22)
    ap.add_argument("--timeout", type=int, default=180)
    a = ap.parse_args()

    e2fsck = shutil.which("e2fsck") or "/usr/sbin/e2fsck"
    debugfs = shutil.which("debugfs") or "/usr/sbin/debugfs"
    kernel = os.path.join(a.build_dir, "kernel.elf")
    os.makedirs(a.log_dir, exist_ok=True)

    work = tempfile.mkdtemp(prefix="a20-ext4-xattr-")
    fat = os.path.join(work, "fat32.img")
    ext = os.path.join(work, "ext4.img")
    shutil.copyfile(os.path.join(a.build_dir, "fat32.img"), fat)
    shutil.copyfile(os.path.join(a.build_dir, "ext4.img"), ext)

    try:
        text = feed(qemu_argv(kernel, fat, ext), WRITE_SCRIPT, a.delay,
                    os.path.join(a.log_dir, "ext4-xattr-write.log"), a.timeout)
        if "EXT4_XATTR: SKIP" in text:
            raise GateError("the ext4 image was not mounted at /extra\n"
                            + tail(text))
        if "EXT4_XATTR: write PASS" not in text:
            raise GateError("write boot failed\n" + tail(text))

        listing = ea_list(ext, "/xa.dat", debugfs)
        for want in ("user.short", "system.posix_acl_access"):
            if want not in listing:
                raise GateError(f"debugfs does not see {want} on the image:\n"
                                + listing)
        fsck_clean(ext, e2fsck)

        text = feed(qemu_argv(kernel, fat, ext), VERIFY_SCRIPT, a.delay,
                    os.path.join(a.log_dir, "ext4-xattr-verify.log"), a.timeout)
        if "EXT4_XATTR: verify PASS" not in text:
            raise GateError("attributes did not survive the reboot\n"
                            + tail(text))
        fsck_clean(ext, e2fsck)
        print("ext4-xattr-gate: PASS (attributes on disk per debugfs, survive "
              "reboot, replacement persisted, ENOSPC left the inode intact)")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def tail(text, lines=25):
    return "  " + "\n  ".join(text.splitlines()[-lines:])


if __name__ == "__main__":
    try:
        main()
    except GateError as exc:
        print(f"ext4-xattr-gate: FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
