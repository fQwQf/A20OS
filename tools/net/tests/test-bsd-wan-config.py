#!/usr/bin/env python3
"""Verify boot-time Ethernet WAN policy, lease cleanup and recovery on the wire."""
import copy
import importlib.util
import json
from pathlib import Path
import socket
import struct
import time

spec = importlib.util.spec_from_file_location(
    'dhcp', Path(__file__).with_name('test-bsd-dhcp4-wan.py'))
dhcp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dhcp)
router = dhcp.policy4
wire = router.wire

DNS2 = socket.inet_aton('192.0.2.54')
LEASE2 = socket.inet_aton('10.20.0.11')
GATEWAY2 = socket.inet_aton('10.20.0.3')
EXTERNAL = socket.inet_aton('198.51.100.9')
BOOTARG = 'a20.wan_config=/extra/wan.conf'
STATIC = '''interface=en3
mode=static
address=10.20.0.12/24
gateway=10.20.0.2
dns=192.0.2.53 192.0.2.54
resolver=/etc/resolv.conf
'''
DYNAMIC = '''interface=en3
mode=dhcp
busybox=/extra/busybox
resolver=/etc/resolv.conf
'''


def shell(guest, command):
    guest.send('echo A20_WAN_TEST_BEGIN; ' + command + '; echo A20_WAN_TEST_END')
    guest.until('A20_WAN_TEST_BEGIN', timeout=10, whole_line=True)
    return guest.until('A20_WAN_TEST_END', timeout=10, whole_line=True)


def link(result, up):
    with socket.create_connection(('127.0.0.1', result['qmp_port']), timeout=5) as monitor:
        with monitor.makefile('rwb', buffering=0) as qmp:
            assert 'QMP' in json.loads(qmp.readline())
            qmp.write(b'{"execute":"qmp_capabilities"}\r\n')
            assert 'return' in json.loads(qmp.readline())
            dhcp.set_link(qmp, up)


def configured(guest, address, nameservers, *, gateway="10.20.0.2"):
    guest.until(f'WANCTL: en3 configured {address}/24', timeout=12)
    output = shell(guest, '/extra/busybox ip -4 addr show dev en3; '
                         '/extra/busybox ip -4 route show; cat /etc/resolv.conf')
    assert f'inet {address}/24'.encode() in output, output
    assert f'default via {gateway} dev en3'.encode() in output, output
    lines = [line for line in output.splitlines() if line.startswith(b'nameserver ')]
    assert lines == [f'nameserver {value}'.encode() for value in nameservers], output


def withdrawn(guest):
    deadline = time.monotonic() + 4
    while True:
        output = shell(guest, '/extra/busybox ip -4 addr show dev en3; /extra/busybox ip -4 route show; '
                             'if [ ! -e /etc/resolv.conf ]; then echo A20_WAN_DNS_GONE; fi')
        if (b'inet 10.20.0.' not in output and b'default via 10.20.0.' not in output and
                b'\nA20_WAN_DNS_GONE\n' in output):
            return
        assert time.monotonic() < deadline, output
        time.sleep(0.1)


def forward(lan, wan, identifier):
    data = b'wan-config-route-' + identifier.to_bytes(2, 'big')
    lan.resolve()
    lan.fragment(EXTERNAL, identifier, wire.udp(lan.peer_ip, EXTERNAL, 33000, 44000, data))
    # The destination is off-link. The router must send to the configured gateway's MAC.
    deadline = time.monotonic() + 5
    while True:
        frame = wan.receive(deadline)
        if frame[12:14] != wire.IPV4 or frame[18:20] != identifier.to_bytes(2, 'big'):
            continue
        assert frame[:12] == wan.mac + wan.router_mac
        assert frame[22:24] == bytes((63, 17)) and frame[26:34] == lan.peer_ip + EXTERNAL
        assert frame[42:42 + len(data)] == data
        return


def resolve_name(guest, wan, server):
    guest.send('/extra/busybox nslookup -type=A wan.test')
    deadline = time.monotonic() + 6
    while True:
        frame = wan.receive(deadline)
        if frame[12:14] != wire.IPV4 or frame[23] != 17 or frame[30:34] != server:
            continue
        ip = frame[14:]
        length = struct.unpack_from('!H', ip, 2)[0]
        udp = ip[(ip[0] & 15) * 4:length]
        port, service = struct.unpack_from('!HH', udp)
        if service != 53:
            continue
        assert frame[:12] == wan.mac + wan.router_mac
        query = udp[8:]
        question = b'\x03wan\x04test\x00\x00\x01\x00\x01'
        assert query[12:12 + len(question)] == question, query
        answer = b'\xc0\x0c' + struct.pack('!HHIH', 1, 1, 20, 4) + socket.inet_aton('203.0.113.55')
        reply = query[:2] + struct.pack('!5H', 0x8180, 1, 1, 0, 0) + question + answer
        wan.fragment(ip[12:16], 0x7490, wire.udp(server, ip[12:16], 53, port, reply), src=server)
        break
    guest.until('203.0.113.55', timeout=6)


def client_pid(guest):
    guest.until('WANCTL: en3 DHCP client pid=', timeout=12)
    result = guest.until('\n')
    return int(result.strip())


def dynamic(guest, lan, wan, standby, result):
    dhcp.acquire(wan, lease=16)
    configured(guest, '10.20.0.10', ['192.0.2.53'])
    forward(lan, wan, 0x7410)
    result['passed'].append('boot_config_applies_dhcp_address_default_route_and_dns')

    frame, renewal = dhcp.receive(wan, 3, timeout=18)
    assert frame[26:34] == dhcp.LEASE + dhcp.SERVER
    dhcp.respond(wan, renewal, 5, lease=16, dns=DNS2, unicast=True)
    configured(guest, '10.20.0.10', ['192.0.2.54'])
    resolve_name(guest, wan, DNS2)
    result['passed'].append('unicast_renewal_updates_resolver_and_lookup_uses_new_dns')

    link(result, False)
    guest.until('WANCTL: en3 carrier down', timeout=8)
    withdrawn(guest)
    link(result, True)
    pid = client_pid(guest)
    dhcp.acquire(wan, address=LEASE2, dns=DNS2, lease=16, gateway=GATEWAY2)
    configured(guest, '10.20.0.11', ['192.0.2.54'], gateway='10.20.0.3')
    wan.peer_ip = GATEWAY2
    forward(lan, wan, 0x7411)
    result['passed'].append('carrier_loss_withdraws_lease_and_new_carrier_reacquires_with_changed_address')

    shell(guest, f'kill -KILL {pid}')
    new_pid = client_pid(guest)
    assert new_pid != pid
    withdrawn(guest)
    wan.peer_ip = dhcp.SERVER
    dhcp.acquire(wan, lease=12)
    configured(guest, '10.20.0.10', ['192.0.2.53'])
    result['passed'].append('client_failure_withdraws_state_and_supervisor_starts_new_client')

    dhcp.receive(wan, 1, timeout=18)
    withdrawn(guest)
    result['passed'].append('lease_expiry_with_unreachable_server_withdraws_owned_state')


def static(guest, lan, wan, standby, result):
    configured(guest, '10.20.0.12', ['192.0.2.53', '192.0.2.54'])
    forward(lan, wan, 0x7412)
    result['passed'].append('boot_config_applies_static_ipv4_default_route_and_dns')
    link(result, False)
    guest.until('WANCTL: en3 carrier down', timeout=8)
    withdrawn(guest)
    link(result, True)
    configured(guest, '10.20.0.12', ['192.0.2.53', '192.0.2.54'])
    forward(lan, wan, 0x7413)
    result['passed'].append('static_mode_withdraws_and_reapplies_after_carrier_change')


def static_reboot(guest, lan, wan, standby, result):
    configured(guest, '10.20.0.12', ['192.0.2.53', '192.0.2.54'])
    forward(lan, wan, 0x7414)
    contents = shell(guest, 'cat /extra/wan.conf')
    assert STATIC.encode() in contents, contents
    result['passed'].append('same_persistent_disk_boots_and_reapplies_configuration')
    shell(guest, 'kill -TERM $(/extra/busybox pidof wanctl)')
    guest.until('[init] wanctl exited', timeout=8)
    withdrawn(guest)
    result['passed'].append('graceful_supervisor_stop_withdraws_owned_configuration')

    router.configure(guest, 'netctl set-ipv4 en4 10.30.0.1 255.255.255.0')
    router.command(guest, 'route add default via 10.30.0.2 dev en4')
    guest.send('/bin/wanctl /extra/wan.conf & wan=$!')
    guest.until('wanctl: static configuration: File exists', timeout=10)
    output = shell(guest, '/extra/busybox ip -4 addr show dev en3; /extra/busybox ip -4 route show')
    assert b'default via 10.30.0.2 dev en4' in output and b'inet 10.20.0.12' not in output, output
    shell(guest, 'kill -TERM "$wan"; wait "$wan"')
    result['passed'].append('foreign_default_route_is_preserved_when_wan_install_rolls_back')

    router.command(guest, 'route del default via 10.30.0.2 dev en4')
    shell(guest, "echo 'nameserver 192.0.2.99' > /etc/resolv.conf")
    guest.send('/bin/wanctl /extra/wan.conf & wan=$!')
    guest.until('wanctl: static configuration: File exists', timeout=10)
    shell(guest, 'kill -TERM "$wan"; wait "$wan"')
    output = shell(guest, '/extra/busybox ip -4 addr show dev en3; '
                         '/extra/busybox ip -4 route show; cat /etc/resolv.conf')
    assert b'default via' not in output and b'inet 10.20.0.12' not in output, output
    assert b'\nnameserver 192.0.2.99\n' in output, output
    result['passed'].append('foreign_resolver_is_preserved_when_wan_install_rolls_back')


def main():
    args = router.arguments(__doc__)
    root = args.out.resolve()
    root.mkdir(parents=True)
    for name, test, config in [('dhcp', dynamic, DYNAMIC), ('static', static, STATIC)]:
        instance = copy.copy(args)
        instance.out = root/name
        router.run_router(instance, test, qmp=True, bootargs=BOOTARG, files={'wan.conf': config})
    image = root/'static'/'payload.img'
    instance = copy.copy(args)
    instance.out = root/'static-reboot'
    router.run_router(instance, static_reboot, qmp=True, bootargs=BOOTARG, image=image)
    print('BSD_WAN_CONFIG: PASS')


if __name__ == '__main__':
    main()
