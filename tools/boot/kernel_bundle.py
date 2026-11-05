#!/usr/bin/env python3
"""Package a bootable Linux-Image-protocol kernel and its actual handoff recipe.

This checks the image protocol, not Linux ABI support: A20OS provides its own
kernel behind that standard firmware handoff. Other protocols need a board
specific packager instead of an invented booti command.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import tomllib
from pathlib import Path


def image_info(elf: Path, kernel: Path) -> tuple[int, int]:
    data = elf.read_bytes()
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01":
        raise ValueError("expected a little-endian ELF64 kernel")
    machine = struct.unpack_from("<H", data, 18)[0]
    phoff = struct.unpack_from("<Q", data, 32)[0]
    phsize, phnum = struct.unpack_from("<HH", data, 54)
    if phsize < 56 or phoff + phsize * phnum > len(data):
        raise ValueError("invalid ELF program header table")
    loads = []
    ends = []
    file_backed = False
    for i in range(phnum):
        p = phoff + i * phsize
        if struct.unpack_from("<I", data, p)[0] == 1:
            offset = struct.unpack_from("<Q", data, p + 8)[0]
            pa, size, memory_size = struct.unpack_from("<QQQ", data, p + 24)
            if size > memory_size or offset + size > len(data) or pa + memory_size >= 1 << 64:
                raise ValueError("invalid or truncated ELF load segment")
            if memory_size:
                loads.append(pa)
                ends.append(pa + memory_size)
                file_backed |= bool(size)
    if not loads or not file_backed:
        raise ValueError("ELF has no file-backed PT_LOAD")
    with kernel.open("rb") as f:
        header = f.read(64)
    if len(header) < 64:
        raise ValueError("kernel has no 64-byte Linux Image header")
    if machine == 183:
        valid = header[56:60] == b"ARM\x64"
    elif machine == 243:
        valid = header[48:56] == b"RISCV\x00\x00\x00" and header[56:60] == b"RSC\x05"
    else:
        raise ValueError(f"ELF machine {machine} has no supported booti protocol")
    if not valid:
        raise ValueError("kernel lacks the Linux Image magic required by booti; "
                         "enable a board Image header or use a board-specific loader")
    image_size = struct.unpack_from("<Q", header, 16)[0]
    if image_size < max(kernel.stat().st_size, max(ends) - min(loads)):
        raise ValueError("Image size omits part of the ELF memory footprint (including BSS)")
    return min(loads), image_size


def package(elf: Path, kernel: Path, out: Path, board: str,
            instance: Path | None = None) -> None:
    load, image_size = image_info(elf, kernel)
    commands: list[str] = []
    note = ""
    instance_record = None
    if instance is not None:
        instance_bytes = instance.read_bytes()
        config = tomllib.loads(instance_bytes.decode("utf-8"))
        instance_record = {"name": config.get("name", instance.stem),
                           "sha256": hashlib.sha256(instance_bytes).hexdigest()}
        if config.get("board") != board:
            raise ValueError("handoff instance and packaged board disagree")
        handoff = config.get("handoff", {})
        commands = handoff.get("commands", [])
        note = handoff.get("note", "")
    out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(kernel, out / "kernel.bin")
    shutil.copyfile(elf, out / "kernel.elf")
    preamble = (f"# {board}: UNVERIFIED ON PHYSICAL HARDWARE\n"
                f"# Reserve the whole Image range 0x{load:x}..0x{load + image_size:x}.\n"
                "# Run at the U-Boot prompt; stop on any failed load/DTB check.\n"
                f"setenv loadaddr 0x{load:x}\n")
    if any("${kernel_addr_r}" in c for c in commands):
        preamble += f"setenv kernel_addr_r 0x{load:x}\n"
    if commands:
        script = preamble + "\n".join(commands) + "\n"
    else:
        script = preamble + ("# No instance recipe supplied. Load kernel.bin here and a matching DTB\n"
                             "# outside the reserved Image range. Then use:\n"
                             "# booti ${loadaddr} - ${fdt_addr_r}\n")
    (out / "uboot.cmd").write_text(script)
    (out / "README.md").write_text(
        f"# {board} handoff bundle\n\n"
        "Hardware validation is pending. This is a kernel payload, not a complete\n"
        "bootable SD image. Preserve the board's working SPL/DRAM firmware and\n"
        "U-Boot/OpenSBI/TF-A chain; arrange FAT or TFTP transfer as its platform\n"
        "guide describes. No bootloader flash is changed by packaging.\n\n"
        "`uboot.cmd` is text to inspect and paste, not a binary U-Boot source script.\n"
        "Its commands come from the selected instance. A DTB must be the actual\n"
        "matching board DTB; the kernel's load address is never a DTB pointer.\n"
        f"Reserve 0x{load:x}..0x{load + image_size:x}, including BSS, for the kernel.\n"
        "Stop after failed loads. Capture the complete serial log; building and\n"
        "checking the Image header do not prove that hardware starts.\n\n" + note + "\n")
    files = {p.name: {"size": p.stat().st_size,
                      "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
             for p in (out / "kernel.bin", out / "kernel.elf", out / "uboot.cmd")}
    (out / "manifest.json").write_text(json.dumps(
        {"board": board, "instance": instance_record,
         "load_address": hex(load), "image_size": image_size,
         "hardware_validated": False, "files": files}, indent=2) + "\n")
    print(f"handoff bundle: {out} (load 0x{load:x}, Image footprint {image_size} bytes)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("elf", type=Path)
    ap.add_argument("kernel", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("board")
    ap.add_argument("instance", nargs="?", default="")
    a = ap.parse_args()
    try:
        package(a.elf, a.kernel, a.out, a.board, Path(a.instance) if a.instance else None)
    except (OSError, ValueError, struct.error) as exc:
        ap.exit(1, f"kernel-bundle: {exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
