#!/usr/bin/env python3
"""Validate IPv4 source policy on three isolated F-Stack router interfaces."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import socket
import struct
import time

spec = importlib.util.spec_from_file_location(
    'pd', Path(__file__).with_name('test-bsd-dhcp6-pd.py'))
pd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pd)
wire = pd.ra.router

DESTINATION = socket.inet_aton('198.51.100.9')
SOURCE_B = socket.inet_aton('192.168.10.3')


def configure(guest, *lines):
    guest.send(' && '.join(lines) + ' && echo A20_POLICY_OK')
    output = guest.until('A20_POLICY_OK', timeout=12, whole_line=True)
    assert b'RTNETLINK answers:' not in output, output
    guest.until('# ')
    return output


def command(guest, text):
    return configure(guest, '/extra/busybox ip ' + text)


def expect_udp(link, src, dst, ident, payload, *, timeout=4, absent=False):
    deadline = time.monotonic() + timeout
    while True:
        try:
            frame = link.receive(deadline)
        except TimeoutError:
            if absent:
                return
            raise
        if frame[12:14] != wire.IPV4 or frame[18:20] != struct.pack('!H', ident):
            continue
        assert not absent, f'packet left the wrong interface: {link.router_mac.hex()}'
        assert frame[:12] == link.mac + link.router_mac
        ip = frame[14:]
        length = struct.unpack_from('!H', ip, 2)[0]
        assert ip[8:10] == bytes((63, 17)) and ip[12:20] == src + dst
        assert wire.checksum(ip[:20]) == 0
        udp = ip[20:length]
        assert udp[8:] == payload and wire.checksum(wire.pseudo(src, dst, len(udp)) + udp) == 0
        return


def exercise(guest, lan, wan_a, wan_b, result):
    for name, ip in [('en3', '10.20.0.1'), ('en4', '10.30.0.1')]:
        configure(guest, f'netctl set-ipv4 {name} {ip} {wire.MASK}')
    for link in (lan, wan_a, wan_b):
        link.resolve()
    command(guest, 'route add default via 10.20.0.2 dev en3')
    command(guest, 'route add 198.51.100.0/24 via 10.20.0.2 dev en3 table 100 metric 10')
    command(guest, 'route add 198.51.100.0/24 via 10.30.0.2 dev en4 table 100 metric 20')
    command(guest, 'route add 198.51.100.0/24 via 10.30.0.2 dev en4 table 200')
    command(guest, 'rule add priority 50 from 192.168.10.2/32 table 900')
    command(guest, 'rule add priority 100 from 192.168.10.2/32 table 100')
    command(guest, 'rule add priority 110 from 192.168.10.3/32 table 200')
    query = command(guest, 'route get 198.51.100.9 from 192.168.10.3')
    assert b'via 10.30.0.2 dev en4' in query, query

    def exchange(src, ident, expected, other):
        payload = b'policy-' + struct.pack('!H', ident)
        lan.fragment(DESTINATION, ident, wire.udp(src, DESTINATION, 32000, 42000, payload), src=src)
        expect_udp(expected, src, DESTINATION, ident, payload)
        expect_udp(other, src, DESTINATION, ident, payload, timeout=0.35, absent=True)

    exchange(lan.peer_ip, 0x7201, wan_a, wan_b)
    exchange(SOURCE_B, 0x7202, wan_b, wan_a)
    result['passed'].append('source_rules_select_distinct_native_fibs_with_empty_table_fallback')

    probe = wire.udp(lan.peer_ip, DESTINATION, 32000, 42000, b'policy-ttl')
    packet = bytearray(wire.ipv4(lan.peer_ip, DESTINATION, 0x72ff, probe))
    packet[8] = 1
    packet[10:12] = b'\0\0'
    struct.pack_into('!H', packet, 10, wire.checksum(bytes(packet[:20])))
    lan.send(wire.IPV4, bytes(packet))
    deadline = time.monotonic() + 4
    while True:
        frame = lan.receive(deadline)
        if frame[12:14] != wire.IPV4 or frame[23] != 1:
            continue
        ip = frame[14:]
        icmp = ip[20:struct.unpack_from('!H', ip, 2)[0]]
        assert ip[12:20] == lan.router_ip + lan.peer_ip
        assert icmp[:2] == bytes((11, 0)) and wire.checksum(icmp) == 0
        assert icmp[20:28] == lan.peer_ip + DESTINATION
        assert icmp[28:32] == probe[:4]
        break
    result['passed'].append('ttl_exceeded_returns_on_main_without_wan_route_to_lan')

    reply = b'policy-return'
    wan_b.fragment(lan.peer_ip, 0x7203, wire.udp(DESTINATION, lan.peer_ip, 42000, 32000, reply),
                   src=DESTINATION)
    expect_udp(lan, DESTINATION, lan.peer_ip, 0x7203, reply)
    result['passed'].append('reply_uses_main_connected_lan_route')

    command(guest, 'route add 198.51.100.9/32 via 10.30.0.2 dev en4 table 100')
    exchange(lan.peer_ip, 0x7204, wan_b, wan_a)
    command(guest, 'route del 198.51.100.9/32 table 100')
    exchange(lan.peer_ip, 0x7205, wan_a, wan_b)
    result['passed'].append('native_longest_prefix_overrides_and_then_restores_table_route')

    guest.send('/extra/busybox ip route add 198.51.100.0/24 via 10.99.0.2 dev en4 '
               'table 100 metric 30; echo A20_POLICY_STATUS=$?')
    output = guest.until('A20_POLICY_STATUS=2', whole_line=True)
    assert b'No route to host' in output, output
    guest.until('# ')
    table = command(guest, 'route show table 100')
    assert b'10.99.0.2' not in table, table
    result['passed'].append('off_link_metric_backup_rejected_without_changing_table')

    command(guest, 'route del 198.51.100.0/24 table 100 metric 10')
    exchange(lan.peer_ip, 0x7206, wan_b, wan_a)
    command(guest, 'route replace 198.51.100.0/24 via 10.20.0.2 dev en3 table 100 metric 20')
    exchange(lan.peer_ip, 0x7207, wan_a, wan_b)
    result['passed'].append('winner_deletion_promotes_metric_backup_and_replace_changes_egress')

    command(guest, 'rule del priority 110 from 192.168.10.3/32 table 200')
    exchange(SOURCE_B, 0x7208, wan_a, wan_b)
    result['passed'].append('removing_rule_restores_native_main_default')


def arguments(description):
    parser = argparse.ArgumentParser(description=description)
    for argument in ('kernel', 'busybox', 'out'):
        parser.add_argument('--' + argument, required=True, type=Path)
    return parser.parse_args()


def run_router(args, test, *, probes=()):
    out = args.out.resolve()
    out.mkdir(parents=True)
    image = pd.make_payload(out, args.busybox, False, None, extra_probes=probes)
    links = [wire.Link(1, '192.168.10.2', '192.168.10.1'),
             wire.Link(2, '10.20.0.2', '10.20.0.1'),
             wire.Link(3, '10.30.0.2', '10.30.0.1')]
    argv = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm', '-m', '1024',
            '-smp', '2', '-nographic', '-monitor', 'none', '-no-reboot',
            '-kernel', str(args.kernel.resolve()), '-append',
            'a20.ip=192.168.10.1 a20.netmask=255.255.255.0 a20.dhcp=0',
            '-drive', f'file={image},if=none,format=raw,id=payload',
            '-device', 'virtio-blk-pci,drive=payload']
    for index, link in enumerate(links):
        argv.extend(link.argv(index, out))
    result = dict(argv=argv, status='failed', passed=[],
                  kernel_sha256=hashlib.sha256(args.kernel.read_bytes()).hexdigest())
    with (out/'serial.log').open('wb') as log:
        guest = wire.backend.Guest(argv, log)
        try:
            guest.until('[INIT] entering scheduler...')
            configure(guest, 'true')
            test(guest, *links, result)
            guest.send('poweroff')
            guest.until('System is going down for power-off NOW')
            assert guest.process.wait(timeout=15) == 0
            result['status'] = 'passed'
        except Exception as error:
            result['error'] = repr(error)
            raise
        finally:
            guest.close()
            for link in links:
                link.sock.close()
            (out/'manifest.json').write_text(json.dumps(result, indent=2) + '\n')


def main():
    run_router(arguments(__doc__), exercise)
    print('BSD_POLICY_V4: PASS')


if __name__ == '__main__':
    main()
