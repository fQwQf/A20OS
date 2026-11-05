#!/usr/bin/env python3
"""Request an IPv6 prefix from a real Kea peer with stock BusyBox or odhcp6c."""
import argparse
import hashlib
import importlib.util
import ipaddress
import json
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import tempfile

spec = importlib.util.spec_from_file_location(
    'pppoe', Path(__file__).with_name('test-bsd-pppoe-kvm.py'))
pppoe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pppoe)


spec = importlib.util.spec_from_file_location(
    'ra', Path(__file__).with_name('test-bsd-router-ipv6-ra.py'))
ra = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ra)


def make_payload(out, busybox, probe, odhcp6c, *, extra_probes=()):
    image = out/'payload.img'
    with tempfile.TemporaryDirectory(prefix='a20-pd-payload-') as directory:
        root = Path(directory)
        shutil.copy2(busybox, root/'busybox')
        sources = list(extra_probes)
        if probe:
            sources.append(Path(__file__).with_name('bsd-udp6-bind-probe.c'))
        for source in sources:
            subprocess.run(['cc', '-static', '-Wall', '-Wextra', '-Werror', '-O2',
                            str(source), '-o', str(root/source.stem.removeprefix('bsd-'))], check=True)
        if odhcp6c:
            shutil.copy2(odhcp6c, root/'odhcp6c')
            event = root/'odhcp-event'
            event.write_text('''#!/bin/mksh
echo "A20_OD_EVENT state=$2 prefixes=$PREFIXES A20_OD_END"
''')
            event.chmod(0o755)
        event = root/'pd-event'
        event.write_text('''#!/bin/mksh
if [ "$1" = bound ] && [ -n "$ipv6" ]; then
    /bin/netctl set-ipv6 en3 "$ipv6" 128 || exit 1
fi
echo "A20_PD_EVENT event=$1 prefix=$ipv6prefix valid=$ipv6prefix_lease"
''')
        event.chmod(0o755)
        fs = Path(directory).with_suffix('.ext4')
        try:
            mkfs = shutil.which('mkfs.ext4') or '/usr/sbin/mkfs.ext4'
            subprocess.run([mkfs, '-q', '-F', '-O', '^orphan_file,^has_journal',
                            '-d', str(root), str(fs), '16M'], check=True)
            with image.open('wb') as disk:
                disk.truncate(18 * 1024 * 1024)
            sfdisk = shutil.which('sfdisk') or '/usr/sbin/sfdisk'
            subprocess.run([sfdisk, str(image)], check=True, stdout=subprocess.DEVNULL,
                           input='label: gpt\nstart=2048,size=32768,type=L,name="A20OS distro"\n',
                           text=True)
            with image.open('r+b') as disk, fs.open('rb') as payload:
                disk.seek(2048 * 512)
                shutil.copyfileobj(payload, disk)
        finally:
            fs.unlink(missing_ok=True)
    return image


def dhcp_options(data):
    while data:
        assert len(data) >= 4
        code, length = struct.unpack_from('!2H', data)
        assert length + 4 <= len(data)
        yield code, data[4:4 + length]
        data = data[4 + length:]


def verify_exchange(capture, renew, bind_probe, pd_only=False):
    data = capture.read_bytes()
    assert data[:4] == bytes.fromhex('d4c3b2a1'), 'unexpected pcap byte order'
    offset = 24
    exchange = []
    wan_reply = False
    while offset + 16 <= len(data):
        _, _, length, _ = struct.unpack_from('<4I', data, offset)
        frame = data[offset + 16:offset + 16 + length]
        offset += 16 + length
        if len(frame) < 62 or frame[12:14] != b'\x86\xdd' or frame[20] != 17:
            continue
        source, destination, udp_length, checksum = struct.unpack_from('!4H', frame, 54)
        udp = frame[54:54 + udp_length]
        if (source, destination) == (45678, 45679) and udp[8:] == b'wan':
            wan_reply = frame[:12] == bytes.fromhex('525400770001525400770002')
        if (source, destination) not in ((546, 547), (547, 546)):
            continue
        pseudo = frame[22:54] + struct.pack('!I3xB', udp_length, 17)
        assert checksum and len(udp) == udp_length and len(udp) >= 12
        assert not pppoe.router.checksum(pseudo + udp)
        if pd_only:
            options = list(dhcp_options(udp[12:]))
            assert all(code != 3 for code, _ in options), 'unexpected IA_NA'
            if udp[8] == 7:
                delegation = next(value for code, value in options if code == 25)
                prefix = next(value for code, value in dhcp_options(delegation[12:]) if code == 26)
                assert struct.unpack_from('!IIB', prefix) == (24, 36, 56)
        exchange.append((source, destination, udp[8], udp[9:12]))
    if bind_probe:
        assert wan_reply, 'bound reply missing from WAN'
    expected = [(546, 547, 1), (547, 546, 2), (546, 547, 3), (547, 546, 7)]
    observed = [(s, d, kind) for s, d, kind, _ in exchange]
    assert observed[:4] == expected, exchange
    if renew:
        assert len(exchange) >= 6 and len(exchange) % 2 == 0, exchange
        expected.extend([(546, 547, 5), (547, 546, 7)] * ((len(exchange) - 4) // 2))
    assert observed == expected, exchange
    assert all(exchange[i][3] == exchange[i+1][3] for i in range(0, len(expected), 2))


def verify_routing(peer, guest, lan, prefix):
    # Kea delegates addresses. The WAN router installs the route to the
    # requesting router's link-local next hop in this isolated topology.
    upstream = '2001:db8:30::1'
    peer.send(f'ip -6 addr add {upstream}/128 dev lo && '
              f'ip -6 route add {prefix} via fe80::5054:ff:fe77:2 dev eth0 && '
              'echo A20_PD_PEER_ROUTE')
    peer.until('A20_PD_PEER_ROUTE', whole_line=True)
    router_ip = prefix.network_address + 1
    host = (prefix.network_address + 2).packed
    guest.send(f'netctl set-ipv6 en2 {router_ip} 64')
    guest.until('NETCTL: IPv6 configured', whole_line=True)
    guest.until('# ')
    # Wait out LAN address DAD before using it as the forwarding interface.
    guest.send('sleep 2; echo A20_PD_LAN_READY')
    guest.until('A20_PD_LAN_READY', whole_line=True)
    guest.until('# ')
    echo = b'\x73\x12\x00\x01delegated-prefix'
    server = ra.ip(upstream)
    ra.icmp(lan, host, server, 128, echo, hops=64)
    ra.receive(lan, 8, lambda f, p, h, s, d, b:
               p == ra.ICMP6 and h == 63 and s == server and d == host
               and f[:6] == lan.mac and b[:1] == b'\x81' and b[4:] == echo
               and not pppoe.router.checksum(ra.pseudo(s, d, p, len(b)) + b),
               neighbor=host)


def verify_bind(guest, lan, wan_port):
    def datagram(source, destination, payload):
        udp = struct.pack('!4H', 45679, 45678, len(payload) + 8, 0) + payload
        check = ra.router.checksum(ra.pseudo(source, destination, 17, len(udp)) + udp) or 0xffff
        return udp[:6] + struct.pack('!H', check) + udp[8:]

    lan_source = ra.ip('fe80::2')
    lan_target = ra.ip('fe80::5054:ff:fe01:1')
    wan_source = ra.ip('fe80::5054:ff:fe77:1')
    wan_target = ra.ip('fe80::5054:ff:fe77:2')
    guest.send('/extra/udp6-bind-probe')
    guest.until('A20_UDP6_BOUND', whole_line=True)
    ra.send(lan, lan_source, lan_target, 17, datagram(lan_source, lan_target, b'wrong'), hops=64)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as inject:
        udp = datagram(wan_source, wan_target, b'wan')
        eth = bytes.fromhex('52540077000252540077000186dd')
        ip = struct.pack('!IHBB16s16s', 6 << 28, len(udp), 17, 64, wan_source, wan_target)
        inject.sendto(eth + ip + udp, ('127.0.0.1', wan_port))
    guest.until('A20_UDP6_UNBOUND', whole_line=True)
    ra.send(lan, lan_source, lan_target, 17, datagram(lan_source, lan_target, b'lan'), hops=64)
    ra.receive(lan, 5, lambda f, p, h, s, d, b:
               p == 17 and s == lan_target and d == lan_source and b[8:] == b'lan',
               neighbor=lan_source)
    guest.until('A20_UDP6_BIND_PASS', whole_line=True)
    guest.until('# ')


def odhcp_lease(guest, peer, event):
    marker = f'A20_OD_EVENT state={event} prefixes='
    output = (guest.until(marker, timeout=35, drain_peer=peer)
              + guest.until(' A20_OD_END', drain_peer=peer))
    match = re.search(rb'prefixes=([^ ,]+),(\d+),(\d+)(?:,[^ ]+)? A20_OD_END', output)
    assert match, output
    preferred, valid = int(match[2]), int(match[3])
    assert 0 < preferred <= 24 and 12 < valid <= 36 and valid - preferred == 12, output
    return ipaddress.IPv6Network(match[1].decode()), preferred, valid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ('kernel', 'linux', 'peer-initrd', 'busybox', 'out'):
        parser.add_argument(f'--{arg}', type=Path, required=True)
    parser.add_argument('--renew', action='store_true', help='keep the client running through renewal')
    parser.add_argument('--bind-probe', action='store_true', help='check IPv6 UDP binding on both links')
    parser.add_argument('--odhcp6c', type=Path, help='test a stock static odhcp6c binary in PD-only mode')
    args = parser.parse_args()
    if args.odhcp6c and args.renew:
        parser.error('--renew is for BusyBox, odhcp6c always renews')
    out = args.out.resolve()
    out.mkdir(parents=True)
    image = make_payload(out, args.busybox, args.bind_probe, args.odhcp6c)
    ports = pppoe.router.backend.reserve_ports(socket.SOCK_DGRAM, 2)
    lan = pppoe.router.Link(1, '192.168.10.2', '192.168.10.1')
    common = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm',
              '-m', '768', '-smp', '2', '-nographic', '-monitor', 'none', '-no-reboot']
    linux = [*common, '-cpu', 'host', '-kernel', str(args.linux.resolve()),
             '-initrd', str(args.peer_initrd.resolve()), '-append', 'console=ttyS0 quiet net.ifnames=0',
             *pppoe.wan_args(ports[1], ports[0], '52:54:00:77:00:01', out/'peer.pcap')]
    a20 = [*common, '-kernel', str(args.kernel.resolve()), '-append',
           'a20.ip=192.168.10.1 a20.netmask=255.255.255.0 a20.dhcp=0 a20.ipv6_ra=en3',
           *lan.argv(0, out),
           *pppoe.wan_args(ports[0], ports[1], '52:54:00:77:00:02', out/'wan.pcap'),
           '-drive', f'file={image},if=none,format=raw,id=payload',
           '-device', 'virtio-blk-pci,drive=payload']
    result = dict(status='failed', passed=[], argv={'router': a20, 'peer': linux},
                  sha256={str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (args.kernel, args.linux, args.peer_initrd, args.busybox)})
    if args.odhcp6c:
        result['sha256'][str(args.odhcp6c)] = hashlib.sha256(args.odhcp6c.read_bytes()).hexdigest()
    Guest = pppoe.router.backend.Guest
    with (out/'peer.log').open('wb') as peer_log, (out/'serial.log').open('wb') as serial:
        peer = Guest(linux, peer_log)
        guest = None
        try:
            peer.until('A20_LINUX_DHCP6_PEER_READY')
            peer.until('DHCP6_STARTED')
            guest = Guest(a20, serial)
            guest.until('[INIT] entering scheduler...')
            guest.send('echo A20_PD_SHELL')
            guest.until('A20_PD_SHELL', whole_line=True)
            guest.until('# ')
            for _ in range(8):
                guest.send('sleep 1; /extra/busybox ip -6 addr show dev en3')
                address = guest.until('# ')
                if (b'fe80::5054:ff:fe77:2/64 scope link' in address
                        and b'2001:db8:20:0:5054:ff:fe77:2/64 scope global' in address
                        and b'tentative' not in address):
                    break
            else:
                raise AssertionError(address)
            result['passed'].append('linux_rtnetlink_reports_wan_link_local_and_slaac')

            if args.odhcp6c:
                guest.send('/extra/odhcp6c -e -l 7 -k -N none -P 56 -F -s /extra/odhcp-event en3 & pd=$!')
                prefix, preferred, valid = odhcp_lease(guest, peer, 'bound')
                result['preferred_seconds'] = preferred
            else:
                command = '/extra/busybox udhcpc6 -f -n -d -i en3 -s /extra/pd-event'
                # BusyBox 1.37 dereferences a NULL IA_NA pointer on PD-only Renew.
                guest.send(command + (' & pd=$!' if args.renew else ' -r no -q'))
                output = guest.until('A20_PD_EVENT event=bound prefix=', timeout=30) + guest.until('valid=36')
                match = re.search(rb'event=bound prefix=(\S+) valid=(\d+)', output)
                assert match and int(match[2]) == 36, output
                prefix = ipaddress.IPv6Network(match[1].decode())
                valid = int(match[2])
            assert prefix.prefixlen == 56 and prefix.subnet_of(ipaddress.IPv6Network('2001:db8:400::/48'))
            result['prefix'] = str(prefix)
            result['valid_seconds'] = valid
            result['passed'].append('kea_prefix_received_by_stock_client')
            if args.odhcp6c:
                renewed, preferred, valid = odhcp_lease(guest, peer, 'updated')
                assert renewed == prefix
                result['renewed_lifetimes'] = [preferred, valid]
            verify_routing(peer, guest, lan, prefix)
            result['passed'].append('delegated_prefix_routes_in_both_directions')
            if args.renew:
                output = guest.until('A20_PD_EVENT event=renew prefix=', timeout=35) + guest.until('valid=36')
                updated = re.search(rb'event=renew prefix=(\S+) valid=36', output)
                assert updated and ipaddress.IPv6Network(updated[1].decode()) == prefix, output
            if args.renew or args.odhcp6c:
                guest.send('kill "$pd"; wait "$pd"; echo A20_PD_CLIENT_STOPPED')
                guest.until('A20_PD_CLIENT_STOPPED', whole_line=True)
                guest.until('# ')
                result['passed'].append('kea_renews_the_existing_delegation')
            if args.bind_probe:
                verify_bind(guest, lan, ports[0])
                result['passed'].append('ipv6_udp_binding_rejects_foreign_ingress_and_egress')
            guest.send('poweroff')
            guest.until('System is going down for power-off NOW')
            assert guest.process.wait(timeout=15) == 0
            verify_exchange(out/'wan.pcap', args.renew or args.odhcp6c, args.bind_probe, bool(args.odhcp6c))
            result['passed'].append('solicit_advertise_request_reply_on_wire')
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
    print('BSD_DHCP6_PD: PASS')


if __name__ == '__main__':
    main()
