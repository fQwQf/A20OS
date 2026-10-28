#!/usr/bin/env python3
"""Three isolated KVM nodes: Linux LAN clients -> A20OS router -> Linux WAN.

QEMU socket links require no host TAP, bridge, firewall, or root changes. The
two LAN clients bind different IPv4 addresses on the same Linux guest to the
same source port; this exercises real NAPT collision/reply ownership on wire.
"""
import argparse
import importlib.util
import json
import ipaddress
from pathlib import Path
import struct
import subprocess

spec = importlib.util.spec_from_file_location('bench', Path(__file__).with_name('bench-net-kvm.py'))
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)
wire_spec = importlib.util.spec_from_file_location('wire', Path(__file__).with_name('check-net-pcap.py'))
wire = importlib.util.module_from_spec(wire_spec)
wire_spec.loader.exec_module(wire)

INIT = '''#!/bin/sh
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
mkdir -p /proc /sys /dev /run /tmp
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
echo A20_BENCH_READY
/bin/sh /etc/a20-benchmark.sh
sleep 600
'''
WAN = '''#!/bin/sh
set -eux
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
ip link set lo up
ip link set eth0 up
ip addr add 10.20.0.2/24 dev eth0
ip addr add 198.51.100.5/32 dev lo
ip route add 192.168.10.0/24 via 10.20.0.1
ip -6 addr add fd42:20::2/64 dev eth0
ip -6 route add fd42:10::/64 via fd42:20::1
/usr/bin/net-microbench server tcp 39021 > /tmp/tcp.log 2>&1 &
/usr/bin/net-microbench server udp 39022 > /tmp/udp.log 2>&1 &
while ! grep -q SERVER_READY /tmp/tcp.log; do sleep .1; done
while ! grep -q SERVER_READY /tmp/udp.log; do sleep .1; done
echo WAN_READY
sleep 600
'''
ROUTER = '''#!/bin/sh
set -eux
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
ip addr replace 192.168.10.1/24 dev en2
ip addr replace 10.20.0.1/24 dev en3
ip route replace 192.168.10.0/24 dev en2
ip route replace 10.20.0.0/24 dev en3
ip route add 198.51.100.5/32 via 10.20.0.2 dev en3 table 100 metric 7
ip rule add priority 100 from 192.168.10.3/32 table 100
ip -6 addr add fd42:10::1/64 dev en2
ip -6 addr add fd42:20::1/64 dev en3
echo 'natadd out proto=tcp action=masquerade' > /proc/a20/netfilter
echo 'natadd out proto=udp action=masquerade' > /proc/a20/netfilter
ip addr show
ip route show
cat /proc/a20/netfilter
echo ROUTER_READY
sleep 600
'''
LAN = '''#!/bin/sh
set -eux
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
ip link set lo up
ip link set eth0 up
ip addr add 192.168.10.2/24 dev eth0
ip addr add 192.168.10.3/24 dev eth0
ip route add default via 192.168.10.1
ip -6 addr add fd42:10::2/64 dev eth0
ip -6 route add default via fd42:10::1
sleep 2
ping -c 2 -W 3 10.20.0.2
ping -6 -c 2 -W 3 fd42:20::2
/usr/bin/net-microbench client tcp 39021 10.20.0.2 16384 1
/usr/bin/net-microbench client udp 39022 10.20.0.2 64 1 192.168.10.2 53000 > /tmp/client2.log 2>&1 &
p2=$!
/usr/bin/net-microbench client udp 39022 10.20.0.2 64 1 192.168.10.3 53000 > /tmp/client3.log 2>&1 &
p3=$!
wait "$p2"
wait "$p3"
cat /tmp/client2.log /tmp/client3.log
# This destination exists only behind the gateway in policy table 100. The
# source selector must choose it; ARP must resolve the gateway, not the /32.
# TCP retains the accepted connection's local /32 address. A wildcard UDP
# sendto server would select eth0's primary source and change the peer tuple.
/usr/bin/net-microbench client tcp 39021 198.51.100.5 64 1 192.168.10.3 53001
if /usr/bin/net-microbench client tcp 39021 198.51.100.5 64 1 192.168.10.2 53002; then
  echo 'unexpected policy route for unmatched source'
  exit 1
fi
echo ROUTER_SUITE_PASS
sleep 600
'''


def verify_wan(path):
    ports, ttls, hops = set(), set(), set()
    policy_packets = 0
    public = ipaddress.ip_address('10.20.0.1').packed
    remote = ipaddress.ip_address('10.20.0.2').packed
    policy = ipaddress.ip_address('198.51.100.5').packed
    lan6 = ipaddress.ip_address('fd42:10::2').packed
    wan6 = ipaddress.ip_address('fd42:20::2').packed
    for frame in wire.ethernet_frames(path):
        if frame[12:14] == b'\x08\x00':
            ip = frame[14:]
            if ip[9] not in (6, 17):
                continue
            ihl = (ip[0] & 15) * 4
            sport, dport = struct.unpack_from('!HH', ip, ihl)
            if ip[12:16] == public and ip[16:20] in (remote, policy):
                ttls.add(ip[8])
                if ip[9] == 17 and dport == 39022:
                    ports.add(sport)
                if ip[9] == 6 and ip[16:20] == policy:
                    if sport == 53002:
                        raise ValueError('unmatched source escaped policy routing')
                    policy_packets += 1
            if ip[12:14] == bytes([192, 168]):
                raise ValueError('private LAN source escaped masquerade')
        elif frame[12:14] == b'\x86\xdd':
            ip = frame[14:]
            if ip[8:24] == lan6 and ip[24:40] == wan6:
                hops.add(ip[7])
    if len(ports) != 2 or 53000 not in ports or ttls != {63} or hops != {63} or not policy_packets:
        raise ValueError(f'forwarding evidence mismatch: {ports=} {ttls=} {hops=} {policy_packets=}')
    return dict(udp_mapping_ports=sorted(ports), ipv4_ttl=sorted(ttls),
                ipv6_hop_limit=sorted(hops), policy_packets=policy_packets)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', required=True, type=Path)
    parser.add_argument('--router-kernel', required=True, type=Path)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--lan-port', type=int, default=42123)
    parser.add_argument('--wan-port', type=int, default=42124)
    parser.add_argument('--router-cpus', default='4,6')
    parser.add_argument('--lan-cpus', default='8,10')
    parser.add_argument('--wan-cpus', default='12,14')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    out = args.out.resolve()
    base = json.loads(args.baseline.read_text())
    inputs = base['inputs']
    manifest = dict(inputs=inputs, router_sha256=bench.apk.sha(args.router_kernel),
                    binary_sha256=bench.apk.sha(args.binary), nodes=[])
    init = out / 'linux-init'
    init.write_text(INIT)
    init.chmod(0o755)
    guests = []

    def boot(name, script_text, affinity, links, linux):
        image = out / f'{name}.img'
        subprocess.run(['cp', '--reflink=auto', '--sparse=always', inputs['world']['path'], str(image)], check=True)
        script = out / f'{name}.sh'
        script.write_text(script_text)
        bench.install(image, script, '/etc/a20-benchmark.sh')
        bench.install(image, args.binary.resolve(), '/usr/bin/net-microbench')
        if linux:
            bench.install(image, init, '/etc/a20-linux-init')
        argv = ['taskset', '-c', affinity, 'qemu-system-x86_64', '-machine', 'q35',
                '-accel', 'kvm', '-cpu', 'host', '-no-reboot', '-m', '2G', '-smp', '2',
                '-display', 'none', '-monitor', 'none', '-serial', 'stdio', '-snapshot',
                '-kernel', inputs['linux_kernel']['path'] if linux else str(args.router_kernel.resolve()),
                '-drive', f'file={inputs["fat"]["path"]},if=none,format=raw,id=x0',
                '-device', 'virtio-blk-pci,drive=x0',
                '-drive', f'file={image},if=none,format=raw,id=xworld',
                '-device', 'virtio-blk-pci,drive=xworld']
        for i, link in enumerate(links):
            argv += ['-netdev', f'socket,id=net{i},{link}', '-device',
                     bench.NIC.replace('netdev=net,', f'netdev=net{i},') + f',mac=52:54:00:{len(guests):02x}:00:{i+1:02x}',
                     '-object', f'filter-dump,id=wire{i},netdev=net{i},file={out / f"{name}-{i}.pcap"}']
        if linux:
            argv += ['-initrd', inputs['linux_initrd']['path'], '-append',
                     'root=/dev/vdb rw init=/etc/a20-linux-init console=ttyS0 quiet net.ifnames=0']
        else:
            argv += ['-append', 'a20.ip=192.168.10.1 a20.netmask=255.255.255.0 a20.dhcp=0 a20.tcpmode=lwip']
        manifest['nodes'].append(dict(name=name, argv=argv, script_sha256=bench.apk.sha(script)))
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2))
        guest = bench.Guest(argv, out / f'{name}-serial.log')
        guests.append(guest)
        return guest

    try:
        wan = boot('wan', WAN, args.wan_cpus, [f'listen=127.0.0.1:{args.wan_port}'], True)
        wan.until(rb'(?:^|\n)WAN_READY\r*\n', 30)
        router = boot('router', ROUTER, args.router_cpus, [f'listen=127.0.0.1:{args.lan_port}', f'connect=127.0.0.1:{args.wan_port}'], False)
        router.until(rb'(?:^|\n)ROUTER_READY\r*\n', 30)
        lan = boot('lan', LAN, args.lan_cpus, [f'connect=127.0.0.1:{args.lan_port}'], True)
        lan.until(rb'(?:^|\n)ROUTER_SUITE_PASS\r*\n', 45)
        manifest['protocol_passed'] = True
    finally:
        for guest in reversed(guests):
            guest.close()
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    manifest['checksums'] = {}
    for pcap in sorted(out.glob('*.pcap')):
        checked = wire.check(pcap)
        manifest['checksums'][pcap.name] = checked
        if checked['errors']:
            raise ValueError(f'wire checksums failed: {pcap.name}')
    manifest['wan_evidence'] = verify_wan(out / 'router-1.pcap')
    manifest['passed'] = True
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    print('ROUTER_SUITE_PASS')
    for image in out.glob('*.img'):
        image.unlink()


if __name__ == '__main__':
    main()
