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
LAN2_MAC = bytes.fromhex('525400020001')
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


def advertisements(capture, mac=LAN_MAC):
    for stamp, frame in frames(capture):
        if len(frame) < 70 or frame[6:12] != mac or frame[12:14] != pd.ra.IPV6:
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
                assert length == 8 and options[2:8] == mac
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


def dhcp_packets(capture):
    for _, frame in frames(capture):
        if frame[12:14] == pd.ra.IPV6:
            ip = frame[14:]
        elif frame[12:14] == b'\x88\x64' and frame[20:22] == b'\0\x57':
            ip = frame[22:]
        else:
            continue
        if len(ip) < 52 or ip[6] != 17:
            continue
        source, dest, length = struct.unpack_from('!3H', ip, 40)
        if (source, dest) not in ((546, 547), (547, 546)):
            continue
        payload = ip[48:40 + length]
        yield source, payload[0], payload[1:4], list(pd.dhcp_options(payload[4:]))


def verified_renewal(capture):
    requests = set()
    for source, kind, xid, options in dhcp_packets(capture):
        if source == 546 and kind == 5:
            requests.add(xid)
        if source == 547 and kind == 7 and xid in requests:
            assert not any(option == 3 for option, _ in options)
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


def exercise_restart(guests, restart_router, out, result):
    a20, wan, lan = guests
    wan.until('A20_LINUX_DHCP6_PEER_READY')
    wan.until('DHCP6_STARTED')
    lan.until('A20_LINUX_LAN_READY')
    manager = 'pd-lan --state /extra/pd-state /extra/odhcp6c en3 56 en2:42 en4:43'
    lans = [('lan', LAN_MAC, 42, 'en2'), ('lan2', LAN2_MAC, 43, 'en4')]

    def start(a20):
        a20.until('[INIT] entering scheduler...')
        rc, output = command(a20, '/extra/busybox ip link set dev en4 up', wan)
        assert rc == 0, output
        a20.send(manager + ' & manager=$!')
        a20.until('pd-lan: running on', 8, drain_peer=wan)

    def wait_pios(a20, suffix, check, *, after=None):
        deadline = time.monotonic() + 30
        for name, mac, subnet, _ in lans:
            capture = out/f'{name}{suffix}.pcap'
            observed = []
            while time.monotonic() < deadline:
                command(a20, 'true', (wan, lan))
                observed = list(advertisements(capture, mac))
                start = after[name] if after else 0
                if any(check(default, pios, subnet) for _, default, pios in observed[start:]):
                    break
                time.sleep(0.4)
            else:
                raise AssertionError((capture, observed))

    def prefix(delegation, subnet):
        return ipaddress.IPv6Network(f'2001:db8:{delegation}:{subnet:x}::/64')

    start(a20)
    wait_pios(a20, '', lambda default, pios, subnet: default and
              pios.get(prefix(400, subnet), (0, 0))[0] > 0)
    rc, output = command(a20, '/extra/busybox cat /extra/pd-state', wan)
    assert rc == 0 and all(str(prefix(400, subnet)).encode() in output for _, _, subnet, _ in lans), output
    result['passed'].append('both_lans_advertise_and_persist_their_own_delegated_prefix')
    rc, output = command(a20, manager, wan)
    assert rc != 0 and b'open prefix journal' in output, output
    result['passed'].append('state_lock_prevents_a_second_manager_from_writing')
    assert command(wan, 'kill -TERM "$(busybox pidof kea-dhcp6)"')[0] == 0

    rc, output = command(a20, 'kill -KILL "$manager"; wait "$manager"; ' +
                         manager + ' & manager=$!', wan)
    assert rc == 0, output
    a20.until('pd-lan: running on', 8, drain_peer=wan)

    def withdrawal(default, pios, subnet):
        return default == 0 and pios == {prefix(400, subnet): (0, 0)}

    wait_pios(a20, '', withdrawal)
    for _, _, _, dev in lans:
        rc, output = command(a20, f'/extra/busybox ip -6 addr show dev {dev}', wan)
        assert rc == 0 and b'2001:db8:400:' not in output, output
    result['passed'].append('manager_sigkill_then_restart_withdraws_both_prefixes_and_removes_owned_addresses')

    a20 = restart_router()
    start(a20)
    wait_pios(a20, '-restart', withdrawal)
    for name, mac, _, dev in lans:
        assert all(default == 0 and all(lifetimes == (0, 0) for lifetimes in pios.values())
                   for _, default, pios in advertisements(out/f'{name}-restart.pcap', mac))
        rc, output = command(a20, f'/extra/busybox ip -6 addr show dev {dev}', wan)
        assert rc == 0 and b'2001:db8:400:' not in output, output
    result['passed'].append('abrupt_router_reboot_with_same_disk_withdraws_old_prefixes_before_dhcpv6_returns')

    assert command(wan, f'ip -6 addr add {UPSTREAM}/128 dev lo && '
                   'ip -6 route add 2001:db8:500::/56 via fe80::5054:ff:fe77:2 dev eth0 && '
                   'busybox sed -i s/2001:db8:400::/2001:db8:500::/ /etc/kea-dhcp6.conf')[0] == 0
    rc, output = command(a20, '/extra/busybox mv /extra/pd-state /extra/pd-backup && '
                         '/extra/busybox mkdir /extra/pd-state', wan)
    assert rc == 0, output
    wan.send('kea-dhcp6 -c /etc/kea-dhcp6.conf &')
    a20.until('pd-lan: save prefix journal', 25, drain_peer=(wan, lan))
    for name, mac, _, _ in lans:
        assert all(default == 0 and all(lifetimes == (0, 0) for lifetimes in pios.values())
                   for _, default, pios in advertisements(out/f'{name}-restart.pcap', mac))
    rc, output = command(a20, '/extra/busybox rmdir /extra/pd-state && '
                         '/extra/busybox mv /extra/pd-backup /extra/pd-state', wan)
    assert rc == 0, output
    result['passed'].append('journal_write_failure_prevents_unrecorded_prefix_advertisements')
    wait_pios(a20, '-restart', lambda default, pios, subnet: default and
              pios.get(prefix(500, subnet), (0, 0))[0] > 0)
    for name, mac, subnet, _ in lans:
        assert all(pios.get(prefix(400, subnet), (0, 0)) == (0, 0)
                   for _, _, pios in advertisements(out/f'{name}-restart.pcap', mac))
    wait_linux(lan, 'ip -6 addr show dev eth0',
               lambda b: b'2001:db8:500:2a:' in b and b'tentative' not in b, (wan, a20))
    assert command(lan, f'busybox ping -6 -c 2 -W 2 {UPSTREAM}', (wan, a20))[0] == 0
    result['passed'].append('replacement_delegation_renumbers_both_lans_and_restores_linux_forwarding')

    def set_carrier(qmp, up):
        request = {'execute': 'set_link', 'arguments': {'name': 'pd-wan', 'up': up}}
        qmp.write(json.dumps(request).encode() + b'\r\n')
        while True:
            message = json.loads(qmp.readline())
            if 'event' not in message:
                assert 'return' in message, message
                return

    def capture_positions():
        return {name: len(list(advertisements(out/f'{name}-restart.pcap', mac)))
                for name, mac, _, _ in lans}

    with socket.create_connection(('127.0.0.1', result['qmp_port']), timeout=5) as monitor:
        with monitor.makefile('rwb', buffering=0) as qmp:
            assert 'QMP' in json.loads(qmp.readline())
            qmp.write(b'{"execute":"qmp_capabilities"}\r\n')
            assert 'return' in json.loads(qmp.readline())
            down = capture_positions()
            set_carrier(qmp, False)
            wait_pios(a20, '-restart', lambda default, pios, subnet:
                      default == 0 and pios.get(prefix(500, subnet)) == (0, 0), after=down)
            up = capture_positions()
            set_carrier(qmp, True)
            wait_pios(a20, '-restart', lambda default, pios, subnet:
                      default and pios.get(prefix(500, subnet), (0, 0))[0] > 0, after=up)
    assert command(lan, f'busybox ping -6 -c 2 -W 2 {UPSTREAM}', (wan, a20))[0] == 0
    result['passed'].append('ethernet_carrier_drop_withdraws_delegation_and_reacquisition_restores_forwarding')
    assert command(a20, 'kill "$manager"; wait "$manager"', wan, timeout=14)[0] == 0


def verify_ppp_delegations(capture):
    identities, prefixes = set(), set()
    for source, kind, _, options in dhcp_packets(capture):
        assert not any(code == 3 for code, _ in options), 'unexpected IA_NA over PPP'
        if source == 546 and kind in (1, 3, 5):
            duid = next(value for code, value in options if code == 1)
            iaid = next(value[:4] for code, value in options if code == 25)
            identities.add((duid, iaid))
        if source == 547 and kind == 7:
            ia_pd = next(value for code, value in options if code == 25)
            for code, value in pd.dhcp_options(ia_pd[12:]):
                if code == 26:
                    preferred, valid, length = struct.unpack_from('!IIB', value)
                    assert (preferred, valid, length) == (24, 36, 56)
                    prefixes.add(ipaddress.IPv6Network((value[9:25], length)))
    assert len(identities) == 1, identities
    assert prefixes == {ipaddress.IPv6Network('2001:db8:400::/56'),
                        ipaddress.IPv6Network('2001:db8:500::/56')}, prefixes


def exercise_ppp(a20, wan, lan, out, result):
    wan.until('A20_LINUX_PPPOE_PEER_READY')
    lan.until('A20_LINUX_LAN_READY')
    a20.until('[INIT] entering scheduler...')
    assert command(a20, 'umask 077; echo a20-client > /tmp/credentials && '
                   'echo a20-test-secret >> /tmp/credentials', wan)[0] == 0
    a20.send(f'pd-lan /extra/odhcp6c ppp-wan 56 en2:{SUBNET} & manager=$!')
    a20.until('pd-lan: running on', 6, drain_peer=wan)
    a20.send('pppoe -6 en3 ppp-wan test chap /tmp/credentials & dialer=$!')
    a20.until('PPPOE IPV6 local=fe80::2 peer=fe80::1', 35, drain_peer=wan)
    wan.until('remote LL address fe80::', timeout=10)
    wan.send('pppoe-pd-start 400 &')
    wan.until('DHCP6_STARTED', timeout=15)
    peers = (wan, a20)
    wait_linux(lan, 'ip -6 addr show dev eth0',
               lambda b: b'2001:db8:400:2a:' in b and b'scope global' in b and b'tentative' not in b,
               peers, timeout=40)
    assert command(lan, f'busybox ping -6 -c 2 -W 2 {UPSTREAM}', peers)[0] == 0
    result['passed'].append('dhcp6_pd_over_pppoe_routes_the_linux_lan')
    deadline = time.monotonic() + 25
    while not verified_renewal(out/'wan.pcap'):
        assert time.monotonic() < deadline, 'PPP DHCPv6 renewal missing'
        command(lan, 'true', peers)
        time.sleep(0.7)
    result['passed'].append('dhcp6_pd_renews_on_the_ppp_uplink')

    assert command(wan, 'kill $(busybox pidof kea-dhcp6 pppd pppoe-server)')[0] == 0
    wait_linux(lan, 'ip -6 route show dev eth0', lambda b: b'default via' not in b, peers, timeout=9)
    old_prefix = ipaddress.IPv6Network('2001:db8:400:2a::/64')
    assert any(default == 0 and prefixes.get(old_prefix) == (0, 0)
               for _, default, prefixes in advertisements(out/'lan.pcap'))
    result['passed'].append('ppp_disconnect_withdraws_old_lan_prefix_and_default')
    wan.send('while busybox pidof pppd pppoe-server kea-dhcp6 >/dev/null; '
             'do sleep 0.1; done; pppoe-peer-start 198.18.0.11 &')
    a20.until('PPPOE UP session=', 35, drain_peer=wan)
    a20.until('ip=198.18.0.11', 8, drain_peer=wan)
    a20.until('PPPOE IPV6 local=fe80::2 peer=fe80::1', 8, drain_peer=wan)
    wan.until('remote LL address fe80::', timeout=10)
    wan.send('pppoe-pd-start 500 &')
    wan.until('DHCP6_STARTED', timeout=15)
    wait_linux(lan, 'ip -6 addr show dev eth0',
               lambda b: b'2001:db8:500:2a:' in b and b'scope global' in b and b'tentative' not in b,
               peers, timeout=45)
    assert command(lan, f'busybox ping -6 -c 2 -W 2 {UPSTREAM}', peers)[0] == 0
    result['passed'].append('new_pd_and_lan_connectivity_after_pppoe_redial')
    verify_ppp_delegations(out/'wan.pcap')
    result['passed'].append('wire_delegations_share_client_duid_and_iaid')
    assert command(a20, 'kill "$manager"; wait "$manager"; kill "$dialer"; wait "$dialer"',
                   wan, timeout=14)[0] == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ('kernel', 'linux', 'peer-initrd', 'busybox', 'odhcp6c', 'out'):
        parser.add_argument(f'--{arg}', type=Path, required=True)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--pppoe-peer-initrd', type=Path, help='run the WAN peer through PPPoE')
    modes.add_argument('--restart-state', action='store_true', help='verify persistent withdrawals on two LANs')
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
    bootargs = 'a20.ip=192.168.10.1 a20.netmask=255.255.255.0 a20.dhcp=0'
    if not args.pppoe_peer_initrd:
        bootargs += ' a20.ipv6_ra=en3'
    if args.trace_odhcp6c:
        bootargs += ' trace=odhcp6c'
    router = [*common, '-kernel', str(args.kernel.resolve()), '-append', bootargs,
              *lan_link(rlan, lan, '52:54:00:01:00:01', out/'lan.pcap'),
              *wan_link(rwan, wan, '52:54:00:77:00:02', out/'wan.pcap'),
              '-drive', f'file={payload},if=none,format=raw,id=payload',
              '-device', 'virtio-blk-pci,drive=payload']
    if args.restart_state:
        qmp_port, = pd.pppoe.router.backend.reserve_ports(socket.SOCK_STREAM, 1)
        router = [arg.replace('virtio-net-pci,netdev=wan', 'virtio-net-pci,id=pd-wan,netdev=wan')
                  for arg in router]
        router.extend(('-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off'))
        rlan2, lan2 = pd.pppoe.router.backend.reserve_ports(socket.SOCK_DGRAM, 2)
        router.extend(item.replace('id=lan', 'id=lan2').replace('netdev=lan', 'netdev=lan2')
                      for item in lan_link(rlan2, lan2, '52:54:00:02:00:01', out/'lan2.pcap'))
    wan_initrd = args.pppoe_peer_initrd or args.peer_initrd
    wan_args = 'console=ttyS0 quiet net.ifnames=0'
    if args.pppoe_peer_initrd:
        wan_args += ' pppauth=chap pppdual=1'
    wan_cmd = [*base[:-1], str(wan_initrd.resolve()), '-append', wan_args,
               *wan_link(wan, rwan, '52:54:00:77:00:01', out/'kea.pcap')]
    lan_cmd = [*base, '-append', 'console=ttyS0 quiet net.ifnames=0 a20_lan_peer=1',
               *lan_link(lan, rlan, '52:54:00:01:00:02', out/'client.pcap')]
    result = dict(status='failed', passed=[], argv={'router': router, 'wan': wan_cmd, 'lan': lan_cmd},
                  sha256={str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (args.kernel, args.linux, args.peer_initrd, wan_initrd,
                                    args.busybox, args.odhcp6c)})
    if args.restart_state:
        result['qmp_port'] = qmp_port
    Guest = pd.pppoe.router.backend.Guest
    with (out/'serial.log').open('wb') as router_log, (out/'wan.log').open('wb') as wan_log, \
         (out/'lan.log').open('wb') as lan_log, (out/'restart.log').open('wb') as restart_log:
        guests = []
        try:
            guests.append(Guest(router, router_log))
            guests.append(Guest(wan_cmd, wan_log))
            guests.append(Guest(lan_cmd, lan_log))
            if args.pppoe_peer_initrd:
                exercise_ppp(*guests, out, result)
            elif args.restart_state:
                def restart_router():
                    guests.pop(0).close()
                    argv = router.copy()
                    for name in ('lan', 'lan2', 'wan'):
                        argv = [arg.replace(f'{name}.pcap', f'{name}-restart.pcap') for arg in argv]
                    result['argv']['restart'] = argv
                    guests.insert(0, Guest(argv, restart_log))
                    return guests[0]

                exercise_restart(guests, restart_router, out, result)
            else:
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
