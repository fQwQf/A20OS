"""Smoke-test execution for a20: boot an instance and check expect patterns.

The QEMU command line is printed by the `_qemu_argv` make target (the single
source of truth for every flag, which already includes machine.extra_qemu via
EXTRA_QEMU), then run with a timeout while [test].commands are injected over
the serial console.  PASS semantics match the historical handwritten smokes:
every [test].expect substring must appear in the log.
"""

from __future__ import annotations

import contextlib
import errno
import fcntl
import os
import re
import shlex
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

from a20_derive import derive_make_vars
from a20_instance import Instance
from a20_make import REPO_ROOT, build_instance

SMOKE_LOG_DIR = REPO_ROOT / ".kernel-build" / "smoke"
DEFAULT_TIMEOUT_S = 20.0
DEFAULT_INPUT_DELAY_S = 8
_TERM_GRACE_S = 5.0

# `make -n` prints the recipe verbatim, but a recursive make prefixes its own
# "make[N]: " chatter, and the binary may appear after an env prefix.  Anchor on
# the token rather than the line so those wrappers do not defeat the match.
_QEMU_TOKEN = re.compile(r"(?:^|\s)((?:\S*/)?qemu-system-[A-Za-z0-9_.-]+)\s")
_TIMEOUT_RE = re.compile(r"^\s*(\d+(?:\.\d+)?)s\s*$")


class InstanceBusy(SystemExit):
    """Another run of this instance already holds the log."""


@contextlib.contextmanager
def _exclusive(instance_name: str):
    """Hold an exclusive claim on one instance's log for the duration.

    Without this, two runs of the same instance -- `make -j`, or two CI jobs on
    one runner -- write into one file and produce a log that belongs to neither.
    flock is used rather than an O_EXCL lockfile because the kernel releases it
    when the holder dies, so a killed run cannot leave the instance permanently
    unusable.
    """
    SMOKE_LOG_DIR.mkdir(parents=True, exist_ok=True)
    lock_path = SMOKE_LOG_DIR / f"{instance_name}.lock"
    fd = os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o644)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as e:
            if e.errno not in (errno.EACCES, errno.EAGAIN):
                raise
            owner = os.read(fd, 64).decode(errors="replace").strip() or "another run"
            raise SystemExit(
                f"error: instance '{instance_name}' is already running ({owner}); "
                f"wait for it or remove {lock_path} if you are sure it is stale") from None
        os.ftruncate(fd, 0)
        os.write(fd, f"pid {os.getpid()}\n".encode())
        yield
    finally:
        with contextlib.suppress(OSError):
            fcntl.flock(fd, fcntl.LOCK_UN)
        os.close(fd)


def _reap(proc: subprocess.Popen[bytes]) -> None:
    """Stop the guest and everything it spawned, then collect it.

    The child leads its own session, so a terminal Ctrl-C does not reach it and
    a plain proc.kill() would leave anything QEMU spawned running. Signalling the
    process group is the only thing that reaps the whole tree.
    """
    if proc.poll() is not None:
        return
    for sig, grace in ((signal.SIGTERM, _TERM_GRACE_S), (signal.SIGKILL, _TERM_GRACE_S)):
        with contextlib.suppress(ProcessLookupError, OSError):
            os.killpg(proc.pid, sig)
        try:
            proc.wait(timeout=grace)
            return
        except subprocess.TimeoutExpired:
            continue
    with contextlib.suppress(subprocess.TimeoutExpired):
        proc.wait(timeout=_TERM_GRACE_S)


def _qemu_cmdline(inst: Instance) -> list[str]:
    # `_qemu_argv` is a printf recipe with no prerequisites, so running it builds
    # nothing and its stdout is the command line itself.  `make -n` would print
    # the printf invocation instead of its output, hence no -n here.
    out = subprocess.run(
        ["make", "-C", str(REPO_ROOT), *derive_make_vars(inst), "_qemu_argv"],
        check=False, capture_output=True, text=True,
    )
    if out.returncode != 0:
        raise SystemExit(f"error: 'make _qemu_argv' failed for {inst.name} "
                         f"(status {out.returncode}):\n{out.stderr.strip()}")
    for line in out.stdout.splitlines():
        m = _QEMU_TOKEN.search(line)
        if m:
            # machine.extra_qemu already reached the Makefile as EXTRA_QEMU and
            # was folded into QEMU_FLAGS, so the emitted line carries it.  Do
            # not append inst.machine.extra_qemu again -- that would double it.
            return shlex.split(line[m.start(1):])
    raise SystemExit(f"error: no qemu-system command found in 'make _qemu_argv' "
                     f"output for {inst.name}:\n{out.stdout}")


def _feed_commands(proc: subprocess.Popen[bytes], inst: Instance, delay: float) -> None:
    try:
        time.sleep(delay)
        if proc.stdin is None:
            return
        for command in inst.test.commands or ():
            proc.stdin.write(command.encode() + b"\n")
            proc.stdin.flush()
        proc.stdin.close()
    except (BrokenPipeError, OSError) as e:
        print(f"{inst.name}: stdin injection stopped early ({e}); the log decides the result",
              file=sys.stderr)


def _parse_timeout(text: str | None) -> float:
    if text is None:
        return DEFAULT_TIMEOUT_S
    m = _TIMEOUT_RE.match(text)
    if m is None:
        raise SystemExit(f"error: [test].timeout must look like '45s', got {text!r}")
    return float(m.group(1))


def run_test(inst: Instance, make_args: list[str], dry_run: bool) -> int:
    """Build, boot, inject [test].commands, and grep the log for [test].expect."""
    if not inst.test.expect:
        raise SystemExit(f"error: {inst.source}: [test].expect is required for 'a20 test'")
    build_rc = build_instance(inst, list(make_args), dry_run)
    if build_rc != 0:
        return build_rc
    qemu_cmd = _qemu_cmdline(inst)
    if dry_run:
        print(shlex.join(qemu_cmd))
        return 0

    delay = float(inst.test.input_delay) if inst.test.input_delay is not None else DEFAULT_INPUT_DELAY_S
    timeout = _parse_timeout(inst.test.timeout)
    log = SMOKE_LOG_DIR / f"{inst.name}.log"

    with _exclusive(inst.name):
        with log.open("wb") as logf:
            proc = subprocess.Popen(
                qemu_cmd, stdin=subprocess.PIPE, stdout=logf,
                stderr=subprocess.STDOUT, cwd=REPO_ROOT,
                start_new_session=True,
            )
            feeder = threading.Thread(target=_feed_commands, args=(proc, inst, delay), daemon=True)
            feeder.start()
            timed_out = True
            try:
                proc.wait(timeout=timeout)
                timed_out = False
            except subprocess.TimeoutExpired:
                _reap(proc)
            except KeyboardInterrupt:
                _reap(proc)
                raise
            finally:
                with contextlib.suppress(Exception):
                    feeder.join(timeout=1.0)
        text = log.read_text(errors="replace")

    missing = [p for p in inst.test.expect or () if p not in text]
    if not missing:
        print(f"{inst.name}: PASS; log saved to {log}")
        return 0
    outcome = "timeout" if timed_out else f"exited with status {proc.returncode}"
    print(f"{inst.name}: FAIL ({outcome}); missing patterns: {missing}; tail of {log}:")
    print("\n".join(text.splitlines()[-80:]))
    return 1
