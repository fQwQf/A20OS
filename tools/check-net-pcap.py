#!/usr/bin/env python3
"""Independently check Ethernet/IPv4/TCP/UDP checksums in a QEMU pcap.

No packet library or guest implementation is used. Fragmented datagrams and
other protocols are counted separately; this is not a fragmentation validator.
"""
import argparse
import json
from pathlib import Path
import struct


def valid_checksum(data):
    if len(data) & 1:
        data += b'\0'
    total = sum(struct.unpack(f'!{len(data) // 2}H', data))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return total == 0xffff


def check(path):
    data = path.read_bytes()
    endian = {b'\xd4\xc3\xb2\xa1': '<', b'\xa1\xb2\xc3\xd4': '>'}.get(data[:4])
    if endian is None or len(data) < 24:
        raise ValueError('expected microsecond classic pcap')
    if struct.unpack_from(endian + 'I', data, 20)[0] != 1:
        raise ValueError('expected Ethernet link type')
    result = dict(packets=0, ipv4=0, tcp=0, udp=0, udp_unchecked=0,
                  fragments_skipped=0, other=0, errors=[])
    pos = 24
    while pos < len(data):
        if pos + 16 > len(data):
            raise ValueError('truncated record header')
        _, _, caplen, wirelen = struct.unpack_from(endian + '4I', data, pos)
        pos += 16
        if caplen != wirelen or pos + caplen > len(data):
            raise ValueError('truncated captured packet')
        frame = data[pos:pos + caplen]
        pos += caplen
        result['packets'] += 1
        try:
            if len(frame) < 14:
                raise ValueError('short Ethernet header')
            offset = 14
            kind = int.from_bytes(frame[12:14], 'big')
            while kind in (0x8100, 0x88a8):
                if len(frame) < offset + 4:
                    raise ValueError('short VLAN header')
                kind = int.from_bytes(frame[offset + 2:offset + 4], 'big')
                offset += 4
            if kind != 0x0800:
                result['other'] += 1
                continue
            ip = frame[offset:]
            if len(ip) < 20 or ip[0] >> 4 != 4:
                raise ValueError('short/invalid IPv4 header')
            ihl, length = (ip[0] & 15) * 4, int.from_bytes(ip[2:4], 'big')
            if ihl < 20 or length < ihl or length > len(ip):
                raise ValueError('invalid IPv4 length')
            result['ipv4'] += 1
            if not valid_checksum(ip[:ihl]):
                raise ValueError('IPv4 checksum')
            if int.from_bytes(ip[6:8], 'big') & 0x3fff:
                result['fragments_skipped'] += 1
                continue
            proto = ip[9]
            segment = ip[ihl:length]
            if proto == 6:
                if len(segment) < 20 or not 20 <= (segment[12] >> 4) * 4 <= len(segment):
                    raise ValueError('invalid TCP length')
                result['tcp'] += 1
            elif proto == 17:
                if len(segment) < 8 or int.from_bytes(segment[4:6], 'big') != len(segment):
                    raise ValueError('invalid UDP length')
                result['udp'] += 1
                if segment[6:8] == b'\0\0':
                    result['udp_unchecked'] += 1
                    continue
            else:
                result['other'] += 1
                continue
            pseudo = ip[12:20] + bytes([0, proto]) + len(segment).to_bytes(2, 'big')
            if not valid_checksum(pseudo + segment):
                raise ValueError('TCP checksum' if proto == 6 else 'UDP checksum')
        except ValueError as error:
            result['errors'].append(dict(packet=result['packets'], error=str(error)))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pcap', type=Path)
    args = parser.parse_args()
    result = check(args.pcap)
    print(json.dumps(result, indent=2))
    return 1 if result['errors'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
