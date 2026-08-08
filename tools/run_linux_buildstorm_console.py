#!/usr/bin/env python3
"""Drive an interactive Linux BuildStorm baseline guest over QEMU stdio."""

from __future__ import annotations

import argparse
import os
import selectors
import signal
import subprocess
import sys
import time
from pathlib import Path


BOOT_MARKER = b"Run /bin/sh as init process"
DONE_MARKER = b"A20_LINUX_BASELINE_DONE status=0"


def terminate_process_group(process: subprocess.Popen[bytes], sig: signal.Signals) -> None:
    if process.poll() is None:
        os.killpg(process.pid, sig)


def append_marker(log_file, message: str) -> None:
    data = (message + "\n").encode()
    log_file.write(data)
    log_file.flush()
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=int, required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--guest-command", required=True)
    parser.add_argument("qemu_command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.qemu_command and args.qemu_command[0] == "--":
        args.qemu_command.pop(0)
    if args.timeout <= 0 or not args.qemu_command:
        parser.error("a positive timeout and QEMU command are required")
    return args


def main() -> int:
    args = parse_args()
    args.log.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + args.timeout
    command_sent = False
    boot_seen = False
    shutdown_deadline: float | None = None
    tail = bytearray()

    with args.log.open("wb") as log_file:
        process = subprocess.Popen(
            args.qemu_command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        assert process.stdin is not None
        assert process.stdout is not None
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ)

        try:
            while True:
                now = time.monotonic()
                effective_deadline = deadline
                if shutdown_deadline is not None:
                    effective_deadline = min(effective_deadline, shutdown_deadline)
                remaining = effective_deadline - now
                if remaining <= 0:
                    if shutdown_deadline is not None and shutdown_deadline <= deadline:
                        append_marker(
                            log_file,
                            "A20_LINUX_BASELINE_DRIVER shutdown_timeout=true",
                        )
                        timeout_status = 126
                    else:
                        append_marker(log_file, "A20_LINUX_BASELINE_DRIVER timeout=true")
                        timeout_status = 124
                    terminate_process_group(process, signal.SIGTERM)
                    try:
                        process.wait(timeout=1)
                    except subprocess.TimeoutExpired:
                        terminate_process_group(process, signal.SIGKILL)
                        process.wait()
                    return timeout_status

                events = selector.select(timeout=min(1.0, remaining))
                for key, _ in events:
                    chunk = os.read(key.fd, 65536)
                    if not chunk:
                        selector.unregister(process.stdout)
                        continue
                    log_file.write(chunk)
                    log_file.flush()
                    sys.stdout.buffer.write(chunk)
                    sys.stdout.buffer.flush()
                    tail.extend(chunk)
                    if len(tail) > 16384:
                        del tail[:-16384]
                    if BOOT_MARKER in tail:
                        boot_seen = True
                    if DONE_MARKER in tail and shutdown_deadline is None:
                        shutdown_deadline = time.monotonic() + 30
                    if boot_seen and not command_sent and b"# " in tail:
                        process.stdin.write(args.guest_command.encode() + b"\n")
                        process.stdin.flush()
                        command_sent = True
                        append_marker(
                            log_file,
                            "A20_LINUX_BASELINE_DRIVER command_sent=true",
                        )

                status = process.poll()
                if status is not None:
                    # Drain any bytes already buffered by the pipe after QEMU exits.
                    while True:
                        chunk = process.stdout.read(65536)
                        if not chunk:
                            break
                        log_file.write(chunk)
                        sys.stdout.buffer.write(chunk)
                    log_file.flush()
                    sys.stdout.buffer.flush()
                    if not command_sent:
                        append_marker(
                            log_file,
                            "A20_LINUX_BASELINE_DRIVER command_sent=false",
                        )
                        return 125
                    return status
        except KeyboardInterrupt:
            terminate_process_group(process, signal.SIGINT)
            try:
                return process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                terminate_process_group(process, signal.SIGKILL)
                return process.wait()
        finally:
            selector.close()
            if process.poll() is None:
                terminate_process_group(process, signal.SIGTERM)
                try:
                    process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    terminate_process_group(process, signal.SIGKILL)
                    process.wait()


if __name__ == "__main__":
    sys.exit(main())
