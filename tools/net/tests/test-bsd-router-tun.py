#!/usr/bin/env python3
"""Exercise a routed BSD TUN through a fixed PPPoE session on a second NIC."""
import argparse
import importlib.util
from pathlib import Path
import socket
import struct

spec = importlib.util.spec_from_file_location(
    'router', Path(__file__).with_name('test-bsd-router-fragments.py'))
router = importlib.util.module_from_spec(spec)
spec.loader.exec_module(router)

PPPOE = b'\x88\x64'
SESSION = b'\x11\x00\x12\x34'
PPP_IPV4 = b'\x00\x21'
TUN_IP = socket.inet_aton('198.18.0.10')
MTU = 1492


def decode(frame):
    if frame[12:14] != PPPOE:
        return b''
    assert frame[14:18] == SESSION and frame[20:22] == PPP_IPV4, 'wrong session'
    length, = struct.unpack_from('!H', frame, 18)
    assert 22 <= length <= MTU + 2 and len(frame) >= 20 + length
    return frame[:12] + router.IPV4 + frame[22:20 + length]


def exercise(guest, lan, wan, result):
    for run in range(2):
        guest.send('bsd_tun_router_test')
        guest.until('TUN_ROUTER_READY', whole_line=True)
        dport = 42000
        sport = 32000 + run
        ident = 0x7500 + 2 * run
        payload = bytes(range(256)) * 17
        request = router.udp(lan.peer_ip, wan.peer_ip, sport, dport, payload)
        lan.send_datagram(wan.peer_ip, ident, request)
        mapped, remote = wan.receive_datagram(ident, TUN_IP, payload, mtu=MTU, decode=decode)
        assert mapped and remote == dport

        reply_payload = b'routed PPP reply'
        reply = router.udp(wan.peer_ip, TUN_IP, dport, mapped, reply_payload)
        packet = router.ipv4(wan.peer_ip, TUN_IP, ident + 1, reply)
        wan.send(PPPOE, SESSION + struct.pack('!H', len(packet) + 2) + PPP_IPV4 + packet)
        assert lan.receive_datagram(ident + 1, wan.peer_ip, reply_payload) == (dport, sport)
        guest.until('TUN_ROUTER_CLOSED', whole_line=True)
        guest.until('# ')
        result['passed'].append(f'tun_default_route_napt_mtu_session_{run + 1}')

    # Both the BSD RIB and the common route table must release the default
    # when the first TUN closes, or the second exclusive add would fail.
    result['passed'].append('default_route_released_on_tun_close')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    router.run_router(args, ['natadd out proto=udp dst=10.20.0.2 action=masquerade'],
                      exercise, wan_ipv4=False)
    print('BSD_ROUTER_TUN: PASS')


if __name__ == '__main__':
    main()
