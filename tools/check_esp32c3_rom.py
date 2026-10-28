#!/usr/bin/env python3
"""Boot the packaged ROM image in the C3 model and check UART commands.

Unlike -kernel, this exercises the ROM's image parsing and SRAM loading.
An emulator result never establishes real-board validation.
"""
from pathlib import Path
import argparse
import json
import selectors
import subprocess
import time
from esp32c3 import OUT, ROOT, check_bundle


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qemu", required=True)
    args = ap.parse_args()
    image = check_bundle().read_bytes()
    if len(image) > 2 * 1024 * 1024:
        raise ValueError("image exceeds minimum supported 2 MiB flash")
    flash = ROOT / "build/esp32-c3/qemu-flash.bin"
    flash.write_bytes(image + b"\xff" * (2 * 1024 * 1024 - len(image)))
    markers = [b"entry 0x40380000", b"A20OS MCU READY", b"preemption=irq-return",
               b"board=esp32-c3 arch=riscv32 mode=M", b"heap free=", b"hz=1000"]
    transcript = bytearray()
    log = ROOT / ".kernel-build/smoke/esp32-c3-rom.log"
    process = subprocess.Popen([args.qemu, "-M", "esp32c3", "-nographic", "-monitor", "none",
                                "-drive", f"file={flash},if=mtd,format=raw,snapshot=on"],
                               cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    sent = False
    deadline = time.monotonic() + 10
    try:
        while time.monotonic() < deadline and process.poll() is None:
            for key, _ in selector.select(.1):
                data = key.fileobj.read1(4096)
                if not data:
                    break
                transcript.extend(data)
            if b"A20OS MCU READY" in transcript and not sent:
                process.stdin.write(b"info\nheap\nticks\ntest\n")
                process.stdin.flush()
                sent = True
            if all(marker in transcript for marker in markers) and transcript.count(b"[MCU TEST] PASS") >= 2:
                break
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        selector.close()
        process.stdin.close()
        process.stdout.close()
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_bytes(transcript)
    ok = (all(marker in transcript for marker in markers) and
          transcript.count(b"[MCU TEST] PASS") >= 2 and
          b"PANIC" not in transcript and b"TRAP cause=" not in transcript)
    report = {"passed": ok, "kind": "esp32c3-qemu-rom", "hardware_validated": False,
              "qemu_version": subprocess.check_output([args.qemu, "--version"], text=True).splitlines()[0],
              "image": str(OUT.relative_to(ROOT) / "a20os.bin"), "log": str(log.relative_to(ROOT))}
    log.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"ESP32-C3 ROM: {'PASS' if ok else 'FAIL'}; {log}")
    if not ok:
        print(transcript.decode(errors="replace"))
    return 0 if ok else 1

if __name__ == "__main__":
    raise SystemExit(main())
