#!/usr/bin/env python3
"""Prove the internal-ESP log fallback survives an unclean power cut.

The laptop shell image normally carries a dedicated "A20OS log" partition.
This gate renames that partition on a disposable copy, so the kernel has to
fall back to the EFI System Partition of the emulated internal NVMe disk.  It
then kills QEMU without a shutdown and reads ``A20LOG/`` back from the image
with mtools.  That is the exact failure shape the USB-only log path cannot
cover: xHCI is dead, so the boot stick is not a writable log surface.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
ESP_OFFSET = 2048 * 512


def wait_for(log: Path, markers: tuple[str, ...], timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = log.read_text(errors="replace") if log.exists() else ""
        if all(marker in text for marker in markers):
            return
        time.sleep(0.25)
    missing = [marker for marker in markers
               if marker not in (log.read_text(errors="replace")
                                 if log.exists() else "")]
    raise RuntimeError(f"timed out waiting for {missing}; log: {log}")


def mtools(image: Path, *args: str) -> str:
    source = f"{image}@@{ESP_OFFSET}"
    return subprocess.check_output(
        ["mdir" if args[0] == "list" else "mtype",
         "-i", source, *args[1:]],
        text=True, errors="replace")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--code", type=Path,
                        default=Path(os.environ.get(
                            "OVMF_CODE", "/usr/share/OVMF/OVMF_CODE_4M.fd")))
    parser.add_argument("--vars", type=Path,
                        default=Path(os.environ.get(
                            "OVMF_VARS", "/usr/share/OVMF/OVMF_VARS_4M.fd")))
    parser.add_argument("--log", type=Path,
                        default=ROOT / ".kernel-build/smoke/laptop-esp-log.log")
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--keep-log-partition", action="store_true",
                        help="exercise explicit esp mode with a dedicated log partition present")
    args = parser.parse_args()
    for path in (args.image, args.code, args.vars):
        if not path.is_file():
            parser.error(f"required file missing: {path}")
    for tool in ("qemu-system-x86_64", "sgdisk", "mdir", "mtype"):
        if not shutil.which(tool):
            parser.error(f"{tool} is required")
    args.log.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="a20-esp-log-") as temporary:
        stage = Path(temporary)
        image = stage / "a20os-esp-log.img"
        shutil.copyfile(args.image, image)
        # Remove the dedicated log partition name, not the partition itself:
        # boot still needs the same GPT layout, but the kernel can no longer
        # take the direct path and must use the ESP fallback.
        if not args.keep_log_partition:
            subprocess.run(["sgdisk", "-c", "2:NOT-A-LOG", str(image)],
                           check=True, stdout=subprocess.DEVNULL)
        variables = stage / "vars.fd"
        shutil.copyfile(args.vars, variables)
        args.log.unlink(missing_ok=True)
        command = [
            "qemu-system-x86_64", "-machine", "q35",
            "-cpu", "max,pdpe1gb=off", "-m", "1024M", "-smp", "1",
            "-display", "none", "-no-reboot", "-net", "none",
            "-drive",
            f"if=pflash,format=raw,readonly=on,file={args.code.resolve()}",
            "-drive", f"if=pflash,format=raw,file={variables}",
            "-drive", f"file={image},format=raw,if=none,id=nvme0",
            "-device", "nvme,drive=nvme0,serial=A20OSLAPTOP",
            "-serial", f"file:{args.log}",
        ]
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
        try:
            wait_for(args.log, (
                "[LOG] persistent log at /var/log/a20-esp/A20LOG",
                "execve mksh",
            ), args.timeout)
            # At least one periodic 5 s fsync plus the attach flush.  The
            # machine is deliberately not shut down.
            time.sleep(8)
            os.kill(process.pid, signal.SIGKILL)
            process.wait(timeout=30)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()

        names = mtools(image, "list", "-b", "::/A20LOG")
        boot_files = [line.strip() for line in names.splitlines()
                      if line.strip().lower().endswith(".jsonl")]
        if not boot_files:
            raise RuntimeError(f"no boot-*.jsonl under A20LOG: {names!r}")
        current = mtools(image, "type", "::/A20LOG/CURRENT")
        if "state=ready" not in current and "state=booting" not in current:
            raise RuntimeError(f"unexpected CURRENT after power cut: {current!r}")

        records = mtools(image, "type", boot_files[0])
        good = 0
        for line in records.splitlines():
            if not line.strip():
                continue
            try:
                json.loads(line)
            except json.JSONDecodeError:
                # A torn final line is the documented power-cut shape; only
                # the complete prefix is required.
                continue
            good += 1
        if good < 20:
            raise RuntimeError(f"only {good} complete JSONL records survived")
        if "[PS2] module init ok" not in records:
            raise RuntimeError("PS/2 init record missing from ESP log")
        if "persistent log at /var/log/a20-esp/A20LOG" not in records:
            raise RuntimeError("fallback attach record missing from ESP log")

        print(f"smoke-laptop-esp-log: PASS; {good} JSONL records survived "
              f"an unclean power cut; log: {args.log}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:  # noqa: BLE001 - the gate must report a reason
        print(f"smoke-laptop-esp-log: FAIL: {error}", file=sys.stderr)
        sys.exit(1)
