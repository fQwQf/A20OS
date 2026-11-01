#!/usr/bin/env python3
"""Run interactive QEMU guest commands and fail on command or bench errors.

The positional interface remains compatible with the former out/run_expect.py:
  signal_guest_runner.py [--perf] LOG TIMEOUT KERNEL IMAGE CMD [CMD ...]

Set A20OS_SIGNAL_RUNNER_LOCK_DIR to use a different shared lock directory.
"""
from __future__ import annotations

import argparse
import fcntl
import os
from pathlib import Path
import re
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_LOCK_DIR = str(ROOT / ".git/parallel-rewrite")
SHUTDOWN_MARKER = "System is going down for power-off NOW"


def validate_bench_output(output: str, returncode: int) -> tuple[bool, str]:
    """Require a successful signal_bench process and its three real results."""
    if returncode != 0:
        return False, f"signal_bench exited with status {returncode}"
    if "SIGNAL_BENCH: FAIL" in output:
        return False, "signal_bench printed FAIL"
    if output.count("SIGNAL_BENCH: DONE") != 1:
        return False, "expected exactly one SIGNAL_BENCH: DONE marker"

    patterns = {
        "killwake": re.compile(r"SIGNAL_BENCH: killwake rounds=(\d+) .*ns_per_op=(\d+)"),
        "pingpong": re.compile(r"SIGNAL_BENCH: pingpong rounds=(\d+) .*ns_per_round=(\d+)"),
        "signalfd": re.compile(r"SIGNAL_BENCH: signalfd records=(\d+) .*ns_per_record=(\d+) records_per_s=(\d+)"),
    }
    for name, pattern in patterns.items():
        matches = pattern.findall(output)
        if len(matches) != 1:
            return False, f"expected exactly one valid {name} result, found {len(matches)}"
        if any(int(value) <= 0 for value in matches[0]):
            return False, f"{name} result contains a zero or negative count/measurement"
    return True, "ok"


def prompt_count(output: str) -> int:
    return len(re.findall(r"(?:^|[\r\n])# ", output))


def command_with_status(command: str, token: str) -> str:
    return f'{command}; __a20_signal_runner_rc=$?; printf \'\\n{token}:%d\\n\' "$__a20_signal_runner_rc"'


def run(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--perf", action="store_true", help="take the exclusive activity lock")
    parser.add_argument("--require-bench", action="store_true",
                        help="validate the first command as a complete signal_bench run")
    parser.add_argument("--qemu", default=os.environ.get("A20OS_SIGNAL_RUNNER_QEMU", "qemu-system-riscv64"),
                        help="QEMU executable (also used by the fake-guest tests)")
    parser.add_argument("log", type=Path)
    parser.add_argument("timeout", type=float)
    parser.add_argument("kernel")
    parser.add_argument("image")
    parser.add_argument("commands", nargs="+")
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("TIMEOUT must be positive")
    if args.require_bench and not any(cmd != "POWEROFF" for cmd in args.commands):
        parser.error("--require-bench needs a guest command")
    if args.require_bench and not args.commands[0].lstrip().startswith("signal_bench"):
        parser.error("--require-bench requires signal_bench as the first guest command")

    lock_dir = Path(os.environ.get("A20OS_SIGNAL_RUNNER_LOCK_DIR", DEFAULT_LOCK_DIR))
    try:
        activity = (lock_dir / "activity.lock").open("r")
        qemu_lock = (lock_dir / "qemu-1.lock").open("r")
    except OSError as exc:
        print(f"signal guest runner: cannot open shared lock files: {exc}", file=sys.stderr)
        return 73

    if args.perf:
        fcntl.flock(activity, fcntl.LOCK_EX)
    else:
        fcntl.flock(activity, fcntl.LOCK_SH)
    try:
        fcntl.flock(qemu_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        fcntl.flock(activity, fcntl.LOCK_UN)
        return 75

    proc = None
    args.log.parent.mkdir(parents=True, exist_ok=True)
    try:
        qemu = [
            args.qemu, "-machine", "virt", "-m", "1G", "-nographic", "-smp", "2",
            "-bios", "default", "-global", "virtio-mmio.force-legacy=false",
            "-drive", f"file={args.image},if=none,format=raw,id=x0",
            "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
            "-netdev", "user,id=net",
            "-device", "virtio-net-device,netdev=net,bus=virtio-mmio-bus.4",
            "-kernel", args.kernel,
        ]
        with args.log.open("wb", buffering=0) as log:
            proc = subprocess.Popen(qemu, stdin=subprocess.PIPE, stdout=log,
                                    stderr=subprocess.STDOUT)
            deadline = time.monotonic() + args.timeout
            command_index = 0
            state = "boot"
            run_start = 0
            run_token = ""
            prompt_before = 0
            bench_pending = False
            while time.monotonic() < deadline:
                output = args.log.read_text(errors="replace")
                prompts = prompt_count(output)
                if state == "boot" and prompts:
                    state = "prompt"
                if state == "command" and run_token:
                    match = re.search(re.escape(run_token) + r":(-?\d+)", output[run_start:])
                    if match and prompts > prompt_before:
                        command_output = output[run_start:]
                        returncode = int(match.group(1))
                        if bench_pending:
                            good, reason = validate_bench_output(command_output, returncode)
                            if not good:
                                print(f"signal guest runner: {reason}; see {args.log}", file=sys.stderr)
                                return 1
                            bench_pending = False
                        elif returncode != 0:
                            print(f"signal guest runner: guest command exited {returncode}; see {args.log}", file=sys.stderr)
                            return 1
                        state = "prompt"
                        run_token = ""
                if state == "prompt" and command_index < len(args.commands):
                    command = args.commands[command_index]
                    command_index += 1
                    if command in ("POWEROFF", "poweroff"):
                        proc.stdin.write(b"poweroff\r\n")
                        proc.stdin.flush()
                        state = "shutdown"
                    else:
                        run_start = len(output)
                        run_token = f"__A20_SIGNAL_RUNNER_EXIT_{command_index}__"
                        wrapped = command_with_status(command, run_token)
                        proc.stdin.write(wrapped.encode() + b"\r\n")
                        proc.stdin.flush()
                        prompt_before = prompts
                        state = "command"
                        bench_pending = args.require_bench and command_index == 1
                elif state == "prompt" and command_index >= len(args.commands):
                    state = "idle"
                if proc.poll() is not None:
                    output = args.log.read_text(errors="replace")
                    if (state in ("shutdown", "idle") and SHUTDOWN_MARKER in output
                            and command_index == len(args.commands)
                            and proc.returncode == 0):
                        return 0
                    print(f"signal guest runner: QEMU exited {proc.returncode} before a successful natural shutdown; see {args.log}", file=sys.stderr)
                    return 1
                time.sleep(0.05)
            print(f"signal guest runner: timed out in state {state}; see {args.log}", file=sys.stderr)
            return 124
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        fcntl.flock(qemu_lock, fcntl.LOCK_UN)
        fcntl.flock(activity, fcntl.LOCK_UN)


if __name__ == "__main__":
    raise SystemExit(run())
