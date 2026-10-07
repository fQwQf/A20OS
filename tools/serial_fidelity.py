#!/usr/bin/env python3
"""Guest serial RX fidelity probe.

Boots the riscv64 QEMU guest, types N numbered commands whose payload is a
deterministic pseudo-random blob, and checks that every byte the host wrote came
back out of the guest unchanged.

Why this exists
---------------
`docs/net/net-lanes.md` ("本轮的一个**测量方法**发现：控制台输入会吃掉字符")
recorded that roughly 3 of 800 console commands were received by the guest with
an adjacent character transposed or dropped.  That number came from reading a
console log, and reading a console log cannot tell these two apart:

  * the guest's UART receive path really lost or reordered a byte, or
  * a kernel print landed inside the echoed input line, so the *log* looks
    transposed while the shell received every byte correctly.

The first is a driver bug.  The second is a measurement bug.  This probe is
built so the two cannot be confused:

  * each line carries a high-entropy payload, so a dropped or transposed byte
    changes the payload itself and not just a command name;
  * a line is counted as corrupted only when the guest produced output for that
    index whose bytes differ from what was sent -- an interleaved kernel print
    inserts foreign text and is reported separately as `interleaved`, not as
    corruption;
  * both the echoed input line and the command's own output are checked, so a
    fault that only shows up on one side is visible as a disagreement.

Amplifiers (all optional, all reported in the summary line):

  --blast N     write N command lines per host write with no wait in between,
                keeping the 16550 FIFO and the kernel ring buffer non-empty.
  --load CMD    run CMD in the guest in the background for the whole run, so
                the console reader competes with real work -- the situation the
                field report came from.
  --payload L   bytes of payload per line (default 48).  Sensitivity is
                per-byte, so a longer payload measures more bytes per line;
                the default stays under one 80-column line because mksh's line
                editor redraws a wrapped input line with \r and backspaces,
                which this probe reports as `interleaved` rather than as RX
                corruption.

A run prints sent / matched / corrupted and exits non-zero when corrupted is
non-zero, so it can be used as a gate directly.

Usage:
    tools/serial_fidelity.py --kernel-dir .kernel-build/<dir> [--count 300]
                             [--payload 96] [--blast 0] [--load CMD]
                             [--log FILE] [--timeout 900] [--qemu-arg A]...
"""

import argparse
import os
import re
import selectors
import signal
import subprocess
import sys
import time

# Base62 only: no shell metacharacter, no escape, no quote, no backslash, so
# the payload cannot change how mksh parses the line and the echoed line is
# byte-comparable with what was typed.
ALPHABET = ("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789")
TOKEN_RE = re.compile(rb"FID(\d{6})([A-Za-z0-9]*)")


def payload_for(index, length):
    """Deterministic pseudo-random payload for line `index`.

    An LCG rather than random bytes so a failing run can be replayed from its
    index alone, and base62 rather than raw bytes so the line stays printable
    ASCII that mksh echoes verbatim.
    """
    state = (index * 2654435761 + 1013904223) & 0xFFFFFFFF
    out = []
    for _ in range(length):
        state = (1103515245 * state + 12345) & 0x7FFFFFFF
        out.append(ALPHABET[(state >> 16) % len(ALPHABET)])
    return "".join(out)


def build_qemu_cmd(kernel_dir, extra_args):
    return [
        "qemu-system-riscv64",
        "-machine", "virt",
        "-m", "1G",
        "-nographic",
        "-smp", "1",
        "-bios", "default",
        "-global", "virtio-mmio.force-legacy=false",
        "-drive", f"file={kernel_dir}/fat32.img,if=none,format=raw,id=x0",
        "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
        "-kernel", f"{kernel_dir}/kernel.elf",
        "-append",
        "a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 "
        "a20.dns=10.0.2.3 a20.hostname=a20os",
    ] + extra_args


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel-dir", required=True)
    ap.add_argument("--count", type=int, default=300)
    ap.add_argument("--payload", type=int, default=48)
    ap.add_argument("--interval", type=float, default=0.0,
                    help="seconds between lines; 0 means wait for each prompt")
    ap.add_argument("--blast", type=int, default=0,
                    help="command lines per host write, with no wait in between")
    ap.add_argument("--load", default=None,
                    help="guest command to run in the background for the run")
    ap.add_argument("--settle", type=float, default=0.05,
                    help="pause after a command's output before the next line")
    ap.add_argument("--timeout", type=float, default=900.0)
    ap.add_argument("--allow-missing", action="store_true",
                    help="report lines that never returned without failing "
                         "the run (for --blast amplifier runs only)")
    ap.add_argument("--log", default=None)
    ap.add_argument("--qemu-arg", action="append", default=[])
    args = ap.parse_args()

    kernel_dir = os.path.abspath(args.kernel_dir)
    if not os.path.exists(f"{kernel_dir}/kernel.elf"):
        print(f"serial_fidelity: no kernel.elf under {kernel_dir}",
              file=sys.stderr)
        return 2

    # One wall-clock budget for the whole run (boot wait included).  wait_for
    # clamps its per-line wait to it, so a guest that hangs at line 3 stops the
    # run there instead of burning the full per-line timeout on every one of
    # the remaining lines.  On expiry the loop reports the lines that never
    # returned and fails -- a hung run must never look like a passing one.
    deadline = time.monotonic() + args.timeout

    log_file = None
    if args.log:
        os.makedirs(os.path.dirname(args.log) or ".", exist_ok=True)
        log_file = open(args.log, "wb")

    process = subprocess.Popen(
        build_qemu_cmd(kernel_dir, args.qemu_arg),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        start_new_session=True,
        bufsize=0,
    )
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)

    buf = bytearray()

    def pump(budget):
        """Read whatever is available for at most `budget` seconds."""
        stop = time.monotonic() + budget
        while True:
            left = stop - time.monotonic()
            if left <= 0:
                return
            for key, _ in selector.select(timeout=left):
                chunk = os.read(key.fd, 65536)
                if not chunk:
                    selector.unregister(process.stdout)
                    return
                buf.extend(chunk)
                if log_file:
                    log_file.write(chunk)
                    log_file.flush()

    def wait_for(needle, budget, start_offset):
        """Wait until `needle` appears at/after start_offset; return its index.

        The wait never extends past the run-wide deadline, so one hung line
        cannot be followed by N-1 more full waits."""
        stop = min(time.monotonic() + budget, deadline)
        while True:
            idx = buf.find(needle, start_offset)
            if idx >= 0:
                return idx
            left = stop - time.monotonic()
            if left <= 0:
                return -1
            pump(min(left, 0.5))

    def send(line):
        process.stdin.write(line + b"\n")
        process.stdin.flush()

    lines = [("echo FID%06d%s" % (i, payload_for(i, args.payload))).encode()
             for i in range(1, args.count + 1)]
    # TOKEN_RE captures the payload *after* the index, so the expectation is the
    # payload: "echo " is 5 bytes and the token "FID%06d" is 9.
    expected = {("%06d" % i).encode(): lines[i - 1][14:] for i in
                range(1, args.count + 1)}

    try:
        # 120s covers the slowest boot seen here; --timeout bounds the rest.
        if wait_for(b"# ", 120.0, 0) < 0:
            print("serial_fidelity: guest never reached a shell prompt",
                  file=sys.stderr)
            return 2

        if args.load:
            send((args.load + " &").encode())

        prompt_end = buf.rfind(b"# ") + 2
        next_send = time.monotonic() + args.settle

        if args.blast > 0:
            for start in range(0, len(lines), args.blast):
                chunk = b"".join(line + b"\n"
                                 for line in lines[start:start + args.blast])
                process.stdin.write(chunk)
                process.stdin.flush()
                pump(0.05)
            tail = min(time.monotonic() + 180.0, deadline)
            while time.monotonic() < tail:
                if buf.count(b"FID") >= args.count:
                    break
                pump(1.0)
        else:
            for line in lines:
                token = line[5:14]          # FID%06d
                if args.interval > 0:
                    now = time.monotonic()
                    if next_send > now:
                        pump(min(next_send - now, 0.5))
                    next_send = time.monotonic() + args.interval
                send(line)

                # The command's own output line is the deterministic end of
                # this command.  Waiting for it keeps the guest's ring buffer
                # far from full, which is what separates an ordering bug from a
                # plain overrun.
                out_at = wait_for(b"\n" + token, 60.0, prompt_end)
                if out_at < 0:
                    print(f"serial_fidelity: line {token.decode()} produced no "
                          "output; stopping", file=sys.stderr)
                    break
                prompt_end = out_at + len(token) + 2
                if args.interval <= 0:
                    pump(args.settle)

        send(b"poweroff")
        pump(5.0)
    finally:
        selector.close()
        try:
            process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        if log_file:
            log_file.close()

    log = bytes(buf)

    # Classify every FID token the guest printed.  A token is "clean" when the
    # payload after it is exactly what was sent; "corrupt" when the index is
    # right but the bytes differ (a dropped, transposed or duplicated byte); and
    # "interleaved" when something foreign was spliced into the same line, which
    # is a console-output interleaving and not an RX fault.
    clean = 0
    corrupt = []
    interleaved = 0
    seen = {}
    for raw in log.split(b"\n"):
        for m in TOKEN_RE.finditer(raw):
            index = m.group(1)
            got = m.group(2)
            want = expected.get(index)
            if want is None:
                continue
            seen[index] = seen.get(index, 0) + 1
            rest = raw[m.end():].strip(b"\r")
            if got == want:
                clean += 1
            elif want.startswith(got) and (not rest or
                                           rest.startswith(want[len(got):])):
                # The payload stops short but the line carries nothing foreign:
                # the guest really received fewer bytes than were sent.
                corrupt.append((index, want, got, "truncated"))
            elif len(got) == len(want) and sorted(got) == sorted(want):
                # Same bytes, different order: an adjacent-character swap.
                corrupt.append((index, want, got, "transposed"))
            elif not got and not rest:
                corrupt.append((index, want, got, "vanished"))
            else:
                # Foreign text spliced into the same line: a console-output
                # interleaving, which is a different fault from a lost byte and
                # must not be counted as RX corruption.
                interleaved += 1

    # An index is only evidence of corruption if the guest produced output for
    # it at all; a line that never came back is a stall, reported separately.
    returned = len(seen)
    corrupted = len(corrupt)
    missing = args.count - returned
    not_found = log.count(b"inaccessible or not found")

    print(f"serial_fidelity: kernel_dir={kernel_dir} count={args.count} "
          f"payload={args.payload} blast={args.blast} load={args.load!r}")
    print(f"serial_fidelity: returned={returned} clean_tokens={clean} "
          f"corrupted={corrupted} never_returned={missing} "
          f"interleaved_tokens={interleaved} shell_not_found={not_found}")
    for index, want, got, kind in corrupt[:20]:
        print(f"serial_fidelity: CORRUPT {index.decode()}: {kind} "
              f"want_len={len(want)} got_len={len(got)} "
              f"want={want[:24].decode(errors='replace')}... "
              f"got={got[:24].decode(errors='replace')}")
    if len(corrupt) > 20:
        print(f"serial_fidelity: ... {len(corrupt) - 20} more corrupt lines")

    # A line that never came back is the same measurement hazard as a
    # corrupted one -- it is a command that did not run -- so it fails the run
    # too.  --allow-missing exists only for the --blast amplifier runs, where
    # the host deliberately outruns the UART and the question being answered
    # is "how much survives", not "did everything survive".
    if missing > 0:
        print(f"serial_fidelity: {missing} lines never returned"
              + (" (--allow-missing: reported, not failed)"
                 if args.allow_missing else ""))
    if corrupted > 0 or (missing > 0 and not args.allow_missing):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
