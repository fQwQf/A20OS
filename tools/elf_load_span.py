#!/usr/bin/env python3
"""Return the physical memory span of an ELF image, including NOLOAD/BSS."""
import argparse
from pathlib import Path
import struct


def load_span(data: bytes) -> tuple[int, int]:
    if len(data) < 16 or data[:4] != b'\x7fELF' or data[6] != 1:
        raise ValueError('invalid ELF identification')
    if data[4] not in (1, 2) or data[5] not in (1, 2):
        raise ValueError('unsupported ELF class or byte order')
    wide = data[4] == 2
    endian = '<' if data[5] == 1 else '>'
    header = struct.Struct(endian + ('HHIQQQIHHHHHH' if wide else 'HHIIIIIHHHHHH'))
    phdr = struct.Struct(endian + ('IIQQQQQQ' if wide else 'IIIIIIII'))
    if len(data) < 16 + header.size:
        raise ValueError('truncated ELF header')
    h = header.unpack_from(data, 16)
    phoff, entsize, count = h[4], h[8], h[9]
    if h[2] != 1 or h[7] != 16 + header.size or entsize < phdr.size or not count or count == 0xffff:
        raise ValueError('invalid or unsupported program header table')
    if phoff > len(data) or count * entsize > len(data) - phoff:
        raise ValueError('truncated program header table')
    ranges = []
    limit = 1 << (64 if wide else 32)
    for index in range(count):
        p = phdr.unpack_from(data, phoff + index * entsize)
        if p[0] != 1:
            continue
        offset, physical, filesz, memsz = (p[2], p[4], p[5], p[6]) if wide else (p[1], p[3], p[4], p[5])
        if filesz > memsz or offset > len(data) or filesz > len(data) - offset:
            raise ValueError('invalid load segment file extent')
        if physical >= limit or memsz > limit - physical:
            raise ValueError('load segment physical address overflow')
        if memsz:
            ranges.append((physical, physical + memsz))
    if not ranges:
        raise ValueError('ELF has no nonempty load segments')
    base = min(start for start, _ in ranges)
    return base, max(end for _, end in ranges) - base


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--expected-base', type=lambda value: int(value, 0), required=True)
    args = parser.parse_args()
    try:
        base, size = load_span(args.elf.read_bytes())
        if base != args.expected_base:
            raise ValueError(f'ELF load base {base:#x} differs from loader address {args.expected_base:#x}')
    except (OSError, ValueError) as error:
        parser.exit(1, f'elf-load-span: {error}\n')
    print(size)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
