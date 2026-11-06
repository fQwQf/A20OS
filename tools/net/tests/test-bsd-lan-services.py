#!/usr/bin/env python3
"""Exercise stock dnsmasq DHCPv4 and DNS forwarding on a F-Stack LAN."""
import argparse
from contextlib import ExitStack
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import tempfile

spec = importlib.util.spec_from_file_location(
    'lan_test', Path(__file__).with_name('test-bsd-pd-lan.py'))
lan_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lan_test)
pd = lan_test.pd
wire = pd.ra.router

LEASE_SCRIPT = '''#!/bin/sh
set -eu
case "$1" in
bound|renew)
    ip addr replace "$ip/24" dev "$interface"
    ip route replace default via "$router"
    printf '%s %s %s\n' "$ip" "$router" "$dns" >/tmp/lease
    ;;
esac
'''
DNSMASQ = ('--conf-file=/dev/null --no-daemon --user=root --group=root --no-resolv '
           '--no-hosts --bind-interfaces --log-facility=-')


def peer_initrd(args, out):
    image = out/'peer.cpio.gz'
    with tempfile.TemporaryDirectory(prefix='a20-lan-services-') as directory:
        root = Path(directory)
        shutil.copy2(args.dnsmasq, root/'dnsmasq')
        (root/'lease-script').write_text(LEASE_SCRIPT)
        (root/'lease-script').chmod(0o755)
        (root/'etc').mkdir()
        (root/'etc/passwd').write_text('root:x:0:0:root:/:/bin/sh\n')
        (root/'etc/group').write_text('root:x:0:\n')
        overlay = subprocess.run(['cpio', '--quiet', '-o', '-H', 'newc'], cwd=root,
                                 input=b'dnsmasq\nlease-script\netc/passwd\netc/group\n', capture_output=True, check=True).stdout
    shutil.copyfile(args.peer_initrd, image)
    with image.open('ab') as file:
        file.write(gzip.compress(overlay, mtime=0))
    return image


def shell(guest, line, peers=(), timeout=15):
    rc, out = lan_test.command(guest, line, peers, timeout)
    assert rc == 0, (line, rc, out[-1600:])
    return out


def udp_frames(path):
    for _, frame in lan_test.frames(path):
        if len(frame) < 42 or frame[12:14] != wire.IPV4 or frame[23] != 17:
            continue
        ip = frame[14:]
        size = struct.unpack_from('!H', ip, 2)[0]
        start = (ip[0] & 15) * 4
        if size < start + 8 or size > len(ip):
            continue
        udp = ip[start:size]
        yield ip[12:16], ip[16:20], *struct.unpack_from('!HH', udp), udp[8:]


def dhcp_type(payload):
    options = payload[240:]
    while options and options[0] != 255:
        tag = options[0]
        if tag == 0:
            options = options[1:]
            continue
        assert len(options) >= 2 and len(options) >= 2 + options[1], payload
        if tag == 53 and options[1] == 1:
            return options[2]
        options = options[2+options[1]:]
    return None


def exercise(router, wan, lan, out, result):
    wan.until('A20_LINUX_LAN_READY')
    lan.until('A20_LINUX_LAN_READY')
    shell(wan, 'ip link set lo up && ip addr add 10.20.0.2/24 dev eth0')
    wan.send(f'/dnsmasq {DNSMASQ} --interface=eth0 --host-record=upstream.test,203.0.113.55 '
             '--host-record=restart.test,203.0.113.56 &')
    wan.until('started, version', timeout=15)
    router.until('[INIT] entering scheduler...')
    shell(router, 'netctl set-ipv4 en3 10.20.0.1 255.255.255.0', (wan, lan))
    service = (f'/extra/dnsmasq {DNSMASQ} --interface=en2 --server=10.20.0.2 --log-dhcp '
               '--dhcp-range=192.168.10.50,192.168.10.60,255.255.255.0,1h '
               '--dhcp-option=3,192.168.10.1 --dhcp-option=6,192.168.10.1 '
               '--dhcp-leasefile=/extra/dnsmasq.leases --pid-file=/tmp/a20-dnsmasq.pid')

    def start():
        router.send(service + ' & dns_pid=$!')
        router.until('started, version', timeout=15, drain_peer=(wan, lan))

    def lease():
        value = shell(lan, 'busybox udhcpc -i eth0 -q -f -n -t 3 -T 2 -s /lease-script && '
                      'cat /tmp/lease && ip -4 route show', (router, wan))
        match = re.search(rb'(?m)^(192\.168\.10\.\d+) 192\.168\.10\.1 192\.168\.10\.1\r?$', value)
        assert match and 50 <= int(match[1].split(b'.')[-1]) <= 60, value
        assert b'default via 192.168.10.1' in value, value
        return match[1].decode()

    def lookup(name, address):
        value = shell(lan, f'busybox nslookup -type=A {name} 192.168.10.1', (router, wan))
        assert address.encode() in value, value

    start()
    first = lease()
    exchanges = {}
    for _, _, source_port, destination_port, payload in udp_frames(out/'lan.pcap'):
        if {source_port, destination_port} == {67, 68} and payload[236:240] == b'\x63\x82\x53\x63':
            exchanges.setdefault(payload[4:8], set()).add(dhcp_type(payload))
    assert any({1, 2, 3, 5} <= kinds for kinds in exchanges.values()), exchanges
    result['lease'] = first
    result['passed'].append('stock_dnsmasq_dora_supplies_linux_lan_address_gateway_and_dns')
    lookup('upstream.test', '203.0.113.55')
    result['passed'].append('linux_lan_resolves_name_via_stock_dnsmasq_forwarder')

    persisted = shell(router, 'cat /extra/dnsmasq.leases', (wan, lan))
    assert first.encode() in persisted, persisted
    shell(router, 'kill "$dns_pid"; wait "$dns_pid"', (wan, lan))
    shell(lan, 'ip -4 addr flush dev eth0', (wan, router))
    start()
    assert lease() == first
    lookup('restart.test', '203.0.113.56')
    result['passed'].append('dnsmasq_restart_preserves_lease_and_restores_dns')
    query_src, server = map(socket.inet_aton, ('10.20.0.1', '10.20.0.2'))
    observed = list(udp_frames(out/'wan.pcap'))
    for name in (b'upstream', b'restart'):
        question = bytes((len(name),)) + name + b'\x04test\x00'
        assert any(src == query_src and dst == server and dport == 53 and question in data
                   for src, dst, _, dport, data in observed)
        assert any(src == server and dst == query_src and sport == 53 and question in data
                   for src, dst, sport, _, data in observed)
    result['passed'].append('wan_capture_confirms_both_dns_queries_and_replies')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('kernel', 'linux', 'peer-initrd', 'busybox', 'dnsmasq', 'out'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True)
    payload = pd.make_payload(out, args.busybox, False, None, executables={'dnsmasq': args.dnsmasq})
    initrd = peer_initrd(args, out)
    a, b, c, d = wire.backend.reserve_ports(socket.SOCK_DGRAM, 4)
    common = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm', '-m', '768',
              '-smp', '2', '-nographic', '-monitor', 'none', '-no-reboot']
    linux = [*common, '-cpu', 'host', '-kernel', str(args.linux.resolve()), '-initrd', str(initrd),
             '-append', 'console=ttyS0 quiet net.ifnames=0 a20_lan_peer=1']

    def link(number, local, remote, mac, capture):
        return [value.replace('id=wan', f'id=nic{number}').replace('netdev=wan', f'netdev=nic{number}')
                for value in pd.pppoe.wan_args(local, remote, mac, out/capture)]

    nodes = {
        'router': [*common, '-kernel', str(args.kernel.resolve()),
                   '-append', 'a20.ip=192.168.10.1 a20.netmask=255.255.255.0 a20.dhcp=0',
                   *link(0, a, b, '52:54:00:01:00:01', 'lan.pcap'),
                   *link(1, c, d, '52:54:00:02:00:01', 'wan.pcap'),
                   '-drive', f'file={payload},if=none,format=raw,id=payload',
                   '-device', 'virtio-blk-pci,drive=payload'],
        'wan': [*linux, *link(0, d, c, '52:54:00:02:00:02', 'wan-peer.pcap')],
        'lan': [*linux, *link(0, b, a, '52:54:00:01:00:02', 'lan-peer.pcap')],
    }
    result = dict(status='failed', passed=[], argv=nodes,
                  sha256={str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (args.kernel, args.linux, initrd, args.busybox, args.dnsmasq)})
    with ExitStack() as stack:
        guests = []
        try:
            for name, argv in nodes.items():
                guest = wire.backend.Guest(argv, stack.enter_context((out/f'{name}.log').open('wb')))
                stack.callback(guest.close)
                guests.append(guest)
            exercise(*guests, out, result)
            guests[0].send('poweroff')
            guests[0].until('System is going down for power-off NOW')
            assert guests[0].process.wait(timeout=15) == 0
            result['status'] = 'passed'
        except BaseException as error:
            result['error'] = repr(error)
            raise
        finally:
            (out/'manifest.json').write_text(json.dumps(result, indent=2)+'\n')
    print('BSD_LAN_SERVICES: PASS')


if __name__ == '__main__':
    main()
