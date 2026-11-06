#!/usr/bin/env python3
"""Exercise stock BusyBox DHCPv4 on a secondary F-Stack router NIC."""
import importlib.util
import json
from pathlib import Path
import socket
import struct
import time

spec = importlib.util.spec_from_file_location(
    'policy4', Path(__file__).with_name('test-bsd-policy-v4.py'))
policy4 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy4)
wire = policy4.wire

ZERO = bytes(4)
BROADCAST = b'\xff' * 4
SERVER = socket.inet_aton('10.20.0.2')
LEASE = socket.inet_aton('10.20.0.10')
DNS = socket.inet_aton('192.0.2.53')
COOKIE = bytes.fromhex('63 82 53 63')


def options(body):
    result = {}
    offset = 0
    while offset < len(body):
        kind = body[offset]
        offset += 1
        if kind == 255:
            return result
        if kind == 0:
            continue
        length = body[offset]
        offset += 1
        assert offset + length <= len(body)
        result[kind] = body[offset:offset + length]
        offset += length
    raise ValueError('missing DHCP End option')


def receive(link, kind, timeout=15):
    deadline = time.monotonic() + timeout
    while True:
        frame = link.receive(deadline)
        if frame[12:14] != wire.IPV4 or frame[23] != 17:
            continue
        header = frame[14:]
        start = (header[0] & 15) * 4
        udp = header[start:struct.unpack_from('!H', header, 2)[0]]
        if udp[:4] != struct.pack('!HH', 68, 67):
            continue
        message = udp[8:]
        if len(message) < 240 or message[236:240] != COOKIE:
            continue
        if options(message[240:]).get(53) != bytes((kind,)):
            continue
        assert frame[6:12] == link.router_mac and message[28:34] == link.router_mac
        assert message[:3] == b'\x01\x01\x06'
        assert wire.checksum(header[:start]) == 0
        assert wire.checksum(wire.pseudo(header[12:16], header[16:20], len(udp)) + udp) == 0
        return frame, message


def respond(link, request, kind, *, lease=12, address=LEASE, dns=DNS, gateway=SERVER, unicast=False):
    response = bytearray(236)
    response[:3] = b'\x02\x01\x06'
    response[4:8] = request[4:8]
    response[10:12] = request[10:12]
    response[16:20] = address
    response[20:24] = SERVER
    response[28:44] = request[28:44]
    opts = [(53, bytes((kind,))), (54, SERVER), (51, struct.pack('!I', lease)),
            (1, b'\xff\xff\xff\x00'), (3, gateway), (6, dns)]
    response += COOKIE + b''.join(bytes((t, len(v))) + v for t, v in opts) + b'\xff'
    response = response.ljust(300, b'\0')
    dst = address if unicast else BROADCAST
    udp = wire.udp(SERVER, dst, 67, 68, response)
    link.send(wire.IPV4, wire.ipv4(SERVER, dst, 0x6800 + kind, udp),
              link.router_mac if unicast else b'\xff' * 6)


def acquire(wan, *, address=LEASE, dns=DNS, lease=12, gateway=SERVER):
    _, discovery = receive(wan, 1)
    respond(wan, discovery, 2, address=address, dns=dns, lease=lease, gateway=gateway)
    _, request = receive(wan, 3)
    opts = options(request[240:])
    assert request[4:8] == discovery[4:8]
    assert opts[50] == address and opts[54] == SERVER
    respond(wan, request, 5, address=address, dns=dns, lease=lease, gateway=gateway)


def set_link(qmp, up):
    command = {'execute': 'set_link', 'arguments': {'name': 'net1', 'up': up}}
    qmp.write(json.dumps(command).encode() + b'\r\n')
    while True:
        response = json.loads(qmp.readline())
        if 'event' not in response:
            assert 'return' in response, response
            return


def verify_carrier(guest, result):
    with socket.create_connection(('127.0.0.1', result['qmp_port']), timeout=5) as monitor:
        with monitor.makefile('rwb', buffering=0) as qmp:
            assert 'QMP' in json.loads(qmp.readline())
            qmp.write(b'{"execute":"qmp_capabilities"}\r\n')
            assert 'return' in json.loads(qmp.readline())
            guest.send('/extra/carrier-watch en3')
            guest.until('A20_CARRIER_READY', timeout=10, whole_line=True)
            set_link(qmp, False)
            guest.until('A20_CARRIER_DOWN', timeout=8, whole_line=True)
            set_link(qmp, True)
            guest.until('A20_CARRIER_UP', timeout=8, whole_line=True)
    result['passed'].append('carrier_transitions_leave_admin_up_and_publish_link_events')
    guest.until('A20_BOUND_WRONG_EGRESS_REJECTED', timeout=8, whole_line=True)
    result['passed'].append('ipv4_device_binding_rejects_another_egress')


def exercise(guest, lan, wan, standby, result):
    script = '''#!/bin/mksh
case $1 in
bound|renew)
    /bin/netctl set-ipv4 en3 "$ip" "$subnet" || exit 1
    echo "A20_WAN_EVENT $1 $ip $lease $router $dns"
    ;;
esac
'''
    policy4.configure(guest, "/extra/busybox printf '%s' '" + script + "' > /tmp/wan-event",
                      '/extra/busybox chmod 755 /tmp/wan-event')
    guest.send('/extra/busybox udhcpc -f -i en3 -s /tmp/wan-event -T 2 -t 3 -A 2 &')
    acquire(wan)
    guest.until('A20_WAN_EVENT bound 10.20.0.10 12 10.20.0.2 192.0.2.53',
                timeout=10, whole_line=True)
    result['passed'].append('discover_offer_request_ack_on_unconfigured_secondary_nic')

    frame, renewal = receive(wan, 3, timeout=16)
    assert frame[:6] == wan.mac and frame[26:34] == LEASE + SERVER
    assert renewal[12:16] == LEASE
    # A valid ACK received on the other NIC must not reach udhcpc.
    respond(lan, renewal, 5, dns=socket.inet_aton('192.0.2.99'), unicast=True)
    try:
        guest.until('A20_WAN_EVENT renew ', timeout=0.5)
    except TimeoutError:
        pass
    else:
        raise AssertionError('lease renewal accepted an ACK from the LAN')
    respond(wan, renewal, 5, unicast=True)
    guest.until('A20_WAN_EVENT renew 10.20.0.10 12 10.20.0.2 192.0.2.53',
                timeout=10, whole_line=True)
    result['passed'].append('unicast_renewal_rejects_lan_ack_and_receives_wan_ack')
    verify_carrier(guest, result)
    guest.until('# ')
    policy4.configure(guest, 'netctl set-ipv4 en4 10.30.0.1 255.255.255.0')
    wan.router_ip = LEASE
    for link in (lan, wan, standby):
        link.resolve()
    result['passed'].append('three_interfaces_answer_arp_with_independent_addresses_and_macs')


def main():
    probe = Path(__file__).with_name('bsd-carrier-watch.c')
    policy4.run_router(policy4.arguments(__doc__), exercise, probes=(probe,), qmp=True)
    print('BSD_DHCP4_WAN: PASS')


if __name__ == '__main__':
    main()
