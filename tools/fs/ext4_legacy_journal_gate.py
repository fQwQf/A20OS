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
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import img  # noqa: E402
from ext4_journal_gate import feed as strict_feed, GateError as BootError


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


def qemu_argv(kernel, fat, clean, dirty, cpus=1):
    argv = [
        "qemu-system-riscv64", "-machine", "virt", "-m", "1G", "-nographic",
        "-smp", str(cpus), "-no-reboot", "-bios", "default",
        "-global", "virtio-mmio.force-legacy=false",
    ]
    for idx, path in enumerate((fat, clean, dirty)):
        argv += ["-drive", f"file={path},if=none,format=raw,id=x{idx}",
                 "-device", f"virtio-blk-device,drive=x{idx},"
                            f"bus=virtio-mmio-bus.{idx}"]
    return argv + ["-kernel", kernel]


def device_script(output):
    """Device names follow discovered VirtIO slots, not the QEMU -drive order."""
    records = re.findall(rb"\[VIRTIO(\d+)\] Block device ready: capacity=(\d+) sectors", output)
    capacities = {int(capacity): int(slot) for slot, capacity in records}
    clean, dirty = 64 * 2048, 68 * 2048
    if clean not in capacities or dirty not in capacities:
        raise GateError("missing unique clean/dirty block capacities")
    if max(capacities[clean], capacities[dirty]) >= 26:
        raise GateError("block slot outside /dev/vda..vdz")
    return (f"ext4_legacy_ro_test /dev/vd{chr(97 + capacities[clean])} clean\n"
            f"ext4_legacy_ro_test /dev/vd{chr(97 + capacities[dirty])} dirty\n"
            "poweroff\n")


def feed(argv, script, delay, log_path, timeout):
    strict_feed(argv, script, delay, log_path, timeout)
    return Path(log_path).read_text(errors="replace")


def run_main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--log-dir", required=True)
    ap.add_argument("--cpus", type=int, default=1)
    ap.add_argument("--delay", type=int, default=22)
    ap.add_argument("--timeout", type=int, default=180)
    a = ap.parse_args()
    if a.cpus < 1: ap.error("--cpus must be positive")

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
        # A harmless trailing region gives the dirty copy a unique capacity;
        # the filesystem geometry remains that of the original 64 MiB image.
        if os.path.getsize(clean) != 64 * 1024 * 1024:
            raise GateError("requires a 64 MiB clean legacy fixture")
        with open(dirty, "r+b") as handle: handle.truncate(68 * 1024 * 1024)
        hashes = {name: hashlib.sha256(Path(path).read_bytes()).hexdigest()
                  for name, path in (("clean", clean), ("dirty", dirty))}
        text = feed(qemu_argv(kernel, fat, clean, dirty, a.cpus), device_script, a.delay,
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

        for name, path in (("clean", clean), ("dirty", dirty)):
            if hashlib.sha256(Path(path).read_bytes()).hexdigest() != hashes[name]:
                raise GateError(name + " legacy image changed during read-only/refusal tests")

        proc = subprocess.run([e2fsck, "-fn", clean], capture_output=True,
                              text=True)
        Path(a.log_dir, "legacy-clean.fsck").write_text(proc.stdout + proc.stderr)
        bad = [l.strip() for l in proc.stdout.splitlines()
               if "Fix?" in l or "WARNING" in l]
        if proc.returncode != 0 or bad:
            raise GateError("e2fsck rejects the clean legacy image:\n  "
                            + "\n  ".join(bad))
        Path(a.log_dir, "report.json").write_text(json.dumps(dict(passed=True, cpus=a.cpus,
            kernel_sha256=hashlib.sha256(Path(kernel).read_bytes()).hexdigest(), images_sha256=hashes), indent=2) + "\n")
        print("ext4-legacy-journal-gate: PASS (clean v1 read-only admitted, "
              "write=EROFS, writable refused, dirty log refused, e2fsck clean)")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def tail(text, lines=25):
    return "  " + "\n  ".join(text.splitlines()[-lines:])


def main():
    root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(root / "tools/cli"))
    from a20_resource import Policy, Requirement, gate
    from smoke import guest_slot_locks, _release_guest_slot_locks
    state, handles = guest_slot_locks()
    if state == "busy": print("ext4-legacy-journal-gate: shared QEMU slot busy"); return 75
    try:
        policy = Policy.from_env()
        gate(Requirement(1024 + policy.reserve_mem_mb, 2, policy.min_disk_mb), policy, root,
             wait=False, label="legacy journal read-only boundary")
        run_main()
        return 0
    except (GateError, BootError, RuntimeError) as exc:
        print(f"ext4-legacy-journal-gate: FAIL: {exc}", file=sys.stderr); return 1
    finally:
        if handles is not None: _release_guest_slot_locks(handles)


if __name__ == "__main__":
    sys.exit(main())
