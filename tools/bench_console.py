#!/usr/bin/env python3
"""Compare default-block yes | dd on A20OS and Linux through the same PTY.

Build the instance first. Original images are copied and QEMU uses -snapshot.
The reader verifies every payload byte; no output is discarded in the guest.
This measures terminal transport without GUI rendering, not /dev/null speed.
"""
from __future__ import annotations
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import pty
import re
import select
import shlex
import statistics
import subprocess
import sys
import tempfile
import termios
import time
import tty

from a20_instance import parse_instance
from a20_make import REPO_ROOT, query_make


class PtyProcess:
    def __init__(self, argv, output_mode):
        self.master, slave = pty.openpty()
        tty.setraw(slave, termios.TCSANOW)
        self.output_mode = output_mode
        self.set_output_mode(slave)
        self.process = subprocess.Popen(argv, stdin=slave, stdout=slave, stderr=slave,
                                        cwd=REPO_ROOT, start_new_session=True)
        os.close(slave)
        os.set_blocking(self.master, False)
        self.output = bytearray()

    def set_output_mode(self, fd=None):
        fd = self.master if fd is None else fd
        settings = termios.tcgetattr(fd)
        if self.output_mode == 'terminal':
            settings[1] |= termios.OPOST | termios.ONLCR
        else:
            settings[1] &= ~termios.OPOST
        termios.tcsetattr(fd, termios.TCSANOW, settings)

    def send(self, data):
        view = memoryview(data)
        while view:
            try:
                view = view[os.write(self.master, view):]
            except BlockingIOError:
                select.select([], [self.master], [], 1)

    def pump(self, timeout=.1):
        if select.select([self.master], [], [], timeout)[0]:
            for _ in range(16):
                try:
                    data = os.read(self.master, 65536)
                except BlockingIOError:
                    break
                except OSError as exc:
                    if exc.errno == errno.EIO:
                        break
                    raise
                if not data:
                    break
                self.output.extend(data)

    def until(self, pattern, start=0, timeout=90):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.pump()
            tail = bytes(self.output[max(start, len(self.output) - 16384):])
            if re.search(pattern, tail, re.M):
                return
            if self.process.poll() is not None:
                raise RuntimeError('process exited before marker: ' + tail.decode(errors='replace'))
        raise TimeoutError('missing marker: ' + repr(pattern))

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        os.close(self.master)


def command(count):
    return (f'echo __CONSOLE_BEGIN__; yes | dd count={count}; '
            'echo __CONSOLE_END__')


def result(data, count, output_mode):
    match = re.search(rb'^__CONSOLE_BEGIN__\r?\n(.*?)^(\d+)\+\d+ records in\r?\n'
                      rb'\d+\+\d+ records out\r?\n(\d+) bytes .*?copied, ([\d.]+) s,',
                      data, re.M | re.S)
    if not match:
        raise ValueError('missing dd result')
    payload, records, length, seconds = match.groups()
    length = int(length)
    if int(records) != count or length != count * 512:
        raise ValueError('dd did not transfer the requested number of bytes')
    expected = b'y\r\n' if output_mode == 'terminal' else b'y\n'
    if payload != expected * (length // 2):
        raise ValueError(f'console corruption: received {len(payload)}, expected {length} bytes')
    seconds = float(seconds)
    return dict(bytes=length, seconds=seconds, bytes_per_second=length / seconds,
                sha256=hashlib.sha256(payload).hexdigest(), byte_exact=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--instance', default='cli-x86_64')
    ap.add_argument('--kernel', type=Path)
    ap.add_argument('--fat32', type=Path)
    ap.add_argument('--world', type=Path)
    ap.add_argument('--count', type=int, default=16384, help='512-byte records per run')
    ap.add_argument('--runs', type=int, default=3)
    ap.add_argument('--stall-seconds', type=float, default=0,
                    help='pause reads to exercise bounded backpressure (not a speed measurement)')
    ap.add_argument('--regressions', action='store_true',
                    help='also run pipe/TTY binaries installed in a test world')
    ap.add_argument('--pty-output', choices=['terminal', 'raw'], default='terminal',
                    help='terminal uses normal ONLCR output; raw isolates transport throughput')
    ap.add_argument('--output', type=Path, default=Path('build/console-comparison'))
    args = ap.parse_args()
    if args.count < 1 or args.runs < 1:
        ap.error('--count and --runs must be positive')
    if not 0 <= args.stall_seconds <= 1:
        ap.error('--stall-seconds must be between 0 and 1')
    manifest = REPO_ROOT / 'instances' / (args.instance + '.toml')
    inst = parse_instance(manifest)
    if inst.arch != 'x86_64' or inst.machine.console != 'virtio':
        ap.error('requires an x86_64 instance with machine.console=virtio')
    values = query_make(inst, ['QEMU', 'QEMU_FLAGS_NO_SDCARD', 'QEMU_BLK_SECOND',
                               'KERNEL_ELF', 'FAT32_IMG', 'PKG_IMAGE_DIR', 'PKG_WORLD', 'PKG_ARCH'])
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(instance=args.instance, count=args.count,
                  sink=f'same {args.pty_output} PTY, continuously drained without GUI rendering',
                  command='yes | dd (bounded with count only)', stall_seconds=args.stall_seconds,
                  guest=[], linux=[])
    with tempfile.TemporaryDirectory(prefix='console-bench-', dir=args.output) as directory:
        work = Path(directory).resolve()
        fat = args.fat32 or Path(values['FAT32_IMG'])
        world = args.world or Path(values['PKG_IMAGE_DIR']) / (values['PKG_WORLD']+'-'+values['PKG_ARCH']+'.img')
        for source, target in [(fat, work/'boot.img'), (world, work/'world.img')]:
            subprocess.run(['cp', '--reflink=auto', str(source.resolve()), str(target)], check=True)
        flags = shlex.split(values['QEMU_FLAGS_NO_SDCARD'])
        for index, value in enumerate(flags):
            if index and flags[index-1] == '-drive':
                parts = value.split(',')
                flags[index] = ','.join('file='+str(work/'boot.img') if part.startswith('file=') else part
                                        for part in parts)
        argv = shlex.split(values['QEMU']) + flags + ['-snapshot', '-drive',
                f'file={work / "world.img"},if=none,format=raw,id=xworld', '-device',
                values['QEMU_BLK_SECOND']+',drive=xworld', '-kernel',
                str((args.kernel or Path(values['KERNEL_ELF'])).resolve())]
        report['qemu'] = argv
        guest = PtyProcess([sys.executable, str(REPO_ROOT/'tools/qemu_console.py'), '--', *argv], args.pty_output)
        try:
            guest.until(rb'root@A20OS')
            # Apply identical PTY output processing after the console runner
            # initializes input termios. Raw-mode and terminal-mode results
            # describe different workloads and must not be compared directly.
            guest.set_output_mode()
            if b'[VPORT] bulk TTY ready' not in guest.output:
                raise RuntimeError('flow-controlled bulk TTY did not bind; refusing to benchmark fallback')
            for _ in range(args.runs):
                start = len(guest.output)
                guest.send((command(args.count)+'\n').encode())
                if args.stall_seconds:
                    guest.until(rb'^__CONSOLE_BEGIN__\r?$', start)
                    time.sleep(args.stall_seconds)
                guest.until(rb'^__CONSOLE_END__\r?$', start)
                sample = result(bytes(guest.output[start:]), args.count, args.pty_output)
                report['guest'].append(sample)
                print('A20OS', f'{sample["bytes_per_second"]/1e6:.2f} MB/s', flush=True)
            if args.regressions:
                start = len(guest.output)
                guest.send(b'/bin/pipe_stream_test; /bin/pipe_ref; /bin/poll_edge; '
                           b'/bin/io_event_test; /bin/tty_stream_test; echo __CHECK_END__\n')
                guest.until(rb'^__CHECK_END__\r?$', start)
                checks = bytes(guest.output[start:])
                for marker in (b'PIPE_STREAM: PASS', b'PIPE_REF:', b'POLL_EDGE: PASS',
                               b'IO_EVENT_TEST: PASS', b'TTY_STREAM: PASS'):
                    if marker not in checks:
                        raise RuntimeError('missing regression result: '+repr(marker))
                records = re.findall(rb'^TTY_STREAM: writer=(\d) seq=(\d\d) '
                                     rb'abcdefghijklmnopqrstuvwxyz0123456789\r?$', checks, re.M)
                if len(records) != 160 or len(set(records)) != 160:
                    raise ValueError('concurrent TTY records were lost or interleaved')
                report['regressions'] = 'pipe, poll/epoll, 160 complete TTY records, tcdrain'
            start = len(guest.output)
            guest.send(b'yes | dd\n')
            until = time.monotonic()+1
            while time.monotonic() < until:
                guest.pump(.01)
            interrupted = len(guest.output)
            begin = time.monotonic()
            guest.send(b'\x03')
            guest.until(rb'root@A20OS', interrupted, timeout=5)
            report['ctrl_c_seconds'] = time.monotonic() - begin
            guest.send(b'poweroff\n')
            until = time.monotonic()+15
            while guest.process.poll() is None and time.monotonic() < until:
                guest.pump()
            (args.output/'guest.log').write_bytes(guest.output)
        finally:
            (args.output/'guest.log').write_bytes(guest.output)
            guest.close()
    for _ in range(args.runs):
        host = PtyProcess(['bash', '-c', command(args.count)], args.pty_output)
        try:
            if args.stall_seconds:
                time.sleep(args.stall_seconds)
            host.until(rb'^__CONSOLE_END__\r?$')
            sample = result(bytes(host.output), args.count, args.pty_output)
            report['linux'].append(sample)
            print('Linux', f'{sample["bytes_per_second"]/1e6:.2f} MB/s', flush=True)
        finally:
            host.close()
    for side in ['guest', 'linux']:
        report[side+'_median_bytes_per_second'] = statistics.median(s['bytes_per_second'] for s in report[side])
    report['ratio'] = report['guest_median_bytes_per_second']/report['linux_median_bytes_per_second']
    (args.output/'results.json').write_text(json.dumps(report, indent=2)+'\n')
    print('A20OS/Linux:', round(report['ratio'], 3), 'Ctrl-C:', round(report['ctrl_c_seconds'], 3), 's')


if __name__ == '__main__':
    main()
