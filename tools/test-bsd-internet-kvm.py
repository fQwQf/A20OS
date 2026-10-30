#!/usr/bin/env python3
"""Boot a real A20OS BSD backend with virtio/SLIRP and verify public-socket
DNS, HTTP and HTTPS. This uses an unmodified Linux guest script, never the
private BSD loopback probe or host socket proxying."""
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
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--cpus', default='4,6')
parser.add_argument('--timeout', type=int, default=75)
parser.add_argument('--probe', type=Path, help='optional static bsd_net_probe ELF')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('net_kvm', root/'tools/bench-net-kvm.py')
net = importlib.util.module_from_spec(spec)
spec.loader.exec_module(net)
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=False)
kernel, fat, world = args.kernel.resolve(), args.fat.resolve(), args.world.resolve()
script = root/'tools/tests/bsd-internet.sh'
image = out/'world.img'
subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(world), str(image)], check=True)
net.install(image, script, '/etc/a20-benchmark.sh')
if args.probe:
    net.install(image, args.probe.resolve(), '/usr/bin/bsd-net-probe')
argv = ['taskset', '-c', args.cpus, 'qemu-system-x86_64', '-machine', 'q35',
        '-accel', 'kvm', '-cpu', 'host', '-m', '2G', '-smp', '2',
        '-display', 'none', '-monitor', 'none', '-serial', 'stdio', '-snapshot',
        '-no-reboot', '-kernel', str(kernel),
        '-drive', f'file={fat},if=none,format=raw,id=x0',
        '-device', 'virtio-blk-pci,drive=x0',
        '-drive', f'file={image},if=none,format=raw,id=xworld',
        '-device', 'virtio-blk-pci,drive=xworld', '-netdev', 'user,id=net',
        '-device', net.NIC, '-object', f'filter-dump,id=pcap,netdev=net,file={out/"bsd.pcap"}',
        '-append',
        'a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 '
        'a20.dns=10.0.2.3 a20.dhcp=0']
manifest = dict(argv=argv, passed=False,
                kernel_sha256=hashlib.sha256(kernel.read_bytes()).hexdigest(),
                fat_sha256=hashlib.sha256(fat.read_bytes()).hexdigest(),
                script_sha256=hashlib.sha256(script.read_bytes()).hexdigest())
guest = None
try:
    guest = net.Guest(argv, out/'serial.log')
    raw, _ = guest.until(rb'INTERNET_DONE\r*\n', args.timeout)
    text = raw.decode(errors='replace').replace('\r', '')
    (out/'results.txt').write_text(text)
    required = ['backend: bsd', 'INTERNET_HTTP_RC 0', 'INTERNET_HTTPS_RC 0']
    manifest['passed'] = all(marker in text for marker in required) and \
        text.count('Example Domain') >= 2 and '577' in text
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
    raise SystemExit(f'BSD_INTERNET_FAILED: {out/"serial.log"}')
print('BSD_INTERNET_PASS', out)
