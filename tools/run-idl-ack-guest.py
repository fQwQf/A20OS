#!/usr/bin/env python3
"""Run the explicit IDL malformed-ACK client against a prepared FAT image."""
import argparse
import hashlib
import json
import os
import re
import selectors
import signal
import subprocess
import sys
import time
from pathlib import Path


REQUIRED = [
    b"RUST_NATIVE_SVC: PASS restarts=1",
    b"IDL_ACK_ACCEPT: case=legacy_echo_accepts_v1 PASS accepted-legacy-in-range response_sent=1 cleanup=PASS",
    b"IDL_ACK_REJECT: case=wrong_nonce PASS rejected-before-fallback response_sent=1 cleanup=PASS",
    b"IDL_ACK_REJECT: case=selection_outside_overlap PASS rejected-before-fallback response_sent=1 cleanup=PASS",
    b"IDL_ACK_REJECT: case=short_frame PASS rejected-before-fallback response_sent=1 cleanup=PASS",
    b"IDL_ACK_REJECT: case=trailing_frame PASS rejected-before-fallback response_sent=1 cleanup=PASS",
    b"IDL_ACK_REJECT: case=status_selection_conflict PASS rejected-before-fallback response_sent=1 cleanup=PASS",
    b"IDL_ACK_REJECT: case=legacy_echo_excludes_v1 PASS rejected-before-fallback response_sent=1 cleanup=PASS",
    b"IDL_ACK_REJECT: case=unknown_status PASS rejected-before-fallback response_sent=1 cleanup=PASS",
    b"IDL_ACK_REJECT: case=unknown_reply_type PASS rejected-before-fallback response_sent=1 cleanup=PASS",
    b"IDL_ACK_REJECT: case=unsupported_envelope_version PASS rejected-before-fallback response_sent=1 cleanup=PASS",
    b"IDL_ACK_ADVERSARIAL: PASS cases=10 cleanup=10",
    b"System is going down for power-off NOW",
    b"[SD] audit errors=0",
]
FORBIDDEN = re.compile(
    rb"(?im)^.*(?:\bPANIC\b|\bLOCK-STALL\b|\bFAULT\b|\bFAIL(?:ED)?\b).*$"
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def acceptance_pass(exit_code: int, sent_command: bool, sent_poweroff: bool,
                    found_count: int, required_count: int, forbidden_count: int,
                    image_unchanged: bool, online_required: bool,
                    online_seen: bool) -> bool:
    return (exit_code == 0 and sent_command and sent_poweroff and
            found_count == required_count and forbidden_count == 0 and
            image_unchanged and (not online_required or online_seen))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--arch", choices=("riscv64", "riscv32", "x86_64", "aarch64"), default="riscv64")
    parser.add_argument("--smp", type=int)
    parser.add_argument("--profile-label", default="unlabeled")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    expected_smp = {"riscv64": 2, "riscv32": 1, "x86_64": 2, "aarch64": 1}[args.arch]
    if args.smp is None:
        args.smp = expected_smp
    if args.smp != expected_smp:
        parser.error(f"{args.arch} ACK profile requires --smp {expected_smp}")
    peer_tag = {"riscv64": "rv", "riscv32": "rv32", "x86_64": "x86_64",
                "aarch64": "aarch64"}[args.arch]
    guest_peer = f"/bin/rust-idl-peer-ack-{peer_tag}"
    online_marker = f"[SMP] {expected_smp}/{expected_smp} configured CPUs online".encode()
    online_required = args.arch in ("riscv64", "x86_64")
    required_markers = list(REQUIRED)
    if online_required:
        required_markers.insert(-2, online_marker)
    if args.arch == "x86_64":
        argv = [
            "qemu-system-x86_64", "-machine", "q35", "-m", "1G",
            "-serial", "stdio", "-monitor", "none", "-smp", str(args.smp),
            "-no-reboot", "-drive", f"file={args.image},if=none,format=raw,id=x0",
            "-device", "virtio-blk-pci,drive=x0", "-kernel", str(args.kernel),
        ]
    elif args.arch == "aarch64":
        # Raw AArch64 boot follows the Linux protocol and passes the DTB in
        # x0, so it takes the .bin and has no -bios.
        argv = [
            "qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a57",
            "-m", "1G", "-serial", "stdio", "-monitor", "none",
            "-smp", str(args.smp),
            "-drive", f"file={args.image},if=none,format=raw,id=x0",
            "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
            "-kernel", str(args.kernel),
        ]
    else:
        argv = [
            f"qemu-system-{args.arch}", "-machine", "virt", "-m", "1G",
            "-serial", "stdio", "-monitor", "none", "-smp", str(args.smp),
            "-bios", "default", "-global", "virtio-mmio.force-legacy=false",
            "-drive", f"file={args.image},if=none,format=raw,id=x0",
            "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
            "-kernel", str(args.kernel),
        ]
    pre = sha256(args.image)
    data = bytearray()
    sent_command = False
    sent_poweroff = False
    start = time.monotonic()
    deadline = start + args.timeout
    prompt = re.compile(rb"(?:^|\r?\n)# $")
    with (args.output / "guest.log").open("wb", buffering=0) as log:
        process = subprocess.Popen(
            argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, bufsize=0, start_new_session=True,
        )
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ)
        while time.monotonic() < deadline:
            if process.poll() is not None:
                break
            for key, _ in selector.select(0.2):
                chunk = os.read(key.fileobj.fileno(), 4096)
                if not chunk:
                    continue
                log.write(chunk)
                data.extend(chunk)
                if not sent_command and prompt.search(data[-16384:]):
                    process.stdin.write((guest_peer + "\n").encode())
                    process.stdin.flush()
                    sent_command = True
                if (sent_command and not sent_poweroff and
                        all(marker in data for marker in required_markers[:-2])):
                    process.stdin.write(b"poweroff\n")
                    process.stdin.flush()
                    sent_poweroff = True
        timed_out = process.poll() is None
        forced_termination = False
        if timed_out:
            forced_termination = True
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            exit_code = 124
        else:
            exit_code = process.returncode
    post = sha256(args.image)
    found = [marker.decode() for marker in required_markers if marker in data]
    forbidden = [match.decode(errors="replace") for match in FORBIDDEN.findall(data)]
    image_unchanged = pre == post
    online_seen = online_marker in data
    passed = (args.smp == expected_smp and acceptance_pass(
        exit_code, sent_command, sent_poweroff, len(found), len(required_markers),
        len(forbidden), image_unchanged, online_required, online_seen))
    report = {
        "exit": exit_code,
        "actual_timeout": timed_out,
        "forced_termination": forced_termination,
        "arch": args.arch,
        "expected_smp": expected_smp,
        "configured_smp": args.smp,
        "smp_evidence": ("kernel_online_marker" if online_required else
                         "runner_qemu_and_NR_CPUS_profile_only"),
        "online_marker_required": online_required,
        "online_marker_seen": online_seen,
        "fat_unchanged": image_unchanged,
        "profile_label": args.profile_label,
        "shell_prompt_seen": sent_command,
        "poweroff_sent_after_adversarial_markers": sent_poweroff,
        "required_found": found,
        "forbidden_found": forbidden,
        "kernel_sha256": sha256(args.kernel),
        "fat_pre_sha256": pre,
        "fat_post_sha256": post,
        "elapsed_seconds": round(time.monotonic() - start, 3),
        "pass": passed,
    }
    (args.output / "guest.argv.json").write_text(
        json.dumps({"argv": argv, "arch": args.arch, "guest_peer": guest_peer,
                    "timeout_seconds": args.timeout,
                    "profile_label": args.profile_label,
                    "required_markers": [m.decode() for m in required_markers],
                    "forbidden_regex": FORBIDDEN.pattern.decode()}, indent=2) + "\n"
    )
    (args.output / "guest.exit").write_text(f"{exit_code}\n")
    (args.output / "guest.report.json").write_text(
        json.dumps(report, indent=2) + "\n"
    )
    print(json.dumps(report))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
