#!/usr/bin/env python3
"""Copy A20's application UF2 to an identified factory M0SENSE USB volume.

Never opens a block device or calls a raw flash programmer. A copy is an
upload attempt; only the LED/UART checks in the platform guide confirm boot.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys

FAMILY = 0x11DE784A


def check_uf2(data: bytes) -> None:
    if not data or len(data) % 512 or len(data) > 128 * 1024:
        raise ValueError("invalid application UF2 size")
    count = len(data) // 512
    for index in range(count):
        b = data[index * 512:(index + 1) * 512]
        expected = (0x0A324655, 0x9E5D5157, 0x2001, index * 256, 256,
                    index, count, FAMILY)
        if struct.unpack_from("<8I", b) != expected or struct.unpack_from("<I", b, 508)[0] != 0x0AB16F30:
            raise ValueError("not the supported factory M0SENSE application UF2")
        if any(b[288:508]):
            raise ValueError("unexpected UF2 extension data")


def mounted_fat_volumes() -> list[Path]:
    """Linux mountinfo is authoritative; macOS exposes mounted volumes here."""
    if sys.platform == "darwin":
        return [p.resolve() for p in Path("/Volumes").iterdir() if p.is_dir() and os.path.ismount(p)]
    result = []
    def unescape(s):
        return re.sub(r"\\([0-7]{3})", lambda m: chr(int(m[1], 8)), s)
    try:
        lines = Path("/proc/self/mountinfo").read_text().splitlines()
    except OSError:
        return result
    for line in lines:
        before, sep, after = line.partition(" - ")
        if not sep or after.split()[0] not in ("vfat", "msdos"):
            continue
        fields = before.split()
        if len(fields) >= 6 and "rw" in fields[5].split(","):
            result.append(Path(unescape(fields[4])).resolve())
    return result


def identify_volume(root: Path, mounted: list[Path]) -> Path:
    if root.is_symlink():
        raise ValueError("UF2 volume path must not be a symlink")
    root = root.resolve(strict=True)
    if root not in mounted or not root.is_dir():
        raise ValueError("destination must be the root of a mounted writable FAT USB volume")
    candidates = [p for p in root.iterdir() if p.name.lower() == "info_uf2.txt"]
    if len(candidates) != 1 or candidates[0].is_symlink():
        raise ValueError("missing trustworthy INFO_UF2.TXT")
    info_path = candidates[0]
    with info_path.open("rb") as f:
        info = f.read(4097)
    if len(info) > 4096:
        raise ValueError("oversized INFO_UF2.TXT")
    text = info.decode("utf-8", errors="strict")
    fields = {}
    for line in text.splitlines():
        if ":" in line:
            name, value = line.split(":", 1)
            fields[name.strip().lower()] = value.strip().lower()
    if fields.get("board-id") != "m0sense-bl702" or fields.get("model") != "sipeed m0sense bl702":
        raise ValueError("UF2 identity must be Board-ID m0sense-bl702 / Model Sipeed M0SENSE BL702")
    if "uf2 bootloader" not in text.lower():
        raise ValueError("missing factory UF2 bootloader identity")
    return root


def find_volume(requested: Path | None, mounted: list[Path]) -> Path:
    if requested is not None:
        return identify_volume(requested, mounted)
    found = []
    for root in mounted:
        try:
            found.append(identify_volume(root, mounted))
        except (OSError, ValueError, UnicodeError):
            pass
    if len(found) != 1:
        raise ValueError(f"found {len(found)} identified M0SENSE volumes; hold BOOT while resetting, or pass --mount")
    return found[0]


def deploy(image: Path, metadata: Path, mount: Path | None, dry_run: bool) -> dict:
    data = image.read_bytes()
    check_uf2(data)
    record = json.loads(metadata.read_text())
    digest = hashlib.sha256(data).hexdigest()
    if (record.get("format") != "m0sense-factory-uf2-application-v1" or
        record.get("uf2_sha256") != digest or record.get("raw_flash_image") is not False or
        record.get("factory_uf2_bootloader_required") is not True):
        raise ValueError("UF2 metadata/sha256 does not match the application bundle")
    mounted = mounted_fat_volumes()
    if dry_run and mount is None:
        print(f"DRY RUN: verify factory M0SENSE volume and copy {image} ({len(data)} bytes).")
        print("Hold software BOOT (GPIO2) while pressing RESET, release BOOT after the M0SENSE volume appears.")
        print("Require INFO_UF2.TXT: Board-ID m0sense-bl702, Model Sipeed M0SENSE BL702.")
        print("Only the application is uploaded through the existing bootloader; no raw flash write.")
        return {"uploaded": False, "dry_run": True, "sha256": digest}
    root = find_volume(mount, mounted)
    destination = root / "A20PROBE.UF2"
    if destination.exists() or destination.is_symlink():
        raise ValueError("A20PROBE.UF2 already exists; reconnect the factory bootloader volume before retrying")
    print(f"{'DRY RUN: ' if dry_run else ''}M0SENSE application upload: {image} -> {destination}")
    if not dry_run:
        # EXCL+NOFOLLOW protects a preexisting host file. The loader may detach
        # after its final UF2 block; an I/O failure is reported, never called PASS.
        fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o644)
        with os.fdopen(fd, "wb", buffering=0) as f:
            written = f.write(data)
            if written != len(data):
                raise OSError("short UF2 write")
            os.fsync(f.fileno())
        print("Application upload completed. Confirm blue LED heartbeat / UART PROBE READY; USB copy does not prove boot.")
    return {"uploaded": not dry_run, "dry_run": dry_run, "sha256": digest, "mount": str(root)}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    default_root = Path(__file__).resolve().parent
    # Works both from the source tree and after copying into the handoff bundle.
    if not (default_root / "probe.uf2").exists():
        default_root = default_root.parent / "build/m0sense-bl702/handoff"
    ap.add_argument("--image", type=Path, default=default_root / "probe.uf2")
    ap.add_argument("--metadata", type=Path, default=default_root / "probe.json")
    ap.add_argument("--mount", type=Path)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    try:
        deploy(args.image, args.metadata, args.mount, args.dry_run)
    except (OSError, ValueError, UnicodeError, struct.error) as error:
        ap.exit(1, f"M0SENSE upload refused/failed: {error}\n")


if __name__ == "__main__":
    main()
