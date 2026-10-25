#!/usr/bin/env python3
"""Package an LS2K0300 RAM image without pretending firmware ABI is verified."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import zlib


def inspect_elf(path):
    data = path.read_bytes()
    if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('expected little-endian ELF64')
    kind, machine = struct.unpack_from('<HH', data, 16)
    entry, phoff = struct.unpack_from('<QQ', data, 24)
    phentsize, phnum = struct.unpack_from('<HH', data, 54)
    if kind != 2 or machine != 258 or phentsize != 56:
        raise ValueError('expected a LoongArch ET_EXEC image')
    if not phnum or phoff + phnum * phentsize > len(data):
        raise ValueError('invalid program header table')
    segments = []
    for index in range(phnum):
        ptype, flags, offset, va, pa, filesz, memsz, align = struct.unpack_from(
            '<IIQQQQQQ', data, phoff + index * phentsize)
        # This vendor's lib/elf.c iterates every phdr without checking p_type.
        # Reject auxiliary/non-load headers rather than letting it copy them.
        if ptype != 1 or not memsz:
            raise ValueError('vendor bootelf requires every program header to be a nonempty PT_LOAD')
        if filesz > memsz or offset + filesz > len(data):
            raise ValueError('invalid load segment size')
        if not (0x9000000002000000 <= va < va + memsz <= 0x9000000008000000):
            raise ValueError('load segment outside the initial LS2K0300 window')
        if pa != va:
            raise ValueError('firmware requires identical cached DMW load and virtual addresses')
        segments.append(dict(vaddr=hex(va), paddr=hex(pa), filesz=filesz,
                             memsz=memsz, flags=flags, offset=offset))
    if entry != 0x9000000002000000 or not segments:
        raise ValueError('unexpected LS2K0300 entry/load map')
    ordered = sorted(segments, key=lambda seg: int(seg['vaddr'], 16))
    for first, second in zip(ordered, ordered[1:]):
        if int(first['vaddr'], 16) + first['memsz'] > int(second['vaddr'], 16):
            raise ValueError('overlapping ELF load segments')
    if not any(seg['flags'] & 1 and int(seg['vaddr'], 16) <= entry <
               int(seg['vaddr'], 16) + seg['filesz'] for seg in segments):
        raise ValueError('entry is not backed by executable file data')
    return dict(entry=hex(entry), segments=segments, elf_size=len(data),
                crc32=f'{zlib.crc32(data):08x}',
                sha256=hashlib.sha256(data).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('binary', type=Path)
    parser.add_argument('out', type=Path)
    args = parser.parse_args()
    try:
        metadata = inspect_elf(args.elf)
        if not args.binary.is_file():
            raise ValueError('missing raw kernel binary')
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))
    args.out.mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.elf, args.out / 'kernel.elf')
    shutil.copy2(args.binary, args.out / 'kernel.bin')
    (args.out / 'image.json').write_text(json.dumps(metadata, indent=2) + '\n')
    (args.out / 'commands.txt').write_text('''# Read-only firmware inventory; capture the serial output first.
# U-Boot only (PMON has a different command language):
version
bdinfo
printenv
help bootelf
help tftpboot
# STOP: inspect README.md before loading or jumping.
''')
    (args.out / 'uboot.cmd.in').write_text('''# Inventory is commands.txt. Do not paste a fixed RAM address.
# The checked host runner obtains bdinfo DRAM/LMB reservations, selects a
# separate cached staging region, checks source size, transfers kernel.elf,
# verifies CRC32 against the local ELF and only then sends bootelf -p.
# USB / SD: python3 tools/ls2k0300_handoff.py boot --serial DEVICE --transfer usb
# TFTP: add --transfer tftp --server-ip HOST_IPV4 --board-ip BOARD_IPV4
# Full source: docs/platforms/2k0300.md. Missing proof stops before any RAM load.
''')
    (args.out / 'README.md').write_text('''# LS2K0300 久久派 RAM-only handoff bundle

Cross-built; physical hardware validation is pending. No firmware is flashed.
Read docs/platforms/2k0300.md for the complete from-arrival workflow.

kernel.elf is a scalar-FPU LoongArch ELF64 program. It is not a Linux Image,
uImage or EFI image. Keep the existing bootloader and boot media intact.
Entry is cached DMW 0x9000000002000000; initial allocator is physical
[0x00200000,0x08000000). Firmware must initialize DDR/UART0 before entry.
No unknown firmware DTB pointer is dereferenced by this stage.

Stage the ELF on an explicitly selected, mounted writable FAT SD/USB volume:

    tools/a20 deploy ls2k0300 --stage-mount /media/USER/YOUR_VOLUME --no-console

The private equivalent is `python3 tools/ls2k0300_handoff.py stage --mount MOUNT`.
It validates ELF/hash, refuses symlinks and different existing content, and
fsyncs the new file/directory; identical content is accepted for repeat runs.
Normally unmount and move the medium to the board, then from the repository:

    python3 tools/ls2k0300_handoff.py boot --serial /dev/serial/by-id/YOUR_UART --transfer usb

The checked host runner owns the serial port: do not open a second terminal.

For a different partition add --device 1:1; default is 0:1. The runner uses
U-Boot's read-only filesystem loader, not dd. It inventories version, DDR,
LMB and required commands; proves the whole kernel allocator is available;
chooses separate low-DDR staging; checks exact size and CRC32 before bootelf.
Incomplete memory evidence, transfer errors or mismatched CRC suppress boot.

TFTP requires Ethernet and two LAN IPv4 addresses. Serve precisely this ELF:

    sudo python3 tools/ls2k0300_handoff.py serve-tftp --bind HOST_IPV4
    python3 tools/ls2k0300_handoff.py boot --serial DEVICE --transfer tftp --server-ip HOST_IPV4 --board-ip BOARD_IPV4

The server serves only the validated, immutable kernel.elf and writes no files.
Press c / power-cycle while capture starts if the firmware has already booted
Linux. PMON is recognized and returns a named pmon failure after help capture;
U-Boot commands are never sent to PMON. See the exact PMON detection steps in
the source guide. Unknown PMON memory reservations are not guessed.

image.json contains ELF SHA-256, CRC32, size and full load-segment footprint.
The runner saves serial.log including failures. First acceptance markers are
[LS2K0300] stage=polled and System ready (bringup, no userspace). IRQ, storage,
network and display drivers in A20OS remain later hardware acceptance work.
''')
    print(f'LS2K0300 handoff bundle: {args.out} (checked host handoff available; hardware unverified)')


if __name__ == '__main__':
    main()
