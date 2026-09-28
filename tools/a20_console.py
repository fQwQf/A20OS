"""Physical console and deploy actions for a20.

`a20 console` attaches to a board's serial port, optionally resets it, waits for
the boot log to prove the kernel came up, runs commands, and asserts the
expected output.  `a20 deploy` does that after programming the board.

Two design points worth stating.

The serial port is spoken with termios/fcntl/select from the standard library.
`tools/a20` declares zero third-party dependencies and docs/instances.md
promises stdlib only, so pulling in pyserial to set six flags would have been
the wrong trade for a tool every contributor runs.

The session is a state machine over an abstract transport rather than a
straight-line script.  A board is not always present, so the logic that decides
"did the kernel actually boot" has to be exercisable without one; a pty pair
provides a real tty for the transport tests, and a fake transport covers the
sequencing.
"""

from __future__ import annotations

import contextlib
import errno
import fcntl
import os
import re
import select
import shlex
import subprocess
import termios
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Protocol, Sequence

from a20_instance import Instance
from a20_make import REPO_ROOT

DEFAULT_BAUD = 115200
DEFAULT_BOOT_WAIT_S = 3.0
DEFAULT_BOOT_TIMEOUT_S = 120.0
DEFAULT_EXPECT_TIMEOUT_S = 60.0
_POLL_S = 0.2
_READ_CHUNK = 4096

# Baud encodings are platform-specific and not a short, memorable list: Linux
# uses B9600=13, B115200=0o10002, while BSD numbers them differently.  The
# termios module already carries the right constants, so the platform owns the
# encoding rather than a hand-written table here.
def _baud_code(baud: int) -> int:
    code = getattr(termios, f"B{baud}", None)
    if code is None:
        supported = sorted(int(n[1:]) for n in dir(termios)
                           if n.startswith("B") and n[1:].isdigit())
        raise ConsoleError(f"unsupported baud rate {baud}; this platform offers "
                           f"{', '.join(str(b) for b in supported)}")
    return code


class ConsoleError(RuntimeError):
    """The board could not be observed, or did not behave as declared."""


class Transport(Protocol):
    """Byte source/sink; a real serial port and a fake both satisfy this."""

    def read(self, timeout: float) -> bytes: ...
    def write(self, data: bytes) -> None: ...
    def close(self) -> None: ...


class SerialTransport:
    """A serial port opened in raw mode, read with select so reads can time out."""

    def __init__(self, device: str, baud: int = DEFAULT_BAUD) -> None:
        self.device = device
        self._fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            self._configure(baud)
        except Exception:
            os.close(self._fd)
            raise

    def _configure(self, baud: int) -> None:
        if os.isatty(self._fd):
            attrs = termios.tcgetattr(self._fd)
            iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs
            iflag &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK
                       | termios.ISTRIP | termios.INLCR | termios.IGNCR
                       | termios.ICRNL | termios.IXON)
            oflag &= ~termios.OPOST
            lflag &= ~(termios.ECHO | termios.ECHONL | termios.ICANON
                       | termios.ISIG | termios.IEXTEN)
            cflag &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB)
            cflag |= termios.CS8 | termios.CREAD | termios.CLOCAL
            speed = _baud_code(baud)
            cc = list(cc)
            cc[termios.VMIN] = 0
            cc[termios.VTIME] = 0
            termios.tcsetattr(self._fd, termios.TCSANOW,
                              [iflag, oflag, cflag, lflag, speed, speed, cc])
        # Refuse to let a second process fight over the same adapter.
        with contextlib.suppress(OSError):
            fcntl.ioctl(self._fd, termios.TIOCEXCL)

    def read(self, timeout: float) -> bytes:
        deadline = time.monotonic() + timeout
        chunks: list[bytes] = []
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                ready, _, _ = select.select([self._fd], [], [], min(remaining, _POLL_S))
            except InterruptedError:
                continue
            if not ready:
                continue
            try:
                data = os.read(self._fd, _READ_CHUNK)
            except BlockingIOError:
                continue
            except OSError as e:
                if e.errno in (errno.EIO, errno.EBADF):
                    break
                raise
            if not data:
                break
            chunks.append(data)
        return b"".join(chunks)

    def write(self, data: bytes) -> None:
        while data:
            try:
                written = os.write(self._fd, data)
            except BlockingIOError:
                time.sleep(0.01)
                continue
            data = data[written:]

    def close(self) -> None:
        with contextlib.suppress(OSError):
            os.close(self._fd)


@dataclass
class Transcript:
    """Everything the board said, so a failure can be read after the fact."""

    text: str = ""
    lines: int = 0

    def append(self, chunk: bytes) -> None:
        self.text += chunk.decode("utf-8", errors="replace")
        self.lines = self.text.count("\n")

    def tail(self, n: int = 80) -> str:
        return "\n".join(self.text.splitlines()[-n:])


@dataclass
class SessionResult:
    ok: bool
    stage: str
    missing: tuple[str, ...] = ()
    seen: tuple[str, ...] = ()
    transcript: Transcript = field(default_factory=Transcript)


def _missing(patterns: Sequence[str], text: str) -> list[str]:
    return [p for p in patterns if p not in text]


def run_console_session(transport: Transport, inst: Instance, *,
                        sleep=time.sleep, now=time.monotonic,
                        on_output: Callable[[str], None] | None = None) -> SessionResult:
    """Reset (optional), wait for the boot signature, run commands, assert.

    Every wait is bounded: a board that never boots, or never answers, ends the
    session with a named stage rather than hanging the terminal.
    """
    t = inst.target
    tr = Transcript()
    patterns = tuple(t.console_check or ())

    def pump(timeout: float) -> None:
        chunk = transport.read(timeout)
        if chunk:
            tr.append(chunk)
            if on_output:
                on_output(chunk.decode("utf-8", errors="replace"))

    try:
        if t.reset:
            _run_reset(t.reset)

        if patterns:
            budget = _seconds(t.boot_timeout, DEFAULT_BOOT_TIMEOUT_S)
            deadline = now() + budget
            while True:
                pump(min(_POLL_S, max(0.0, deadline - now())))
                if not _missing(patterns, tr.text):
                    break
                if now() >= deadline:
                    return SessionResult(False, "console_check",
                                         tuple(_missing(patterns, tr.text)), patterns, tr)
        else:
            pump(0.0)

        if t.boot_wait:
            sleep(t.boot_wait)
        elif t.commands:
            pump(0.5)

        if t.commands:
            transport.write(b"".join(c.encode() + b"\n" for c in t.commands))
            expect = tuple(t.expect or ())
            budget = _seconds(t.boot_timeout, DEFAULT_EXPECT_TIMEOUT_S)
            deadline = now() + budget
            while True:
                pump(min(_POLL_S, max(0.0, deadline - now())))
                if not _missing(expect, tr.text):
                    break
                if now() >= deadline:
                    return SessionResult(False, "expect",
                                         tuple(_missing(expect, tr.text)), expect, tr)
        return SessionResult(True, "done", (), (), tr)
    finally:
        transport.close()


def _seconds(text: str | None, default: float) -> float:
    if text is None:
        return default
    m = re.fullmatch(r"(\d+(?:\.\d+)?)s", text.strip())
    if m is None:
        raise ConsoleError(f"expected a duration like '90s', got {text!r}")
    return float(m.group(1))


def _run_reset(command: str) -> None:
    """Run the declared reset command.

    shlex.split rather than a shell, so a manifest cannot smuggle in a pipeline;
    quoting inside the string still works the way it reads.
    """
    argv = shlex.split(command)
    if not argv:
        raise ConsoleError("target.reset is empty")
    proc = subprocess.run(argv, check=False, capture_output=True, text=True)
    if proc.returncode != 0:
        raise ConsoleError(f"reset command failed ({proc.returncode}): {command}\n"
                           f"{proc.stderr.strip()}")


def console_log_path(inst: Instance, repo_root: Path = REPO_ROOT) -> Path:
    return repo_root / (inst.target.log or f".kernel-build/console/{inst.name}.log")


def open_transport(inst: Instance) -> Transport:
    t = inst.target
    if not t.serial:
        raise ConsoleError(f"{inst.source}: [target].serial is required to attach a console")
    return SerialTransport(t.serial, t.baud or DEFAULT_BAUD)


def write_console_log(path: Path, tr: Transcript) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(tr.text, encoding="utf-8", errors="replace")
