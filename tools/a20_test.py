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
from a20_error import EXIT_FAIL, EXIT_OK, InstanceBusy, ToolError
from a20_instance import Instance
from a20_make import REPO_ROOT, build_instance
from a20_resource import Policy, preflight

SMOKE_LOG_DIR = REPO_ROOT / ".kernel-build" / "smoke"
DEFAULT_TIMEOUT_S = 20.0
DEFAULT_INPUT_DELAY_S = 8
_TERM_GRACE_S = 5.0
_PROGRESS_HEARTBEAT_S = 15.0

# `make -n` prints the recipe verbatim, but a recursive make prefixes its own
# "make[N]: " chatter, and the binary may appear after an env prefix.  Anchor on
# the token rather than the line so those wrappers do not defeat the match.
_QEMU_TOKEN = re.compile(r"(?:^|\s)((?:\S*/)?qemu-system-[A-Za-z0-9_.-]+)\s")
_TIMEOUT_RE = re.compile(r"^\s*(\d+(?:\.\d+)?)s\s*$")



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
            raise InstanceBusy(
                f"instance '{instance_name}' is already running ({owner})",
                hint=f"wait for it to finish, or remove {lock_path} if you "
                     f"are sure that run is gone") from None
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
    try:
        out = subprocess.run(
            ["make", "-C", str(REPO_ROOT), *derive_make_vars(inst), "_qemu_argv"],
            check=False, capture_output=True, text=True,
        )
    except FileNotFoundError as exc:
        raise ToolError("`make` is not on PATH",
                        hint="a20 builds through make; install GNU make or fix PATH",
                        status=127) from exc
    if out.returncode != 0:
        # make's own diagnosis is the useful part; keep it in the message.
        detail = out.stderr.strip() or out.stdout.strip()
        raise ToolError(f"make _qemu_argv failed for {inst.name} "
                        f"(status {out.returncode})"
                        + (f"\n{detail}" if detail else ""),
                        status=out.returncode)
    for line in out.stdout.splitlines():
        m = _QEMU_TOKEN.search(line)
        if m:
            # machine.extra_qemu already reached the Makefile as EXTRA_QEMU and
            # was folded into QEMU_FLAGS, so the emitted line carries it.  Do
            # not append inst.machine.extra_qemu again -- that would double it.
            return shlex.split(line[m.start(1):])
    raise ToolError(f"no qemu-system command in 'make _qemu_argv' output "
                    f"for {inst.name}",
                    hint=out.stdout.strip() or None)


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


def _progress_read(log: Path, offset: int) -> tuple[str, int]:
    """Read whatever QEMU has appended to the log since `offset`."""
    try:
        with log.open("r", errors="replace") as fh:
            fh.seek(offset)
            chunk = fh.read()
            return chunk, offset + len(chunk)
    except OSError:
        return "", offset


def _watch(proc: subprocess.Popen[bytes], log: Path, expected: tuple[str, ...],
           timeout: float, echo, poll: float = 0.25) -> bool:
    """Wait for the guest, reporting progress.  Returns True if it timed out.

    QEMU's output only ever went to the log file, so a boot that takes four
    minutes to reach its first marker produced four minutes of a completely
    silent terminal followed by one PASS or FAIL line.  That is indistinguishable
    from a hang, and it is the single worst thing about running a smoke.

    So each expected marker is announced the moment it appears, and while
    nothing has appeared yet a heartbeat says how long this has been going and
    how much of the log has been written.  Both are cheap: this only reads the
    log the guest is already writing.
    """
    start = time.monotonic()
    last_beat = start
    seen: set[str] = set()
    offset = 0
    text_so_far = ""
    while True:
        rc = proc.poll()
        now = time.monotonic()
        if rc is not None:
            return False
        if now - start >= timeout:
            return True
        chunk, offset = _progress_read(log, offset)
        if chunk:
            text_so_far += chunk
            for pat in expected:
                if pat not in seen and pat in text_so_far:
                    seen.add(pat)
                    echo(f"  [{now - start:5.1f}s] seen: {pat}")
        if now - last_beat >= _PROGRESS_HEARTBEAT_S:
            echo(f"  [{now - start:5.1f}s] waiting: {len(seen)}/{len(expected)} "
                 f"markers, {offset} bytes of guest output so far")
            last_beat = now
        time.sleep(poll)


def run_test(inst: Instance, make_args: list[str], dry_run: bool) -> int:
    """Build, boot, inject [test].commands, and check [test].expect."""
    from a20_error import A20Error
    if not inst.test.expect:
        raise A20Error(f"{inst.source}: [test].expect is required for 'a20 test'")
    expected = tuple(inst.test.expect or ())
    delay = float(inst.test.input_delay) if inst.test.input_delay is not None else DEFAULT_INPUT_DELAY_S
    timeout = _parse_timeout(inst.test.timeout)
    log = SMOKE_LOG_DIR / f"{inst.name}.log"

    # The lock covers the build, not just the run.  It used to be taken after
    # build_instance, so two runs of one instance -- `make -j`, or two CI jobs
    # on one runner -- both did the full multi-minute build writing into the
    # same BUILD_DIR, and only then did the loser discover it was a loser.  The
    # lock exists to stop exactly that, so it has to be taken first.
    with _exclusive(inst.name):
        build_instance(inst, list(make_args), dry_run)
        qemu_cmd = _qemu_cmdline(inst)
        if dry_run:
            print(shlex.join(qemu_cmd))
            return EXIT_OK

        # Re-check here, not only before the build.  The gate above runs before a
        # build that can take minutes, and the scarce resources -- the host
        # ports in particular -- can be taken in between.  This is the
        # authoritative one; the earlier gate exists to fail fast, not to grant
        # permission.
        preflight(inst, Policy.from_env(), REPO_ROOT, wait=True,
                  echo=lambda m: print(m, flush=True), guest=True)

        with log.open("wb") as logf:
            proc = subprocess.Popen(
                qemu_cmd, stdin=subprocess.PIPE, stdout=logf,
                stderr=subprocess.STDOUT, cwd=REPO_ROOT,
                start_new_session=True,
            )
            feeder = threading.Thread(target=_feed_commands, args=(proc, inst, delay), daemon=True)
            feeder.start()
            try:
                timed_out = _watch(proc, log, expected, timeout,
                                   echo=lambda m: print(m, flush=True))
            finally:
                # Reap on *every* exit path.  It used to run only on timeout and
                # Ctrl-C, so any other exception left QEMU alive holding its
                # hostfwd ports and its 1-4 GiB -- and because the process was
                # started in a new session, a SIGKILL of a20 could not reach it
                # either, while the kernel released the flock immediately.  The
                # instance then looked free to the next run, which lost the port
                # race against a guest nobody was tracking any more.
                if proc.poll() is None:
                    _reap(proc)
                with contextlib.suppress(Exception):
                    feeder.join(timeout=1.0)
        text = log.read_text(errors="replace")

    missing = [p for p in expected if p not in text]
    if not missing:
        print(f"{inst.name}: PASS; log saved to {log}")
        return EXIT_OK
    # "exited with status 0" reads as a contradiction next to FAIL: a guest that
    # powers off cleanly has every right to exit 0, and that is the normal path.
    # The guest's exit status is secondary to whether the markers appeared.
    if timed_out:
        why = f"the guest did not finish within {_parse_timeout(inst.test.timeout):g}s"
    else:
        why = f"the guest exited with status {proc.returncode}"
    print(f"{inst.name}: FAIL -- {len(missing)} of {len(expected)} expected "
          f"marker(s) never appeared ({why})")
    for pat in missing:
        print(f"  missing: {pat}")
    print(f"  transcript: {log} (last 80 lines below)")
    print("\n".join(text.splitlines()[-80:]))
    return EXIT_FAIL
