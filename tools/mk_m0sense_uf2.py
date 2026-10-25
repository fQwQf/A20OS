#!/usr/bin/env python3
"""Package a raw BL702 XIP application for the FACTORY M0SENSE UF2 loader.

Matches Sipeed/M0sense_BL702_example at 97156083b1ee3a94b7808bfaafd8a28ec78ed466:
misc/utils/uf2_conv.c and libs/uf2_format/uf2_format.h. The zero-based target
addresses are application-relative, not a request to overwrite flash offset 0.
This file deliberately does not produce a ROM bootheader or flash erase image.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

MAGIC0, MAGIC1, MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
FLAGS, FAMILY, PAYLOAD = 0x2001, 0x11DE784A, 256
XIP_BASE, MAX_APP = 0x23000000, 64 * 1024
SOURCE = "https://github.com/sipeed/M0sense_BL702_example/tree/97156083b1ee3a94b7808bfaafd8a28ec78ed466"


def validate_application(elf: bytes, image: bytes) -> None:
    if len(elf) < 52 or elf[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("requires little-endian ELF32")
    kind, machine, version, entry, phoff = struct.unpack_from("<HHIII", elf, 16)
    phsize, phnum = struct.unpack_from("<HH", elf, 42)
    flags = struct.unpack_from("<I", elf, 36)[0]
    if (kind, machine, version, entry) != (2, 243, 1, XIP_BASE) or flags & 6:
        raise ValueError("requires executable RV32 soft-float ELF at 0x23000000")
    if not 0 < len(image) <= MAX_APP or phsize != 32 or phnum == 0 or phnum > 32:
        raise ValueError("invalid application size or program-header table")
    if phoff < 52 or phoff + phsize * phnum > len(elf):
        raise ValueError("truncated program-header table")
    expected = bytearray(len(image))
    used: list[tuple[int, int]] = []
    boot_seen = False
    code_seen = False
    for index in range(phnum):
        typ, off, va, pa, filesz, memsz, perm, align = struct.unpack_from("<8I", elf, phoff + index * phsize)
        if typ != 1:
            continue
        if filesz > memsz or off > len(elf) or filesz > len(elf) - off:
            raise ValueError("invalid ELF load segment")
        regions = ((XIP_BASE, XIP_BASE + MAX_APP), (0x22010000, 0x22020000),
                   (0x42020000, 0x42024000))
        if not any(lo <= va and va + memsz <= hi for lo, hi in regions):
            raise ValueError("ELF virtual segment lies outside the probe memory map")
        if perm & 2 and perm & 1:
            raise ValueError("writable executable ELF segment")
        if filesz:
            first, last = pa - XIP_BASE, pa - XIP_BASE + filesz
            if first < 0 or last > len(image):
                raise ValueError("ELF load bytes lie outside the application binary")
            if any(first < end and begin < last for begin, end in used):
                raise ValueError("overlapping ELF load bytes")
            expected[first:last] = elf[off:off + filesz]
            used.append((first, last))
            boot_seen |= va == pa == XIP_BASE and bool(perm & 1)
            code_seen |= va == 0x22010000 and bool(perm & 1)
    if not boot_seen or not code_seen or bytes(expected) != image:
        raise ValueError("binary does not match the XIP-copy probe ELF")


def encode_uf2(image: bytes) -> bytes:
    if not 0 < len(image) <= MAX_APP:
        raise ValueError("application must contain 1..65536 bytes")
    count = (len(image) + PAYLOAD - 1) // PAYLOAD
    blocks = []
    for index in range(count):
        data = image[index * PAYLOAD:(index + 1) * PAYLOAD].ljust(476, b"\0")
        blocks.append(struct.pack("<8I", MAGIC0, MAGIC1, FLAGS, index * PAYLOAD,
                                  PAYLOAD, index, count, FAMILY) + data + struct.pack("<I", MAGIC_END))
    return b"".join(blocks)


def decode_uf2(uf2: bytes) -> bytes:
    if not uf2 or len(uf2) % 512 or len(uf2) > MAX_APP * 2:
        raise ValueError("invalid M0Sense UF2 length")
    count = len(uf2) // 512
    image = bytearray()
    for index in range(count):
        b = uf2[index * 512:(index + 1) * 512]
        head = struct.unpack_from("<8I", b)
        expected = (MAGIC0, MAGIC1, FLAGS, index * PAYLOAD, PAYLOAD, index, count, FAMILY)
        if head != expected or struct.unpack_from("<I", b, 508)[0] != MAGIC_END or any(b[288:508]):
            raise ValueError("wrong UF2 magic/family/flags/order/address/padding")
        image.extend(b[32:288])
    return bytes(image)


def package(elf_path: Path, bin_path: Path, out: Path) -> dict:
    elf, image = elf_path.read_bytes(), bin_path.read_bytes()
    validate_application(elf, image)
    uf2 = encode_uf2(image)
    if decode_uf2(uf2) != image.ljust((len(image) + 255) // 256 * 256, b"\0"):
        raise ValueError("UF2 round trip failed")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(uf2)
    record = {
        "format": "m0sense-factory-uf2-application-v1", "hardware_validated": False,
        "factory_uf2_bootloader_required": True, "raw_flash_image": False,
        "family_id": hex(FAMILY), "uf2_flags": hex(FLAGS), "xip_entry": hex(XIP_BASE),
        "target_addresses": "application-relative, starting at zero",
        "application_bytes": len(image), "uf2_blocks": len(uf2) // 512,
        "application_sha256": hashlib.sha256(image).hexdigest(),
        "uf2_sha256": hashlib.sha256(uf2).hexdigest(), "official_contract": SOURCE,
    }
    out.with_suffix(".json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"M0SENSE application UF2: {out} ({len(uf2) // 512} blocks, {len(image)} application bytes)")
    return record


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("elf", type=Path)
    ap.add_argument("binary", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()
    try:
        package(args.elf, args.binary, args.output)
    except (OSError, ValueError, struct.error) as error:
        ap.exit(1, f"M0SENSE UF2: {error}\n")


if __name__ == "__main__":
    main()
