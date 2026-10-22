#!/usr/bin/env python3
"""Legacy (v1/v2) JBD2 journal gate.

Two images, one boot:

  * a clean v1 journal must be admitted READ-ONLY through the explicit
    downgrade, must serve reads, and must reject writes with EROFS; mounting it
    writable must be refused (there is no replay or write support for a legacy
    log);
  * the same image with a non-zero journal start (a log holding transactions)
    must be refused even for a read-only mount, because a legacy log cannot be
    validated.

The host's e2fsck has the last word on the clean image.
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import img  # noqa: E402


class GateError(Exception):
    pass


def make_dirty(src, dst):
    """Point the legacy journal's s_start at block 1: a log with work in it."""
    shutil.copyfile(src, dst)
    path = Path(dst)
    raw = path.read_bytes()
    off = img._journal_superblock_offset(raw)
    meta = bytearray(raw[off:off + 4096])
    if img._be32(meta, 0) != img.JBD2_MAGIC:
        raise GateError("no JBD2 superblock in the legacy image")
    struct.pack_into(">I", meta, 28, 1)          # s_start
    path.write_bytes(raw[:off] + bytes(meta) + raw[off + 4096:])


def qemu_argv(kernel, fat, clean, dirty):
    argv = [
        "qemu-system-riscv64", "-machine", "virt", "-m", "1G", "-nographic",
        "-smp", "1", "-no-reboot", "-bios", "default",
        "-global", "virtio-mmio.force-legacy=false",
    ]
    for idx, path in enumerate((fat, clean, dirty)):
        argv += ["-drive", f"file={path},if=none,format=raw,id=x{idx}",
                 "-device", f"virtio-blk-device,drive=x{idx},"
                            f"bus=virtio-mmio-bus.{idx}"]
    return argv + ["-kernel", kernel]


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--log-dir", required=True)
    ap.add_argument("--delay", type=int, default=22)
    ap.add_argument("--timeout", type=int, default=180)
    a = ap.parse_args()

    e2fsck = shutil.which("e2fsck") or "/usr/sbin/e2fsck"
    kernel = os.path.join(a.build_dir, "kernel.elf")
    clean_src = os.path.join(a.build_dir, "ext4-journal-legacy.img")
    if not os.path.exists(clean_src):
        raise GateError(f"missing {clean_src}; build the legacy image first")
    os.makedirs(a.log_dir, exist_ok=True)

    work = tempfile.mkdtemp(prefix="a20-legacy-journal-")
    fat = os.path.join(work, "fat32.img")
    clean = os.path.join(work, "legacy-clean.img")
    dirty = os.path.join(work, "legacy-dirty.img")
    shutil.copyfile(os.path.join(a.build_dir, "fat32.img"), fat)
    shutil.copyfile(clean_src, clean)
    try:
        make_dirty(clean_src, dirty)
        script = ("ext4_legacy_ro_test /dev/vdb clean\n"
                  "ext4_legacy_ro_test /dev/vdc dirty\n"
                  "poweroff\n")
        text = feed(qemu_argv(kernel, fat, clean, dirty), script, a.delay,
                    os.path.join(a.log_dir, "ext4-legacy-journal-riscv64.log"),
                    a.timeout)
        if "LEGACY_JOURNAL: SKIP" in text:
            raise GateError("the block device was not attached\n" + tail(text))
        if not re.search(r"LEGACY_JOURNAL: PASS ro-admitted", text):
            raise GateError("clean v1 journal was not admitted read-only\n"
                            + tail(text))
        if "LEGACY_JOURNAL: FAIL" in text:
            raise GateError("legacy journal behaviour was wrong\n" + tail(text))
        if not re.search(r"LEGACY_JOURNAL: PASS dirty-rejected", text):
            raise GateError("a dirty legacy log was not refused\n" + tail(text))

        proc = subprocess.run([e2fsck, "-fn", clean], capture_output=True,
                              text=True)
        bad = [l.strip() for l in proc.stdout.splitlines()
               if "Fix?" in l or "WARNING" in l]
        if proc.returncode != 0 or bad:
            raise GateError("e2fsck rejects the clean legacy image:\n  "
                            + "\n  ".join(bad))
        print("ext4-legacy-journal-gate: PASS (clean v1 read-only admitted, "
              "write=EROFS, writable refused, dirty log refused, e2fsck clean)")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def tail(text, lines=25):
    return "  " + "\n  ".join(text.splitlines()[-lines:])


if __name__ == "__main__":
    try:
        main()
    except GateError as exc:
        print(f"ext4-legacy-journal-gate: FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
