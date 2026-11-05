#!/usr/bin/env python3
"""Exercise policy routing on connected UDP and TCP sockets in a router guest."""
import importlib.util
from pathlib import Path
import socket
import struct
import time

spec = importlib.util.spec_from_file_location(
    'policy4', Path(__file__).with_name('test-bsd-policy-v4.py'))
policy4 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy4)
wire = policy4.wire
ra = policy4.pd.ra

PORT = 42000
SYN, ACK, PUSH = 2, 16, 8


class Flow:
    def __init__(self, guest, version, source, destination, gateway, protocol):
        self.guest = guest
        self.version = version
        self.src = socket.inet_pton(socket.AF_INET if version == 4 else socket.AF_INET6, source)
        self.dst = socket.inet_pton(socket.AF_INET if version == 4 else socket.AF_INET6, destination)
        self.gateway = gateway
        self.protocol = protocol
        self.file = f'/tmp/policy-{version}-{protocol}-{destination.replace(":", "-")}'
        self.sequence = 100000

    def start(self, source, destination, *, device=None):
        kind = 'tcp' if self.protocol == 6 else 'udp'
        binding = f' {device}' if device else ''
        self.guest.send(f'/extra/policy-local-probe {kind} {source} {destination} {PORT} {self.file}{binding} &')

    def ready(self):
        self.guest.until(f'A20_POLICY_PROBE_READY {self.file}', timeout=10, whole_line=True)

    def pseudo(self, src, dst, length):
        if self.version == 6:
            return ra.pseudo(src, dst, self.protocol, length)
        return src + dst + struct.pack('!BBH', 0, self.protocol, length)

    def incoming(self, link, timeout, predicate, *, optional=False):
        def match(frame, protocol, hops, src, dst, packet):
            if (protocol, src, dst) != (self.protocol, self.src, self.dst):
                return False
            if len(packet) < (20 if self.protocol == 6 else 8):
                return False
            if struct.unpack_from('!H', packet, 2)[0] != PORT or not predicate(packet):
                return False
            assert frame[:12] == link.mac + link.router_mac
            assert wire.checksum(self.pseudo(src, dst, len(packet)) + packet) == 0
            return True

        try:
            if self.version == 6:
                return ra.receive(link, timeout, match, neighbor=self.gateway)[1]
            deadline = time.monotonic() + timeout
            while True:
                frame = link.receive(deadline)
                if frame[12:14] != wire.IPV4 or len(frame) < 34:
                    continue
                ip = frame[14:]
                length = struct.unpack_from('!H', ip, 2)[0]
                packet = ip[(ip[0] & 15) * 4:length]
                if match(frame, ip[9], ip[8], ip[12:16], ip[16:20], packet):
                    assert wire.checksum(ip[:20]) == 0
                    return packet
        except TimeoutError:
            if not optional:
                raise
        return None

    def send(self, link, packet, checksum_offset):
        checksum = wire.checksum(self.pseudo(self.dst, self.src, len(packet)) + packet) or 0xffff
        packet = packet[:checksum_offset] + struct.pack('!H', checksum) + packet[checksum_offset + 2:]
        if self.version == 6:
            ra.send(link, self.dst, self.src, self.protocol, packet, hops=64)
        else:
            header = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(packet), 0x7001,
                                 0, 64, self.protocol, 0, self.dst, self.src)
            header = header[:10] + struct.pack('!H', wire.checksum(header)) + header[12:]
            link.send(wire.IPV4, header + packet)

    def tcp_send(self, link, port, acknowledgement, flags, data=b''):
        packet = struct.pack('!HHIIBBHHH', PORT, port, self.sequence, acknowledgement,
                             5 << 4, flags, 8192, 0, 0)
        self.send(link, packet + data, 16)
        self.sequence += len(data) + bool(flags & SYN)

    def handshake(self, expected, other):
        predicate = lambda packet: packet[13] & (SYN | ACK) == SYN
        syn = self.incoming(expected, 5, predicate)
        assert self.incoming(other, 0.25, predicate, optional=True) is None, 'SYN on wrong WAN'
        # FreeBSD emits the MSS option first. This WAN has MTU 1280.
        assert syn[20:22] == b'\x02\x04', syn.hex()
        assert struct.unpack_from('!H', syn, 22)[0] == (1240 if self.version == 4 else 1220), syn.hex()
        port, _, sequence = struct.unpack_from('!HHI', syn)
        self.tcp_send(expected, port, sequence + 1, SYN | ACK)
        self.ready()

    def echo(self, expected, other, tag):
        data = tag.encode()
        self.guest.send(f'echo {tag} > {self.file}')
        offset = (lambda packet: (packet[12] >> 4) * 4) if self.protocol == 6 else (lambda packet: 8)
        predicate = lambda packet: packet[offset(packet):] == data
        packet = self.incoming(expected, 5, predicate)
        assert self.incoming(other, 0.25, predicate, optional=True) is None, 'data on wrong WAN'
        port = struct.unpack_from('!H', packet)[0]
        if self.protocol == 6:
            sequence = struct.unpack_from('!I', packet, 4)[0]
            self.tcp_send(expected, port, sequence + len(data), ACK | PUSH, data)
        else:
            self.send(expected, struct.pack('!4H', PORT, port, 8 + len(data), 0) + data, 6)
        self.guest.until(f'A20_POLICY_PROBE_ECHO {tag}', timeout=8, whole_line=True)

    def finish(self):
        policy4.configure(self.guest, f'echo quit > {self.file}')


def exercise(guest, lan, wan_a, wan_b, result):
    for name, ipv4, ipv6 in [('en3', '10.20.0.1', '2001:db8:20::1'),
                              ('en4', '10.30.0.1', '2001:db8:30::1')]:
        policy4.configure(guest, f'netctl set-ipv4 {name} {ipv4} 255.255.255.0')
        policy4.configure(guest, f'netctl set-ipv6 {name} {ipv6} 64')
    policy4.configure(guest, 'netctl set-ipv6 en2 2001:db8:10::1 64')
    policy4.command(guest, 'link set dev en4 mtu 1280')
    time.sleep(1.5)
    for link in (wan_a, wan_b):
        link.resolve()

    for version, source, target, network, gateways, other_src, other_target in (
        (4, '192.168.10.1', '198.51.100.9', '198.51.100.0/24', ('10.20.0.2', '10.30.0.2'),
         '10.30.0.1', '203.0.113.7'),
        (6, '2001:db8:10::1', '2001:db8:99::9', '2001:db8:99::/64', ('fe80::2', 'fe80::2'),
         '2001:db8:30::1', '2001:db8:100::7'),
    ):
        def ip(text):
            return policy4.command(guest, f'-{version} {text}')

        gateway = ra.ip('fe80::2') if version == 6 else None
        source_prefix = f'{source}/{32 if version == 4 else 128}'
        ip(f'route add {network} via {gateways[0]} dev en3')
        ip(f'route add {network} via {gateways[1]} dev en4 table 200')
        rule = f'priority 100 from {source_prefix} to {network} table 200'
        for protocol in (17, 6):
            ip(f'rule add {rule}')
            flow = Flow(guest, version, source, target, gateway, protocol)
            flow.start(source, target)
            if protocol == 6:
                flow.handshake(wan_b, wan_a)
            else:
                flow.ready()
            flow.echo(wan_b, wan_a, f'v{version}-{protocol}-alternate')
            ip(f'rule del {rule}')
            flow.echo(wan_a, wan_b, f'v{version}-{protocol}-main')
            ip(f'rule add {rule}')
            flow.echo(wan_b, wan_a, f'v{version}-{protocol}-restored')
            flow.finish()
            ip(f'rule del {rule}')
            result['passed'].append(f'ipv{version}_{"tcp" if protocol == 6 else "udp"}_bound_socket_reselects_fib')

        if version == 6:
            ip(f'rule add {rule}')
            flow = Flow(guest, version, source, target, gateway, 17)
            flow.file += '-bound-device'
            flow.start(source, target, device='en3')
            flow.ready()
            flow.echo(wan_a, wan_b, 'v6-explicit-interface')
            flow.finish()
            ip(f'rule del {rule}')
            result['passed'].append('ipv6_explicit_interface_keeps_native_route')

        prefix = f'{other_target}/{32 if version == 4 else 128}'
        ip(f'route add {prefix} via {gateways[1]} dev en4 table 200')
        ip(f'rule add priority 120 to {prefix} table 200')
        flow = Flow(guest, version, other_src, other_target, gateway, 17)
        flow.start('-', other_target)
        flow.ready()
        flow.echo(wan_b, wan_a, f'v{version}-unbound-destination-rule')
        flow.finish()
        result['passed'].append(f'ipv{version}_destination_only_rule_selects_source_without_main_route')


def main():
    probe = Path(__file__).with_name('bsd-policy-local-probe.c')
    policy4.run_router(policy4.arguments(__doc__), exercise, probes=(probe,))
    print('BSD_POLICY_LOCAL: PASS')


if __name__ == '__main__':
    main()
