#!/usr/bin/env python3
"""Verify RV64 console SIGINT with no reader, using real UART IRQ counters."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import selectors
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tools/cli'))
from a20_resource import Policy, Requirement, gate
from smoke import guest_slot_locks, _release_guest_slot_locks

def accepted(elapsed, irq_delta, poll_delta, exited, clean):
    return elapsed < 2 and irq_delta >= 1 and poll_delta == 0 and exited == 0 and clean


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--smp', type=int, choices=(1, 2), default=1)
    args = parser.parse_args()
    policy = Policy.from_env()
    gate(Requirement(1024 + policy.reserve_mem_mb, args.smp, policy.min_disk_mb),
         policy, ROOT, wait=False, label='RV64 UART no-reader SIGINT')
    args.output.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'smp': args.smp}
    data = bytearray()
    process = None
    try:
        symbols = subprocess.check_output(['riscv64-unknown-elf-nm', str(args.kernel)], text=True)
        addresses = {}
        for name in ('rx_irq_bytes', 'rx_polls_rx_bytes'):
            match = re.search(r'^([0-9a-f]+) [a-zA-Z] ' + name + r'$', symbols, re.M)
            if not match:
                raise ValueError('missing UART counter: ' + name)
            # RV64 virt's normal direct map; NOMMU is a separate profile.
            addresses[name] = int(match[1], 16) - 0xffffffc000000000
        with tempfile.TemporaryDirectory(prefix='a20-uart-') as temporary:
            qmp_path = Path(temporary) / 'qmp.sock'
            argv = ['qemu-system-riscv64', '-machine', 'virt', '-m', '1G',
                    '-smp', str(args.smp), '-bios', 'default', '-display', 'none',
                    '-serial', 'stdio', '-monitor', 'none', '-snapshot',
                    '-qmp', f'unix:{qmp_path},server=on,wait=off',
                    '-global', 'virtio-mmio.force-legacy=false', '-drive',
                    f'file={args.image.resolve()},if=none,format=raw,id=x0',
                    '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0',
                    '-kernel', str(args.kernel.resolve())]
            report['argv'] = argv
            process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, bufsize=0)
            poller = selectors.DefaultSelector()
            poller.register(process.stdout, selectors.EVENT_READ)

            def wait_for(pattern, timeout, start=0):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    if re.search(pattern, data[start:], re.M):
                        return
                    if process.poll() is not None:
                        break
                    for key, _ in poller.select(0.02):
                        chunk = key.fileobj.read(65536)
                        if chunk:
                            data.extend(chunk)
                raise TimeoutError(f'guest did not produce {pattern!r}')

            wait_for(rb'^# ', 45)
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as qmp:
                qmp.settimeout(3)
                qmp.connect(str(qmp_path))
                stream = qmp.makefile('rwb')
                json.loads(stream.readline())  # QMP greeting

                def command(execute, arguments=None):
                    request = {'execute': execute}
                    if arguments is not None:
                        request['arguments'] = arguments
                    stream.write(json.dumps(request).encode() + b'\n')
                    stream.flush()
                    while True:
                        reply = json.loads(stream.readline())
                        if 'error' in reply:
                            raise ValueError(reply['error'])
                        if 'return' in reply:
                            return reply['return']

                command('qmp_capabilities')

                def counters():
                    result = {}
                    for name, address in addresses.items():
                        reply = command('human-monitor-command',
                                        {'command-line': f'xp /1uw {address:#x}'})
                        match = re.search(r':\s*(\d+)', reply)
                        if not match:
                            raise ValueError('unreadable UART counter: ' + reply)
                        result[name] = int(match[1])
                    return result

                # The marker is split on input so terminal echo cannot prove
                # shell execution. The shell then waits for a job that never
                # reads stdin. A timeout kills this test, never passes it.
                start = len(data)
                process.stdin.write(b"echo 'UART_JOB_'START; /bin/sleep 60\n")
                process.stdin.flush()
                wait_for(rb'^UART_JOB_START\r?$', 5, start)
                time.sleep(0.2)
                before = counters()
                start = len(data)
                sent = time.monotonic()
                process.stdin.write(b'\x03')
                process.stdin.flush()
                wait_for(rb'(?:^|[\r\n])# ', 2, start)
                elapsed = time.monotonic() - sent
                after = counters()
                report.update(before=before, after=after, interrupt_seconds=elapsed)
            process.stdin.write(b'poweroff\n')
            process.stdin.flush()
            deadline = time.monotonic() + 10
            while process.poll() is None and time.monotonic() < deadline:
                for key, _ in poller.select(0.05):
                    chunk = key.fileobj.read(65536)
                    if chunk:
                        data.extend(chunk)
            exited = process.poll()
            clean = (b'[SD] POWER_OFF entry' in data and
                     not re.search(rb'PANIC|LOCK-STALL|Kernel BUG|SIGSEGV', data))
            if args.smp == 2:
                clean = clean and b'[SMP] 2/2 configured CPUs online' in data
            report['passed'] = accepted(elapsed,
                (after['rx_irq_bytes'] - before['rx_irq_bytes']) & 0xffffffff,
                (after['rx_polls_rx_bytes'] - before['rx_polls_rx_bytes']) & 0xffffffff,
                exited, clean)
            report['exit'] = exited
    except Exception as error:
        report['error'] = repr(error)
    finally:
        if process and process.poll() is None:
            process.kill()
            process.wait()
        (args.output / 'guest.log').write_bytes(data)
        report['kernel_sha256'] = hashlib.sha256(args.kernel.read_bytes()).hexdigest()
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    state, handles = guest_slot_locks()
    if state == 'busy':
        print('UART_CTRL_C: shared QEMU slot busy')
        raise SystemExit(75)
    try:
        raise SystemExit(main())
    finally:
        if handles is not None:
            _release_guest_slot_locks(handles)
