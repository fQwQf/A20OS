#!/usr/bin/env python3
"""Pinned V3s SPL/U-Boot preparation and ARM32 legacy-uImage SD packaging."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
import pine_a64 as common

BASE = common.ROOT / 'build/lichee-pi-zero'
DTB_NAME = 'sun8i-v3s-licheepi-zero.dtb'
LOAD = 0x40080000
PAYLOAD = 0x42000000
FDT = 0x41800000
SCRIPT = 0x41900000


def elf_layout(data: bytes) -> tuple[int, int]:
    if len(data) < 52 or data[:6] != b'\x7fELF\x01\x01' or struct.unpack_from('<H', data, 18)[0] != 40:
        raise common.PrepareError('expected little-endian ARM ELF32')
    if struct.unpack_from('<I', data, 24)[0] != LOAD:
        raise common.PrepareError('ARM entry must be physical 0x40080000')
    table = struct.unpack_from('<I', data, 28)[0]
    step, count = struct.unpack_from('<HH', data, 42)
    ranges = []
    for i in range(count):
        p = table + i * step
        if step < 32 or p + 32 > len(data):
            raise common.PrepareError('invalid ELF32 program headers')
        kind, offset, _, address, filesz, memsz = struct.unpack_from('<6I', data, p)
        if kind == 1 and memsz:
            if filesz > memsz or offset + filesz > len(data) or address + memsz > 0x44000000:
                raise common.PrepareError('invalid ARM32 load segment')
            ranges.append((address, address + memsz))
    if not ranges or min(a for a, _ in ranges) != LOAD:
        raise common.PrepareError('ARM32 ELF physical load range mismatch')
    size = max(b for _, b in ranges) - LOAD
    if LOAD + size >= FDT:
        raise common.PrepareError('ARM32 kernel footprint reaches DTB staging window')
    return LOAD, size


def validate_uimage(data: bytes, kernel: bytes) -> None:
    if len(data) < 64:
        raise common.PrepareError('truncated legacy uImage')
    magic, hcrc, _, size, load, entry, dcrc, os_id, arch, kind, comp, _ = struct.unpack('>7I4B32s', data[:64])
    header = data[:4] + bytes(4) + data[8:64]
    if (magic != 0x27051956 or zlib.crc32(header) != hcrc or size != len(kernel) or
            data[64:] != kernel or zlib.crc32(kernel) != dcrc or
            (load, entry, os_id, arch, kind, comp) != (LOAD, LOAD, 5, 2, 2, 0)):
        raise common.PrepareError('legacy ARM uImage header/data CRC or boot addresses mismatch')
    if PAYLOAD + len(data) >= 0x43000000:
        raise common.PrepareError('legacy uImage reaches U-Boot high-RAM reservation budget')


def validate_binary(elf: bytes, kernel: bytes) -> None:
    """Require objcopy's physical PT_LOAD bytes, rather than another build."""
    elf_layout(elf)
    table = struct.unpack_from('<I', elf, 28)[0]
    step, count = struct.unpack_from('<HH', elf, 42)
    segments = []
    for i in range(count):
        kind, offset, _, address, filesz, _ = struct.unpack_from('<6I', elf, table + i * step)
        if kind == 1 and filesz:
            segments.append((address - LOAD, elf[offset:offset + filesz]))
    end = max((offset + len(data) for offset, data in segments), default=0)
    if not end or end != len(kernel):
        raise common.PrepareError('kernel.bin length differs from ARM ELF loadable bytes')
    expected = bytearray(end)
    for offset, data in segments:
        expected[offset:offset + len(data)] = data
    if expected != kernel:
        raise common.PrepareError('kernel.bin differs from ARM ELF physical segments')


def validate_firmware(directory: Path) -> dict:
    meta = json.loads((directory / 'manifest.json').read_text())
    if meta.get('board') != 'lichee-pi-zero' or meta.get('defconfig') != 'LicheePi_Zero_defconfig':
        raise common.PrepareError('expected V3s Lichee Pi Zero prepared firmware')
    for name in ('u-boot-sunxi-with-spl.bin', DTB_NAME, 'mkimage'):
        if meta['files'].get(name) != common.artifact(directory / name):
            raise common.PrepareError('modified prepared firmware: ' + name)
    blob = (directory / 'u-boot-sunxi-with-spl.bin').read_bytes()
    if blob[4:12] != b'eGON.BT0' or len(blob) + common.FIRMWARE_OFFSET >= common.PARTITION_OFFSET:
        raise common.PrepareError('invalid SPL signature or firmware exceeds MBR gap')
    if 'licheepi,licheepi-zero' not in common.root_compatible((directory / DTB_NAME).read_bytes()):
        raise common.PrepareError('wrong V3s board DTB')
    return meta


def prepare(args) -> None:
    make, gcc = common.tool('make'), common.tool(args.cross_compile + 'gcc')
    for name in ('bison', 'flex', 'swig', 'openssl'):
        common.tool(name)
    env = dict(os.environ, SOURCE_DATE_EPOCH=common.SOURCE_DATE_EPOCH, TZ='UTC')
    work, out = args.work.resolve(), args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    deps = work / 'python-deps'
    if not (deps / 'elftools').is_dir():
        common.run([sys.executable, '-m', 'pip', 'install', '--target', str(deps), '--disable-pip-version-check', 'pyelftools==0.32'])
    env['PYTHONPATH'] = str(deps) + os.pathsep + env.get('PYTHONPATH', '')
    source = common.source_tree('u-boot', args.cache.resolve())
    ub = work / 'u-boot'
    cmd = [make, '-C', str(source), f'O={ub}', f'CROSS_COMPILE={args.cross_compile}', f'PYTHON={sys.executable}', f'PYTHON3={sys.executable}']
    common.run([*cmd, 'LicheePi_Zero_defconfig'], env=env)
    common.run([str(source / 'scripts/config'), '--file', str(ub / '.config'), '--enable', 'CRC32_VERIFY', '--enable', 'BOOTMETH_SCRIPT', '--enable', 'LEGACY_IMAGE_FORMAT'], env=env)
    common.run([*cmd, 'olddefconfig'], env=env)
    common.run([*cmd, f'-j{args.jobs}'], env=env)
    dtbs = list(ub.rglob(DTB_NAME))
    if not dtbs or len({common.sha256(p) for p in dtbs}) != 1:
        raise common.PrepareError('missing/ambiguous V3s DTB')
    config = (ub / '.config').read_text()
    for setting in ('CONFIG_ARMV7_NONSEC=y', 'CONFIG_SUNXI_MINIMUM_DRAM_MB=64', 'CONFIG_LEGACY_IMAGE_FORMAT=y'):
        if setting not in config:
            raise common.PrepareError('unexpected V3s firmware configuration: ' + setting)
    # Mainline 64-MiB layout already reserves FDT at +24MiB and script at +25MiB.
    # Payload at +32MiB is below the conservative upper 16-MiB U-Boot budget.
    out.mkdir(parents=True, exist_ok=True)
    outputs = {'u-boot-sunxi-with-spl.bin': ub / 'u-boot-sunxi-with-spl.bin', DTB_NAME: dtbs[0],
               'mkimage': ub / 'tools/mkimage', 'u-boot.config': ub / '.config'}
    for name, path in outputs.items():
        common.regular_output(out / name)
        shutil.copy2(path, out / name)
    meta = {'format': 'a20os-lichee-pi-zero-firmware-v1', 'board': 'lichee-pi-zero', 'ram_mib': 64,
            'hardware_validated': False, 'defconfig': 'LicheePi_Zero_defconfig', 'sources': {'u-boot': common.SOURCES['u-boot']},
            'source_date_epoch': common.SOURCE_DATE_EPOCH, 'compiler': common.run([gcc, '--version'], capture=True).splitlines()[0],
            'config_overrides': ['CONFIG_CRC32_VERIFY=y', 'CONFIG_BOOTMETH_SCRIPT=y', 'CONFIG_LEGACY_IMAGE_FORMAT=y'],
            'files': {name: common.artifact(out / name) for name in outputs}}
    common.regular_output(out / 'manifest.json')
    (out / 'manifest.json').write_text(json.dumps(meta, indent=2) + '\n')
    validate_firmware(out)
    print(f'Prepared V3s SPL/U-Boot: {out}')


def boot_script(payload: bytes, tree: bytes) -> str:
    return f'''echo A20OS Lichee Pi Zero V3s SD boot
setenv loadaddr 0x{PAYLOAD:x}
setenv fdt_addr_r 0x{FDT:x}
setenv bootargs a20.hostname=lichee-pi-zero
if fatload mmc 0:1 ${{loadaddr}} kernel.uimg && itest ${{filesize}} == 0x{len(payload):x} && crc32 -v ${{loadaddr}} ${{filesize}} {zlib.crc32(payload):08x}; then
    if fatload mmc 0:1 ${{fdt_addr_r}} {DTB_NAME} && itest ${{filesize}} == 0x{len(tree):x} && crc32 -v ${{fdt_addr_r}} ${{filesize}} {zlib.crc32(tree):08x}; then
        bootm ${{loadaddr}} - ${{fdt_addr_r}}
    fi
fi
echo A20OS V3s SD boot failed
exit 1
'''


def sdcard(args) -> None:
    firmware, bundle, output = args.firmware.resolve(), args.bundle.resolve(), args.output.absolute()
    fw = validate_firmware(firmware)
    elf = args.elf.read_bytes()
    address, size = elf_layout(elf)
    kernel = args.kernel.read_bytes()
    validate_binary(elf, kernel)
    if not kernel or len(kernel) > size:
        raise common.PrepareError('kernel.bin exceeds ELF memory footprint')
    bundle.mkdir(parents=True, exist_ok=True)
    for name in ('kernel.uimg', 'uboot.cmd', 'manifest.json'):
        common.regular_output(bundle / name)
    for name, source in (('kernel.elf', args.elf), ('kernel.bin', args.kernel), (DTB_NAME, firmware / DTB_NAME)):
        common.regular_output(bundle / name)
        if source.resolve() != (bundle / name).resolve():
            shutil.copy2(source, bundle / name)
    env = dict(os.environ, SOURCE_DATE_EPOCH=common.SOURCE_DATE_EPOCH, TZ='UTC', MTOOLS_SKIP_CHECK='1')
    common.run([str(firmware / 'mkimage'), '-A', 'arm', '-O', 'linux', '-T', 'kernel', '-C', 'none',
                '-a', hex(LOAD), '-e', hex(LOAD), '-n', 'A20OS V3s', '-d', str(bundle / 'kernel.bin'), str(bundle / 'kernel.uimg')], env=env)
    payload, tree = (bundle / 'kernel.uimg').read_bytes(), (bundle / DTB_NAME).read_bytes()
    validate_uimage(payload, kernel)
    if len(tree) > SCRIPT - FDT:
        raise common.PrepareError('DTB overlaps script staging')
    import tomllib
    recipe = tomllib.loads(args.instance.read_text())
    if recipe.get('board') != 'lichee-pi-zero':
        raise common.PrepareError('handoff instance must target lichee-pi-zero')
    (bundle / 'uboot.cmd').write_text('\n'.join(recipe['handoff']['commands']) + '\n')
    names = ['kernel.elf', 'kernel.bin', 'kernel.uimg', DTB_NAME, 'uboot.cmd']
    meta = {'format': 'a20os-kernel-bundle-v1', 'arch': 'arm32', 'board': 'lichee-pi-zero', 'boot_protocol': 'bootm',
            'instance': {'name': recipe.get('name', args.instance.stem),
                         'sha256': hashlib.sha256(args.instance.read_bytes()).hexdigest()},
            'payload_name': 'kernel.uimg', 'payload_load_address': hex(PAYLOAD), 'load_address': hex(address),
            'image_size': size, 'hardware_validated': False,
            'files': {name: common.artifact(bundle / name) for name in names}}
    (bundle / 'manifest.json').write_text(json.dumps(meta, indent=2) + '\n')
    mkfs, mcopy = common.tool('mkfs.fat', '/usr/sbin/mkfs.fat', '/sbin/mkfs.fat'), common.tool('mcopy')
    common.regular_output(output)
    common.regular_output(output.with_suffix(output.suffix + '.json'))
    output.parent.mkdir(parents=True, exist_ok=True)
    image_size = args.size_mib * 1024 * 1024
    partition = common.mbr(image_size)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix='.v3s-') as directory:
        stage = Path(directory)
        fat = stage / 'boot.fat'
        with fat.open('wb') as stream:
            stream.truncate(image_size - common.PARTITION_OFFSET)
        common.run([mkfs, '--invariant', '-F', '32', '-n', 'A20V3S', str(fat)], env=env)
        (stage / 'boot.cmd').write_text(boot_script(payload, tree))
        common.run([str(firmware / 'mkimage'), '-A', 'arm', '-O', 'linux', '-T', 'script', '-C', 'none',
                    '-n', 'A20OS V3s SD', '-d', str(stage / 'boot.cmd'), str(stage / 'boot.scr')], env=env)
        files = {'kernel.uimg': bundle / 'kernel.uimg', DTB_NAME: bundle / DTB_NAME,
                 'boot.cmd': stage / 'boot.cmd', 'boot.scr': stage / 'boot.scr'}
        verify = stage / 'verify'
        verify.mkdir()
        for name, source in files.items():
            common.run([mcopy, '-i', str(fat), str(source), '::/' + name], env=env)
            common.run([mcopy, '-i', str(fat), '::/' + name, str(verify / name)], env=env)
            if common.sha256(source) != common.sha256(verify / name):
                raise common.PrepareError('FAT readback mismatch: ' + name)
        image = stage / 'sdcard.img'
        with image.open('wb') as stream:
            stream.truncate(image_size)
            stream.write(partition)
            stream.seek(common.FIRMWARE_OFFSET)
            stream.write((firmware / 'u-boot-sunxi-with-spl.bin').read_bytes())
            stream.seek(common.PARTITION_OFFSET)
            with fat.open('rb') as contents:
                shutil.copyfileobj(contents, stream)
        manifest = {'format': 'a20os-lichee-pi-zero-sdcard-v1', 'board': 'lichee-pi-zero', 'hardware_validated': False,
                    'image': common.artifact(image), 'partition_offset': common.PARTITION_OFFSET,
                    'raw_firmware_offset': common.FIRMWARE_OFFSET, 'firmware': fw, 'kernel': meta,
                    'fat_files': {name: common.artifact(path) for name, path in files.items()}}
        image.replace(output)
        output.with_suffix(output.suffix + '.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'V3s SD image ready: {output} (no physical device written)')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    p = sub.add_parser('prepare')
    p.add_argument('--cache', type=Path, default=BASE / 'source-cache')
    p.add_argument('--work', type=Path, default=BASE / 'firmware-build')
    p.add_argument('--out', type=Path, default=BASE / 'firmware')
    p.add_argument('--cross-compile', default='arm-linux-gnueabihf-')
    p.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 1))
    p.set_defaults(func=prepare)
    p = sub.add_parser('sdcard')
    p.add_argument('--firmware', type=Path, default=BASE / 'firmware')
    p.add_argument('--bundle', type=Path, default=BASE / 'handoff')
    p.add_argument('--elf', type=Path, required=True)
    p.add_argument('--kernel', type=Path, required=True)
    p.add_argument('--instance', type=Path, default=common.ROOT / 'instances/lichee-pi-zero.toml')
    p.add_argument('--output', type=Path, default=BASE / 'a20os-lichee-pi-zero-sdcard.img')
    p.add_argument('--size-mib', type=int, default=128)
    p.set_defaults(func=sdcard)
    args = parser.parse_args()
    try:
        args.func(args)
    except (common.PrepareError, OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f'lichee-pi-zero: {error}', file=sys.stderr)
        return 1
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
