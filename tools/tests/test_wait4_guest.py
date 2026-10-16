#!/usr/bin/env python3
"""Run the wait4 option matrix in a QEMU guest and report the verdict.

The FAT image is the shared workload input: the same image bytes are fed to
the C-reference kernel and the Rust kernel, so this runner takes the image as
given, copies it into the output directory, drives the console, and hashes it
before and after the run.  A changed image, a missing marker, a forbidden
marker, or a guest that does not power off cleanly fails the run.

Usage:
    test_wait4_guest.py --kernel KERNEL_ELF --image GUEST_IMG \
        --output OUT_DIR [--label NAME] [--timeout 180]
"""
import argparse
import hashlib
import json
import os
import re
import selectors
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

REQUIRED = [
    b"SYSCALL_SMOKE: start",
    b"SYSCALL_SMOKE: wait4-pgid-selectors PASS",
    b"SYSCALL_SMOKE: wait4-wnothread PASS",
    b"SYSCALL_SMOKE: wait4-stop-continue PASS",
    b"SYSCALL_SMOKE: wait4-eintr PASS",
    b"SYSCALL_SMOKE: wait4-reap-storm PASS rounds=16",
    b"SYSCALL_SMOKE: PASS",
    b"System is going down for power-off NOW",
    b"[SD] audit errors=0",
]
FORBIDDEN = re.compile(
    rb"(?im)^.*(?:\bPANIC\b|\bLOCK-STALL\b|SYSCALL_SMOKE: FAIL|\bWERROR\b).*$"
)
WORKLOAD_START = b"SYSCALL_SMOKE: start"
WORKLOAD_END_RE = re.compile(rb"SYSCALL_SMOKE: (?:PASS|FAIL[^\n]*)\n")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--label", default="run")
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--smp", type=int, default=2)
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    image = args.output / "guest.img"
    shutil.copyfile(args.image, image)
    fat_pre = sha256_file(image)
    kernel_sha = sha256_file(args.kernel)
    source_image_sha = sha256_file(args.image)

    qemu = [
        "qemu-system-riscv64", "-machine", "virt", "-m", "1G",
        "-smp", str(args.smp), "-nographic", "-monitor", "none",
        "-bios", "default", "-global", "virtio-mmio.force-legacy=false",
        "-drive", f"file={image},if=none,format=raw,id=x0",
        "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
        "-kernel", str(args.kernel),
    ]

    smp_marker = f"[SMP] {args.smp}/{args.smp} configured CPUs online".encode()
    required = [*REQUIRED[:-2], smp_marker, *REQUIRED[-2:]]

    started = time.monotonic()
    process = subprocess.Popen(
        qemu, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    sel = selectors.DefaultSelector()
    sel.register(process.stdout, selectors.EVENT_READ)
    deadline = started + args.timeout
    buffer = b""
    log = bytearray()
    sent_command = False
    sent_poweroff = False
    workload_done = None
    forced_termination = False
    exit_code = None

    def pump(wait: float) -> bytes:
        nonlocal buffer
        chunk = b""
        for key, _ in sel.select(wait):
            data = os.read(key.fileobj.fileno(), 4096)
            if not data:
                sel.unregister(key.fileobj)
                break
            chunk += data
        buffer += chunk
        log.extend(chunk)
        return chunk

    try:
        # Boot: wait for the mksh prompt, then run the workload.
        while time.monotonic() < deadline:
            if process.poll() is not None:
                break
            pump(0.25)
            if not sent_command and b"execve mksh" in buffer and b"\n# " in buffer:
                process.stdin.write(b"syscall_smoke\n")
                process.stdin.flush()
                sent_command = True
            if sent_command and workload_done is None:
                if b"SYSCALL_SMOKE: PASS" in buffer:
                    workload_done = "pass"
                elif FORBIDDEN.search(buffer) and b"SYSCALL_SMOKE:" in buffer:
                    workload_done = "fail"
            if sent_command and workload_done is not None and not sent_poweroff:
                process.stdin.write(b"poweroff\n")
                process.stdin.flush()
                sent_poweroff = True
            if sent_poweroff and b"System is going down for power-off NOW" in buffer:
                break
        # Drain until the process exits (or a short grace for shutdown).
        grace = time.monotonic() + 15.0
        while process.poll() is None and time.monotonic() < grace:
            pump(0.25)
        if process.poll() is None:
            forced_termination = True
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        if process.poll() is not None and process.poll() < 0:
            forced_termination = True
        exit_code = process.returncode
    finally:
        if process.stdin:
            try:
                process.stdin.close()
            except OSError:
                pass

    elapsed = time.monotonic() - started
    fat_post = sha256_file(image)

    required_found = [m.decode() for m in required if m in buffer]
    required_missing = [m.decode() for m in required if m not in buffer]
    forbidden_found = [m.group(0).decode(errors="replace").strip()
                       for m in FORBIDDEN.finditer(buffer)]

    workload = b""
    start = buffer.find(WORKLOAD_START)
    if start >= 0:
        end = WORKLOAD_END_RE.search(buffer, start)
        if end:
            workload = buffer[start:end.end()]
    (args.output / "workload.txt").write_bytes(workload)

    checks = {
        "exit_ok": exit_code == 0 and not forced_termination,
        "command_sent": sent_command,
        "poweroff_sent": sent_poweroff,
        "workload_pass": workload_done == "pass",
        "required_all_found": not required_missing,
        "forbidden_none": not forbidden_found,
        "image_unchanged": fat_pre == fat_post,
        "power_off_marker": b"System is going down for power-off NOW" in buffer,
        "audit_zero": b"[SD] audit errors=0" in buffer,
    }
    report = {
        "label": args.label,
        "pass": all(checks.values()),
        "checks": checks,
        "exit": exit_code,
        "forced_termination": forced_termination,
        "elapsed_seconds": round(elapsed, 3),
        "required_found": required_found,
        "required_missing": required_missing,
        "forbidden_found": forbidden_found,
        "fat_pre_sha256": fat_pre,
        "fat_post_sha256": fat_post,
        "kernel_sha256": kernel_sha,
        "image_source_sha256": source_image_sha,
        "qemu_argv": qemu,
        "log_sha256": hashlib.sha256(bytes(log)).hexdigest(),
    }
    (args.output / f"{args.label}.log").write_bytes(bytes(log))
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in (
        "label", "pass", "checks", "exit", "forced_termination",
        "elapsed_seconds", "fat_pre_sha256", "fat_post_sha256",
        "kernel_sha256")}, indent=2))
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
