#!/usr/bin/env python3
"""Exercise real VirtIO evdev subscribers in a disposable four-CPU VM.

Build ARCH=<arch> ABI=both NR_CPUS=4 CONFIG_KERNEL_PREEMPT=1 dev-build first.
The serial READY markers synchronize button injection with the guest assertions.
"""
import argparse
import copy
import os
from pathlib import Path
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from smoke import CASES, resolve_case_build_paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=('x86_64', 'riscv64'), default='x86_64')
    parser.add_argument('--kernel', help='override kernel for a negative control')
    parser.add_argument('--log', type=Path)
    args = parser.parse_args()
    case = copy.deepcopy(CASES['smoke-cpuinfo' if args.arch == 'riscv64'
                              else 'smoke-vm-fault-x86_64'])
    case['build']['vars'] = ['ARCH=' + args.arch, 'ABI=both', 'NR_CPUS=4',
                            'CONFIG_KERNEL_PREEMPT=1', 'BRINGUP=0']
    case = resolve_case_build_paths(case)
    argv = case['argv']
    argv[argv.index('-smp') + 1] = '4'
    argv.append('-snapshot')
    if args.arch == 'x86_64':
        if os.access('/dev/kvm', os.R_OK | os.W_OK):
            argv.extend(['-accel', 'kvm'])
        argv.extend(['-device', 'virtio-keyboard-pci', '-device', 'virtio-mouse-pci'])
    else:
        # Slot 5 belongs to the separate user-placement uinputd probe.
        argv.extend(['-device', 'virtio-keyboard-device,bus=virtio-mmio-bus.7',
                     '-device', 'virtio-mouse-device,bus=virtio-mmio-bus.6'])
    if args.kernel:
        argv[argv.index('-kernel') + 1] = args.kernel
    log_path = args.log or Path('build/evdev-stream-' + args.arch + '.log')
    log_path.parent.mkdir(parents=True, exist_ok=True)
    data = bytearray()
    phases = set()
    sent = False
    conn = None
    with tempfile.TemporaryDirectory(prefix='evdev-', dir=log_path.parent) as tmp:
        tmp_path = Path(tmp).resolve()
        # QEMU snapshots protect guest writes, but not a concurrent rebuild of
        # the base image. Pin private copies for the lifetime of this run.
        kernel_index = argv.index('-kernel') + 1
        shutil.copyfile(argv[kernel_index], tmp_path / 'kernel.elf')
        argv[kernel_index] = str(tmp_path / 'kernel.elf')
        for i, value in enumerate(argv):
            if value.startswith('file=') and 'fat32.img' in value:
                filename, options = value[5:].split(',', 1)
                shutil.copyfile(filename, tmp_path / 'fat32.img')
                argv[i] = 'file=' + str(tmp_path / 'fat32.img') + ',' + options
        monitor = str(tmp_path / 'monitor')
        argv.extend(['-monitor', 'unix:' + monitor + ',server=on,wait=off'])
        proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)

        def hmp(command):
            conn.sendall((command + '\n').encode())
            time.sleep(.05)
            conn.recv(65536)

        def click():
            hmp('mouse_move 17 19')
            hmp('mouse_button 1')
            time.sleep(.1)
            hmp('mouse_button 0')

        try:
            deadline = time.monotonic() + 120
            with log_path.open('wb') as log:
                while time.monotonic() < deadline and proc.poll() is None:
                    readable, _, _ = select.select([proc.stdout], [], [], .1)
                    if not readable:
                        continue
                    chunk = os.read(proc.stdout.fileno(), 65536)
                    if not chunk:
                        break
                    data.extend(chunk)
                    log.write(chunk)
                    log.flush()
                    if not sent and b'# ' in data:
                        conn = socket.socket(socket.AF_UNIX)
                        conn.settimeout(3)
                        conn.connect(monitor)
                        conn.recv(65536)
                        proc.stdin.write(b'evdev_stress\nevdev_stress --stream\n')
                        proc.stdin.flush()
                        sent = True
                    for phase in ('normal', 'held', 'release', 'grabbed', 'final', 'overflow'):
                        if phase in phases or ('EVDEV_STREAM: READY ' + phase).encode() not in data:
                            continue
                        phases.add(phase)
                        if phase in ('normal', 'grabbed', 'final'):
                            click()
                        elif phase == 'held':
                            hmp('mouse_button 1')
                        elif phase == 'release':
                            hmp('mouse_button 0')
                        else:
                            for _ in range(300):
                                hmp('mouse_move 1 0')
                    failure = data.find(b'EVDEV_STRESS: FAIL')
                    if b'EVDEV_STREAM: PASS' in data or (failure >= 0 and b'\n' in data[failure:]):
                        break
        finally:
            if conn:
                conn.close()
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
    passed = (b'EVDEV_STRESS: PASS' in data and b'EVDEV_STREAM: PASS' in data and
              all(x not in data for x in (b'EVDEV_STRESS: FAIL', b'PANIC', b'unresolved symbol')))
    print(f'evdev-stream-{args.arch}: {"PASS" if passed else "FAIL"}; log: {log_path}')
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
