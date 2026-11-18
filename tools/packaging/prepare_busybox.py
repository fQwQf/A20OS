#!/usr/bin/env python3
"""Fetch a pinned static BusyBox without apk, a cross compiler, or target execution."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import tarfile
import tempfile
import urllib.request

LOCK = Path(__file__).with_name("busybox.lock.json")
MACHINES = {"x86_64": (2, 62), "aarch64": (2, 183), "riscv64": (2, 243),
            "loongarch64": (2, 258), "arm32": (1, 40), "riscv32": (1, 243),
            "ppc64le": (2, 21)}


def validate_elf(data: bytes, arch: str) -> None:
    """Reject wrong-ISA, malformed, or dynamically interpreted executables."""
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[5] != 1:
        raise ValueError("BusyBox must be a little-endian ELF executable")
    elf_class, machine = MACHINES[arch]
    if data[4] != elf_class or struct.unpack_from("<H", data, 18)[0] != machine:
        raise ValueError(f"BusyBox ELF architecture does not match {arch}")
    if struct.unpack_from("<H", data, 16)[0] not in (2, 3):
        raise ValueError("BusyBox must be ET_EXEC or static PIE")
    if elf_class == 2:
        phoff = struct.unpack_from("<Q", data, 32)[0]
        entsize, count = struct.unpack_from("<HH", data, 54)
        minimum = 56
    else:
        phoff = struct.unpack_from("<I", data, 28)[0]
        entsize, count = struct.unpack_from("<HH", data, 42)
        minimum = 32
    if not count or entsize < minimum or phoff + count * entsize > len(data):
        raise ValueError("invalid BusyBox ELF program headers")
    if any(struct.unpack_from("<I", data, phoff + i * entsize)[0] == 3
           for i in range(count)):
        raise ValueError("BusyBox requires a dynamic interpreter; provide a static build")


def write_changed(path: Path, data: bytes, mode: int = 0o755) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file() and path.read_bytes() == data:
        path.chmod(mode)
        return
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as out:
        temporary = Path(out.name)
        out.write(data)
    try:
        temporary.chmod(mode)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def prepare(arch: str, output: Path, cache: Path, mirror: str,
            binary: Path | None = None) -> None:
    if binary is not None:
        data = binary.read_bytes()
    else:
        lock = json.loads(LOCK.read_text())
        entry = lock["architectures"][arch]
        package = cache / arch / entry["package"]
        if not package.is_file():
            url = f"{mirror.rstrip('/')}/{lock['repository']}/{entry['alpine_arch']}/{entry['package']}"
            print(f"[BUSYBOX] downloading {url}", flush=True)
            with urllib.request.urlopen(url, timeout=60) as response:
                archive = response.read(16 * 1024 * 1024 + 1)
            if hashlib.sha256(archive).hexdigest() != entry["sha256"]:
                raise ValueError("BusyBox package SHA-256 mismatch")
            write_changed(package, archive, 0o644)
        archive = package.read_bytes()
        if hashlib.sha256(archive).hexdigest() != entry["sha256"]:
            raise ValueError(f"BusyBox cache SHA-256 mismatch: {package}")
        # Read exactly one regular member; never extract an archive into the host tree.
        with tarfile.open(package, "r:gz") as apk:
            member = apk.getmember("bin/busybox.static")
            if not member.isfile() or member.size > 16 * 1024 * 1024:
                raise ValueError("invalid BusyBox package payload")
            data = apk.extractfile(member).read()
            metadata = apk.extractfile(".PKGINFO").read()
        write_changed(output.with_suffix(".PKGINFO"), metadata, 0o644)
    validate_elf(data, arch)
    write_changed(output, data)
    print(f"[BUSYBOX] {arch}: {output} ({len(data)} bytes)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=MACHINES, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cache-dir", type=Path, default=Path("build/cache/busybox"))
    parser.add_argument("--mirror", default="https://dl-cdn.alpinelinux.org/alpine")
    parser.add_argument("--binary", type=Path, help="offline/custom static BusyBox (must include init and ash)")
    args = parser.parse_args()
    try:
        prepare(args.arch, args.output, args.cache_dir, args.mirror, args.binary)
    except (OSError, ValueError, KeyError, tarfile.TarError) as error:
        parser.exit(1, f"busybox: {error}\n")


if __name__ == "__main__":
    main()
