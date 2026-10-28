#!/usr/bin/env python3
"""Same Linux ELF, fresh Linux/A20OS KVM boot per TCP population.

The host peer uses an event loop, without one process/thread per connection.
SLIRP is a controlled functional path, not a line-rate scalability claim.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import selectors
import socket
import subprocess
import threading

spec = importlib.util.spec_from_file_location('bench', Path(__file__).with_name('bench-net-kvm.py'))
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class EchoPeer:
    def __init__(self, port):
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(('127.0.0.1', port))
        self.listener.listen(1024)
        self.listener.setblocking(False)
        self.stop = threading.Event()
        self.error = None
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        selector = selectors.DefaultSelector()
        selector.register(self.listener, selectors.EVENT_READ, None)
        try:
            while not self.stop.is_set():
                for key, events in selector.select(.1):
                    connection, pending = key.fileobj, key.data
                    if pending is None:
                        accepted, _ = connection.accept()
                        accepted.setblocking(False)
                        selector.register(accepted, selectors.EVENT_READ, bytearray())
                        continue
                    if events & selectors.EVENT_READ:
                        try:
                            data = connection.recv(4096)
                        except BlockingIOError:
                            continue
                        except ConnectionResetError:
                            data = b''
                        if not data:
                            selector.unregister(connection)
                            connection.close()
                            continue
                        pending.extend(data)
                    if pending:
                        try:
                            sent = connection.send(pending)
                            del pending[:sent]
                        except BlockingIOError:
                            pass
                    selector.modify(connection, selectors.EVENT_READ |
                        (selectors.EVENT_WRITE if pending else 0), pending)
        except Exception as exc:
            self.error = repr(exc)
        finally:
            for key in list(selector.get_map().values()):
                key.fileobj.close()
            selector.close()

    def close(self):
        self.stop.set()
        self.thread.join()


def memory_snapshots(output):
    snapshots = {}
    for phase, body in re.findall(r'MEMORY_BEGIN (\w+)\n(.*?)MEMORY_END \1', output, re.S):
        values = {key: int(value) for key, value in
                  re.findall(r'^(MemTotal|MemFree|MemAvailable|Slab|SReclaimable|SUnreclaim):\s+(\d+) kB', body, re.M)}
        snapshots[phase] = dict(kB=values, raw=body)
    return snapshots


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', required=True, type=Path)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--counts', default='1,64,256,320')
    parser.add_argument('--sides', default='linux-kvm,a20-kvm')
    parser.add_argument('--cpus', default='4,6')
    parser.add_argument('--port', type=int, default=39031)
    parser.add_argument('--settle-seconds', type=float, default=3,
                        help='allow boot-time work to settle before each fresh probe')
    parser.add_argument('--a20-setup', type=Path,
                        help='explicit configuration ablation, copied and hashed')
    args = parser.parse_args()
    counts = [int(x) for x in args.counts.split(',')]
    if not counts or any(x < 1 or x > 8192 for x in counts):
        parser.error('counts must be in 1..8192')
    args.out.mkdir(parents=True, exist_ok=False)
    out = args.out.resolve()
    inputs = json.loads(args.baseline.read_text())['inputs']
    manifest = dict(inputs=inputs, kernel_sha256=bench.apk.sha(args.kernel),
        binary_sha256=bench.apk.sha(args.binary), counts=counts, boots=[], samples=[],
        settle_seconds=args.settle_seconds,
        memory_scope='Whole guest snapshots; closed includes TIME_WAIT and allocator caches',
        cpu_scope='Whole QEMU process, all threads, including VMM and proc snapshots')
    def save():
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    setup = args.a20_setup.read_text() if args.a20_setup else None
    if setup:
        (out / 'a20-setup.sh').write_text(setup)
        manifest['a20_setup_sha256'] = bench.apk.sha(args.a20_setup)
    save()
    peer = EchoPeer(args.port)
    try:
        for count in counts:
            for side in args.sides.split(','):
                if side not in ('linux-kvm', 'a20-kvm'):
                    raise ValueError(f'unknown side {side}')
                label = f'{side}-{count}'
                image = out / f'{label}.img'
                subprocess.run(['cp', '--reflink=auto', '--sparse=always', inputs['world']['path'], str(image)], check=True)
                lines = ['#!/bin/sh', 'export PATH=/usr/sbin:/usr/bin:/sbin:/bin',
                         'sha256sum /usr/bin/net-connection-probe']
                if side == 'linux-kvm':
                    lines += ['ip link set lo up', 'ip link set eth0 up',
                              'ip addr add 10.0.2.15/24 dev eth0', 'ip route add default via 10.0.2.2']
                elif setup:
                    lines += ['set -e', setup, 'set +e']
                lines += [f'sleep {args.settle_seconds}', "printf '\\nSCALE_BEGIN\\n'",
                    f'/usr/bin/net-connection-probe 10.0.2.2 {args.port} {count}',
                    'scale_rc=$?']
                if side == 'a20-kvm':
                    lines += ['cat /proc/a20/netfilter']
                lines += ['echo SCALE_END $scale_rc', 'sleep 600']
                script = out / f'{label}.sh'
                script.write_text('\n'.join(lines) + '\n')
                bench.install(image, script, '/etc/a20-benchmark.sh')
                bench.install(image, args.binary.resolve(), '/usr/bin/net-connection-probe')
                argv = ['taskset', '-c', args.cpus, 'qemu-system-x86_64', '-machine', 'q35',
                    '-accel', 'kvm', '-cpu', 'host', '-no-reboot', '-m', '2G', '-smp', '2',
                    '-display', 'none', '-monitor', 'none', '-serial', 'stdio', '-snapshot',
                    '-kernel', str(args.kernel.resolve()) if side == 'a20-kvm' else inputs['linux_kernel']['path'],
                    '-drive', f'file={inputs["fat"]["path"]},if=none,format=raw,id=x0',
                    '-device', 'virtio-blk-pci,drive=x0', '-drive', f'file={image},if=none,format=raw,id=xworld',
                    '-device', 'virtio-blk-pci,drive=xworld', '-netdev', 'user,id=net', '-device', bench.NIC]
                if side == 'linux-kvm':
                    init = out / 'linux-init'
                    init.write_text('#!/bin/sh\nexport PATH=/usr/sbin:/usr/bin:/sbin:/bin\nmkdir -p /proc /sys /dev /run /tmp\nmount -t proc proc /proc\nmount -t sysfs sysfs /sys\nmount -t devtmpfs devtmpfs /dev\necho A20_BENCH_READY\n/bin/sh /etc/a20-benchmark.sh\nsleep 600\n')
                    bench.install(image, init, '/etc/a20-linux-init')
                    argv += ['-initrd', inputs['linux_initrd']['path'], '-append',
                        'root=/dev/vdb rw init=/etc/a20-linux-init console=ttyS0 quiet net.ifnames=0']
                else:
                    argv += ['-append', 'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dhcp=0 a20.tcpmode=lwip']
                manifest['boots'].append(dict(side=side, count=count, argv=argv,
                                              script_sha256=bench.apk.sha(script)))
                save()
                guest = bench.Guest(argv, out / f'{label}-serial.log')
                try:
                    before, _ = guest.until(rb'(?:^|\n)SCALE_BEGIN\r*\n', 15)
                    if manifest['binary_sha256'].encode() not in before:
                        raise RuntimeError('guest binary fingerprint mismatch')
                    cpu = bench.process_cpu_ns(guest.p.pid)
                    raw, match = guest.until(rb'(?:^|\n)SCALE_END (\d+)\r*\n', 90)
                    output = raw.decode(errors='replace').replace('\r', '')
                    result = re.search(r'CONNECTION_RESULT requested=(\d+) established=(\d+) verified=(\d+) errno=(\d+) seconds=([\d.]+)', output)
                    row = dict(side=side, count=count, rc=int(match[1]),
                        qemu_cpu_ns=bench.process_cpu_ns(guest.p.pid)-cpu, memory=memory_snapshots(output))
                    if result:
                        row['result'] = dict(requested=int(result[1]), established=int(result[2]),
                            verified=int(result[3]), errno=int(result[4]), seconds=float(result[5]))
                    manifest['samples'].append(row)
                    save()
                    print(label, row.get('result'), flush=True)
                finally:
                    guest.close()
                image.unlink()
                if peer.error:
                    raise RuntimeError(peer.error)
    except BaseException as exc:
        manifest['error'] = repr(exc)
        raise
    finally:
        peer.close()
        save()
    failures = [r for r in manifest['samples'] if r['rc'] or not r.get('result', {}).get('verified')]
    manifest['failed_populations'] = [dict(side=r['side'], count=r['count']) for r in failures]
    save()
    if failures:
        raise SystemExit('Some populations could not be sustained; see preserved results')


if __name__ == '__main__':
    main()
