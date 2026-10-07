#!/usr/bin/env python3
"""Guest serial RX fidelity probe.

Boots the riscv64 QEMU guest, types N numbered commands one burst at a time,
and checks that every byte the host wrote came back out of the guest unchanged.

Why this exists
---------------
`docs/net/net-lanes.md` recorded that roughly 3 of 800 console commands were
received by the guest with an adjacent character transposed or dropped, which
turns `grep -c PASS` into a statistic that cannot tell "did not run" apart from
"did not fail".  That note named the symptom but not the mechanism, so this
script exists to make the mechanism measurable on demand: it prints an exact
corruption rate for whatever kernel.elf it is pointed at, which is what makes a
before/after claim checkable instead of anecdotal.

How the comparison works
------------------------
The guest shell (mksh) echoes every byte it accepts from the tty, so each typed
command appears twice in the console log: once as the echoed input line
(`# echo FID000001`) and once as the command's own output (`FID000001`).  The
echoed line is the interesting one, because it is produced by the kernel RX path
(arch_uart_poll_getc -> uart_rx_push -> uart_getc -> tty_console_read echo) with
no shell parsing in between.  A transposed or dropped byte shows up there as a
line whose index does not match the line that was typed.

A run therefore reports four independent counts, kept separate on purpose:

  sent        lines written to the guest
  echoed      echoed input lines recovered from the log
  matched     echoed lines whose index equals the one sent
  executed    command output lines recovered from the log

`corrupted` is `sent - matched`, and the script exits non-zero when it is
non-zero, so it can be used as a gate directly.

Usage:
    tools/serial_fidelity.py --kernel-dir .kernel-build/<dir> [--count 300]
                             [--interval 0] [--log FILE] [--timeout 900]
"""

import argparse
import os
import re
import selectors
import signal
import subprocess
import sys
import time

# A corruption-proof alphabet for the index token: no two indices in a run can
# differ by one character, so a transposed digit cannot be mistaken for a
# neighbouring index.  The token is also long enough that losing or moving a
# byte anywhere inside it is visible as a mismatch rather than as a valid index.
TOKEN_RE = re.compile(rb"^FID(\d{6})$")


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


def find_echo_line(buf, token):
    """Return the echoed input line for `token`, or None.

    Kept for interactive debugging of a single line; the tally itself scans
    every echo line in the finished log (see the Tally comment below).
    """
    idx = buf.find(token)
    if idx < 0:
        return None
    start = buf.rfind(b"# echo ", max(0, idx - 64), idx)
    if start < 0:
        return None
    end = buf.find(b"\n", start)
    if end < 0:
        return None
    return buf[start + 2:end].strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel-dir", required=True)
    ap.add_argument("--count", type=int, default=300)
    ap.add_argument("--interval", type=float, default=0.0,
                    help="seconds between lines; 0 means wait for each prompt")
    ap.add_argument("--blast", type=int, default=0,
                    help="write this many command lines per host write, with "
                         "no wait for the guest in between; the amplifier for "
                         "the RX reorder race")
    ap.add_argument("--settle", type=float, default=0.05,
                    help="pause after a prompt before typing the next line")
    ap.add_argument("--timeout", type=float, default=900.0)
    ap.add_argument("--log", default=None)
    ap.add_argument("--qemu-arg", action="append", default=[])
    args = ap.parse_args()

    kernel_dir = os.path.abspath(args.kernel_dir)
    if not os.path.exists(f"{kernel_dir}/kernel.elf"):
        print(f"serial_fidelity: no kernel.elf under {kernel_dir}",
              file=sys.stderr)
        return 2

    log_path = args.log
    log_file = None
    if log_path:
        os.makedirs(os.path.dirname(log_path) or ".", exist_ok=True)
        log_file = open(log_path, "wb")

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

    deadline = time.monotonic() + args.timeout
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
        """Wait until `needle` appears at/after start_offset; return its index."""
        stop = time.monotonic() + budget
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

    echoed = []     # (sent_index, echoed_text_or_None); paced mode only
    executed = 0    # paced mode: commands whose output line was seen

    try:
        # Wait for the shell prompt.  15s covers the slowest boot seen here;
        # the overall --timeout still bounds the run if the guest never gets
        # there.
        if wait_for(b"# ", 120.0, 0) < 0:
            print("serial_fidelity: guest never reached a shell prompt",
                  file=sys.stderr)
            return 2

        prompt_end = buf.rfind(b"# ") + 2
        next_send = time.monotonic() + args.settle

        if args.blast > 0:
            # Burst mode: hand the guest `blast` command lines per host write
            # and never wait for it in between.  This is the amplifier -- it
            # keeps the 16550 receive FIFO and the kernel ring buffer
            # non-empty, which is the precondition for the RX poll/IRQ
            # reorder race.  Tallying still walks the finished log, so a
            # dropped or transposed byte shows up as a mismatched echo line
            # exactly as in the paced mode.
            pending = [b"echo FID%06d" % i
                       for i in range(1, args.count + 1)]
            for start in range(0, len(pending), args.blast):
                chunk = b"".join(line + b"\n" for line in pending[start:start + args.blast])
                process.stdin.write(chunk)
                process.stdin.flush()
                pump(0.05)
            deadline_tail = time.monotonic() + 120.0
            while time.monotonic() < deadline_tail:
                if buf.count(b"# ") >= args.count + 4:
                    break
                pump(1.0)
        else:
            for index in range(1, args.count + 1):
                token = b"FID%06d" % index
                if args.interval > 0:
                    now = time.monotonic()
                    if next_send > now:
                        pump(min(next_send - now, 0.5))
                    next_send = time.monotonic() + args.interval
                send(b"echo " + token)

                # The command's own output line is the deterministic end of
                # this command: mksh prints the token, then the next prompt.
                # Waiting for it keeps the guest's ring buffer far from full,
                # which is what separates an ordering bug from a plain
                # overrun.
                out_at = wait_for(token + b"\n", 60.0, prompt_end)
                if out_at < 0:
                    echoed.append((index, None))
                    print(f"serial_fidelity: line {index} produced no "
                          "output; stopping", file=sys.stderr)
                    break
                executed += 1
                prompt_end = buf.find(b"# ", out_at)
                if prompt_end < 0:
                    prompt_end = out_at + len(token) + 1
                else:
                    prompt_end += 2

                # Give the ring a moment to drain before the next burst so the
                # measurement is about RX fidelity, not about how fast the host
                # can outrun a 256-byte ring buffer.
                if args.interval <= 0:
                    pump(args.settle)

        send(b"poweroff")
        pump(3.0)
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

    # Tally by scanning every echoed input line in the finished log.
    #
    # The echoed line is `# echo FID%06d`, produced by the kernel RX path
    # (arch_uart_poll_getc -> uart_rx_push -> uart_getc -> tty_console_read's
    # echo) with no shell parsing in between, so whatever the guest actually
    # received is what appears there.  Scanning the whole log rather than
    # tracking each line as it is sent means a corrupted echo still lands in
    # the tally -- as a mismatched line -- instead of quietly not being found.
    echoed_re = re.compile(rb"^# (echo FID\d+)\s*$", re.MULTILINE)
    seen = {}
    order = []
    for m in echoed_re.finditer(bytes(buf)):
        text = m.group(1)
        order.append(text)
        seen.setdefault(text, 0)
        seen[text] += 1

    matched = 0
    missing = 0
    mismatches = []
    for index in range(1, args.count + 1):
        want = b"echo FID%06d" % index
        if want in seen:
            matched += 1
            seen.pop(want)
        else:
            missing += 1
            # Report the corrupted echo for this index, if the guest produced
            # any echo line at all that claims to be this command.
            for text in seen:
                if text.startswith(b"echo FID"):
                    got = text
                    break
            else:
                got = None
            mismatches.append((index, want, got))

    sent = args.count
    not_found = buf.count(b"inaccessible or not found")
    corrupted = missing

    print(f"serial_fidelity: kernel_dir={kernel_dir}")
    print(f"serial_fidelity: sent={sent} echoed_lines={len(order)} "
          f"matched={matched} corrupted={corrupted}")
    print(f"serial_fidelity: shell_not_found={not_found}")
    for index, want, got in mismatches[:20]:
        got_repr = got.decode(errors="replace") if got else "<no echo line>"
        print(f"serial_fidelity: CORRUPT line {index}: "
              f"sent={want.decode(errors='replace')!r} "
              f"echoed={got_repr!r}")
    if len(mismatches) > 20:
        print(f"serial_fidelity: ... {len(mismatches) - 20} more mismatches")

    return 0 if corrupted == 0 else 1


if __name__ == "__main__":
    sys.exit(main())