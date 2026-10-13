#!/usr/bin/env python3
"""Run the ordinary OpenCode main TUI through a bounded, drain-safe PTY.

All model interaction remains in the official interactive CLI. Input is forwarded
byte-for-byte except Ctrl+] (0x1d), which prints a compact screen snapshot.
The child PTY is continuously drained into a 64 KiB in-memory ring buffer; no
terminal stream, environment dump, or credentials are persisted.
"""
from __future__ import annotations

import argparse
import fcntl
import os
import pty
import shutil
import struct
import subprocess
import sys
import termios
import threading
import tty
import unicodedata

RING_LIMIT = 64 * 1024
SNAPSHOT_LIMIT = 6000
SNAPSHOT_LINES = 48
SNAPSHOT_KEY = b"\x1d"  # Ctrl+]


class OutputRing:
    """Bounded raw-output history; it is intentionally memory-only."""
    def __init__(self, limit: int = RING_LIMIT):
        self.limit = limit
        self.data = bytearray()

    def append(self, chunk: bytes) -> None:
        self.data.extend(chunk)
        if len(self.data) > self.limit:
            del self.data[:-self.limit]

class Screen:
    """Small bounded VT screen model for useful snapshots, not a full emulator."""
    def __init__(self, rows: int, cols: int):
        self.rows, self.cols = max(10, rows), max(40, cols)
        self.cells = [[" "] * self.cols for _ in range(self.rows)]
        self.row = self.col = 0
        self.mode = "text"
        self.csi = ""
        self.saved = (0, 0)
        self.pending = b""

    def feed(self, data: bytes) -> None:
        data = self.pending + data
        try:
            text = data.decode("utf-8")
            self.pending = b""
        except UnicodeDecodeError as exc:
            usable = exc.start
            if exc.reason == "unexpected end of data":
                usable = exc.start
                self.pending = data[usable:]
            else:
                usable = len(data)
                self.pending = b""
            text = data[:usable].decode("utf-8", errors="replace")
        for ch in text:
            if self.mode == "esc":
                if ch == "[":
                    self.mode, self.csi = "csi", ""
                elif ch == "]":
                    self.mode = "osc"
                elif ch == "7":
                    self.saved = (self.row, self.col); self.mode = "text"
                elif ch == "8":
                    self.row, self.col = self.saved; self.mode = "text"
                else:
                    self.mode = "text"
                continue
            if self.mode == "osc":
                if ch == "\a": self.mode = "text"
                elif ch == "\x1b": self.mode = "osc-esc"
                continue
            if self.mode == "osc-esc":
                self.mode = "text" if ch == "\\" else "osc"
                continue
            if self.mode == "csi":
                if "@" <= ch <= "~":
                    self._csi(ch); self.mode = "text"
                else:
                    self.csi += ch
                continue
            n = ord(ch)
            if ch == "\x1b": self.mode = "esc"
            elif ch == "\r": self.col = 0
            elif ch == "\n": self._linefeed()
            elif ch == "\b": self.col = max(0, self.col - 1)
            elif n >= 32 and n != 127: self._put(ch)

    def _linefeed(self):
        if self.row < self.rows - 1: self.row += 1
        else:
            self.cells.pop(0); self.cells.append([" "] * self.cols)

    def _put(self, ch):
        if self.col >= self.cols: self.col = 0; self._linefeed()
        self.cells[self.row][self.col] = ch
        self.col += 2 if unicodedata.east_asian_width(ch) in "WF" else 1
        if self.col >= self.cols: self.col = self.cols - 1

    def _csi(self, final):
        raw = self.csi.lstrip("?")
        try: p = [int(x) if x else 0 for x in raw.split(";")]
        except ValueError: p = [0]
        n = p[0] or 1
        if final in "Hf":
            self.row = min(self.rows - 1, max(0, (p[0] or 1) - 1))
            self.col = min(self.cols - 1, max(0, (p[1] if len(p) > 1 and p[1] else 1) - 1))
        elif final == "A": self.row = max(0, self.row - n)
        elif final == "B": self.row = min(self.rows - 1, self.row + n)
        elif final == "C": self.col = min(self.cols - 1, self.col + n)
        elif final == "D": self.col = max(0, self.col - n)
        elif final == "G": self.col = min(self.cols - 1, max(0, n - 1))
        elif final == "d": self.row = min(self.rows - 1, max(0, n - 1))
        elif final == "J" and (not p or p[0] in (2, 3)):
            self.cells = [[" "] * self.cols for _ in range(self.rows)]; self.row = self.col = 0
        elif final == "K":
            mode = p[0] if p else 0
            start, end = (self.col, self.cols) if mode == 0 else ((0, self.col + 1) if mode == 1 else (0, self.cols))
            for c in range(start, end): self.cells[self.row][c] = " "
        elif final == "s": self.saved = (self.row, self.col)
        elif final == "u": self.row, self.col = self.saved

    def snapshot(self) -> str:
        lines = ["".join(row).rstrip() for row in self.cells]
        while lines and not lines[-1]: lines.pop()
        return "\n".join(lines[-SNAPSHOT_LINES:])[-SNAPSHOT_LIMIT:]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default=os.path.expanduser("~/.opencode/versions/v2.0.28/node_modules/.bin/opencode2"))
    ap.add_argument("--cwd", default=os.getcwd())
    args = ap.parse_args()
    binary = shutil.which(args.binary) if os.path.sep not in args.binary else args.binary
    if not binary or not os.path.isfile(binary):
        print(f"OpenCode executable not found: {args.binary}", file=sys.stderr)
        return 127

    master, slave = pty.openpty()
    try:
        size = fcntl.ioctl(sys.stdin.fileno(), termios.TIOCGWINSZ, b"\0" * 8)
        fcntl.ioctl(slave, termios.TIOCSWINSZ, size)
    except (OSError, termios.error):
        pass
    proc = subprocess.Popen([binary, "."], cwd=args.cwd, stdin=slave, stdout=slave,
                            stderr=slave, close_fds=True, start_new_session=True)
    os.close(slave)
    ring = OutputRing()
    rows, cols = 35, 120
    try:
        packed = struct.unpack("HHHH", fcntl.ioctl(sys.stdin.fileno(), termios.TIOCGWINSZ, b"\0" * 8))
        rows, cols = packed[0], packed[1]
    except (OSError, struct.error):
        pass
    screen = Screen(rows, cols)
    lock = threading.Lock()
    stopping = threading.Event()

    def drain() -> None:
        while not stopping.is_set():
            try:
                data = os.read(master, 8192)
            except OSError:
                break
            if not data:
                break
            with lock:
                ring.append(data)
                screen.feed(data)
        stopping.set()

    thread = threading.Thread(target=drain, name="opencode-pty-drain", daemon=True)
    thread.start()
    old = None
    try:
        old = termios.tcgetattr(sys.stdin.fileno())
        tty.setraw(sys.stdin.fileno())
    except (OSError, termios.error):
        old = None

    def snapshot() -> None:
        with lock:
            visible = screen.snapshot()
        sys.stdout.write("\r\n--- OpenCode screen snapshot (bounded) ---\r\n")
        sys.stdout.write(visible + "\r\n--- end snapshot; Ctrl+] refreshes ---\r\n")
        sys.stdout.flush()

    print(f"OpenCode main TUI started; pid={proc.pid}; cwd={args.cwd}; ring_limit={RING_LIMIT} bytes. Ctrl+] prints a compact snapshot.", flush=True)
    try:
        while proc.poll() is None and not stopping.is_set():
            try:
                inp = os.read(sys.stdin.fileno(), 4096)
            except OSError:
                break
            if not inp:
                break
            if SNAPSHOT_KEY in inp:
                before, _, after = inp.partition(SNAPSHOT_KEY)
                if before:
                    os.write(master, before)
                if after:
                    os.write(master, after)
                snapshot()
            else:
                os.write(master, inp)
    except KeyboardInterrupt:
        pass
    finally:
        if old is not None:
            try:
                termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, old)
            except (OSError, termios.error):
                pass
        if proc.poll() is None:
            # Forward an ordinary Ctrl-C to the TUI process group; do not kill it.
            try:
                os.write(master, b"\x03")
                proc.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                pass
        stopping.set()
        thread.join(timeout=1)
        os.close(master)
    return proc.returncode or 0


if __name__ == "__main__":
    raise SystemExit(main())
