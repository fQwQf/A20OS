#!/usr/bin/env python3
"""Validate every ELF member in a RISC-V ILP32D libgcc archive."""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ar", required=True, help="matching GNU ar executable")
    parser.add_argument("archive", help="libgcc archive to inspect")
    args = parser.parse_args()

    try:
        members = subprocess.check_output(
            [args.ar, "t", args.archive], text=True, stderr=subprocess.DEVNULL
        ).splitlines()
        if not members:
            raise ValueError("archive has no members")
        for member in members:
            data = subprocess.check_output(
                [args.ar, "p", args.archive, member], stderr=subprocess.DEVNULL
            )
            if len(data) < 40 or data[:4] != b"\x7fELF":
                raise ValueError(f"{member}: member is not an ELF object")
            elf_class, endian = data[4], data[5]
            if elf_class != 1 or endian != 1:
                raise ValueError(f"{member}: expected little-endian ELF32")
            machine = struct.unpack_from("<H", data, 18)[0]
            flags = struct.unpack_from("<I", data, 36)[0]
            if machine != 243:
                raise ValueError(f"{member}: expected EM_RISCV (243), got {machine}")
            # EF_RISCV_FLOAT_ABI_DOUBLE is bit 2 in the ABI mask (bits 1..2).
            if flags & 0x6 != 0x4:
                raise ValueError(
                    f"{member}: expected double-float ABI flags (mask 0x6 == 0x4), "
                    f"got e_flags=0x{flags:x}"
                )
    except (OSError, subprocess.CalledProcessError, ValueError) as exc:
        print(f"RV32 libgcc ABI check failed: {exc}", file=sys.stderr)
        return 1

    print(f"RV32 libgcc ABI OK: {len(members)} ELF32 RISC-V ILP32D members")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
