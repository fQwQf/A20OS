#!/usr/bin/env python3
"""Fill and refill a BSD router conntrack table using packets on two NICs.

Build with NET_ROLE=router NET_STACK=bsd RAMFS_USER=1 RAMFS_NET_TESTS=1
NR_CPUS=2. Pass --capacity when overriding the 4096-entry router default.
"""
import argparse
import importlib.util
from pathlib import Path
import re
import struct
import time

spec = importlib.util.spec_from_file_location(
    'router', Path(__file__).with_name('test-bsd-router-fragments.py'))
router = importlib.util.module_from_spec(spec)
spec.loader.exec_module(router)

REMOTE_PORT = 42000
FIRST_PORT = 20000


def stats(guest):
    guest.send('cat /proc/a20/netfilter')
    output = guest.until('# ')
    return {key.decode(): int(value) for key, value in
            re.findall(rb'(?m)^(ct_\w+): (\d+)\r?$', output)}


def send_flow(lan, wan, ident, port):
    payload = struct.pack('!HH', ident, port) + b'A20-conntrack-wire'
    lan.fragment(wan.peer_ip, ident,
                 router.udp(lan.peer_ip, wan.peer_ip, port, REMOTE_PORT, payload))
    return payload


def fill(lan, wan, count, first_id):
    ports = []
    for start in range(0, count, 16):
        batch = []
        for flow in range(start, min(start + 16, count)):
            ident = first_id + flow
            port = FIRST_PORT + flow
            batch.append((ident, send_flow(lan, wan, ident, port)))
        for ident, payload in batch:
            mapped, remote = wan.receive_datagram(ident, wan.router_ip, payload)
            if not mapped or remote != REMOTE_PORT:
                raise AssertionError(f'bad translated ports for flow {ident}')
            ports.append(mapped)
    if len(set(ports)) != count:
        raise AssertionError('a WAN reply tuple has multiple owners')
    return ports


def exercise(guest, lan, wan, result, capacity):
    start = stats(guest)
    assert start['ct_capacity'] == capacity and start['ct_tracked'] == 0, start
    result['samples'] = {'empty': start}

    timer = time.monotonic()
    ports = fill(lan, wan, capacity, 1)
    full = stats(guest)
    assert full['ct_tracked'] == capacity and full['ct_evicted'] == 0, full
    assert full['ct_created'] - start['ct_created'] == capacity, full
    result['samples']['full'] = full
    result['first_fill_seconds'] = time.monotonic() - timer
    result['passed'].append('wire_flows_fill_table')

    send_flow(lan, wan, 0xd000, FIRST_PORT + capacity)
    wan.expect_no_datagram(0xd000)
    assert stats(guest)['ct_created'] == full['ct_created']
    result['passed'].append('full_table_refuses_new_tuple')

    for number, flow in enumerate((0, capacity // 2, capacity - 1)):
        ident = 0xd001 + number
        payload = b'existing-flow-reply' + struct.pack('!H', flow)
        wan.fragment(wan.router_ip, ident,
                     router.udp(wan.peer_ip, wan.router_ip, REMOTE_PORT, ports[flow], payload))
        assert lan.receive_datagram(ident, wan.peer_ip, payload) == (REMOTE_PORT, FIRST_PORT + flow)
    assert stats(guest)['ct_tracked'] == capacity
    result['passed'].append('live_bindings_keep_reply_ownership')

    guest.send("echo 'cttimeout 0 0 2000' > /proc/a20/netfilter && echo A20_CT_SHORT")
    guest.until('A20_CT_SHORT', whole_line=True)
    guest.until('# ')
    timer = time.monotonic()
    deadline = timer + 10
    while True:
        after = stats(guest)
        if after['ct_tracked'] == 0:
            break
        if time.monotonic() >= deadline:
            raise TimeoutError(f'idle conntrack entries still held: {after}')
        time.sleep(0.2)
    assert after['ct_timeout'] - full['ct_timeout'] == capacity, after
    assert after['ct_evicted'] == full['ct_evicted'], after
    assert after['ct_sweeps'] > full['ct_sweeps'], after
    result['samples']['expired'] = after
    result['reclaim_seconds'] = time.monotonic() - timer
    result['passed'].append('idle_sweep_reclaims_every_slot')

    guest.send("echo 'cttimeout 0 0 120000' > /proc/a20/netfilter && echo A20_CT_LONG")
    guest.until('A20_CT_LONG', whole_line=True)
    guest.until('# ')
    fill(lan, wan, capacity, 0x8000)
    refilled = stats(guest)
    assert refilled['ct_tracked'] == capacity and refilled['ct_evicted'] == 0, refilled
    assert refilled['ct_created'] - after['ct_created'] == capacity, refilled
    result['samples']['refilled'] = refilled
    result['passed'].append('all_slots_reusable_after_expiry')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--capacity', type=int, default=4096,
                        choices=(256, 512, 1024, 2048, 4096, 8192, 16384))
    args = parser.parse_args()
    router.run_router(args, [
        'natadd out proto=udp dst=10.20.0.2 action=masquerade',
        'cttimeout 0 0 120000',
    ], lambda guest, lan, wan, result: exercise(guest, lan, wan, result, args.capacity))
    print('BSD_ROUTER_CAPACITY: PASS')


if __name__ == '__main__':
    main()
