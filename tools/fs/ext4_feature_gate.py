#!/usr/bin/env python3
"""Validate existing ext4 orphan/encryption feature guards against e2fsprogs images."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import selectors
import shutil
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tools/cli'))
from img import enable_journal_checksums_stream
from a20_resource import Policy, Requirement, gate
from smoke import guest_slot_locks, _release_guest_slot_locks

MARKER = b'EXTERNAL_EXT4_FEATURE_FIXTURE_OK'


def external(command, log):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log.write_bytes(result.stdout)
    if result.returncode:
        raise RuntimeError(f'{command[0]} failed ({result.returncode}); see {log}')
    return result.stdout


def boot(kernel, fat, image, log):
    argv = ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G', '-smp', '2',
            '-bios', 'default', '-display', 'none', '-serial', 'stdio', '-monitor', 'none',
            '-global', 'virtio-mmio.force-legacy=false',
            '-drive', f'file={fat},if=none,format=raw,id=fat,snapshot=on',
            '-device', 'virtio-blk-device,drive=fat,bus=virtio-mmio-bus.0',
            '-drive', f'file={image},if=none,format=raw,id=ext',
            '-device', 'virtio-blk-device,drive=ext,bus=virtio-mmio-bus.1',
            '-kernel', str(kernel)]
    process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, bufsize=0)
    output = bytearray()
    poller = selectors.DefaultSelector()
    poller.register(process.stdout, selectors.EVENT_READ)
    sent = False
    queries = 0
    deadline = time.monotonic() + 90
    try:
        while time.monotonic() < deadline:
            for key, _ in poller.select(.03):
                data = key.fileobj.read(65536)
                if data: output.extend(data)
            seen = output.count(b'\x1b[6n')
            if process.poll() is None and seen > queries:
                process.stdin.write(b'\x1b[1;1R' * (seen - queries))
                process.stdin.flush()
                queries = seen
            if process.poll() is None and not sent and re.search(rb'^# ', output, re.M):
                process.stdin.write(b'cat /extra/external-marker\npoweroff\n')
                process.stdin.flush()
                sent = True
            if re.search(rb'PANIC|Kernel panic|Kernel BUG|LOCK-STALL', output):
                raise RuntimeError('guest kernel failed')
            if process.poll() is not None:
                # Drain the pipe after natural exit before checking its tail.
                output.extend(process.stdout.read() or b'')
                if process.returncode or b'[SD] POWER_OFF entry' not in output:
                    raise RuntimeError(f'unclean guest exit {process.returncode}')
                return bytes(output)
        raise TimeoutError('guest did not shut down within 90 seconds')
    finally:
        if process.poll() is None:
            process.kill(); process.wait()
        poller.close()
        log.write_bytes(output)


def run(args):
    policy = Policy.from_env()
    gate(Requirement(1024 + policy.reserve_mem_mb, 2, policy.min_disk_mb),
         policy, ROOT, wait=False, label='ext4 external feature fixtures')
    args.output.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'cases': []}
    mkfs, debugfs, fsck = ('/usr/sbin/' + name for name in ('mkfs.ext4', 'debugfs', 'e2fsck'))
    kernel, fat = args.build_dir / 'kernel.elf', args.build_dir / 'fat32.img'
    report['kernel_sha256'] = hashlib.sha256(kernel.read_bytes()).hexdigest()
    try:
        with tempfile.TemporaryDirectory(prefix='a20-ext4-features-') as d:
            d = Path(d)
            pristine = d / 'pristine.img'
            pristine.touch(); pristine.open('r+b').truncate(64 * 1024 * 1024)
            external([mkfs, '-q', '-F', '-b', '4096', '-O', 'orphan_file', str(pristine)],
                     args.output / 'mkfs.log')
            marker = d / 'marker'; marker.write_bytes(MARKER + b'\n')
            external([debugfs, '-w', '-R', f'write {marker} /external-marker', str(pristine)],
                     args.output / 'populate.log')
            enable_journal_checksums_stream(pristine)
            external([fsck, '-fn', str(pristine)], args.output / 'pristine-fsck.log')
            for name, feature, rejection in (
                ('clean-orphan-file', None, None),
                ('active-orphan-file', 'orphan_present', b'active orphan_file recovery is unsupported'),
                ('encryption-incompat', 'encrypt', b'Unsupported incompat features'),
            ):
                image = d / (name + '.img'); shutil.copyfile(pristine, image)
                if feature:
                    external([debugfs, '-w', '-R', f'feature {feature}', str(image)],
                             args.output / (name + '-feature.log'))
                external([debugfs, '-R', 'features', str(image)], args.output / (name + '-features.log'))
                with image.open('rb') as stream:
                    stream.seek(1024 + 92); compat, incompat, ro = struct.unpack('<III', stream.read(12))
                if not compat & 0x1000:
                    raise RuntimeError('mke2fs did not create the orphan_file fixture')
                if bool(ro & 0x10000) != (feature == 'orphan_present') or bool(incompat & 0x10000) != (feature == 'encrypt'):
                    raise RuntimeError('external feature toggles did not produce the expected superblock fields')
                before = hashlib.sha256(image.read_bytes()).hexdigest()
                output = boot(kernel, fat, image, args.output / (name + '-guest.log'))
                marker_seen = re.search(rb'^' + MARKER + rb'\r?$', output, re.M) is not None
                if rejection is None:
                    if not marker_seen: raise RuntimeError('clean orphan_file image was not readable')
                    external([fsck, '-fn', str(image)], args.output / (name + '-fsck.log'))
                elif marker_seen or rejection not in output:
                    raise RuntimeError(f'{name}: missing explicit mount refusal or filesystem was exposed')
                after = hashlib.sha256(image.read_bytes()).hexdigest()
                if rejection is not None and before != after:
                    raise RuntimeError(f'{name}: refused image was modified')
                report['cases'].append(dict(name=name, compat=hex(compat), incompat=hex(incompat),
                    ro_compat=hex(ro), marker_seen=marker_seen, before_sha256=before, after_sha256=after))
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    finally:
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))
    return 0 if report['passed'] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.build_dir = args.build_dir.resolve()
    state, handles = guest_slot_locks()
    if state == 'busy':
        print('EXT4_FEATURE: shared QEMU slot busy'); return 75
    try:
        return run(args)
    finally:
        if handles is not None: _release_guest_slot_locks(handles)


if __name__ == '__main__':
    raise SystemExit(main())
