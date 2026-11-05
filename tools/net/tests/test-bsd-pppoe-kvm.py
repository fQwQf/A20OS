#!/usr/bin/env python3
"""Dial a real Linux pppoe-server/pppd peer and route LAN UDP over it.

The peer initramfs starts an unnumbered PPPoE access concentrator on eth0,
an IPv4 UDP echo on port 42000, and a shell on the serial console.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import signal
import socket
import struct
import time

spec = importlib.util.spec_from_file_location(
    'router', Path(__file__).with_name('test-bsd-router-fragments.py'))
router = importlib.util.module_from_spec(spec)
spec.loader.exec_module(router)

spec = importlib.util.spec_from_file_location(
    'ra', Path(__file__).with_name('test-bsd-router-ipv6-ra.py'))
ra = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ra)

PEER_IP = socket.inet_aton('10.20.0.2')
TUN_IP = '198.18.0.10'
REDIAL_IP = '198.18.0.11'


def wan_args(local, remote, mac, capture):
    return [
        '-netdev', f'socket,id=wan,udp=127.0.0.1:{remote},localaddr=127.0.0.1:{local}',
        '-device', f'virtio-net-pci,netdev=wan,mac={mac}',
        '-object', f'filter-dump,id=wanpcap,netdev=wan,file={capture}',
    ]


def echo(lan, payload, sport):
    datagram = router.udp(lan.peer_ip, PEER_IP, sport, 42000, payload)
    lan.send_datagram(PEER_IP, 0x6000, datagram)
    # Linux chooses the IP ID independently, so identify the reply by its tuple.
    deadline = time.monotonic() + 10
    parts = {}
    final = None
    ident = None
    while True:
        frame = lan.receive(deadline)
        if frame[12:14] != router.IPV4 or len(frame) < 34:
            continue
        ip = frame[14:]
        if ip[0] != 0x45 or ip[9] != 17 or ip[12:20] != PEER_IP + lan.peer_ip:
            continue
        assert ip[8] == 63 and router.checksum(ip[:20]) == 0
        length, incoming, flags = (int.from_bytes(ip[i:i+2], 'big') for i in (2, 4, 6))
        assert 20 < length <= len(ip)
        if ident is None:
            ident = incoming
        if incoming != ident:
            continue
        off = (flags & 0x1fff) * 8
        parts[off] = ip[20:length]
        if not flags & 0x2000:
            final = off + length - 20
        if final == sum(map(len, parts.values())):
            break
    data = bytearray()
    for offset, part in sorted(parts.items()):
        assert offset == len(data)
        data += part
    assert bytes(data[8:]) == payload
    assert int.from_bytes(data[:2], 'big') == 42000 and int.from_bytes(data[2:4], 'big') == sport
    assert router.checksum(router.pseudo(PEER_IP, lan.peer_ip, len(data)) + data) == 0


def wait_up(guest, mode, address, peer):
    output = guest.until('mtu=1492', timeout=50, drain_peer=peer)
    match = re.search(rb'PPPOE UP session=(\d+) auth=(pap|chap) ip=([^ ]+) mtu=1492', output)
    assert match and match[2].decode() == mode and match[3].decode() == address
    return int(match[1])


def verify_ipv6(guest, peer, lan, first):
    guest.until('PPPOE IPV6 local=fe80::', timeout=8, drain_peer=peer)
    output = guest.until(' peer=fe80::1', timeout=8, drain_peer=peer)
    assert output.startswith(b'2 peer='), output
    server = '2001:db8:30::1'
    peer.send(('ip -6 addr add ' + server + '/128 dev lo && ' if first else '') +
              'ip -6 route add 2001:db8:10::/64 dev ppp0 && '
              'busybox ping -6 -c 1 -W 3 -I ppp0 fe80::2 && echo A20_PPPOE_V6_READY')
    peer.until('A20_PPPOE_V6_READY', timeout=10, whole_line=True)
    guest.send('cat /tmp/ppp-dns')
    dns = guest.until('# ', drain_peer=peer)
    assert b'nameserver 192.0.2.53' in dns and b'nameserver 192.0.2.54' in dns, dns
    host = ra.LAN_HOST
    target = ra.ip(server)
    token = b'\xa2\x06\x00' + (b'\x01first' if first else b'\x02redial')
    ra.icmp(lan, host, target, 128, token, hops=64)
    ra.receive(lan, 8, lambda frame, proto, hop, src, dst, payload:
               proto == ra.ICMP6 and hop == 63 and src == target and dst == host and
               payload[:1] == b'\x81' and payload[4:] == token and
               not router.checksum(ra.pseudo(src, dst, proto, len(payload)) + payload),
               neighbor=host)


def verify_ipv6_wire(path):
    data = path.read_bytes()
    assert data[:4] == bytes.fromhex('d4c3b2a1')
    offset, opened, traffic, dns = 24, {}, {}, set()
    generation = 0
    while offset + 16 <= len(data):
        _, _, length, _ = struct.unpack_from('<4I', data, offset)
        frame = data[offset + 16:offset + 16 + length]
        offset += 16 + length
        if len(frame) >= 20 and frame[12:14] == b'\x88\x63' and frame[15] == 0x65:
            generation += 1
        if len(frame) < 22 or frame[12:14] != b'\x88\x64':
            continue
        session, size, proto = struct.unpack_from('!3H', frame, 16)
        session = (generation, session)
        direction = frame[6:12]
        payload = frame[22:20 + size]
        if proto == 0x8057 and len(payload) == 14 and payload[0] == 2 and payload[4:6] == b'\x01\x0a':
            opened.setdefault(session, set()).add(direction)
        if proto == 0x57 and len(payload) >= 48 and payload[6] == 58 and payload[40] in (128, 129):
            traffic.setdefault(session, set()).add(direction)
        if proto == 0x8021 and len(payload) >= 4 and payload[0] == 2:
            for option in (b'\x81\x06\xc0\x00\x02\x35', b'\x83\x06\xc0\x00\x02\x36'):
                if option in payload:
                    dns.add(option)
    assert len(dns) == 2 and sum(len(traffic.get(s, ())) == 2 for s in opened if len(opened[s]) == 2) >= 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--linux', type=Path, required=True)
    parser.add_argument('--peer-initrd', type=Path, required=True)
    parser.add_argument('--auth', choices=('pap', 'chap'), required=True)
    parser.add_argument('--out', type=Path, required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--dual', action='store_true', help='verify IPv6CP and IPCP DNS')
    mode.add_argument('--dns-reject', action='store_true', help='request DNS from a peer with no DNS')
    parser.add_argument('--silent', action='store_true', help='pause peer to test LCP echo timeout')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True)
    ports = router.backend.reserve_ports(socket.SOCK_DGRAM, 2)
    lan = router.Link(1, '192.168.10.2', '192.168.10.1')
    common = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm',
              '-m', '512', '-smp', '2', '-nographic', '-monitor', 'none', '-no-reboot']
    linux_argv = [*common, '-cpu', 'host', '-kernel', str(args.linux.resolve()),
                  '-initrd', str(args.peer_initrd.resolve()), '-append',
                  f'console=ttyS0 quiet net.ifnames=0 pppauth={args.auth} pppdual={int(args.dual)}',
                  *wan_args(ports[1], ports[0], '52:54:00:77:00:01', out/'peer.pcap')]
    a20_argv = [*common, '-kernel', str(args.kernel.resolve()), '-append',
                'a20.ip=192.168.10.1 a20.netmask=255.255.255.0 a20.dhcp=0',
                *lan.argv(0, out),
                *wan_args(ports[0], ports[1], '52:54:00:77:00:02', out/'wan.pcap')]
    result = dict(status='failed', auth=args.auth, dual=args.dual,
                  dns_reject=args.dns_reject, passed=[],
                  argv={'router': a20_argv, 'peer': linux_argv},
                  sha256={str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (args.kernel, args.linux, args.peer_initrd)})
    with (out/'peer.log').open('wb') as peer_log, (out/'serial.log').open('wb') as router_log:
        peer = router.backend.Guest(linux_argv, peer_log)
        guest = None
        try:
            peer.until('A20_LINUX_PPPOE_PEER_READY')
            guest = router.backend.Guest(a20_argv, router_log)
            guest.until('[INIT] entering scheduler...')
            guest.send('echo A20_PPPOE_SHELL')
            guest.until('A20_PPPOE_SHELL', whole_line=True)
            guest.until('# ')
            guest.send("umask 077; echo a20-client > /tmp/credentials; "
                       "echo a20-test-secret >> /tmp/credentials")
            guest.until('# ')
            guest.send("echo 'natadd out proto=udp dst=10.20.0.2 action=masquerade' > /proc/a20/netfilter")
            guest.until('# ')
            options = '-6 ' if args.dual else ''
            if args.dual or args.dns_reject:
                options += '-d /tmp/ppp-dns '
            if args.dual:
                guest.send('netctl set-ipv6 en2 2001:db8:10::1 64')
                guest.until('NETCTL: IPv6 configured', drain_peer=peer)
                guest.until('# ')
            guest.send(f'pppoe {options}en3 ppp-wan test {args.auth} /tmp/credentials & pp=$!')
            result['first_session'] = wait_up(guest, args.auth, TUN_IP, peer)
            if args.dns_reject:
                peer.until('sent [IPCP ConfRej')
            peer.until(f'remote IP address {TUN_IP}')
            if args.dual:
                verify_ipv6(guest, peer, lan, True)
                result['passed'].append('ipv6cp_and_ipcp_dns_on_first_session')
            if args.dns_reject:
                guest.send('[ ! -e /tmp/ppp-dns ] && echo A20_PPPOE_DNS_ABSENT')
                guest.until('A20_PPPOE_DNS_ABSENT', whole_line=True, drain_peer=peer)
                result['passed'].append('optional_dns_rejection_preserves_ipv4')
            lan.resolve()
            echo(lan, bytes(range(256)) * 17, 32000)
            result['passed'].append('authenticated_ipcp_and_lan_napt')
            if args.silent:
                peer.process.send_signal(signal.SIGSTOP)
                try:
                    guest.until('PPPOE DOWN Operation timed out', timeout=30)
                    result['passed'].append('silent_peer_detected_by_lcp_echo')
                finally:
                    peer.process.send_signal(signal.SIGCONT)
            peer.send('kill $(busybox pidof pppd) $(busybox pidof pppoe-server); '
                      'while busybox pidof pppd pppoe-server >/dev/null; do sleep 0.1; done; '
                      f'pppoe-peer-start {REDIAL_IP} &')
            if not args.silent:
                guest.until('PPPOE DOWN', timeout=30)
            result['second_session'] = wait_up(guest, args.auth, REDIAL_IP, peer)
            peer.until(f'remote IP address {REDIAL_IP}')
            if args.dual:
                verify_ipv6(guest, peer, lan, False)
                result['passed'].append('ipv6_and_dns_restored_after_redial')
            # Reuse the five-tuple so a stale MASQUERADE mapping cannot pass.
            echo(lan, b'reconnect-verified', 32000)
            result['passed'].append('peer_disconnect_redials_with_new_address')

            guest.send('kill "$pp"; wait "$pp"; [ ! -e /tmp/ppp-dns ] && echo A20_PPPOE_STOPPED')
            guest.until('A20_PPPOE_STOPPED', whole_line=True)
            guest.until('# ')
            guest.send("echo a20-client > /tmp/credentials; echo incorrect >> /tmp/credentials")
            guest.until('# ')
            guest.send(f'pppoe en3 ppp-wan test {args.auth} /tmp/credentials & pp=$!')
            failed = guest.until('PPPOE DOWN Permission denied', timeout=30)
            assert b'PPPOE UP' not in failed, 'invalid credentials installed an uplink'
            peer.until('sent [PAP AuthNak' if args.auth == 'pap' else 'sent [CHAP Failure')
            guest.send('kill "$pp"; wait "$pp"; cat /proc/net/status')
            guest.until('tun: controls=0/4 interfaces=0/2 attached=0')
            guest.until('# ')
            result['passed'].append('wrong_credentials_leave_no_tun')
            guest.send('poweroff')
            guest.until('System is going down for power-off NOW')
            assert guest.process.wait(timeout=15) == 0
            if args.dual:
                verify_ipv6_wire(out/'wan.pcap')
                result['passed'].append('wire_ipv6cp_ipv6_and_dns_options')
            result['status'] = 'passed'
        except Exception as error:
            result['error'] = repr(error)
            raise
        finally:
            if guest:
                guest.close()
            peer.close()
            lan.sock.close()
            (out/'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'BSD_PPPOE_{args.auth.upper()}: PASS')


if __name__ == '__main__':
    main()
