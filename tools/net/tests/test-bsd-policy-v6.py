#!/usr/bin/env python3
"""Validate IPv6 source/destination policy and scoped gateways on two WANs."""
import importlib.util
from pathlib import Path
import struct
import time

spec = importlib.util.spec_from_file_location(
    'policy4', Path(__file__).with_name('test-bsd-policy-v4.py'))
policy4 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy4)
ra = policy4.pd.ra
wire = policy4.wire

LAN_ROUTER = ra.ip('2001:db8:10::1')
SOURCE_A = ra.LAN_HOST
SOURCE_B = ra.ip('2001:db8:10:0:80::3')
DESTINATION = ra.ip('2001:db8:99::9')
OTHER_DESTINATION = ra.ip('2001:db8:98::9')
GATEWAY = ra.ip('fe80::2')


def command(guest, text):
    return policy4.command(guest, '-6 ' + text)


def udp(src, dst, data, sport=32000, dport=42000):
    packet = struct.pack('!4H', sport, dport, 8 + len(data), 0) + data
    csum = wire.checksum(ra.pseudo(src, dst, 17, len(packet)) + packet) or 0xffff
    return packet[:6] + struct.pack('!H', csum) + packet[8:]


def expect_udp(link, src, dst, packet, *, neighbor):
    def match(frame, protocol, hops, source, destination, data):
        if protocol != 17 or data != packet:
            return False
        assert frame[:12] == link.mac + link.router_mac
        assert (hops, source, destination) == (63, src, dst)
        assert wire.checksum(ra.pseudo(src, dst, 17, len(data)) + data) == 0
        return True
    ra.receive(link, 5, match, neighbor=neighbor)


def exercise(guest, lan, wan_a, wan_b, result):
    policy4.configure(guest, 'netctl set-ipv6 en2 2001:db8:10::1 64')
    time.sleep(1.5)  # Wait for Duplicate Address Detection on the static LAN address.
    command(guest, 'route add default via fe80::2 dev en3')
    command(guest, 'route add 2001:db8:99::/64 via fe80::2 dev en3 table 100 metric 10')
    command(guest, 'route add 2001:db8:99::/64 via fe80::2 dev en4 table 100 metric 20')
    command(guest, 'route add 2001:db8:99::/64 via fe80::2 dev en4 table 200')
    command(guest, 'rule add priority 50 from 2001:db8:10::2/128 table 900')
    command(guest, 'rule add priority 100 from 2001:db8:10::2/128 table 100')
    command(guest, 'rule add priority 110 from 2001:db8:10:0:80::/73 to 2001:db8:99::/64 table 200')
    query = command(guest, 'route get 2001:db8:99::9 from 2001:db8:10:0:80::3')
    assert b'via fe80::2 dev en4' in query, query
    snapshot = command(guest, 'route show table 200')
    assert b'2001:db8:99::/64 via fe80::2 dev en4' in snapshot, snapshot
    rules = command(guest, 'rule show')
    assert b'lookup 200' in rules and b'/73' in rules, rules
    result['passed'].append('rtnetlink_source_lookup_and_ipv6_dumps_use_table_ids')

    def exchange(src, tag, expected, other, dst=DESTINATION):
        packet = udp(src, dst, b'ipv6-policy-' + tag)
        ra.send(lan, src, dst, 17, packet, hops=64)
        expect_udp(expected, src, dst, packet, neighbor=GATEWAY)
        ra.absent(other, 0.35, lambda f, p, h, s, d, b: p == 17 and b == packet)

    exchange(SOURCE_A, b'wan-a', wan_a, wan_b)
    exchange(SOURCE_B, b'wan-b', wan_b, wan_a)
    exchange(SOURCE_B, b'dst-selector', wan_a, wan_b, dst=OTHER_DESTINATION)
    result['passed'].append('two_wans_with_same_link_local_gateway_and_source_destination_rules')

    reply = udp(DESTINATION, SOURCE_A, b'ipv6-policy-return', sport=42000, dport=32000)
    ra.send(wan_b, DESTINATION, SOURCE_A, 17, reply, hops=64)
    expect_udp(lan, DESTINATION, SOURCE_A, reply, neighbor=SOURCE_A)
    result['passed'].append('return_uses_main_connected_lan_route')

    expired = udp(SOURCE_A, DESTINATION, b'ipv6-policy-expired')
    ra.send(lan, SOURCE_A, DESTINATION, 17, expired, hops=1)
    ra.receive(lan, 5, lambda f, p, h, s, d, b:
               p == ra.ICMP6 and b[:2] == bytes((3, 0)) and s == LAN_ROUTER and d == SOURCE_A
               and b[16:48] == SOURCE_A + DESTINATION and b[48:52] == expired[:4]
               and wire.checksum(ra.pseudo(s, d, p, len(b)) + b) == 0, neighbor=SOURCE_A)
    result['passed'].append('hop_limit_error_returns_on_main')

    command(guest, 'route add 2001:db8:99::9/128 via fe80::2 dev en4 table 100')
    exchange(SOURCE_A, b'host-route', wan_b, wan_a)
    command(guest, 'route del 2001:db8:99::9/128 table 100')
    exchange(SOURCE_A, b'host-route-removed', wan_a, wan_b)
    command(guest, 'route del 2001:db8:99::/64 table 100 metric 10')
    exchange(SOURCE_A, b'backup', wan_b, wan_a)
    command(guest, 'route replace 2001:db8:99::/64 via fe80::2 dev en3 table 100 metric 20')
    exchange(SOURCE_A, b'replacement', wan_a, wan_b)
    result['passed'].append('longest_prefix_metric_backup_and_replacement_change_wire_egress')

    guest.send('/extra/busybox ip -6 route add 2001:db8:99::/64 via 2001:db8:ff::2 dev en4 '
               'table 100 metric 30; echo A20_POLICY_STATUS=$?')
    output = guest.until('A20_POLICY_STATUS=2', timeout=12, whole_line=True)
    assert b'No route to host' in output, output
    guest.until('# ')
    assert b'2001:db8:ff::2' not in command(guest, 'route show table 100')
    result['passed'].append('off_link_gateway_rejected_without_changing_route_list')

    policy4.configure(guest, 'netctl set-ipv4 en3 10.20.0.1 255.255.255.0')
    policy4.command(guest, 'route add 198.51.100.0/24 via 10.20.0.2 dev en3 table 200')
    policy4.command(guest, 'route del 198.51.100.0/24 table 200')
    exchange(SOURCE_B, b'ipv4-table-delete', wan_b, wan_a)
    command(guest, 'rule del priority 110 from 2001:db8:10:0:80::/73 to 2001:db8:99::/64 table 200')
    exchange(SOURCE_B, b'rule-removed', wan_a, wan_b)
    result['passed'].append('table_lifetime_is_shared_across_families_and_rule_deletion_restores_main')


def main():
    policy4.run_router(policy4.arguments(__doc__), exercise)
    print('BSD_POLICY_V6: PASS')


if __name__ == '__main__':
    main()
