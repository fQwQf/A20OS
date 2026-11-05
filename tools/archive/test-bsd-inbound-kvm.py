#!/usr/bin/env python3
"""Host-driven inbound TCP/UDP checks over QEMU user hostfwd.

This verifies that the BSD backend accepts real external connections through
the public socket ABI, not only loopback/listen smoke tests.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--kernel', type=Path, required=True)
parser.add_argument('--fat', type=Path, required=True)
parser.add_argument('--world', type=Path, required=True)
parser.add_argument('--binary', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--cpus', default='4,5,6,7')
parser.add_argument('--timeout', type=int, default=180)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('net_kvm', root/'tools/bench/bench-net-kvm.py')
net = importlib.util.module_from_spec(spec)
spec.loader.exec_module(net)
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=False)
kernel, fat, world, binary = (p.resolve() for p in
                              (args.kernel, args.fat, args.world, args.binary))
script = root/'tools/tests/bsd-inbound.sh'
image = out/'world.img'
subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(world), str(image)], check=True)
net.install(image, binary, '/usr/bin/net-microbench')
net.install(image, script, '/etc/a20-benchmark.sh')
argv = ['taskset', '-c', args.cpus, 'qemu-system-x86_64', '-machine', 'q35',
        '-accel', 'kvm', '-cpu', 'host', '-m', '2G', '-smp', '2',
        '-display', 'none', '-monitor', 'none', '-serial', 'stdio', '-snapshot',
        '-no-reboot', '-kernel', str(kernel),
        '-drive', f'file={fat},if=none,format=raw,id=x0',
        '-device', 'virtio-blk-pci,drive=x0',
        '-drive', f'file={image},if=none,format=raw,id=xworld',
        '-device', 'virtio-blk-pci,drive=xworld',
        '-netdev', 'user,id=net,hostfwd=tcp:127.0.0.1:39051-:39051,hostfwd=udp:127.0.0.1:39052-:39052',
        '-device', net.NIC,
        '-object', f'filter-dump,id=pcap,netdev=net,file={out/"bsd.pcap"}',
        '-append',
        'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 '
        'a20.dns=10.0.2.3 a20.dhcp=0']
manifest = dict(argv=argv, passed=False, cases=[],
                kernel_sha256=hashlib.sha256(kernel.read_bytes()).hexdigest(),
                world_sha256=hashlib.sha256(world.read_bytes()).hexdigest(),
                binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                script_sha256=hashlib.sha256(script.read_bytes()).hexdigest())
guest = None
try:
    guest = net.Guest(argv, out/'serial.log')
    cases = [
        ('tcp-client', 'BSD_INBOUND_TCP_READY',
         ['client', 'tcp', '39051', '127.0.0.1', '16384', '2']),
        ('tcp-multi', 'BSD_INBOUND_TCP_READY',
         ['multi', 'tcp', '39051', '127.0.0.1', '16384', '2']),
        ('udp-client', 'BSD_INBOUND_UDP_READY',
         ['client', 'udp', '39052', '127.0.0.1', '64', '2']),
        ('udp-multi', 'BSD_INBOUND_UDP_READY',
         ['multi', 'udp', '39052', '127.0.0.1', '64', '2']),
    ]
    consumed = set()
    for name, ready, command in cases:
        if ready not in consumed:
            guest.until((ready + r'\r*\n').encode(), 90)
            consumed.add(ready)
        cmd = ['taskset', '-c', args.cpus, str(binary), *command]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        output = result.stdout + result.stderr
        parsed = net.result(output)
        manifest['cases'].append(dict(name=name, argv=cmd, rc=result.returncode,
                                      result=parsed, output=output))
        manifest['passed'] = result.returncode == 0 and parsed and parsed.get('verified') == 1
        if not manifest['passed']:
            raise RuntimeError(f'inbound case failed: {name}: {output!r}')
    guest.until(rb'BSD_INBOUND_DONE\r*\n', args.timeout)
    text = (out/'serial.log').read_text(errors='replace').replace('\r', '')
    (out/'results.txt').write_text(text)
    manifest['passed'] = all(marker in text for marker in [
        'backend: bsd', 'BSD_INBOUND_DONE']) and all(
            case['result'] and case['result'].get('verified') == 1
            for case in manifest['cases'])
    if not manifest['passed']:
        print(text[-12000:])
except Exception as error:
    manifest['error'] = str(error)
    print(error)
finally:
    if guest:
        guest.close()
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2))
if not manifest['passed']:
    raise SystemExit(f'BSD_INBOUND_FAILED: {out/"serial.log"}')
print('BSD_INBOUND_PASS', out)
