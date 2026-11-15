#!/usr/bin/env python3
"""Cold-boot the shared console/PTY controlling-terminal acceptance programs."""
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
sys.path[:0] = [str(ROOT / "tools/fs"), str(ROOT / "tools/cli"), str(ROOT / "tools")]
from ext4_journal_gate import feed
from a20_resource import Policy, Requirement, gate
from smoke import guest_slot_locks, _release_guest_slot_locks

SECTIONS = {
    "TTY_JOBCTRL": ("ownership", "background-read", "background-ioctl", "background-write", "detach"),
    "TTY_LIFECYCLE": ("detach-signals", "leader-exit", "master-close", "thread-sharing", "orphan-access", "automatic-acquire",
                      "vhangup", "blocked-reader", "privileged-steal"),
}


def validate(text, cpus, cycles, require_audit):
    lines = text.replace("\r", "").splitlines()
    if f"[SMP] {cpus}/{cpus} configured CPUs online" not in text:
        raise ValueError("missing configured SMP CPUs")
    if re.search(r"TTY_(?:JOBCTRL|LIFECYCLE): FAIL|\[PANIC\]|Kernel panic|UBSAN:", text):
        raise ValueError("guest failure diagnostic")
    for prefix, sections in SECTIONS.items():
        for section in sections:
            if lines.count(f"{prefix}: {section} PASS") != cycles:
                raise ValueError("missing repeated section: " + section)
    if lines.count("TTY_JOBCTRL: PASS") != cycles or lines.count("TTY_LIFECYCLE: PASS") != 2 * cycles:
        raise ValueError("missing complete program results")
    if "[SD] POWER_OFF entry" not in text and "[SD] audit errors=0" not in text:
        raise ValueError("missing shutdown entry or completed audit")
    audits = re.findall(r"\[SD\] audit errors=(\d+)", text)
    if any(int(value) != 0 for value in audits) or (require_audit and not audits):
        raise ValueError("missing/failed required shutdown audit")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args):
    args.output.mkdir(parents=True, exist_ok=True)
    result = {"passed": False, "arch": args.arch, "cpus": args.cpus, "cycles": args.cycles,
              "kernel_sha256": digest(args.kernel), "input_fat_sha256": digest(args.fat),
              "require_shutdown_audit": args.require_audit, "sections": SECTIONS,
              "probe_sha256": digest(Path(__file__)), "boots": []}
    try:
        with tempfile.TemporaryDirectory(prefix="a20-tty-gate-") as temporary:
            work = Path(temporary)
            for boot in range(args.boots):
                fat = work / "fat.img"
                shutil.copyfile(args.fat, fat)
                if args.user_bin:
                    binaries = {}
                    for name in ("tty_jobcontrol_test", "tty_lifecycle_test"):
                        source = args.user_bin / name
                        binaries[name] = digest(source)
                        subprocess.run(["mcopy", "-o", "-i", str(fat), str(source), "::/" + name], check=True)
                    result["program_sha256"] = binaries
                if args.arch == "riscv64":
                    command = ["qemu-system-riscv64", "-machine", "virt", "-bios", "default",
                               "-cpu", "rv64,h=true,sstc=true"]
                else:
                    command = ["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a57"]
                command += ["-m", "2G", "-smp", str(args.cpus), "-nographic", "-no-reboot",
                            "-global", "virtio-mmio.force-legacy=false",
                            "-drive", f"file={fat},if=none,format=raw,id=xf",
                            "-device", "virtio-blk-device,drive=xf,bus=virtio-mmio-bus.0",
                            "-kernel", str(args.kernel.resolve())]
                stem = f"boot-{boot + 1}"
                (args.output / (stem + "-argv.json")).write_text(json.dumps(command, indent=2) + "\n")
                script = ("/bin/tty_jobcontrol_test\n/bin/tty_lifecycle_test\n"
                          "/bin/tty_lifecycle_test --revocation\n") * args.cycles + "poweroff\n"
                log = args.output / (stem + ".log")
                feed(command, script, 0, str(log), args.timeout)
                validate(log.read_text(errors="replace"), args.cpus, args.cycles, args.require_audit)
                checked = subprocess.run([shutil.which("fsck.fat") or "/usr/sbin/fsck.fat", "-n", str(fat)],
                                         capture_output=True, text=True)
                (args.output / (stem + ".fsck")).write_text(checked.stdout + checked.stderr)
                if checked.returncode:
                    raise ValueError("post-shutdown FAT consistency check failed")
                result["boots"].append({"passed": True, "qemu_exit": 0, "fsck_exit": 0})
                print("TTY_GATE: " + stem + " PASS", flush=True)
        if digest(args.fat) != result["input_fat_sha256"]:
            raise ValueError("input image changed")
        result["passed"] = True
    except Exception as error:
        result["error"] = str(error)
        print("TTY_GATE: FAIL " + str(error), file=sys.stderr)
    finally:
        (args.output / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    return 0 if result["passed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("riscv64", "aarch64"), required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--fat", type=Path, required=True)
    parser.add_argument("--user-bin", type=Path, help="stage freshly built matching-arch test binaries on the private FAT copy")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cpus", type=int, choices=(2, 4), default=2)
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--boots", type=int, default=2)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--require-audit", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.cycles <= 8 or not 1 <= args.boots <= 4 or args.timeout <= 0:
        parser.error("cycles 1..8, boots 1..4 and positive timeout required")
    policy = Policy.from_env()
    gate(Requirement(2048 + policy.reserve_mem_mb, args.cpus, policy.min_disk_mb), policy, ROOT,
         wait=False, label="tty job control")
    state, handles = guest_slot_locks()
    if state == "busy":
        print("TTY_GATE: shared QEMU slot busy", file=sys.stderr)
        return 75
    try:
        return run(args)
    finally:
        if handles is not None:
            _release_guest_slot_locks(handles)


if __name__ == "__main__":
    raise SystemExit(main())
