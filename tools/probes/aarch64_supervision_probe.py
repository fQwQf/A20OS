#!/usr/bin/env python3
"""Cold-boot the AArch64 SMP4 Native RTC and supervision recovery gates."""
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
sys.path.insert(0, str(ROOT / "tools/fs"))
sys.path.insert(0, str(ROOT / "tools/cli"))
from ext4_journal_gate import feed
from a20_resource import Policy, Requirement, gate


def validate(text, case, cycles):
    if "[SMP] 4/4 configured CPUs online" not in text:
        raise ValueError("missing four-CPU boot")
    if not re.search(r"\[SD\] audit errors=0(?![0-9])", text):
        raise ValueError("missing clean shutdown audit")
    if re.search(r"NATIVE_\w+: FAIL|FLAP budget|SVC_MGR: FAIL|UFSD:.*failed|"
                 r"rtcd:.*failed|\[PANIC\]|Kernel panic|UBSAN:", text):
        raise ValueError("guest failure diagnostic")
    if case == "rtc":
        if text.count("NATIVE_RTCD: PASS") != 2 or "NATIVE_SVC: PASS restarts=2" not in text:
            raise ValueError("missing RTC or echo recovery")
        alarms = [int(n) for n in re.findall(r"NATIVE_RTCD: alarm_ms=(\d+)", text)]
        if len(alarms) != 2 or any(n < 100 or n > 5000 for n in alarms):
            raise ValueError("missing/early/late PL031 alarm")
    else:
        if "UFSD: serving /ufs as fat" not in text:
            raise ValueError("UFSD did not start on the independent FAT disk")
        for marker in ("NATIVE_REGISTRY: PASS", "NATIVE_REGISTRY: echod hang silent=PASS",
                       "NATIVE_REGISTRY: echod healed echo=PASS",
                       "NATIVE_REGISTRY: echod alive after rejected kill=PASS"):
            if text.count(marker) != cycles:
                raise ValueError("missing repeated check: " + marker)
        if len(re.findall(r"foreign kill rejected st=-\d+", text)) != cycles:
            raise ValueError("missing permission negative controls")
        if text.count("health ping timeout name=echod") < cycles:
            raise ValueError("missing supervisor health timeouts")
        # Native EXITED events carry the signed raw exit code (-SIGKILL),
        # unlike TASK_WAIT's shell-normalized 137. C prints its u64 wire
        # representation; Rust prints the signed value.
        exits = [int(value) for value in re.findall(r"name=echod exit_code=(-?\d+)", text)]
        killed = sum(value in (-9, (1 << 64) - 9) for value in exits)
        if killed != cycles:
            raise ValueError("missing confirmed SIGKILL exits")


def run(args):
    args.output.mkdir(parents=True, exist_ok=True)
    result = {"passed": False, "cpus": 4, "cycles": args.cycles,
              "scope": "Native development image; desktop world is a separate gate",
              "kernel_sha256": hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
              "fat_sha256": hashlib.sha256(args.fat.read_bytes()).hexdigest(), "cases": []}
    try:
        with tempfile.TemporaryDirectory(prefix="a20-aa64-supervision-") as temporary:
            work = Path(temporary)
            for case in ("rtc", "supervisor"):
                fat, scratch = work / "fat.img", work / "scratch.img"
                shutil.copyfile(args.fat, fat)
                with scratch.open("wb") as output:
                    output.truncate(64 * 1024 * 1024)
                subprocess.run([shutil.which("mkfs.fat") or "/usr/sbin/mkfs.fat", "-F", "32", str(scratch)], check=True,
                               stdout=subprocess.DEVNULL)
                command = ["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a57",
                           "-m", "2G", "-smp", "4", "-nographic", "-no-reboot",
                           "-global", "virtio-mmio.force-legacy=false",
                           "-drive", f"file={fat},if=none,format=raw,id=xf",
                           "-device", "virtio-blk-device,drive=xf,bus=virtio-mmio-bus.0",
                           "-drive", f"file={scratch},if=none,format=raw,id=xs",
                           "-device", "virtio-blk-device,drive=xs,bus=virtio-mmio-bus.1",
                           "-kernel", str(args.kernel.resolve()), "-append", "a20.ufsd_blk=1"]
                (args.output / (case + "-argv.json")).write_text(json.dumps(command, indent=2) + "\n")
                if case == "rtc":
                    script = "/bin/svcman-aarch64\n" + "/bin/native-rtcd-aarch64\n" * 2
                else:
                    script = "/bin/svcmgr-aarch64 &\nsleep 2\n" + "/bin/native-registry-aarch64\n" * args.cycles
                log = args.output / (case + ".log")
                feed(command, script + "poweroff\n", 0, str(log), args.timeout)
                validate(log.read_text(errors="replace").replace("\r", ""), case, args.cycles)
                checked = subprocess.run([shutil.which("fsck.fat") or "/usr/sbin/fsck.fat", "-n", str(scratch)], capture_output=True, text=True)
                (args.output / (case + ".fsck")).write_text(checked.stdout + checked.stderr)
                if checked.returncode:
                    raise ValueError("scratch FAT consistency check failed")
                result["cases"].append({"name": case, "passed": True, "qemu_exit": 0, "fsck_exit": 0})
                print("AARCH64_SUPERVISION: " + case + " PASS", flush=True)
        result["passed"] = True
    except Exception as error:
        result["error"] = str(error)
        print("AARCH64_SUPERVISION: FAIL " + str(error), file=sys.stderr)
    finally:
        (args.output / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    return 0 if result["passed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True, help="AArch64 kernel.bin Image")
    parser.add_argument("--fat", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=150)
    args = parser.parse_args()
    if not 1 <= args.cycles <= 4 or args.timeout <= 0:
        parser.error("cycles must be 1..4 (within the service flap budget), timeout positive")
    policy = Policy.from_env()
    gate(Requirement(2048 + policy.reserve_mem_mb, 4, policy.min_disk_mb), policy, ROOT, wait=False, label="aa64-supervision")
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
