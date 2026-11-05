#!/usr/bin/env python3
"""Pack Kea, radvd and local Linux virtio modules into a DHCPv6-PD test peer."""
import argparse
import gzip
import hashlib
import json
import lzma
from pathlib import Path
import re
import subprocess
import tempfile


def build(args, root):
    inputs = {}

    def read(path):
        data = path.read_bytes()
        inputs[str(path.resolve())] = hashlib.sha256(data).hexdigest()
        return data

    def install(source, destination):
        path = root / destination.lstrip('/')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(read(source))
        path.chmod(source.stat().st_mode & 0o777)

    install(args.busybox, '/bin/busybox')
    for source, destination in [(args.kea, '/usr/sbin/kea-dhcp6'), (args.radvd, '/usr/sbin/radvd')]:
        install(source, destination)
        for line in subprocess.check_output(['ldd', str(source.resolve())], text=True).splitlines():
            if '=> not found' in line:
                raise ValueError(f'{source}: {line.strip()}')
            match = re.search(r'(\S+) => (/\S+) \(0x', line)
            if match:
                install(Path(match[2]), f'/lib64/{match[1]}')
            else:
                match = re.search(r'(/\S+) \(0x', line)
                if match:
                    install(Path(match[1]), match[1])

    for path in ('dev', 'etc', 'proc', 'sys', 'tmp', 'run/kea', 'modules', 'var/lib/kea', 'var/run/kea'):
        (root/path).mkdir(parents=True, exist_ok=True)
    for name in ('sh', 'mount', 'insmod', 'ip', 'sleep'):
        (root/'bin'/name).symlink_to('busybox')

    modules = subprocess.check_output([args.modprobe, '-S', args.kver,
                                       '--show-depends', 'virtio_net'], text=True)
    loads = []
    for line in modules.splitlines():
        if not line.startswith('insmod '):
            continue
        source = Path(line.split()[1])
        data = read(source)
        if source.suffix == '.xz':
            data = lzma.decompress(data)
        elif source.suffix == '.gz':
            data = gzip.decompress(data)
        elif source.suffix == '.zst':
            data = subprocess.check_output(['zstd', '-dc', str(source)])
        elif source.suffix != '.ko':
            raise ValueError(f'unsupported module: {source}')
        name = source.name.split('.ko', 1)[0] + '.ko'
        (root/'modules'/name).write_bytes(data)
        loads.append(f'insmod /modules/{name}')

    (root/'etc/kea-dhcp6.conf').write_text(json.dumps({'Dhcp6': {
        'interfaces-config': {'interfaces': ['eth0']},
        'lease-database': {'type': 'memfile', 'persist': False},
        'preferred-lifetime': 24,
        'valid-lifetime': 36,
        'renew-timer': 10,
        'rebind-timer': 20,
        'subnet6': [{
            'id': 1, 'interface': 'eth0', 'subnet': '2001:db8:20::/64',
            'pools': [{'pool': '2001:db8:20::10-2001:db8:20::1f'}],
            'pd-pools': [{'prefix': '2001:db8:400::', 'prefix-len': 48, 'delegated-len': 56}],
        }],
        'loggers': [{'name': 'kea-dhcp6', 'output_options': [{'output': 'stdout'}],
                     'severity': 'DEBUG', 'debuglevel': 40}],
    }}, indent=2) + '\n')
    (root/'etc/radvd.conf').write_text('''interface eth0 {
    AdvSendAdvert on;
    MinRtrAdvInterval 3;
    MaxRtrAdvInterval 4;
    prefix 2001:db8:20::/64 { AdvOnLink on; AdvAutonomous on; };
};
''')
    init = root/'init'
    init.write_text("""#!/bin/sh
set -eu
export PATH=/usr/sbin:/bin
export LD_LIBRARY_PATH=/lib64
mount -t proc proc /proc
mount -t devtmpfs devtmpfs /dev
mount -t sysfs sysfs /sys
""" + '\n'.join(loads) + """
echo 0 > /proc/sys/net/ipv6/conf/eth0/accept_dad
ip link set eth0 up
ip -6 addr add 2001:db8:20::1/64 dev eth0
ip -6 addr show dev eth0
echo 1 > /proc/sys/net/ipv6/conf/all/forwarding
sleep 2
kea-dhcp6 -c /etc/kea-dhcp6.conf &
radvd -n -C /etc/radvd.conf -m stderr &
echo A20_LINUX_DHCP6_PEER_READY
exec /bin/sh
""")
    init.chmod(0o755)
    files = sorted(str(p.relative_to(root)) for p in root.rglob('*'))
    archive = subprocess.check_output(['cpio', '--null', '--create', '--format=newc', '--quiet'],
                                      cwd=root, input=('\0'.join(files)+'\0').encode())
    args.out.mkdir(parents=True)
    image = args.out/'peer-initramfs.cpio.gz'
    image.write_bytes(gzip.compress(archive, mtime=0))
    read(image)
    (args.out/'inputs.json').write_text(json.dumps(inputs, indent=2)+'\n')
    print(image)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kea', type=Path, required=True)
    parser.add_argument('--radvd', type=Path, required=True)
    parser.add_argument('--busybox', type=Path, required=True, help='statically linked BusyBox')
    parser.add_argument('--kver', required=True)
    parser.add_argument('--modprobe', default='/usr/sbin/modprobe')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='a20-dhcp6-peer-') as directory:
        build(args, Path(directory))


if __name__ == '__main__':
    main()
