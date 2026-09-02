"""Smoke-test execution for a20: boot an instance and check expect patterns.

The QEMU command line is extracted from `make -n _run_impl` (the single
source of truth), extended with the instance's machine.extra_qemu, then run
with a timeout while [test].commands are injected over the serial console.
PASS semantics match the historical handwritten smokes: every [test].expect
substring must appear in the log.
"""

from __future__ import annotations

import shlex
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


def _qemu_cmdline(inst: Instance) -> list[str]:
    out = subprocess.run(
        ["make", "-C", str(REPO_ROOT), "-n", *derive_make_vars(inst), "_run_impl"],
        check=False, capture_output=True, text=True,
    )
    for line in out.stdout.splitlines():
        if line.startswith("qemu-system"):
            cmd = shlex.split(line)
            cmd.extend(inst.machine.extra_qemu or ())
            return cmd
    raise SystemExit(f"error: no qemu-system command found in 'make -n _run_impl' output:\n{out.stdout}")


def _feed_commands(proc: subprocess.Popen[bytes], inst: Instance, delay: float) -> None:
    try:
        time.sleep(delay)
        assert proc.stdin is not None
        for command in inst.test.commands or ():
            proc.stdin.write(command.encode() + b"\n")
            proc.stdin.flush()
        proc.stdin.close()
    except (BrokenPipeError, OSError) as e:
        print(f"{inst.name}: stdin injection stopped early ({e}); the log decides the result",
              file=sys.stderr)


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

    SMOKE_LOG_DIR.mkdir(parents=True, exist_ok=True)
    log = SMOKE_LOG_DIR / f"{inst.name}.log"
    delay = float(inst.test.input_delay) if inst.test.input_delay is not None else DEFAULT_INPUT_DELAY_S
    timeout = float(inst.test.timeout[:-1]) if inst.test.timeout is not None else DEFAULT_TIMEOUT_S
    with log.open("wb") as logf:
        proc = subprocess.Popen(
            qemu_cmd, stdin=subprocess.PIPE, stdout=logf,
            stderr=subprocess.STDOUT, cwd=REPO_ROOT,
        )
        feeder = threading.Thread(target=_feed_commands, args=(proc, inst, delay), daemon=True)
        feeder.start()
        timed_out = True
        try:
            proc.wait(timeout=timeout)
            timed_out = False
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    text = log.read_text(errors="replace")
    missing = [p for p in inst.test.expect or () if p not in text]
    if not missing:
        print(f"{inst.name}: PASS; log saved to {log}")
        return 0
    outcome = "timeout" if timed_out else f"exited with status {proc.returncode}"
    print(f"{inst.name}: FAIL ({outcome}); missing patterns: {missing}; tail of {log}:")
    print("\n".join(text.splitlines()[-80:]))
    return 1
