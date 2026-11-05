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
import time

spec = importlib.util.spec_from_file_location(
    'router', Path(__file__).with_name('test-bsd-router-fragments.py'))
router = importlib.util.module_from_spec(spec)
spec.loader.exec_module(router)

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


def wait_up(guest, mode, address):
    output = guest.until('mtu=1492', timeout=50)
    match = re.search(rb'PPPOE UP session=(\d+) auth=(pap|chap) ip=([^ ]+) mtu=1492', output)
    assert match and match[2].decode() == mode and match[3].decode() == address
    return int(match[1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--linux', type=Path, required=True)
    parser.add_argument('--peer-initrd', type=Path, required=True)
    parser.add_argument('--auth', choices=('pap', 'chap'), required=True)
    parser.add_argument('--out', type=Path, required=True)
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
                  f'console=ttyS0 quiet net.ifnames=0 pppauth={args.auth}',
                  *wan_args(ports[1], ports[0], '52:54:00:77:00:01', out/'peer.pcap')]
    a20_argv = [*common, '-kernel', str(args.kernel.resolve()), '-append',
                'a20.ip=192.168.10.1 a20.netmask=255.255.255.0 a20.dhcp=0',
                *lan.argv(0, out),
                *wan_args(ports[0], ports[1], '52:54:00:77:00:02', out/'wan.pcap')]
    result = dict(status='failed', auth=args.auth, passed=[],
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
            guest.send(f'pppoe en3 ppp-wan test {args.auth} /tmp/credentials & pp=$!')
            result['first_session'] = wait_up(guest, args.auth, TUN_IP)
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
            result['second_session'] = wait_up(guest, args.auth, REDIAL_IP)
            # Reuse the five-tuple so a stale MASQUERADE mapping cannot pass.
            echo(lan, b'reconnect-verified', 32000)
            result['passed'].append('peer_disconnect_redials_with_new_address')

            guest.send('kill "$pp"; wait "$pp"; echo A20_PPPOE_STOPPED')
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
