#!/usr/bin/env python3
"""ESP32-C3 ROM image packaging and explicit serial programming.

No SDK is linked. esptool is an optional host dependency installed in a local
venv by `a20 prepare`; ordinary kernel builds need neither it nor the network.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import platform
import tarfile
import urllib.request
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import venv

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build/esp32-c3/handoff"
PYTHON = ROOT / "build/esp32-c3/venv/bin/python"
ESPTOOL_VERSION = "4.8.1"
IRAM = (0x40380000, 0x403A0000)
DRAM = (0x3FCA0000, 0x3FCDC000)


def elf_segments(path: Path) -> tuple[int, list[tuple[int, bytes, int]]]:
    data = path.read_bytes()
    if len(data) < 52 or data[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("expected ELF32 little-endian")
    h = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    if h[1] != 2 or h[2] != 243 or h[7] & 14:
        raise ValueError("expected executable RISC-V soft-float ELF")
    entry, phoff, phsize, phnum = h[4], h[5], h[9], h[10]
    if phsize != 32 or phoff + phsize * phnum > len(data):
        raise ValueError("invalid ELF program headers")
    segments = []
    for i in range(phnum):
        typ, offset, va, pa, size, memsize, flags, _ = struct.unpack_from("<IIIIIIII", data, phoff + i * phsize)
        if typ != 1 or not memsize:
            continue
        if size > memsize or offset + size > len(data) or va != pa:
            raise ValueError("invalid ELF load segment")
        region = next((r for r in (IRAM, DRAM) if r[0] <= pa and pa + memsize <= r[1]), None)
        if region is None or (flags & 1 and region != IRAM):
            raise ValueError("ELF exceeds disjoint C3 SRAM/ROM-reserved budgets")
        if any(pa < p + n and p < pa + memsize for p, _, n in segments):
            raise ValueError("overlapping ELF load segments")
        segments.append((pa, data[offset:offset + size], memsize))
    if not any(p <= entry < p + len(b) and IRAM[0] <= p < IRAM[1] for p, b, _ in segments):
        raise ValueError("entry is outside loaded IRAM")
    return entry, segments


def verify_image(path: Path, elf: Path) -> dict:
    entry, expected = elf_segments(elf)
    data = path.read_bytes()
    if len(data) < 24 or data[0] != 0xE9 or struct.unpack_from("<I", data, 4)[0] != entry:
        raise ValueError("invalid ESP ROM image magic/entry")
    if struct.unpack_from("<H", data, 12)[0] != 5:
        raise ValueError("image chip ID is not ESP32-C3")
    loaded = []
    pos, checksum = 24, 0xEF
    for _ in range(data[1]):
        if pos + 8 > len(data): raise ValueError("truncated image segment")
        address, size = struct.unpack_from("<II", data, pos)
        pos += 8
        if pos + size > len(data): raise ValueError("truncated image payload")
        payload = data[pos:pos + size]
        for value in payload: checksum ^= value
        if size:
            if not any(a <= address and address + size <= b for a, b in (IRAM, DRAM)):
                raise ValueError("image segment outside SRAM budget")
            if any(address < a + len(b) and a < address + size for a, b in loaded):
                raise ValueError("overlapping image segments")
            loaded.append((address, payload))
        pos += size
    checksum_end = (pos + 16) & ~15
    if checksum_end > len(data) or data[checksum_end - 1] != checksum:
        raise ValueError("ROM image checksum mismatch")
    if any(data[pos:checksum_end - 1]):
        raise ValueError("invalid ROM image padding")
    if data[23] != 1 or data[checksum_end:checksum_end + 32] != hashlib.sha256(data[:checksum_end]).digest():
        raise ValueError("ROM image SHA256 mismatch")
    if checksum_end + 32 != len(data): raise ValueError("unexpected bytes after image hash")
    for address, payload, _ in expected:
        # esptool may split a load segment by ELF sections or merge adjacent ones.
        for offset, value in enumerate(payload):
            at = address + offset
            if not any(a <= at < a + len(b) and b[at - a] == value for a, b in loaded):
                raise ValueError(f"image differs from ELF at 0x{at:x}")
    return {"entry": entry, "segments": [{"address": a, "bytes": len(b)} for a, b in loaded]}


def esptool(*args: str) -> None:
    if not PYTHON.is_file(): raise ValueError("run tools/a20 prepare esp32-c3 first")
    subprocess.run([str(PYTHON), "-m", "esptool", "--chip", "esp32c3", *args], check=True, cwd=ROOT)


def package(elf: Path) -> None:
    elf_segments(elf)
    OUT.mkdir(parents=True, exist_ok=True)
    image = OUT / "a20os.bin"
    temporary = OUT / "a20os.bin.tmp"
    esptool("elf2image", "--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "2MB", "-o", str(temporary), str(elf))
    record = verify_image(temporary, elf)
    temporary.replace(image)
    shutil.copyfile(elf, OUT / "kernel.elf")
    shutil.copyfile(ROOT / "docs/platforms/esp32-c3.md", OUT / "README.md")
    record.update(format="a20-esp32c3-rom-v1", flash_offset=0,
                  hardware_validated=False, esptool=ESPTOOL_VERSION,
                  sha256=hashlib.sha256(image.read_bytes()).hexdigest(),
                  elf_sha256=hashlib.sha256(elf.read_bytes()).hexdigest())
    (OUT / "image.json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"ESP32-C3 ROM image verified: {image}")


def check_bundle() -> Path:
    image = OUT / "a20os.bin"
    record = json.loads((OUT / "image.json").read_text())
    if record.get("format") != "a20-esp32c3-rom-v1" or record.get("flash_offset") != 0:
        raise ValueError("unexpected image manifest")
    if hashlib.sha256(image.read_bytes()).hexdigest() != record.get("sha256"):
        raise ValueError("image hash differs from manifest")
    if hashlib.sha256((OUT / "kernel.elf").read_bytes()).hexdigest() != record.get("elf_sha256"):
        raise ValueError("ELF hash differs from manifest")
    verify_image(image, OUT / "kernel.elf")
    return image


def prepare_qemu() -> None:
    if platform.system() != "Linux" or platform.machine() not in ("x86_64", "AMD64"):
        raise ValueError("automatic QEMU preparation currently supports Linux x86_64; set ESP32C3_QEMU for another host")
    url = ("https://github.com/espressif/qemu/releases/download/esp-develop-9.2.2-20260417/"
           "qemu-riscv32-softmmu-esp_develop_9.2.2_20260417-x86_64-linux-gnu.tar.xz")
    digest = "547f03e04701a92cbb699f7f7d015adc1f5b5ef93cbb94c0dd9b7107e2d84e77"
    archive = ROOT / "build/esp32-c3/qemu.tar.xz"
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not hasattr(tarfile, "data_filter"):
        raise ValueError("safe QEMU archive extraction needs Python 3.11.8+ or 3.12+")
    if not archive.is_file() or hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
        temporary = archive.with_suffix(".tmp")
        with urllib.request.urlopen(url, timeout=30) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != digest:
            temporary.unlink()
            raise ValueError("Espressif QEMU archive SHA256 mismatch")
        temporary.replace(archive)
    destination = archive.parent / "qemu"
    destination.mkdir(exist_ok=True)
    with tarfile.open(archive) as bundle:
        # Python 3.11.8+ supplies the data filter, blocking archive escapes.
        bundle.extractall(destination, filter="data")
    print(f"Espressif QEMU prepared in {destination}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("action", choices=("prepare", "package", "verify", "flash", "reset", "prepare-qemu"))
    ap.add_argument("--elf", type=Path)
    ap.add_argument("--port", default="")
    ap.add_argument("--baud", type=int, default=460800)
    args = ap.parse_args()
    try:
        if args.action == "prepare-qemu":
            prepare_qemu()
        elif args.action == "prepare":
            if not PYTHON.is_file(): venv.create(PYTHON.parent.parent, with_pip=True)
            subprocess.run([str(PYTHON), "-m", "pip", "install", f"esptool=={ESPTOOL_VERSION}"], check=True)
        elif args.action == "package":
            if not args.elf: raise ValueError("package requires --elf")
            package(args.elf)
        elif args.action == "verify":
            print(f"verified {check_bundle()}")
        elif args.action == "flash":
            if not args.port or args.port == "auto": raise ValueError("set ESPTOOL_PORT to an explicit serial device")
            image = check_bundle()
            # esptool checks chip identity and detected flash size before writing.
            esptool("--port", args.port, "--baud", str(args.baud), "write_flash", "--flash_size", "detect", "0x0", str(image))
        elif args.action == "reset":
            if not args.port or args.port == "auto": raise ValueError("reset needs an explicit serial device")
            if Path(sys.prefix).resolve() != PYTHON.parent.parent.resolve():
                os.execv(str(PYTHON), [str(PYTHON), __file__, "reset", "--port", args.port])
            import serial
            from esptool.reset import HardReset
            with serial.Serial(args.port, 115200) as port:
                HardReset(port)()
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"esp32-c3: {exc}", file=sys.stderr)
        return 1

if __name__ == "__main__":
    raise SystemExit(main())
