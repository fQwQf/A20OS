#!/usr/bin/env python3
"""Exercise a real BSD backend through Linux ELF programs and external peers.

Build with NET_STACK=bsd RAMFS_USER=1 RAMFS_NET_TESTS=1. This uses QEMU user-net
bound only to host loopback; no TAP, host routes or firewall changes are needed.
"""
import argparse
import concurrent.futures
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import select
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('tcp_probe', ROOT/'tools/probes/lwip_lane_host_probe.py')
tcp_probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tcp_probe)


def reserve_ports(kind, count):
    sockets = []
    try:
        for _ in range(count):
            s = socket.socket(socket.AF_INET, kind)
            s.bind(('127.0.0.1', 0))
            sockets.append(s)
        return [s.getsockname()[1] for s in sockets]
    finally:
        for s in sockets:
            s.close()


class Guest:
    def __init__(self, argv, log):
        self.process = subprocess.Popen(argv, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.log = log
        self.pending = b''

    def until(self, marker, timeout=90, *, whole_line=False):
        deadline = time.monotonic() + timeout
        pattern = re.escape(marker.encode())
        if whole_line:
            pattern = rb'(?:^|\r?\n)' + pattern + rb'(?=\r?\n)'
        elif marker == '# ':
            pattern = rb'(?:^|\r?\n)# $'
        expression = re.compile(pattern)
        while not (match := expression.search(self.pending)):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f'guest did not emit {marker!r}')
            ready, _, _ = select.select([self.process.stdout], [], [], min(1, remaining))
            if not ready:
                continue
            data = os.read(self.process.stdout.fileno(), 65536)
            if not data:
                raise RuntimeError(f'guest exited before {marker!r}')
            self.log.write(data)
            self.log.flush()
            self.pending += data
            if b'KERNEL PANIC' in self.pending or b'Kernel panic' in self.pending or b': FAIL' in self.pending:
                raise RuntimeError('guest reported a failure; inspect serial.log')
        pos = match.end()
        result, self.pending = self.pending[:pos], self.pending[pos:]
        return result

    def send(self, command):
        self.process.stdin.write((command + '\n').encode())
        self.process.stdin.flush()

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.log.write(self.process.stdout.read())
        self.log.flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=['x86_64', 'aarch64', 'riscv64', 'loongarch64'], required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--cpus', type=int, default=2)
    args = parser.parse_args()
    kernel = args.kernel.resolve(strict=True)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    ports = reserve_ports(socket.SOCK_STREAM, 4)
    udp_port, = reserve_ports(socket.SOCK_DGRAM, 1)
    network = 'user,id=net' + ''.join(f',hostfwd=tcp:127.0.0.1:{p}-:{p}' for p in ports)
    network += f',hostfwd=udp:127.0.0.1:{udp_port}-:18085'
    platform = {
        'x86_64': ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm'],
        'aarch64': ['qemu-system-aarch64', '-machine', 'virt', '-cpu', 'cortex-a57'],
        'riscv64': ['qemu-system-riscv64', '-machine', 'virt', '-bios', 'default'],
        'loongarch64': ['qemu-system-loongarch64', '-machine', 'virt'],
    }[args.arch]
    nic = ('virtio-net-pci' if args.arch in ('x86_64', 'loongarch64')
           else 'virtio-net-device,bus=virtio-mmio-bus.4')
    bootargs = 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dhcp=0'
    argv = [*platform, '-m', '1024', '-smp', str(args.cpus), '-nographic', '-monitor', 'none',
            '-no-reboot', '-netdev', network, '-device', nic + ',netdev=net',
            '-kernel', str(kernel), '-append', bootargs]
    if args.arch in ('aarch64', 'riscv64'):
        argv += ['-global', 'virtio-mmio.force-legacy=false']
    manifest = dict(arch=args.arch, kernel=str(kernel),
                    kernel_sha256=hashlib.sha256(kernel.read_bytes()).hexdigest(),
                    argv=argv, passed=[], status='running')
    guest = None
    try:
        with (out/'serial.log').open('wb') as log:
            guest = Guest(argv, log)
            if args.arch == 'loongarch64':
                # QEMU's direct LoongArch ELF boot does not supply -append.
                # Build this target with UART_CMDLINE=y.
                guest.until('none\r\n[UARTCMD] ')
                guest.send(bootargs)
            # SMP can print the first shell prompt in the middle of the boot
            # banner. Synchronize with a shell response after the last boot
            # marker, without confusing command echo or banner '#' with it.
            guest.until('[INIT] entering scheduler...')
            guest.send('echo A20_BSD_SHELL_READY')
            guest.until('A20_BSD_SHELL_READY', whole_line=True)
            guest.until('# ')
            guest.send('cat /proc/net/status')
            guest.until('backend: bsd')
            guest.until('# ')
            for command, marker in [
                ('tcp_loopback_test', 'TCP_LOOPBACK_TEST: PASS'),
                ('udp_loopback_test', 'UDP_LOOPBACK_TEST: PASS'),
                ('ipv6_loopback_test', 'IPV6_LOOPBACK_TEST: PASS'),
                ('bsd_scale_test', 'BSD_SCALE: PASS'),
            ]:
                guest.send(command)
                guest.until(marker)
                guest.until('# ')
                manifest['passed'].append(command)
            guest.send('net_lane_hostfwd_test ' + ' '.join(map(str, ports)))
            guest.until('NET_LANE_HOSTFWD_READY:')
            expected = tcp_probe.payload()
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                transfers = list(pool.map(lambda p: tcp_probe.run_one(p, expected), ports))
            manifest['tcp_transfers'] = transfers
            guest.until('NET_LANE_HOSTFWD_TEST: PASS')
            guest.until('# ')
            manifest['passed'].append('external_tcp_4x64KiB')
            guest.send('udp_hostfwd_test')
            result = subprocess.run([sys.executable, str(ROOT/'tools/probes/net_udp_host_probe.py'), str(udp_port)],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=50)
            (out/'udp-peer.log').write_bytes(result.stdout)
            if result.returncode:
                raise RuntimeError('external UDP peer failed')
            guest.until('UDP_HOSTFWD_TEST: PASS')
            guest.until('# ')
            manifest['passed'].append('external_udp_16x512B')
            guest.send('poweroff')
            guest.until('System is going down for power-off NOW')
            if guest.process.wait(timeout=15) != 0:
                raise RuntimeError('QEMU did not shut down cleanly')
            manifest['passed'].append('clean_shutdown')
            manifest['status'] = 'passed'
    except Exception as error:
        manifest['status'] = 'failed'
        manifest['error'] = repr(error)
    finally:
        if guest:
            # The with-block has already closed the log. Reopen to drain final bytes.
            with (out/'serial.log').open('ab') as log:
                guest.log = log
                guest.close()
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))
    return 0 if manifest['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
