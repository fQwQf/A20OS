#!/usr/bin/env python3
"""Build and verify a Hi3861 kernel-A firmware image.

The HiSilicon FlashBoot accepts a standard "hupg" kernel file in the kernel A
partition at flash offset 0xd000.  Its 0x3c0-byte header is followed by a flat
RISC-V image that is executed in place from SPI flash.  This tool only produces
that kernel file; the factory ROM/FlashBoot and the serial programmer remain the
vendor's responsibility.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
OUT = ROOT / "build/hi3861/handoff"

FLASH_BASE = 0x00400000
KERNEL_PARTITION = 0x0000D000
KERNEL_PARTITION_SIZE = 0x000E4000
IMAGE_BASE = 0x0040D3C0
IMAGE_HEADER_SIZE = 0x3C0
RAM_BASE = 0x000D8000
RAM_END = 0x0011A000
FLASH_END = FLASH_BASE + KERNEL_PARTITION + KERNEL_PARTITION_SIZE
MAX_PAYLOAD = KERNEL_PARTITION_SIZE - IMAGE_HEADER_SIZE

IMAGE_ID = 0x3C78961E
FILE_TYPE_KERNEL = 0xF0
UNENCRYPTED = 0x42
FILE_ATTR_KERNELA_UNENCRYPTED = 0x44
SIGN_ALG_SHA256 = 0x3F
COMMON_HEAD_SIZE = 96
KEY_SIZE = 288
SIGN_SIZE = 256
SECTION_HEAD_SIZE = 64
SECTION_OFFSET = COMMON_HEAD_SIZE + KEY_SIZE + SIGN_SIZE       # 0x280
SECTION_SIGN_OFFSET = SECTION_OFFSET + SECTION_HEAD_SIZE        # 0x2c0
PAYLOAD_OFFSET = SECTION_SIGN_OFFSET + SIGN_SIZE                # 0x3c0


def elf_load_segments(path: Path) -> tuple[int, list[tuple[int, int, bytes, int, int]]]:
    """Return (entry, [(paddr, vaddr, file bytes, memsz, flags)])."""
    data = path.read_bytes()
    if len(data) < 52 or data[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("expected ELF32 little-endian")
    h = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    e_type, e_machine, e_version, entry = h[1], h[2], h[3], h[4]
    phoff, phsize, phnum = h[5], h[9], h[10]
    e_flags = h[7]
    if e_type != 2 or e_machine != 243 or e_version != 1:
        raise ValueError("expected an executable RISC-V ELF")
    if e_flags & 0x0E:
        raise ValueError("expected the RV32 soft-float ABI")
    if phsize != 32 or phoff + phsize * phnum > len(data):
        raise ValueError("invalid ELF program headers")
    segments: list[tuple[int, int, bytes, int, int]] = []
    for i in range(phnum):
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, _ = \
            struct.unpack_from("<IIIIIIII", data, phoff + i * phsize)
        if p_type != 1 or (not p_memsz and not p_filesz):
            continue
        if p_filesz > p_memsz or p_offset + p_filesz > len(data):
            raise ValueError("invalid ELF load segment")
        payload = data[p_offset:p_offset + p_filesz]
        segments.append((p_paddr, p_vaddr, payload, p_memsz, p_flags))
    if not segments:
        raise ValueError("ELF has no loadable segments")
    return entry, segments


def flat_image(path: Path) -> bytes:
    """Rebuild the flash payload from ELF LMAs, without relying on objcopy."""
    entry, segments = elf_load_segments(path)
    if entry != IMAGE_BASE:
        raise ValueError(f"entry is 0x{entry:x}, expected 0x{IMAGE_BASE:x}")
    loadable = [s for s in segments if s[2]]
    if not loadable:
        raise ValueError("ELF has no file-backed load segment")
    base = min(s[0] for s in loadable)
    end = max(s[0] + len(s[2]) for s in loadable)
    if base != IMAGE_BASE:
        raise ValueError(f"lowest LMA is 0x{base:x}, expected 0x{IMAGE_BASE:x}")
    if end > FLASH_END:
        raise ValueError(f"payload ends at 0x{end:x}, past kernel partition 0x{FLASH_END:x}")
    if end - base > MAX_PAYLOAD:
        raise ValueError(f"payload is too large for kernel A ({end - base} > {MAX_PAYLOAD})")
    image = bytearray(end - base)
    occupied: list[tuple[int, int]] = []
    for paddr, vaddr, payload, memsz, flags in loadable:
        lo, hi = paddr, paddr + len(payload)
        if any(lo < b and a < hi for a, b in occupied):
            raise ValueError("overlapping ELF load segments")
        occupied.append((lo, hi))
        image[lo - base:hi - base] = payload
        if not (FLASH_BASE <= paddr and hi <= FLASH_END):
            raise ValueError(f"load segment 0x{paddr:x} is outside the kernel partition")
        if vaddr < RAM_BASE or vaddr + memsz > RAM_END:
            if not (FLASH_BASE <= vaddr and vaddr + memsz <= FLASH_END):
                raise ValueError(f"segment 0x{vaddr:x} is outside the flash/RAM budget")
    return bytes(image)


def build_hupg(payload: bytes) -> bytes:
    """Wrap a flat payload in the unsecure SHA-256 hupg format."""
    if not payload:
        raise ValueError("refusing to build an empty kernel image")
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("kernel payload exceeds the 912 KiB kernel A partition")
    total_without_pad = PAYLOAD_OFFSET + len(payload)
    padding = (-total_without_pad) & 15
    file_len = total_without_pad + padding
    section_len = file_len - SECTION_OFFSET

    common = bytearray(COMMON_HEAD_SIZE)
    struct.pack_into("<IIII", common, 0, IMAGE_ID, 0, SECTION_OFFSET, section_len)
    common[16:24] = b"Hi3861\0\0"  # same chip-product field as the vendor tool
    common[48:52] = bytes((FILE_TYPE_KERNEL, 0, UNENCRYPTED,
                           FILE_ATTR_KERNELA_UNENCRYPTED))
    struct.pack_into("<I", common, 52, file_len)
    struct.pack_into("<I", common, 56, KEY_SIZE)
    struct.pack_into("<I", common, 60, SIGN_ALG_SHA256 << 16)

    key = bytes(KEY_SIZE)
    common_sign = hashlib.sha256(bytes(common) + key).digest() + bytes(SIGN_SIZE - 32)

    section = bytearray(SECTION_HEAD_SIZE)
    struct.pack_into("<III", section, 0, IMAGE_ID, 0, SIGN_ALG_SHA256 << 16)
    section[12] = 1
    struct.pack_into("<I", section, 44, PAYLOAD_OFFSET)
    struct.pack_into("<I", section, 48, len(payload))
    section_sign = hashlib.sha256(bytes(section) + payload + bytes(padding)).digest() + \
        bytes(SIGN_SIZE - 32)

    image = (bytes(common) + key + common_sign + bytes(section) + section_sign +
             payload + bytes(padding))
    if len(image) != file_len:
        raise AssertionError("hupg length accounting failed")
    return image


def verify_hupg(image: bytes, payload: bytes) -> dict:
    if len(image) < PAYLOAD_OFFSET or len(image) != struct.unpack_from("<I", image, 52)[0]:
        raise ValueError("hupg header/file length mismatch")
    image_id, _, section_offset, section_len = struct.unpack_from("<IIII", image, 0)
    file_type, encrypt_flag, file_attr = image[48], image[50], image[51]
    key_len, param = struct.unpack_from("<II", image, 56)
    if image_id != IMAGE_ID or file_type != FILE_TYPE_KERNEL:
        raise ValueError("unexpected hupg image id or file type")
    if encrypt_flag != UNENCRYPTED or file_attr != FILE_ATTR_KERNELA_UNENCRYPTED:
        raise ValueError("only unencrypted kernel A images are supported")
    if key_len != KEY_SIZE or param != SIGN_ALG_SHA256 << 16:
        raise ValueError("unexpected hupg key or signature algorithm")
    if section_offset != SECTION_OFFSET or section_len != len(image) - SECTION_OFFSET:
        raise ValueError("unexpected hupg section layout")
    common_sign = image[COMMON_HEAD_SIZE + KEY_SIZE:COMMON_HEAD_SIZE + KEY_SIZE + 32]
    if hashlib.sha256(image[:COMMON_HEAD_SIZE + KEY_SIZE]).digest() != common_sign:
        raise ValueError("hupg common-header SHA256 mismatch")
    if any(image[COMMON_HEAD_SIZE + KEY_SIZE + 32:SECTION_OFFSET]):
        raise ValueError("nonzero hupg common-signature padding")
    sec = image[SECTION_OFFSET:SECTION_SIGN_OFFSET]
    sig = image[SECTION_SIGN_OFFSET:SECTION_SIGN_OFFSET + 32]
    if hashlib.sha256(sec + image[PAYLOAD_OFFSET:]).digest() != sig:
        raise ValueError("hupg section SHA256 mismatch")
    if any(image[SECTION_SIGN_OFFSET + 32:PAYLOAD_OFFSET]):
        raise ValueError("nonzero hupg section-signature padding")
    section_id, _, section_param, count = struct.unpack_from("<IIIB", sec, 0)
    sec0_offset, sec0_len = struct.unpack_from("<II", sec, 44)
    if section_id != IMAGE_ID or section_param != SIGN_ALG_SHA256 << 16 or count != 1:
        raise ValueError("unexpected hupg section header")
    if sec0_offset != PAYLOAD_OFFSET or sec0_len != len(payload):
        raise ValueError("hupg section does not describe the flat payload")
    if image[PAYLOAD_OFFSET:PAYLOAD_OFFSET + len(payload)] != payload:
        raise ValueError("hupg payload differs from the ELF image")
    if any(image[PAYLOAD_OFFSET + len(payload):]):
        raise ValueError("nonzero hupg padding")
    return {"section_offset": section_offset, "payload_offset": PAYLOAD_OFFSET,
            "payload_bytes": len(payload), "file_bytes": len(image)}


def package(elf: Path) -> None:
    payload = flat_image(elf)
    image = build_hupg(payload)
    verify_hupg(image, payload)

    OUT.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(elf, OUT / "kernel.elf")
    (OUT / "kernel-flat.bin").write_bytes(payload)
    (OUT / "a20os.hupg").write_bytes(image)
    shutil.copyfile(ROOT / "docs/platforms/hi3861.md", OUT / "README.md")
    record = {
        "format": "a20-hi3861-hupg-v1",
        "chip": "Hi3861V100",
        "partition": "kernel A",
        "flash_base": f"0x{FLASH_BASE:08x}",
        "partition_offset": f"0x{KERNEL_PARTITION:05x}",
        "image_header_bytes": IMAGE_HEADER_SIZE,
        "payload_offset": f"0x{IMAGE_BASE:08x}",
        "payload_bytes": len(payload),
        "file_bytes": len(image),
        "max_payload_bytes": MAX_PAYLOAD,
        "sha256": hashlib.sha256(image).hexdigest(),
        "payload_sha256": hashlib.sha256(payload).hexdigest(),
        "elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(),
        "hardware_validated": False,
    }
    (OUT / "image.json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"Hi3861 kernel A image: {OUT / 'a20os.hupg'}")


def verify() -> Path:
    manifest_path = OUT / "image.json"
    record = json.loads(manifest_path.read_text())
    if record.get("format") != "a20-hi3861-hupg-v1":
        raise ValueError("unexpected Hi3861 image manifest")
    image = (OUT / "a20os.hupg").read_bytes()
    payload = (OUT / "kernel-flat.bin").read_bytes()
    elf = OUT / "kernel.elf"
    if len(payload) != record.get("payload_bytes") or len(image) != record.get("file_bytes"):
        raise ValueError("image lengths differ from the manifest")
    if hashlib.sha256(image).hexdigest() != record.get("sha256"):
        raise ValueError("hupg SHA256 differs from the manifest")
    if hashlib.sha256(payload).hexdigest() != record.get("payload_sha256"):
        raise ValueError("flat payload SHA256 differs from the manifest")
    if hashlib.sha256(elf.read_bytes()).hexdigest() != record.get("elf_sha256"):
        raise ValueError("ELF SHA256 differs from the manifest")
    if flat_image(elf) != payload:
        raise ValueError("flat payload differs from the ELF")
    verify_hupg(image, payload)
    return OUT / "a20os.hupg"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("action", choices=("package", "verify"))
    ap.add_argument("--elf", type=Path)
    args = ap.parse_args()
    try:
        if args.action == "package":
            if args.elf is None:
                raise ValueError("package requires --elf")
            package(args.elf)
        else:
            print(f"verified {verify()}")
        return 0
    except (OSError, ValueError) as exc:
        print(f"hi3861: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
