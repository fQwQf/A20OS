#!/usr/bin/env python3

import os
import signal
import subprocess
import sys
import time


def parse_duration(value):
    multipliers = {"s": 1, "m": 60, "h": 3600}
    suffix = value[-1].lower()
    if suffix in multipliers:
        return float(value[:-1]) * multipliers[suffix]
    return float(value)


def main():
    args = sys.argv[1:]
    if args and args[0] == "--foreground":
        args.pop(0)
    if len(args) < 2:
        print("usage: run_with_timeout.py [--foreground] DURATION COMMAND [ARG]...", file=sys.stderr)
        return 2

    try:
        duration = parse_duration(args.pop(0))
    except ValueError:
        print("invalid timeout duration", file=sys.stderr)
        return 2

    process = subprocess.Popen(args, start_new_session=True)
    try:
        return process.wait(timeout=duration)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
        return 124
    except KeyboardInterrupt:
        os.killpg(process.pid, signal.SIGINT)
        time.sleep(0.1)
        return process.wait()


if __name__ == "__main__":
    sys.exit(main())
