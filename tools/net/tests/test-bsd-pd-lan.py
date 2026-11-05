#!/usr/bin/env python3
"""Prove DHCPv6-PD to LAN SLAAC with independent Linux WAN and LAN guests."""
import argparse
import hashlib
import importlib.util
import ipaddress
import json
from pathlib import Path
import re
import socket
import struct
import time

spec = importlib.util.spec_from_file_location(
    'pd', Path(__file__).with_name('test-bsd-dhcp6-pd.py'))
pd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pd)

LAN_MAC = bytes.fromhex('525400010001')
WAN_MAC = bytes.fromhex('525400770002')
UPSTREAM = '2001:db8:30::1'
SUBNET = 0x2A


def command(guest, text, peer=None, timeout=8):
    guest.send(text + '; echo A20_CMD_STATUS=$?')
    output = guest.until('A20_CMD_STATUS=', timeout, drain_peer=peer)
    # The console can echo the typed command before executing it.
    while True:
        rest = guest.until('\n', timeout, drain_peer=peer)
        match = re.match(rb'(\d+)\r?\n', rest)
        if match:
            return int(match[1]), output
        output += rest + guest.until('A20_CMD_STATUS=', timeout, drain_peer=peer)


def wait_linux(guest, command_text, predicate, peer, timeout=25):
    deadline = time.monotonic() + timeout
    while True:
        rc, output = command(guest, command_text, peer)
        assert rc == 0, output
        if predicate(output):
            return output
        if time.monotonic() >= deadline:
            raise AssertionError(output)
        time.sleep(0.7)


def frames(capture):
    data = capture.read_bytes()
    assert data[:4] == bytes.fromhex('d4c3b2a1'), capture
    off = 24
    while off + 16 <= len(data):
        sec, usec, length, _ = struct.unpack_from('<4I', data, off)
        start = off + 16
        off = start + length
        if off > len(data):
            break
        yield sec + usec / 1000000, data[start:off]


def advertisements(capture):
    for stamp, frame in frames(capture):
        if len(frame) < 70 or frame[6:12] != LAN_MAC or frame[12:14] != pd.ra.IPV6:
            continue
        size, proto, hops, src, dst = struct.unpack('!HBB16s16s', frame[18:54])
        packet = frame[54:54 + size]
        if proto != 58 or packet[0] != 134:
            continue
        assert dst == pd.ra.ALL_NODES and frame[:6] == bytes.fromhex('333300000001')
        assert hops == 255 and packet[1] == 0 and ipaddress.IPv6Address(src).is_link_local
        assert len(packet) == size and not pd.ra.router.checksum(pd.ra.pseudo(src, dst, proto, size) + packet)
        default = struct.unpack_from('!H', packet, 6)[0]
        options = packet[16:]
        prefixes = {}
        seen_link = False
        while options:
            assert len(options) >= 2 and options[1] and len(options) >= options[1] * 8
            kind, length = options[0], options[1] * 8
            if kind == 1:
                assert length == 8 and options[2:8] == LAN_MAC
                seen_link = True
            elif kind == 3:
                assert length == 32 and options[2:4] == b'\x40\xc0'
                valid, preferred = struct.unpack_from('!II', options, 4)
                prefix = ipaddress.IPv6Network((options[16:32], 64))
                assert 0 <= preferred <= valid <= 36
                prefixes[prefix] = (preferred, valid)
            options = options[length:]
        assert seen_link and prefixes
        yield stamp, default, prefixes


def verified_renewal(capture):
    request = set()
    for _, frame in frames(capture):
        if len(frame) < 66 or frame[12:14] != pd.ra.IPV6 or frame[20] != 17:
            continue
        ports = struct.unpack_from('!2H', frame, 54)
        kind, xid = frame[62], frame[63:66]
        if ports == (546, 547) and kind == 5:
            request.add(xid)
        if ports == (547, 546) and kind == 7 and xid in request:
            options = list(pd.dhcp_options(frame[66:54 + struct.unpack_from('!H', frame, 58)[0]]))
            assert not any(kind == 3 for kind, _ in options)
            return True
    return False


def exercise(a20, wan, lan, out, result):
    wan.until('A20_LINUX_DHCP6_PEER_READY')
    wan.until('DHCP6_STARTED')
    lan.until('A20_LINUX_LAN_READY')
    a20.until('[INIT] entering scheduler...')
    assert command(a20, 'true', wan)[0] == 0
    wait_linux(a20, '/extra/busybox ip -6 addr show dev en3',
               lambda b: b'scope global' in b and b'tentative' not in b, wan)

    rc, output = command(a20, 'netctl reuse-ipv6-udp', wan)
    assert rc == 0 and b'NETCTL: IPv6 UDP port reuse passed' in output
    result['passed'].append('udp6_reuse_matches_linux_and_cloexec_is_per_descriptor')
    a20.send(f'pd-lan /extra/odhcp6c en3 56 en2:{SUBNET} & manager=$!')
    a20.until('pd-lan: running on', 6, drain_peer=wan)
    peers = (wan, a20)
    capture = out/'lan.pcap'
    addresses = wait_linux(lan, 'ip -6 addr show dev eth0',
                           lambda b: b'scope global' in b and b'tentative' not in b, peers, timeout=45)
    address = ipaddress.IPv6Interface(re.search(rb'inet6 (\S+/64) scope global', addresses)[1].decode())
    prefix = address.network
    delegation = prefix.supernet(new_prefix=56)
    assert prefix == ipaddress.IPv6Network((int(delegation.network_address) | SUBNET << 64, 64))
    result['delegation'] = str(delegation)
    result['lan_prefix'] = str(prefix)
    result['linux_address'] = str(address.ip)
    initial = list(advertisements(capture))
    assert initial and any(default and prefix in prefixes and prefixes[prefix][1] for _, default, prefixes in initial)
    result['passed'].append('linux_slaac_uses_the_selected_delegated_subnet')

    assert command(wan, f'ip -6 addr add {UPSTREAM}/128 dev lo && '
                   f'ip -6 route add {delegation} via fe80::5054:ff:fe77:2 dev eth0')[0] == 0
    wait_linux(lan, 'ip -6 route show dev eth0', lambda b: b'default via fe80::' in b, peers)
    assert command(lan, f'busybox ping -6 -c 2 -W 2 {UPSTREAM}', peers)[0] == 0
    result['passed'].append('linux_lan_and_wan_ping_with_automatic_ra_address_and_route')

    deadline = time.monotonic() + 28
    while time.monotonic() < deadline:
        command(lan, 'true', peers)
        observed = list(advertisements(capture))
        if verified_renewal(out/'wan.pcap') and any(
            prefixes.get(prefix, (0, 0))[1] > 26 for _, _, prefixes in observed
        ):
            break
        time.sleep(0.7)
    else:
        raise AssertionError('renewal failed to extend the LAN advertisement')
    result['passed'].append('odhcp6c_renewal_refreshes_the_lan_prefix_lifetime')

    assert command(wan, 'kill -TERM "$(busybox pidof radvd)"')[0] == 0
    wait_linux(lan, 'ip -6 route show dev eth0', lambda b: b'default via' not in b, peers)
    assert any(default == 0 and prefixes.get(prefix, (0, 0))[1] > 0
               for _, default, prefixes in advertisements(capture))
    result['passed'].append('wan_default_loss_withdraws_only_the_lan_default')
    wan.send('radvd -n -C /etc/radvd.conf -m stderr &')
    wait_linux(lan, 'ip -6 route show dev eth0', lambda b: b'default via fe80::' in b, peers)
    assert command(lan, f'busybox ping -6 -c 2 -W 2 {UPSTREAM}', peers)[0] == 0
    result['passed'].append('wan_default_recovery_restores_lan_connectivity')

    assert command(wan, 'kill -TERM "$(busybox pidof kea-dhcp6)"')[0] == 0
    wait_linux(lan, 'ip -6 addr show dev eth0', lambda b: b'scope global' not in b, peers, timeout=52)
    assert any(default == 0 and prefixes.get(prefix) == (0, 0)
               for _, default, prefixes in advertisements(capture))
    wait_linux(lan, 'ip -6 route show dev eth0', lambda b: b'default via' not in b, peers)
    rc, output = command(a20, '/extra/busybox ip -6 addr show dev en2', wan)
    assert rc == 0 and b'scope global' not in output, output
    result['passed'].append('lease_loss_withdraws_and_expires_the_lan_prefix')

    replacement = ipaddress.IPv6Network('2001:db8:500::/56')
    new_prefix = ipaddress.IPv6Network((int(replacement.network_address) | SUBNET << 64, 64))
    assert command(wan, f'ip -6 route add {replacement} via fe80::5054:ff:fe77:2 dev eth0 && '
                   "busybox sed -i s/2001:db8:400::/2001:db8:500::/ /etc/kea-dhcp6.conf")[0] == 0
    wan.send('kea-dhcp6 -c /etc/kea-dhcp6.conf &')
    wait_linux(lan, 'ip -6 addr show dev eth0',
               lambda b: b'2001:db8:500:2a:' in b and b'scope global' in b and b'tentative' not in b,
               peers, timeout=50)
    assert command(lan, f'busybox ping -6 -c 2 -W 2 {UPSTREAM}', peers)[0] == 0
    result['passed'].append('same_manager_reacquires_after_expiry_and_renumbers_linux_lan')
    assert command(a20, 'kill "$manager"; wait "$manager"', wan, timeout=14)[0] == 0
    assert any(default == 0 and prefixes.get(new_prefix) == (0, 0)
               for _, default, prefixes in advertisements(capture))
    rc, output = command(a20, '/extra/busybox ip -6 addr show dev en2', wan)
    assert rc == 0 and b'scope global' not in output, output
    result['passed'].append('orderly_stop_withdraws_advertisements_and_address')
    assert not any(len(frame) > 54 and frame[6:12] in (LAN_MAC, WAN_MAC) and
                   frame[12:14] == pd.ra.IPV6 and frame[20] == 58 and frame[54] == 134
                   for _, frame in frames(out/'wan.pcap')), 'LAN advertisement leaked to WAN'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ('kernel', 'linux', 'peer-initrd', 'busybox', 'odhcp6c', 'out'):
        parser.add_argument(f'--{arg}', type=Path, required=True)
    parser.add_argument('--trace-odhcp6c', action='store_true')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    payload = pd.make_payload(out, args.busybox, False, args.odhcp6c)
    rlan, lan, rwan, wan = pd.pppoe.router.backend.reserve_ports(socket.SOCK_DGRAM, 4)
    common = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm',
              '-m', '768', '-smp', '2', '-nographic', '-monitor', 'none', '-no-reboot']
    base = [*common, '-cpu', 'host', '-kernel', str(args.linux.resolve()),
            '-initrd', str(args.peer_initrd.resolve())]
    wan_link = pd.pppoe.wan_args
    lan_link = lambda local, remote, mac, cap: [
        item.replace('id=wan', 'id=lan').replace('netdev=wan', 'netdev=lan')
        for item in wan_link(local, remote, mac, cap)
    ]
    bootargs = 'a20.ip=192.168.10.1 a20.netmask=255.255.255.0 a20.dhcp=0 a20.ipv6_ra=en3'
    if args.trace_odhcp6c:
        bootargs += ' trace=odhcp6c'
    router = [*common, '-kernel', str(args.kernel.resolve()), '-append', bootargs,
              *lan_link(rlan, lan, '52:54:00:01:00:01', out/'lan.pcap'),
              *wan_link(rwan, wan, '52:54:00:77:00:02', out/'wan.pcap'),
              '-drive', f'file={payload},if=none,format=raw,id=payload',
              '-device', 'virtio-blk-pci,drive=payload']
    wan_cmd = [*base, '-append', 'console=ttyS0 quiet net.ifnames=0',
               *wan_link(wan, rwan, '52:54:00:77:00:01', out/'kea.pcap')]
    lan_cmd = [*base, '-append', 'console=ttyS0 quiet net.ifnames=0 a20_lan_peer=1',
               *lan_link(lan, rlan, '52:54:00:01:00:02', out/'client.pcap')]
    result = dict(status='failed', passed=[], argv={'router': router, 'wan': wan_cmd, 'lan': lan_cmd},
                  sha256={str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (args.kernel, args.linux, args.peer_initrd, args.busybox, args.odhcp6c)})
    Guest = pd.pppoe.router.backend.Guest
    with (out/'serial.log').open('wb') as router_log, (out/'wan.log').open('wb') as wan_log, \
         (out/'lan.log').open('wb') as lan_log:
        guests = []
        try:
            guests.append(Guest(router, router_log))
            guests.append(Guest(wan_cmd, wan_log))
            guests.append(Guest(lan_cmd, lan_log))
            exercise(*guests, out, result)
            guests[0].send('poweroff')
            guests[0].until('System is going down for power-off NOW')
            assert guests[0].process.wait(timeout=15) == 0
            result['status'] = 'passed'
        except Exception as error:
            result['error'] = repr(error)
            raise
        finally:
            for guest in guests:
                guest.close()
            (out/'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    print('BSD_PD_LAN: PASS')


if __name__ == '__main__':
    main()
