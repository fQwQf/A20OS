#!/usr/bin/env python3
"""Check ext4 inode initialization accounting against the host's e2fsck.

Requires an x86_64 ABI=both, four-CPU preempt dev-build. All disks are private.
Use --kernel for a negative control. A dense inode table makes the wrong
tail count hide existing files after an allocation or a non-tail unlink.
"""
import argparse
import copy
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from smoke import CASES, resolve_case_build_paths, run_qemu, report


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernel')
    ap.add_argument('--log', default='build/ext4-inode-tail.log')
    args = ap.parse_args()
    log = Path(args.log).resolve()
    log.parent.mkdir(parents=True, exist_ok=True)
    fsck = shutil.which('e2fsck') or '/usr/sbin/e2fsck'
    mkfs = shutil.which('mkfs.ext4') or '/usr/sbin/mkfs.ext4'
    debugfs = shutil.which('debugfs') or '/usr/sbin/debugfs'
    c = copy.deepcopy(CASES['smoke-vm-fault-x86_64'])
    c['build']['vars'] = ['ARCH=x86_64', 'ABI=both', 'NR_CPUS=4',
                          'CONFIG_KERNEL_PREEMPT=1', 'BRINGUP=0']
    c = resolve_case_build_paths(c)
    with tempfile.TemporaryDirectory(prefix='inode-tail-', dir=log.parent) as tmp:
        root = Path(tmp)
        seed = root / 'seed'
        seed.mkdir()
        for i in range(200):
            (seed / ('seed-%03d' % i)).write_text('preserve me\n')
        image = root / 'ext4.img'
        with image.open('wb') as f:
            f.truncate(64 * 1024 * 1024)
        subprocess.run([mkfs, '-q', '-F', '-b', '4096', '-N', '256',
                        '-O', '^has_journal,extent,huge_file,flex_bg,uninit_bg,dir_index',
                        '-d', str(seed), str(image)], check=True)
        subprocess.run([fsck, '-fn', str(image)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        a = c['argv']
        # The general VM-fault case uses -snapshot. This test must inspect
        # durable guest writes, with isolation provided by private disks.
        while '-snapshot' in a:
            a.remove('-snapshot')
        ki = a.index('-kernel') + 1
        shutil.copyfile(args.kernel or a[ki], root / 'kernel.elf')
        a[ki] = str(root / 'kernel.elf')
        for i, value in enumerate(a):
            if value.startswith('file='):
                src, options = value[5:].split(',', 1)
                if 'fat32.img' in src:
                    shutil.copyfile(src, root / 'fat32.img')
                    a[i] = 'file=' + str(root / 'fat32.img') + ',' + options
                elif 'ext4.img' in src:
                    a[i] = 'file=' + str(image) + ',' + options
        a[a.index('-smp') + 1] = '4'
        if os.access('/dev/kvm', os.R_OK | os.W_OK):
            a += ['-accel', 'kvm']
        c.update(log=str(log), timeout='45s', stdin={
            'kind': 'pipe', 'delay': 8,
            'lines': ['cat /proc/version > /extra/fresh.txt',
                      'rm /extra/seed-150',
                      'cat /proc/version > /extra/reused.txt',
                      'sync', 'poweroff']},
            expect=['System is going down for power-off'],
            forbid=['KERNEL PANIC', 'Read-only file system', 'No space left'])
        if report('ext4-inode-tail-guest', c, run_qemu(c)):
            return 1
        listing = subprocess.check_output([debugfs, '-R', 'ls -p /', str(image)],
                                          stderr=subprocess.DEVNULL, text=True)
        names = {line.split('/')[5] for line in listing.splitlines()
                 if line.startswith('/') and len(line.split('/')) > 5}
        if not {'seed-151', 'fresh.txt', 'reused.txt'} <= names or 'seed-150' in names:
            raise RuntimeError('guest allocation/unlink/reuse did not persist')
        for name in ('fresh.txt', 'reused.txt'):
            content = subprocess.check_output(
                [debugfs, '-R', 'cat /' + name, str(image)],
                stderr=subprocess.DEVNULL)
            if b'A20OS' not in content:
                raise RuntimeError('guest file payload did not persist: ' + name)
        result = subprocess.run([fsck, '-fn', str(image)], capture_output=True,
                                text=True)
        Path(str(log) + '.fsck').write_text(result.stdout + result.stderr)
        print('ext4-inode-tail:', 'PASS' if not result.returncode else 'FAIL')
        return int(bool(result.returncode))


if __name__ == '__main__':
    raise SystemExit(main())
