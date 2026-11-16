#!/usr/bin/env python3
"""Check ext4 hardlink/open-unlink lifetimes against external fsck.

--ufsd selects a matching Native service binary instead of the kernel mount.
Service death, crash cuts and injected metadata failures are separate gates.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "tools/cli"), str(ROOT / "tools/fs")]
from img import enable_journal_checksums_stream
from ext4_journal_gate import feed
from a20_resource import Policy, Requirement, gate
from smoke import guest_slot_locks, _release_guest_slot_locks


def validate(text, cycles):
    matches = re.findall(r"^FS_ORPHAN: PASS old_ino=(\d+) bytes=24576\r?$", text, re.M)
    if len(matches) != cycles or any(int(value) < 11 for value in matches):
        raise ValueError("missing completed hardlink/open-unlink cycles")
    if re.search(r"FS_ORPHAN: FAIL|\[PANIC\]|Kernel panic|UBSAN:|LOCK-STALL", text):
        raise ValueError("guest failure diagnostic")
    if "[SMP] 2/2 configured CPUs online" not in text:
        raise ValueError("missing SMP2 boot")
    if "[SD] POWER_OFF entry" not in text and "[SD] audit errors=0" not in text:
        raise ValueError("missing shutdown")
    if re.search(r"\[SD\] audit errors=[1-9]", text):
        raise ValueError("shutdown audit failed")
    return sorted(set(int(value) for value in matches))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args):
    args.output.mkdir(parents=True, exist_ok=True)
    result = {"passed": False, "scope": "kernel ext4 POSIX lifetime, not Native UFSD/crash/failure certification",
              "kernel_sha256": sha(args.kernel), "input_fat_sha256": sha(args.fat),
              "test_sha256": sha(args.test), "cycles": args.cycles, "cases": []}
    if args.ufsd:
        result["scope"] = "Native UFSD ext4 POSIX lifetime; crash and injected failures are separate"
        result["ufsd_sha256"] = sha(args.ufsd)
    try:
        with tempfile.TemporaryDirectory(prefix="a20-ext4-orphan-") as temporary:
            work = Path(temporary)
            for journal in (False, True):
                name = "journal" if journal else "nojournal"
                ext, fat = work / (name + ".img"), work / "fat.img"
                with ext.open("wb") as stream:
                    stream.truncate(64 * 1024 * 1024)
                features = ("has_journal" if journal else "^has_journal") + ",^metadata_csum,^64bit,^orphan_file"
                subprocess.run(["/usr/sbin/mkfs.ext4", "-q", "-F", "-b", "4096", "-O", features, str(ext)], check=True)
                if journal:
                    enable_journal_checksums_stream(ext)
                token = hashlib.sha256((name + result["test_sha256"]).encode()).hexdigest()
                if args.ufsd:
                    marker = work / "volume-marker"
                    marker.write_text(token)
                    populated = subprocess.run(["/usr/sbin/debugfs", "-w", "-R",
                                                f"write {marker} /a20-orphan-gate-volume", str(ext)],
                                               capture_output=True, text=True, check=True)
                    (args.output / (name + "-populate.log")).write_text(populated.stdout + populated.stderr)
                shutil.copyfile(args.fat, fat)
                subprocess.run(["mcopy", "-o", "-i", str(fat), str(args.test), "::/fs_orphan_lifecycle"], check=True)
                if args.ufsd:
                    subprocess.run(["mcopy", "-o", "-i", str(fat), str(args.ufsd), "::/orphan-ufsd"], check=True)
                command = ["qemu-system-riscv64", "-machine", "virt", "-bios", "default",
                           "-cpu", "rv64,h=true,sstc=true", "-m", "1G", "-smp", "2", "-nographic", "-no-reboot",
                           "-global", "virtio-mmio.force-legacy=false",
                           "-drive", f"file={fat},if=none,format=raw,id=xf",
                           "-device", "virtio-blk-device,drive=xf,bus=virtio-mmio-bus." + ("1" if args.ufsd else "0"),
                           ]
                if args.ufsd:
                    # RV64 generic drivers bind MMIO slots in reverse order.
                    # Slot 2 becomes disk0 (/extra), slot1 disk1 (FAT /bin),
                    # slot0 disk2 (UFSD only). The volume token independently
                    # rejects a server that opened a different disk.
                    dummy = work / "dummy-ext4.img"
                    with dummy.open("wb") as stream:
                        stream.truncate(64 * 1024 * 1024)
                    subprocess.run(["/usr/sbin/mkfs.ext4", "-q", "-F", "-b", "4096", "-O",
                                    "^has_journal,^metadata_csum,^64bit,^orphan_file", str(dummy)], check=True)
                    command += ["-drive", f"file={dummy},if=none,format=raw,id=xd",
                                "-device", "virtio-blk-device,drive=xd,bus=virtio-mmio-bus.2"]
                command += ["-drive", f"file={ext},if=none,format=raw,id=xe", "-device",
                            "virtio-blk-device,drive=xe,bus=virtio-mmio-bus." + ("0" if args.ufsd else "1"),
                            "-kernel", str(args.kernel.resolve())]
                (args.output / (name + "-argv.json")).write_text(json.dumps(command, indent=2) + "\n")
                log = args.output / (name + ".log")
                script = "/bin/fs_orphan_lifecycle /extra\n" * args.cycles
                if args.ufsd:
                    script = ("/bin/orphan-ufsd /ufs 2 ext4 &\n" +
                              (f"FS_ORPHAN_EXPECT_UXFS=1 FS_ORPHAN_EXPECT_VOLUME_TOKEN={token} /bin/fs_orphan_lifecycle /ufs\n") * args.cycles +
                              "umount /ufs && echo FS_ORPHAN_UNMOUNT_PASS\n")
                try:
                    feed(command, script + "poweroff\n", 0, str(log), args.timeout)
                    text = log.read_text(errors="replace")
                    inodes = validate(text, args.cycles)
                    if args.ufsd and (text.count("FS_ORPHAN: mounted=UXFS") != args.cycles or
                                      "UFSD: serving /ufs as ext4" not in text or
                                      "FS_ORPHAN_UNMOUNT_PASS" not in text.splitlines()):
                        raise ValueError("missing real Native ext4 service or completed unmount")
                except Exception:
                    shutil.copyfile(ext, args.output / (name + "-failure.img"))
                    checked = subprocess.run(["/usr/sbin/e2fsck", "-fn", str(ext)], capture_output=True, text=True)
                    (args.output / (name + ".failure.fsck")).write_text(checked.stdout + checked.stderr)
                    raise
                checked = subprocess.run(["/usr/sbin/e2fsck", "-fn", str(ext)], capture_output=True, text=True)
                (args.output / (name + ".fsck")).write_text(checked.stdout + checked.stderr)
                if checked.returncode:
                    shutil.copyfile(ext, args.output / (name + "-failure.img"))
                    raise ValueError(name + " postimage e2fsck failed")
                for inode in inodes:
                    query = subprocess.run(["/usr/sbin/debugfs", "-R", f"testi <{inode}>", str(ext)],
                                           capture_output=True, text=True, check=True)
                    text = query.stdout + query.stderr
                    (args.output / f"{name}-inode-{inode}.txt").write_text(text)
                    if "is not in use" not in text:
                        raise ValueError("last-close inode remains allocated")
                result["cases"].append({"name": name, "passed": True, "qemu_exit": 0,
                                        "e2fsck_exit": 0, "reaped_inodes": inodes})
                print("EXT4_ORPHAN_LIFECYCLE: " + name + " PASS", flush=True)
        if sha(args.fat) != result["input_fat_sha256"]:
            raise ValueError("input FAT changed")
        result["passed"] = True
    except Exception as error:
        result["error"] = str(error)
        print("EXT4_ORPHAN_LIFECYCLE: FAIL " + str(error), file=sys.stderr)
    finally:
        (args.output / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    return 0 if result["passed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True, help="RV64 SMP2 kernel.elf")
    parser.add_argument("--fat", type=Path, required=True)
    parser.add_argument("--test", type=Path, required=True, help="matching RV64 fs_orphan_lifecycle binary")
    parser.add_argument("--ufsd", type=Path, help="matching Native RV64 UFSD binary; use a dedicated disk 2")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    if not 1 <= args.cycles <= 16 or args.timeout <= 0:
        parser.error("cycles 1..16 and positive timeout required")
    policy = Policy.from_env()
    gate(Requirement(1024 + policy.reserve_mem_mb, 2, policy.min_disk_mb), policy, ROOT,
         wait=False, label="ext4 orphan lifecycle")
    state, handles = guest_slot_locks()
    if state == "busy":
        print("EXT4_ORPHAN_LIFECYCLE: shared QEMU slot busy", file=sys.stderr)
        return 75
    try:
        return run(args)
    finally:
        if handles is not None:
            _release_guest_slot_locks(handles)


if __name__ == "__main__":
    raise SystemExit(main())
