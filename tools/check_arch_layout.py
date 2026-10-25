#!/usr/bin/env python3
"""Reject architecture leakage and obsolete board/EFI source organization."""
from pathlib import Path
import argparse
import re

ROOT = Path(__file__).resolve().parents[1]
ARCHES = ('aarch64', 'arm32', 'armv7m', 'riscv32', 'riscv64', 'loongarch32', 'loongarch64', 'ppc64le', 'x86_64')
INCLUDE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]', re.M)
DIRECT_ISA_HEADER = re.compile(r'^(?:kernel/)?arch/(?:' + '|'.join(ARCHES) + r')/')
ASM_INSTRUCTION = re.compile(r'\b(?:__asm__|asm)\s+(?:__volatile__|volatile)\s*\(\s*"([^"\n]+)')
LEGACY_TRANSPORT_SCAN = re.compile(r'\b(?:arch_virtio_(?:blk|net)_probe|virtio_(?:blk|net)_init)\s*\(')

def violations(root: Path) -> list[str]:
    errors = []
    if (root / 'kernel/platform').exists():
        errors.append('kernel/platform must not exist: boards belong to arch/<arch>/boards')
    if (root / 'kernel/boot/uefi').exists():
        errors.append('kernel/boot/uefi must not exist: EFI loaders belong to their architecture')
    for area in ('kernel/drivers/stm32f1', 'kernel/include/drivers/stm32f1'):
        if (root / area).exists():
            errors.append(f'{area} must not exist: STM32 drivers and private headers belong to their board')
    if (root / 'kernel/mcu/main.c').exists():
        errors.append('kernel/mcu/main.c must not exist: MCU hardware entry points belong to their board')
    owners = {}
    private_headers = set()
    for directory in sorted((root / 'kernel/arch').glob('*/boards/*')):
        if not directory.is_dir():
            continue
        arch = directory.parent.parent.name
        if arch not in ARCHES:
            errors.append(f'{directory.relative_to(root)}: unsupported architecture owner')
        if directory.name in owners:
            errors.append(f'board {directory.name}: duplicate architecture ownership')
        owners[directory.name] = arch
        private_headers.update(str(p.relative_to(directory / 'include'))
                               for p in (directory / 'include').rglob('*.h'))
        for source in directory.rglob('*'):
            if source.suffix not in ('.c', '.h', '.S'):
                continue
            for header in INCLUDE.findall(source.read_text()):
                match = DIRECT_ISA_HEADER.match(header)
                if match and f'arch/{arch}/' not in match.group(0):
                    errors.append(f'{source.relative_to(root)}: cross-architecture include {header}')
    for arch in ARCHES:
        directory = root / 'kernel/arch' / arch
        if not directory.exists():
            continue
        if not (directory / 'boot/context.c').exists():
            errors.append(f'arch/{arch}: missing architecture boot context adapter')
    for source in (root / 'kernel').rglob('*'):
        if source.suffix not in ('.c', '.h', '.S') or source.relative_to(root / 'kernel').parts[0] == 'external':
            continue
        data = source.read_text()
        if LEGACY_TRANSPORT_SCAN.search(data):
            errors.append(f'{source.relative_to(root)}: obsolete private VirtIO discovery; publish bus devices before binding')
        if source.relative_to(root / 'kernel').parts[0] == 'arch':
            continue
        for header in INCLUDE.findall(data):
            if header in private_headers or header.startswith('drivers/stm32f1/'):
                errors.append(f'{source.relative_to(root)}: architecture implementation include {header}')
    # Shared firmware owns parsing/model behavior. Physical mappings and CPU
    # instructions must remain behind architecture callbacks.
    for area in ('kernel/boot', 'kernel/firmware', 'kernel/mcu'):
        for source in (root / area).rglob('*'):
            if source.suffix not in ('.c', '.h', '.S'):
                continue
            data = source.read_text()
            if source.suffix == '.S' or ASM_INSTRUCTION.search(data):
                errors.append(f'{source.relative_to(root)}: CPU instructions in shared boot/firmware/MCU')
            for header in INCLUDE.findall(data):
                if DIRECT_ISA_HEADER.match(header) or header in ('platform.h', 'cpu.h', 'page_table.h', 'trap_frame.h', 'firmware.h', 'board.h', 'peripherals.h'):
                    errors.append(f'{source.relative_to(root)}: architecture implementation include {header}')
    return errors

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args()
    errors = violations(args.root)
    if errors:
        for error in errors:
            print(error)
        return 1
    print('check-arch-layout: PASS')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
