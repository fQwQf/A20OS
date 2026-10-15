#!/usr/bin/env python3
"""Run the Native C IDL client against an echod ELF installed in a FAT image."""
import argparse
import hashlib
import json
import os
import re
import selectors
import signal
import subprocess
import time
from pathlib import Path


REQUIRED = [
    b"[SMP] 2/2 configured CPUs online",
    b"NATIVE_IDL_SERVICE: PASS client=c negotiation=1 echo=1 ping=1 ping-crash=ignored ping-close=alive exit=42",
    b"System is going down for power-off NOW",
    b"[SD] audit errors=0",
]
FORBIDDEN = re.compile(rb"(?im)^.*(?:\bPANIC\b|\bLOCK-STALL\b|\bFAULT\b|\bFAIL(?:ED)?\b).*$")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def readback(image: Path, guest_name: str, out: Path) -> str:
    subprocess.run(["mcopy", "-o", "-i", str(image), f"::/{guest_name.lstrip('/')}", str(out)],
                   check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return sha256(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--command", required=True)
    parser.add_argument("--client-elf", required=True, type=Path)
    parser.add_argument("--client-fat-name", required=True)
    parser.add_argument("--server-elf", required=True, type=Path)
    parser.add_argument("--server-fat-name", required=True)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    client_readback = readback(args.image, args.client_fat_name,
                               args.output / "client-fat-readback.elf")
    server_readback = readback(args.image, args.server_fat_name,
                               args.output / "server-fat-readback.elf")
    client_source = sha256(args.client_elf)
    server_source = sha256(args.server_elf)
    if client_readback != client_source or server_readback != server_source:
        raise SystemExit("FAT ELF readback differs from the declared client/server input")

    argv = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "1G",
        "-serial", "stdio", "-monitor", "none", "-smp", "2", "-no-reboot",
        "-drive", f"file={args.image},if=none,format=raw,id=x0",
        "-device", "virtio-blk-pci,drive=x0", "-kernel", str(args.kernel),
    ]
    image_pre = sha256(args.image)
    data = bytearray()
    sent_command = False
    sent_poweroff = False
    timed_out = False
    forced_termination = False
    start = time.monotonic()
    deadline = start + args.timeout
    prompt_re = re.compile(rb"(?:^|\r?\n)# $")
    with (args.output / "guest.log").open("wb", buffering=0) as log:
        proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, bufsize=0, start_new_session=True)
        selector = selectors.DefaultSelector()
        selector.register(proc.stdout, selectors.EVENT_READ)
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                break
            for key, _ in selector.select(0.2):
                chunk = os.read(key.fileobj.fileno(), 4096)
                if not chunk:
                    continue
                log.write(chunk)
                data.extend(chunk)
                if not sent_command and prompt_re.search(data[-16384:]):
                    proc.stdin.write((args.command + "\n").encode())
                    proc.stdin.flush()
                    sent_command = True
                if sent_command and not sent_poweroff and all(m in data for m in REQUIRED[:2]):
                    proc.stdin.write(b"poweroff\n")
                    proc.stdin.flush()
                    sent_poweroff = True
        timed_out = proc.poll() is None
        if timed_out:
            forced_termination = True
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
            exit_code = 124
        else:
            exit_code = proc.returncode

    image_post = sha256(args.image)
    found = [m.decode() for m in REQUIRED if m in data]
    forbidden = [x.decode(errors="replace") for x in FORBIDDEN.findall(data)]
    passed = (exit_code == 0 and sent_command and sent_poweroff and
              len(found) == len(REQUIRED) and not forbidden and image_pre == image_post)
    report = {
        "pass": passed,
        "exit": exit_code,
        "actual_timeout": timed_out,
        "forced_termination": forced_termination,
        "configured_smp": 2,
        "online_marker_seen": REQUIRED[0] in data,
        "command_sent": sent_command,
        "poweroff_sent": sent_poweroff,
        "required_found": found,
        "forbidden_found": forbidden,
        "fat_unchanged": image_pre == image_post,
        "client": {"guest_path": args.client_fat_name, "source_sha256": client_source,
                   "fat_readback_sha256": client_readback},
        "server": {"guest_path": args.server_fat_name, "source_sha256": server_source,
                   "fat_readback_sha256": server_readback},
        "kernel_sha256": sha256(args.kernel),
        "fat_pre_sha256": image_pre,
        "fat_post_sha256": image_post,
        "elapsed_seconds": round(time.monotonic() - start, 3),
    }
    (args.output / "guest.argv.json").write_text(json.dumps({
        "argv": argv, "command": args.command, "timeout_seconds": args.timeout,
        "required_markers": [m.decode() for m in REQUIRED],
        "forbidden_regex": FORBIDDEN.pattern.decode(),
    }, indent=2) + "\n")
    (args.output / "guest.exit").write_text(f"{exit_code}\n")
    (args.output / "guest.report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
