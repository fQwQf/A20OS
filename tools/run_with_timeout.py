#!/usr/bin/env python3

import os
import selectors
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
    # --foreground keeps the child in this process group so Ctrl-C reaches it
    # directly; the default detaches into its own session for timeout killpg.
    foreground = False
    if args and args[0] == "--foreground":
        foreground = True
        args.pop(0)

    expected = []
    send_lines = []
    # (marker, line) pairs, each sent when its own marker is seen.  Needed
    # when one input depends on the program having consumed an earlier one --
    # e.g. a guest that asks for its command line on the serial port and only
    # afterwards has a shell to type into.  A single --expect plus a batch of
    # --send-line sends them all at once, which works for a shell prompt (the
    # guest buffers them) and not for anything with a round trip in it.
    steps = []
    while args and args[0] in ("--expect", "--send-line", "--expect-line"):
        option = args.pop(0)
        if option == "--expect-line":
            if len(args) < 2:
                print("missing value for --expect-line", file=sys.stderr)
                return 2
            steps.append((args.pop(0).encode(), args.pop(0).encode()))
            continue
        if not args:
            print(f"missing value for {option}", file=sys.stderr)
            return 2
        value = args.pop(0)
        if option == "--expect":
            expected.append(value.encode())
        else:
            send_lines.append(value.encode())

    if steps:
        # The markers gate the streaming path, and `tail` is trimmed by marker
        # length, so the step markers have to be in `expected` and in order.
        expected = [marker for marker, _ in steps]
        send_lines = []

    if len(args) < 2:
        print(
            "usage: run_with_timeout.py [--foreground] "
            "[--expect MARKER] [--send-line LINE] "
            "[--expect-line MARKER LINE]... DURATION COMMAND [ARG]...",
            file=sys.stderr,
        )
        return 2

    try:
        duration = parse_duration(args.pop(0))
    except ValueError:
        print("invalid timeout duration", file=sys.stderr)
        return 2

    if not expected and not send_lines:
        process = subprocess.Popen(args, start_new_session=not foreground)
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

    process = subprocess.Popen(
        args,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        start_new_session=not foreground,
        bufsize=0,
    )
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + duration
    marker_index = 0
    tail = bytearray()
    sent = False
    sent_steps = []

    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                return 124

            for key, _ in selector.select(timeout=min(1.0, remaining)):
                chunk = os.read(key.fd, 65536)
                if not chunk:
                    selector.unregister(process.stdout)
                    continue
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
                tail.extend(chunk)
                while marker_index < len(expected):
                    offset = tail.find(expected[marker_index])
                    if offset < 0:
                        break
                    del tail[:offset + len(expected[marker_index])]
                    marker_index += 1
                max_marker = max((len(marker) for marker in expected), default=1)
                if len(tail) > max(16384, max_marker * 2):
                    del tail[:-max(16384, max_marker * 2)]

            if marker_index == len(expected) and not sent:
                for line in send_lines:
                    process.stdin.write(line + b"\n")
                process.stdin.flush()
                sent = True

            if steps:
                for index in range(marker_index):
                    if index < len(sent_steps) or index >= len(steps):
                        continue
                    process.stdin.write(steps[index][1] + b"\n")
                    sent_steps.append(index)
                if sent_steps:
                    process.stdin.flush()

            status = process.poll()
            if status is not None:
                while True:
                    chunk = process.stdout.read(65536)
                    if not chunk:
                        break
                    sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
                return status
    except KeyboardInterrupt:
        os.killpg(process.pid, signal.SIGINT)
        time.sleep(0.1)
        return process.wait()
    finally:
        selector.close()


if __name__ == "__main__":
    sys.exit(main())
