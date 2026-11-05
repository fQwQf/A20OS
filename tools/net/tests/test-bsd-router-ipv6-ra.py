#!/usr/bin/env python3
"""Exercise IPv6 router discovery on the WAN of a dual-link BSD router."""
import argparse
import importlib.util
from pathlib import Path
import socket
import struct
import time

spec = importlib.util.spec_from_file_location(
    'router', Path(__file__).with_name('test-bsd-router-fragments.py'))
router = importlib.util.module_from_spec(spec)
spec.loader.exec_module(router)

IPV6 = b'\x86\xdd'
ICMP6 = 58


def ip(text):
    return socket.inet_pton(socket.AF_INET6, text)


ALL_NODES = ip('ff02::1')
WAN_ROUTER = ip('fe80::1')
WAN_PREFIX = ip('2001:db8:20::')
ROGUE_PREFIX = ip('2001:db8:bad::')
OFF_LINK = ip('2001:db8:30::2')
LAN_HOST = ip('2001:db8:10::2')


def pseudo(src, dst, protocol, length):
    return src + dst + struct.pack('!I3xB', length, protocol)


def send(link, src, dst, protocol, payload, *, hops=255, mac=None):
    header = struct.pack('!IHBB16s16s', 6 << 28, len(payload), protocol, hops, src, dst)
    link.send(IPV6, header + payload, mac)


def icmp(link, src, dst, kind, body, *, hops=255, mac=None):
    data = struct.pack('!BBH', kind, 0, 0) + body
    csum = router.checksum(pseudo(src, dst, ICMP6, len(data)) + data)
    send(link, src, dst, ICMP6, data[:2] + struct.pack('!H', csum) + data[4:],
         hops=hops, mac=mac)


def advertise(link, prefix, *, lifetime=30, hops=255):
    body = struct.pack('!BBHII', 64, 0, lifetime, 0, 0)
    body += b'\x01\x01' + link.mac
    if prefix is not None:
        body += struct.pack('!BBBBIII16s', 3, 4, 64, 0xc0, 60, 30, 0, prefix)
    icmp(link, WAN_ROUTER, ALL_NODES, 134, body, hops=hops,
         mac=bytes.fromhex('33 33 00 00 00 01'))


def receive(link, timeout, match, *, neighbor=None):
    deadline = time.monotonic() + timeout
    while True:
        frame = link.receive(deadline)
        if frame[12:14] != IPV6 or len(frame) < 54:
            continue
        size, protocol, hops, src, dst = struct.unpack('!HBB16s16s', frame[18:54])
        payload = frame[54:54 + size]
        if len(payload) != size:
            continue
        if protocol == ICMP6 and payload[:1] == b'\x87' and any(src):
            target = payload[8:24]
            if target == neighbor:
                flags = 0x60 if target == LAN_HOST else 0xe0
                icmp(link, target, src, 136, bytes([flags, 0, 0, 0]) + target +
                     b'\x02\x01' + link.mac)
        if match(frame, protocol, hops, src, dst, payload):
            return frame, payload


def dad_for(prefix):
    return lambda f, p, h, s, d, b: (
        p == ICMP6 and s == bytes(16) and b[:1] == b'\x87' and b[8:16] == prefix[:8])


def absent(link, timeout, match):
    try:
        receive(link, timeout, match)
    except TimeoutError:
        return
    raise AssertionError('unexpected IPv6 frame')


def exercise(guest, lan, wan, result):
    guest.send('netctl set-ipv6 en2 2001:db8:10::1 64')
    guest.until('NETCTL: IPv6 configured', whole_line=True)
    guest.until('# ')
    advertise(lan, ROGUE_PREFIX)
    absent(lan, 1.5, dad_for(ROGUE_PREFIX))
    result['passed'].append('lan_ra_ignored')
    advertise(wan, WAN_PREFIX, hops=254)
    absent(wan, 1.5, dad_for(WAN_PREFIX))
    result['passed'].append('bad_hop_limit_ignored')
    advertise(wan, WAN_PREFIX)
    _, solicitation = receive(wan, 5, dad_for(WAN_PREFIX))
    address = solicitation[8:24]
    assert address == WAN_PREFIX[:8] + bytes.fromhex('505400fffe010002')
    result['passed'].append('wan_ra_triggers_slaac_dad')

    # Send an echo request to the learned address after DAD completes.
    echo = b'\x73\x11\x00\x01router-slaac'
    time.sleep(2)
    icmp(wan, WAN_ROUTER, address, 128, echo, hops=64)
    receive(wan, 5, lambda f, p, h, s, d, b:
            p == ICMP6 and s == address and d == WAN_ROUTER and b[:1] == b'\x81'
            and b[4:] == echo and not router.checksum(pseudo(s, d, p, len(b)) + b))
    result['passed'].append('wan_slaac_address_reachable')

    body = b'routed IPv6 through the RA default'
    udp = struct.pack('!4H', 32000, 42000, 8 + len(body), 0) + body
    csum = router.checksum(pseudo(LAN_HOST, OFF_LINK, 17, len(udp)) + udp) or 0xffff
    udp = udp[:6] + struct.pack('!H', csum) + udp[8:]

    def forwarded(frame, protocol, hops, src, dst, payload):
        return (protocol == 17 and hops == 63 and frame[:6] == wan.mac
                and src == LAN_HOST and dst == OFF_LINK and payload == udp)

    send(lan, LAN_HOST, OFF_LINK, 17, udp, hops=64)
    receive(wan, 5, forwarded, neighbor=WAN_ROUTER)
    result['passed'].append('wan_ra_default_forwards_lan')

    icmp(wan, OFF_LINK, LAN_HOST, 128, echo, hops=64)
    receive(lan, 5, lambda f, p, h, s, d, b:
            p == ICMP6 and h == 63 and s == OFF_LINK and d == LAN_HOST
            and f[:6] == lan.mac and b[:1] == b'\x80' and b[4:] == echo
            and not router.checksum(pseudo(s, d, p, len(b)) + b), neighbor=LAN_HOST)
    result['passed'].append('return_traffic_reaches_lan')

    advertise(wan, None, lifetime=0)
    time.sleep(0.5)
    send(lan, LAN_HOST, OFF_LINK, 17, udp, hops=64)
    absent(wan, 1.5, forwarded)
    result['passed'].append('zero_router_lifetime_withdraws_default')

    advertise(wan, None, lifetime=4)
    time.sleep(0.25)
    send(lan, LAN_HOST, OFF_LINK, 17, udp, hops=64)
    receive(wan, 3, forwarded, neighbor=WAN_ROUTER)
    time.sleep(6)
    send(lan, LAN_HOST, OFF_LINK, 17, udp, hops=64)
    absent(wan, 1.5, forwarded)
    result['passed'].append('default_expires_without_refresh')

    advertise(wan, WAN_PREFIX)
    time.sleep(0.25)
    send(lan, LAN_HOST, OFF_LINK, 17, udp, hops=64)
    receive(wan, 5, forwarded, neighbor=WAN_ROUTER)
    result['passed'].append('default_recovers_after_new_advertisement')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    router.run_router(args, [], exercise, wan_ipv4=False, bootargs=('a20.ipv6_ra=en3',))
    print('BSD_ROUTER_IPV6_RA: PASS')


if __name__ == '__main__':
    main()
