#!/usr/bin/env python3
"""Pack a local Linux pppoe-server and pppd into an isolated test initramfs."""
import argparse
import gzip
import hashlib
import json
import lzma
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def build(args, root):
    inputs = {}

    def remember(path):
        inputs[str(path.resolve())] = hashlib.sha256(path.read_bytes()).hexdigest()

    def put(src, destination):
        remember(src)
        target = root / destination.lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, target)
        target.chmod(src.stat().st_mode & 0o777)

    for source, destination in [
        (args.busybox, '/bin/busybox'), (args.pppd, '/usr/sbin/pppd'),
        (args.server, '/usr/sbin/pppoe-server'), (args.plugin, '/lib/pppoe.so'),
        (args.legacy_provider, '/lib/openssl/legacy.so'),
    ]:
        put(source, destination)
        for line in subprocess.check_output(['ldd', str(source)], text=True).splitlines():
            match = re.search(r'(/\S+) \(0x', line)
            if match:
                put(Path(match[1]), match[1])

    if args.kea:
        put(args.kea, '/usr/sbin/kea-dhcp6')
        for line in subprocess.check_output(['ldd', str(args.kea)], text=True).splitlines():
            if '=> not found' in line:
                raise ValueError(f'{args.kea}: {line.strip()}')
            match = re.search(r'(\S+) => (/\S+) \(0x', line)
            if match:
                put(Path(match[2]), f'/lib/kea/{match[1]}')
            else:
                match = re.search(r'(/\S+) \(0x', line)
                if match:
                    put(Path(match[1]), match[1])

        for directory in ('etc/ppp', 'run/kea', 'var/lib/kea', 'var/run/kea'):
            (root/directory).mkdir(parents=True, exist_ok=True)
        configuration = {'Dhcp6': {
            'interfaces-config': {'interfaces': ['ppp0']},
            'lease-database': {'type': 'memfile', 'persist': False},
            'preferred-lifetime': 24, 'valid-lifetime': 36,
            'renew-timer': 10, 'rebind-timer': 20,
            'subnet6': [{'id': 1, 'interface': 'ppp0', 'subnet': '2001:db8:20::/64',
                         'pd-pools': [{'prefix': '2001:db8:POOL::',
                                       'prefix-len': 48, 'delegated-len': 56}]}],
            'loggers': [{'name': 'kea-dhcp6', 'output_options': [{'output': 'stdout'}],
                         'severity': 'INFO'}],
        }}
        path = root/'etc/ppp/kea-pd.json'
        path.write_text(json.dumps(configuration))
        start_pd = root/'bin/pppoe-pd-start'
        start_pd.write_text("""#!/bin/sh
set -eu
case "$1" in 400|500) pool="$1" ;; *) exit 2 ;; esac
ip -6 addr replace 2001:db8:30::1/128 dev lo
# Kea 2.6 reads IFA_ADDRESS rather than IFA_LOCAL for the PPP address
# pair. Keep the negotiated local address in the normal single form.
ip -6 addr flush dev ppp0 scope link
ip -6 addr add fe80::1/64 dev ppp0
ip -6 addr replace 2001:db8:20::1/64 dev ppp0
ip -6 route replace "2001:db8:$pool::/56" dev ppp0
busybox sed "s/POOL/$pool/g" /etc/ppp/kea-pd.json > /run/kea-pd.json
export LD_LIBRARY_PATH=/lib/kea
exec /usr/sbin/kea-dhcp6 -c /run/kea-pd.json
""")
        start_pd.chmod(0o755)

    for name in ('sh', 'mount', 'mknod', 'insmod', 'ip', 'sleep'):
        (root/'bin'/name).symlink_to('busybox')
    for name in ('etc/ppp', 'dev', 'proc', 'sys', 'tmp', 'run', 'modules', 'var/run'):
        (root/name).mkdir(parents=True, exist_ok=True)
    loads = []
    for module in ('virtio_net', 'pppoe'):
        deps = subprocess.check_output([args.modprobe, '-S', args.kver, '--show-depends', module],
                                       text=True)
        for line in deps.splitlines():
            if not line.startswith('insmod '):
                continue
            src = Path(line.split()[1])
            remember(src)
            raw = src.read_bytes()
            if src.suffix == '.xz':
                raw = lzma.decompress(raw)
            elif src.suffix == '.zst':
                raw = subprocess.check_output(['zstd', '-dc', str(src)])
            elif src.suffix == '.gz':
                raw = gzip.decompress(raw)
            elif src.suffix != '.ko':
                raise ValueError(f'unsupported module: {src}')
            name = src.name.split('.ko', 1)[0] + '.ko'
            if (root/'modules'/name).exists():
                continue
            (root/'modules'/name).write_bytes(raw)
            loads.append(f'insmod /modules/{name}')

    for kind in ('pap', 'chap'):
        path = root/'etc/ppp'/f'{kind}-secrets'
        path.write_text('a20-client a20-peer a20-test-secret *\n')
        path.chmod(0o600)

    echo = ROOT/'tools/bench/net-microbench.c'
    remember(echo)
    subprocess.run([args.cc, '-static', '-O2', '-pthread', '-o',
                    str(root/'bin/net-microbench'), str(echo)], check=True)
    start = root/'bin/pppoe-peer-start'
    start.write_text("""#!/bin/sh
export OPENSSL_MODULES=/lib/openssl
exec /usr/sbin/pppoe-server -F -k -q /usr/sbin/pppd -I eth0 -N 1 -L 10.20.0.2 \\
    -R "${1:-198.18.0.10}" -C a20-peer -S test -O /etc/ppp/peer-options -g /lib/pppoe.so
""")
    start.chmod(0o755)
    init = root/'init'
    init.write_text("""#!/bin/sh
set -eu
export PATH=/usr/sbin:/bin
mount -t proc proc /proc
mount -t devtmpfs devtmpfs /dev
mount -t sysfs sysfs /sys
""" + '\n'.join(loads) + """
busybox syslogd -O /dev/console
[ -c /dev/ppp ] || mknod /dev/ppp c 108 0
ip link set eth0 up
case "$(busybox cat /proc/cmdline)" in
    *pppauth=chap*) auth=require-chap ;;
    *) auth=require-pap ;;
esac
printf '%s\\n' "$auth" 'name a20-peer' 'debug' 'hide-password' 'logfd 2' > /etc/ppp/peer-options
case "$(busybox cat /proc/cmdline)" in
    *pppdual=1*)
        printf '%s\\n' '+ipv6' 'ipv6 ::1,::2' 'ms-dns 192.0.2.53' 'ms-dns 192.0.2.54' >> /etc/ppp/peer-options
        ;;
    *) echo noipv6 >> /etc/ppp/peer-options ;;
esac
net-microbench server udp 42000 &
pppoe-peer-start &
echo A20_LINUX_PPPOE_PEER_READY
exec /bin/sh
""")
    init.chmod(0o755)
    files = sorted(str(p.relative_to(root)) for p in root.rglob('*'))
    archive = subprocess.check_output(['cpio', '--null', '--create', '--format=newc', '--quiet'],
                                      cwd=root, input=('\0'.join(files)+'\0').encode())
    args.out.mkdir(parents=True, exist_ok=False)
    image = args.out/'peer-initramfs.cpio.gz'
    image.write_bytes(gzip.compress(archive, mtime=0))
    remember(image)
    (args.out/'inputs.json').write_text(json.dumps(inputs, indent=2)+'\n')
    print(image)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--plugin', type=Path, required=True)
    parser.add_argument('--kver', required=True)
    parser.add_argument('--legacy-provider', type=Path, required=True)
    parser.add_argument('--pppd', type=Path, default=Path('/usr/sbin/pppd'))
    parser.add_argument('--kea', type=Path, help='include a DHCPv6-PD server for PPP')
    parser.add_argument('--busybox', type=Path, default=Path('/bin/busybox'))
    parser.add_argument('--modprobe', default='/usr/sbin/modprobe')
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='a20-pppoe-peer-') as directory:
        build(args, Path(directory))


if __name__ == '__main__':
    main()
