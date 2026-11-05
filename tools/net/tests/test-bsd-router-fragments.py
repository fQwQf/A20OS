#!/usr/bin/env python3
"""Run IPv4 fragment/NAPT tests on a dual-NIC A20OS BSD router.

Build the kernel with NET_ROLE=router NET_STACK=bsd RAMFS_USER=1
RAMFS_NET_TESTS=1 NR_CPUS=2. QEMU UDP links carry Ethernet to local
packet peers and require neither TAP nor host routing changes.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import socket
import struct
import time

spec = importlib.util.spec_from_file_location(
    'backend', Path(__file__).with_name('test-bsd-backend-qemu.py'))
backend = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backend)

ARP = b'\x08\x06'
IPV4 = b'\x08\x00'
MASK = '255.255.255.0'


def checksum(data):
    data += b'\x00' * (len(data) % 2)
    total = sum(struct.unpack(f'!{len(data) // 2}H', data))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return ~total & 0xffff


def pseudo(src, dst, length):
    return src + dst + struct.pack('!BBH', 0, 17, length)


def udp(src, dst, sport, dport, payload):
    length = 8 + len(payload)
    data = struct.pack('!HHHH', sport, dport, length, 0) + payload
    csum = checksum(pseudo(src, dst, length) + data) or 0xffff
    return data[:6] + struct.pack('!H', csum) + payload


def ipv4(src, dst, ident, data, offset=0, more=False):
    frag = offset // 8 | (0x2000 if more else 0)
    ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(data), ident,
                     frag, 64, 17, 0, src, dst)
    return ip[:10] + struct.pack('!H', checksum(ip)) + ip[12:] + data


class Link:
    def __init__(self, index, peer_ip, router_ip):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', 0))
        self.peer_ip = socket.inet_aton(peer_ip)
        self.router_ip = socket.inet_aton(router_ip)
        self.mac = bytes.fromhex(f'02 00 00 01 00 {index:02x}')
        self.router_mac = bytes.fromhex(f'52 54 00 01 00 {index:02x}')
        self.port, = backend.reserve_ports(socket.SOCK_DGRAM, 1)

    def argv(self, index, out):
        return [
            '-netdev', f'socket,id=net{index},udp=127.0.0.1:{self.sock.getsockname()[1]},'
                       f'localaddr=127.0.0.1:{self.port}',
            '-device', f'virtio-net-pci,netdev=net{index},mac={self.router_mac.hex(":")}',
            '-object', f'filter-dump,id=cap{index},netdev=net{index},file={out / f"link{index}.pcap"}',
        ]

    def send(self, ethertype, data, dst=None):
        frame = (dst or self.router_mac) + self.mac + ethertype + data
        self.sock.sendto(frame.ljust(60, b'\0'), ('127.0.0.1', self.port))

    def arp(self, op, dst_mac, dst_ip, ethernet_dst=None):
        self.send(ARP, struct.pack('!HHBBH', 1, 0x800, 6, 4, op) +
                  self.mac + self.peer_ip + dst_mac + dst_ip, ethernet_dst)

    def receive(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('wire receive deadline')
        self.sock.settimeout(remaining)
        frame = self.sock.recv(4096)
        if (frame[12:14] == ARP and frame[20:22] == b'\x00\x01'
                and frame[38:42] == self.peer_ip):
            self.arp(2, frame[22:28], frame[28:32], frame[22:28])
        return frame

    def resolve(self):
        self.arp(1, b'\0' * 6, self.router_ip, b'\xff' * 6)
        deadline = time.monotonic() + 5
        while True:
            frame = self.receive(deadline)
            if (frame[12:14] == ARP and frame[20:22] == b'\x00\x02'
                    and frame[22:32] == self.router_mac + self.router_ip
                    and frame[32:42] == self.mac + self.peer_ip):
                return

    def fragment(self, dst, ident, data, offset=0, more=False, *, src=None):
        self.send(IPV4, ipv4(src or self.peer_ip, dst, ident, data, offset, more))

    def send_datagram(self, dst, ident, data, stride=1200):
        offsets = range(0, len(data), stride)
        for offset in reversed(offsets):
            self.fragment(dst, ident, data[offset:offset + stride], offset,
                          offset + stride < len(data))

    def receive_datagram(self, ident, src, payload, timeout=5, *, mtu=1500, decode=None):
        deadline = time.monotonic() + timeout
        received = {}
        last = None
        while True:
            frame = self.receive(deadline)
            if decode is not None:
                frame = decode(frame)
            if frame[12:14] != IPV4 or frame[18:20] != struct.pack('!H', ident):
                continue
            assert frame[:12] == self.mac + self.router_mac, 'wrong Ethernet endpoints'
            ip = frame[14:]
            _, _, length, _, flags, ttl, proto, _, source, dst = struct.unpack(
                '!BBHHHBBH4s4s', ip[:20])
            assert ip[0] == 0x45 and checksum(ip[:20]) == 0 and 20 < length <= mtu
            assert (ttl, proto, source, dst) == (63, 17, src, self.peer_ip)
            offset = (flags & 0x1fff) * 8
            assert offset not in received, 'duplicate egress fragment'
            received[offset] = ip[20:length]
            if not flags & 0x2000:
                assert last is None, 'multiple final fragments'
                last = offset + length - 20
            if last == sum(map(len, received.values())):
                break

        data = bytearray()
        for offset, part in sorted(received.items()):
            assert offset == len(data), 'gap or overlap on egress'
            data.extend(part)
        data = bytes(data)
        sport, dport, length, csum = struct.unpack('!HHHH', data[:8])
        assert length == len(data) and data[8:] == payload, 'incorrect UDP contents'
        assert csum and checksum(pseudo(src, self.peer_ip, length) + data) == 0
        fragment_payload = (mtu - 20) // 8 * 8
        assert len(received) == (length + fragment_payload - 1) // fragment_payload, 'wrong egress fragment count'
        return sport, dport

    def expect_no_datagram(self, ident):
        deadline = time.monotonic() + 0.75
        while True:
            try:
                frame = self.receive(deadline)
            except TimeoutError:
                return
            if frame[12:14] == IPV4 and frame[18:20] == struct.pack('!H', ident):
                raise AssertionError(f'unexpected egress for ID {ident:#x}')


def exercise(lan, wan, passed):
    sport, dport = 32000, 42000
    payload = bytes(range(256)) * 17
    data = udp(lan.peer_ip, wan.peer_ip, sport, dport, payload)

    # Start the timer early and prove the router received traffic on this
    # link while the first fragment was queued.
    expiry_id = 0x7010
    lan.fragment(wan.peer_ip, expiry_id, data[:1200], more=True)
    started = time.monotonic()

    lan.send_datagram(wan.peer_ip, 0x7000, data)
    mapped, remote = wan.receive_datagram(0x7000, wan.router_ip, payload)
    assert mapped and remote == dport
    passed.append('out_of_order_snat')

    reply_payload = payload[::-1]
    reply = udp(wan.peer_ip, wan.router_ip, dport, mapped, reply_payload)
    wan.send_datagram(wan.router_ip, 0x7001, reply)
    assert lan.receive_datagram(0x7001, wan.peer_ip, reply_payload) == (dport, sport)
    passed.append('out_of_order_reverse_nat')

    blocked = udp(lan.peer_ip, wan.peer_ip, sport, 42001, payload)
    lan.send_datagram(wan.peer_ip, 0x7002, blocked)
    wan.expect_no_datagram(0x7002)
    passed.append('filter_after_reassembly')

    lan.fragment(wan.peer_ip, 0x7003, data[:1200], more=True)
    lan.fragment(wan.peer_ip, 0x7003, data[800:2000], 800, True)
    lan.fragment(wan.peer_ip, 0x7003, data[2000:], 2000)
    wan.expect_no_datagram(0x7003)
    passed.append('overlap_drop')

    # Exercise more fragments than NIC RX tokens without exceeding the
    # per-datagram cap. These fragments must own independent mbuf storage.
    lan.send_datagram(wan.peer_ip, 0x7004, data, stride=80)
    assert wan.receive_datagram(0x7004, wan.router_ip, payload) == (mapped, dport)
    passed.append('fragment_storage_independent_of_rx_tokens')

    # 65 queued fragments with a hole must evict the queue. The later pieces
    # and the final missing fragment cannot produce a completed datagram.
    for offset in range(64, len(data), 64):
        lan.fragment(wan.peer_ip, 0x7005, data[offset:offset + 64], offset,
                     offset + 64 < len(data))
        time.sleep(0.002)
    lan.fragment(wan.peer_ip, 0x7005, data[:64], more=True)
    wan.expect_no_datagram(0x7005)
    passed.append('per_datagram_quota')

    # Identical IP keys on two links must keep separate queues.
    isolation_id = 0x7007
    lan.fragment(wan.peer_ip, isolation_id, data[:1200], more=True)
    for offset in range(1200, len(data), 1200):
        wan.fragment(wan.peer_ip, isolation_id, data[offset:offset + 1200], offset,
                     offset + 1200 < len(data), src=lan.peer_ip)
    wan.expect_no_datagram(isolation_id)
    for offset in range(1200, len(data), 1200):
        lan.fragment(wan.peer_ip, isolation_id, data[offset:offset + 1200], offset,
                     offset + 1200 < len(data))
    assert wan.receive_datagram(isolation_id, wan.router_ip, payload) == (mapped, dport)
    passed.append('ingress_interface_isolation')

    public_port, private_port = 45000, 45001
    inbound = udp(wan.peer_ip, wan.router_ip, dport, public_port, payload)
    wan.send_datagram(wan.router_ip, 0x7008, inbound)
    assert lan.receive_datagram(0x7008, wan.peer_ip, payload) == (dport, private_port)
    response = udp(lan.peer_ip, wan.peer_ip, private_port, dport, reply_payload)
    lan.send_datagram(wan.peer_ip, 0x7009, response)
    assert wan.receive_datagram(0x7009, wan.router_ip, reply_payload) == (public_port, dport)
    passed.append('out_of_order_dnat_and_reply')

    time.sleep(max(0, 33 - (time.monotonic() - started)))
    for offset in range(1200, len(data), 1200):
        lan.fragment(wan.peer_ip, expiry_id, data[offset:offset + 1200], offset,
                     offset + 1200 < len(data))
    wan.expect_no_datagram(expiry_id)
    passed.append('incomplete_datagram_timeout')

    lan.send_datagram(wan.peer_ip, 0x7006, data)
    assert wan.receive_datagram(0x7006, wan.router_ip, payload) == (mapped, dport)
    passed.append('forwarding_after_fragment_drops')


def run_router(args, rules, test, *, wan_ipv4=True, bootargs=()):
    """Boot the shared dual-link wire fixture and persist evidence."""
    kernel = args.kernel.resolve(strict=True)
    out = args.out.resolve()
    out.mkdir(parents=True)
    lan = Link(1, '192.168.10.2', '192.168.10.1')
    wan = Link(2, '10.20.0.2', '10.20.0.1')
    argv = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm',
            '-m', '1024', '-smp', '2', '-nographic', '-monitor', 'none',
            '-no-reboot', '-kernel', str(kernel), '-append',
            ' '.join((f'a20.ip=192.168.10.1 a20.netmask={MASK} a20.dhcp=0', *bootargs)),
            *lan.argv(0, out), *wan.argv(1, out)]
    result = dict(argv=argv, kernel_sha256=hashlib.sha256(kernel.read_bytes()).hexdigest(),
                  passed=[], status='failed')
    with (out / 'serial.log').open('wb') as serial:
        guest = backend.Guest(argv, serial)
        try:
            guest.until('[INIT] entering scheduler...')
            guest.send('echo A20_ROUTER_SHELL_READY')
            guest.until('A20_ROUTER_SHELL_READY', whole_line=True)
            guest.until('# ')
            guest.send('cat /proc/net/status')
            guest.until('backend: bsd')
            guest.until('# ')
            if wan_ipv4:
                guest.send(f'netctl set-ipv4 en3 10.20.0.1 {MASK}')
                guest.until('NETCTL: IPv4 configured')
                guest.until('# ')
            if rules:
                setup = ' && '.join(f"echo '{rule}' > /proc/a20/netfilter" for rule in rules)
                guest.send(setup + ' && echo A20_ROUTER_POLICY_READY')
                guest.until('A20_ROUTER_POLICY_READY', whole_line=True)
                guest.until('# ')
            lan.resolve()
            if wan_ipv4:
                wan.resolve()
            test(guest, lan, wan, result)
            guest.send('poweroff')
            guest.until('System is going down for power-off NOW')
            assert guest.process.wait(timeout=15) == 0
            result['status'] = 'passed'
        except Exception as error:
            result['error'] = repr(error)
            raise
        finally:
            guest.close()
            lan.sock.close()
            wan.sock.close()
            (out / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    rules = [
        'natadd out proto=udp dst=10.20.0.2 action=masquerade',
        'add in proto=udp dport=42001 action=drop',
        'natadd in proto=udp dport=45000 action=dnat to=192.168.10.2 toport=45001',
    ]
    run_router(args, rules, lambda guest, lan, wan, result: exercise(lan, wan, result['passed']))
    print('BSD_ROUTER_FRAGMENTS: PASS')


if __name__ == '__main__':
    main()
