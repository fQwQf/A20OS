#!/usr/bin/env python3
"""Run an instance's QEMU line with the accelerator forced to TCG.

`a20 test` derives its QEMU command line through tools/a20_test.py, which
calls make's `_qemu_argv` without forwarding the `--` passthrough variables
(tools/a20_test.py:99-105).  On an x86_64 host that Makefile picks kvm whenever
/dev/kvm exists (Makefile:807), and QEMU honours the first accelerator that
initialises, so appending `-accel tcg` through machine.extra_qemu does not
switch it.  The load this probe measures has to run under TCG to occupy the
CPU for hundreds of milliseconds at all, so the accelerator is substituted here
instead.

Everything else -- the command line itself, the images, the injected
[test].commands and the [test].expect check -- is taken unchanged from the
instance manifest, so the numbers this produces are the ones `a20 test` would
report for the same manifest on a host without KVM.
"""
from __future__ import annotations

import argparse

import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

from a20_instance import parse_instance  # noqa: E402
from a20_test import _parse_timeout, _qemu_cmdline  # noqa: E402

DEFAULT_INPUT_DELAY_S = 60


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("instance")
    ap.add_argument("--log", default=None)
    ap.add_argument("--artifact-dir", default=None,
                    help="substitute this .kernel-build subdirectory for the "
                         "one the manifest derives; used to boot a kernel "
                         "built with different CONFIG_* flags")
    args = ap.parse_args()

    inst = parse_instance(REPO / "instances" / f"{args.instance}.toml")
    cmd = _qemu_cmdline(inst)
    if args.artifact_dir:
        def swap(arg: str) -> str:
            # Both the bare image paths and the `-drive file=...` form carry the
            # build directory; leaving one behind would boot a kernel against a
            # different tree's rootfs.
            m = re.match(r"^(file=)?(\.kernel-build/[^/]+)/", arg)
            if not m:
                return arg
            return f"{m.group(1) or ''}{args.artifact_dir}/{arg[m.end():]}"
        cmd = [swap(a) for a in cmd]
    if "-accel" not in cmd:
        print(f"{inst.name}: no -accel in the derived line; QEMU default (TCG) already applies")
    else:
        i = cmd.index("-accel")
        print(f"{inst.name}: substituting accel {cmd[i + 1]!r} -> 'tcg,thread=multi'")
        cmd[i + 1] = "tcg,thread=multi"

    log = Path(args.log) if args.log else REPO / ".kernel-build" / "smoke" / f"{inst.name}-tcg.log"
    log.parent.mkdir(parents=True, exist_ok=True)

    delay = float(inst.test.input_delay) if inst.test.input_delay is not None \
        else DEFAULT_INPUT_DELAY_S
    timeout = _parse_timeout(inst.test.timeout)

    print(f"{inst.name}: {' '.join(shlex.quote(c) for c in cmd)}")
    print(f"{inst.name}: log={log} input_delay={delay}s timeout={timeout}s")

    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    start = time.monotonic()
    chunks: list[bytes] = []

    import threading

    def drain():
        while True:
            b = proc.stdout.read(1)
            if not b:
                return
            chunks.append(b)

    t = threading.Thread(target=drain, daemon=True)
    t.start()

    def feed():
        time.sleep(delay)
        for c in inst.test.commands or ():
            try:
                proc.stdin.write((c + "\n").encode())
                proc.stdin.flush()
            except (BrokenPipeError, ValueError):
                return
            time.sleep(1)

    f = threading.Thread(target=feed, daemon=True)
    f.start()

    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    t.join(timeout=5)
    data = b"".join(chunks)
    log.write_bytes(data)
    print(f"{inst.name}: guest exited after {time.monotonic() - start:.1f}s, "
          f"{len(data)} bytes -> {log}")

    text = data.decode(errors="replace")
    missing = [p for p in (inst.test.expect or ()) if p not in text]
    for p in inst.test.expect or ():
        for line in text.splitlines():
            if p in line:
                print(f"  HIT {p!r}: {line.strip()}")
                break
    if missing:
        print(f"{inst.name}: FAIL -- missing {missing}")
        return 1
    print(f"{inst.name}: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())