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
    b"IDL_ACK_REJECT: case=wrong_nonce PASS",
    b"IDL_ACK_REJECT: case=selection_outside_overlap PASS",
    b"IDL_ACK_REJECT: case=short_frame PASS",
    b"IDL_ACK_REJECT: case=trailing_frame PASS",
    b"IDL_ACK_REJECT: case=status_selection_conflict PASS",
    b"IDL_ACK_REJECT: case=legacy_echo_excludes_v1 PASS",
    b"IDL_ACK_ADVERSARIAL: PASS cases=6 cleanup=6",
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--smp", type=int, default=2)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    argv = [
        "qemu-system-riscv64", "-machine", "virt", "-m", "1G",
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
                    process.stdin.write(b"/bin/rust-idl-peer-ack-rv\n")
                    process.stdin.flush()
                    sent_command = True
                if (sent_command and not sent_poweroff and
                        all(marker in data for marker in REQUIRED[:-2])):
                    process.stdin.write(b"poweroff\n")
                    process.stdin.flush()
                    sent_poweroff = True
        if process.poll() is None:
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
    found = [marker.decode() for marker in REQUIRED if marker in data]
    forbidden = [match.decode(errors="replace") for match in FORBIDDEN.findall(data)]
    passed = (exit_code == 0 and sent_command and sent_poweroff and
              len(found) == len(REQUIRED) and not forbidden)
    report = {
        "exit": exit_code,
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
        json.dumps({"argv": argv, "timeout_seconds": args.timeout,
                    "required_markers": [m.decode() for m in REQUIRED],
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
