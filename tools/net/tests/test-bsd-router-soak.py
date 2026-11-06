#!/usr/bin/env python3
"""Run concurrent TCP, UDP and ICMP through a BSD router with a DHCP WAN."""
import argparse
from contextlib import ExitStack
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import time

spec = importlib.util.spec_from_file_location(
    'lan_test', Path(__file__).with_name('test-bsd-pd-lan.py'))
lan_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lan_test)
pd = lan_test.pd
command = lan_test.command
backend = pd.pppoe.router.backend

WAN = '10.20.0.2'
PROBE = '198.51.100.9'
NETWORKS = ('192.168.10', '192.168.11')
WAN_CONFIG = '''interface=en3
mode=dhcp
busybox=/extra/busybox
resolver=/etc/resolv.conf
'''
DHCP_CONFIG = '''start 10.20.0.10
end 10.20.0.10
interface eth0
option subnet 255.255.255.0
option router 10.20.0.2
option dns 10.20.0.2
option lease 30
lease_file /tmp/soak.leases
'''
CLIENT = '''#!/bin/sh
set -eu
case "$1" in
    tcp) size=32768; port=39021 ;;
    udp) size=512; port=39022 ;;
    *) exit 2 ;;
esac
exec /net-microbench client "$1" "$port" 10.20.0.2 "$size" "$2"
'''


def peer_image(args, out):
    path = out / 'peer.cpio.gz'
    with tempfile.TemporaryDirectory(prefix='a20-router-soak-') as directory:
        root = Path(directory)
        source = Path(__file__).resolve().parents[2] / 'bench/net-microbench.c'
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
                        str(source), '-o', str(root/'net-microbench')], check=True)
        (root/'soak-dhcp.conf').write_text(DHCP_CONFIG)
        (root/'soak-client').write_text(CLIENT)
        (root/'soak-client').chmod(0o755)
        overlay = subprocess.run(['cpio', '--quiet', '-o', '-H', 'newc'], cwd=root,
                                 input=b'net-microbench\nsoak-dhcp.conf\nsoak-client\n',
                                 capture_output=True, check=True).stdout
        shutil.copyfile(args.peer_initrd, path)
        with path.open('ab') as stream:
            stream.write(gzip.compress(overlay, mtime=0))
    return path


def shell(guest, line, peers=(), timeout=25):
    status, output = command(guest, line, peers, timeout)
    if status:
        raise RuntimeError(f'{line!r} exited {status}: {output[-1500:]!r}')
    return output


def sample(router, peers, start):
    out = shell(router, 'cat /proc/a20/netmem /proc/a20/netfilter /proc/meminfo /proc/net/dev', peers)
    backing = re.search(rb'(?m)^BSD backing: ([^\r\n]+)', out)
    assert backing, out
    memory = {key.decode(): int(value) for key, value in
              re.findall(rb'(MemAvailable|MemFree|Slab):\s+(\d+) kB', out)}
    track = {key.decode(): int(value) for key, value in
             re.findall(rb'(?m)^(ct_\w+): (\d+)\r?$', out)}
    devices = {}
    for name, row in re.findall(rb'(?m)^(en\d+): (rx=[^\r\n]+)', out):
        devices[name.decode()] = {key.decode(): int(value) for key, value in
                                  re.findall(rb'(\w+)=(\d+)', row)}
    assert 'MemAvailable' in memory and 'ct_packets' in track and len(devices) == 3, out
    return dict(seconds=round(time.monotonic()-start, 3), memory_kib=memory, conntrack=track,
                devices=devices, bsd_bytes={key.decode(): int(value) for key, value in
                                            re.findall(rb'(\w+)=(\d+)', backing[1])})


def carrier(port, up):
    with socket.create_connection(('127.0.0.1', port), timeout=5) as monitor:
        with monitor.makefile('rwb', buffering=0) as qmp:
            assert 'QMP' in json.loads(qmp.readline())
            qmp.write(b'{"execute":"qmp_capabilities"}\r\n')
            assert 'return' in json.loads(qmp.readline())
            request = {'execute': 'set_link', 'arguments': {'name': 'net1', 'up': up}}
            qmp.write(json.dumps(request).encode() + b'\r\n')
            while True:
                reply = json.loads(qmp.readline())
                if 'event' not in reply:
                    assert 'return' in reply, reply
                    break


def benchmark(client, path, seconds, peers):
    shell(client, f'(sh /soak-client tcp {seconds} >{path}.tcp 2>&1 & t=$!; '
          f'sh /soak-client udp {seconds} >{path}.udp 2>&1 & u=$!; '
          f'wait "$t"; tr=$?; wait "$u"; ur=$?; echo "$tr $ur" >{path}.status) </dev/null & true', peers)


def await_result(client, path, peers):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        status, out = command(client, f'test -f {path}.status && cat {path}.status {path}.tcp {path}.udp',
                              peers)
        if status == 0:
            assert re.search(rb'(?m)^0 0\r?$', out), out
            rows = [dict(field.split('=', 1) for field in line.decode().split())
                    for line in re.findall(rb'(?m)^RESULT ([^\r\n]+)', out)]
            assert {row['protocol'] for row in rows} == {'tcp', 'udp'}, out
            assert all(row['verified'] == '1' and int(row['count']) > 10 for row in rows), rows
            return rows
        time.sleep(0.4)
    raise TimeoutError(path)


def exercise(args, guests, result, checkpoint):
    router, wan, *clients = guests
    for peer in (wan, *clients):
        peer.until('A20_LINUX_LAN_READY')
    shell(wan, f'ip link set lo up && ip addr add {WAN}/24 dev eth0 && '
          f'ip addr add {PROBE}/32 dev lo && '
          '(busybox udhcpd -f -a 10 /soak-dhcp.conf >/tmp/dhcp.log 2>&1 & '
          '/net-microbench server tcp 39021 >/tmp/tcp.log 2>&1 & '
          '/net-microbench server udp 39022 >/tmp/udp.log 2>&1 & true)')
    router.until('[INIT] entering scheduler...')
    if args.router_image:
        router.until('# ', timeout=30)
        shell(router, 'test -x /bin/netctl && test -x /bin/wanctl && test -x /bin/pppoe && '
              'test -x /bin/pd-lan && test ! -e /bin/bsd_scale_test', (wan, *clients))
        result['passed'].append('uefi_router_image_contains_production_tools_without_net_diagnostics')
        shell(router, 'test -f /extra/wan.conf && netctl set-ipv4 en2 192.168.10.1 255.255.255.0',
              (wan, *clients))
        router.send('/bin/wanctl /extra/wan.conf &')
    router.until('WANCTL: en3 configured 10.20.0.10/24', timeout=30)
    shell(router, 'netctl set-ipv4 en4 192.168.11.1 255.255.255.0 && '
          f"echo 'natadd out proto=tcp dst={WAN} action=masquerade' >/proc/a20/netfilter && "
          f"echo 'natadd out proto=udp dst={WAN} action=masquerade' >/proc/a20/netfilter && "
          "echo 'natadd out proto=icmp action=masquerade' >/proc/a20/netfilter",
          (wan, *clients))
    for client, network in zip(clients, NETWORKS):
        out = shell(client, f'ip addr add {network}.2/24 dev eth0 && '
                    f'ip route add default via {network}.1 && busybox ping -c 2 -W 3 {PROBE}', (router, wan))
        assert f'bytes from {PROBE}:'.encode() in out, out
    result['passed'].append('two_linux_clients_on_distinct_lans_use_dhcp_wan')

    # Collect live tuples inside the WAN guest to keep serial I/O outside
    # the timing-critical client processes.
    shell(wan, '((for n in $(busybox seq 1 8); do echo A20_TCP_SAMPLE; '
          'cat /proc/net/tcp; sleep 1; done) >/tmp/tcp-tuple) </dev/null & true',
          (router, *clients))
    start = time.monotonic()
    samples = result['samples'] = [sample(router, (wan, *clients), start)]
    for client in clients:
        benchmark(client, '/tmp/steady', args.duration, (router, wan))
        shell(client, f'busybox ping -i 1 -c {args.duration} -W 3 {PROBE} '
              '>/tmp/steady.ping 2>&1 </dev/null & true', (router, wan))
    deadline = time.monotonic() + args.duration
    while True:
        time.sleep(max(0, min(args.interval, deadline-time.monotonic())))
        samples.append(sample(router, (wan, *clients), start))
        checkpoint()
        if time.monotonic() >= deadline:
            break
    out = shell(wan, 'cat /tmp/tcp-tuple', (router, *clients))
    for snapshot in out.split(b'A20_TCP_SAMPLE'):
        ports = re.findall(rb'(?m)^\s*\d+: 0200140A:986D 0A00140A:([0-9A-F]{4}) 01\b', snapshot)
        if len(set(ports)) >= 2:
            result['nat_source_ports'] = [int(port, 16) for port in ports]
            break
    assert 'nat_source_ports' in result, out
    result['passed'].append('wan_observes_both_live_nat_connections')
    out = shell(wan, 'cat /tmp/dhcp.log', (router, *clients))
    result['dhcp_acknowledgements_before_failure'] = len(re.findall(rb'sending ACK', out, re.I))
    assert result['dhcp_acknowledgements_before_failure'] >= max(2, args.duration // 30), out[-2000:]
    result['passed'].append('real_dhcp_server_renews_wan_lease_during_traffic')
    metrics = result['clients'] = []
    for client in clients:
        rows = await_result(client, '/tmp/steady', (router, wan))
        assert all(float(row['seconds']) >= args.duration for row in rows), rows
        out = shell(client, 'cat /tmp/steady.ping', (router, wan))
        loss = re.search(rb'(\d+) packets transmitted, (\d+) packets received', out)
        assert loss, out
        sent, received = map(int, loss.groups())
        assert sent == args.duration and received >= sent * 0.99, out[-1500:]
        metrics.append(dict(streams=rows, icmp_sent=sent, icmp_received=received))
    result['passed'].append('sustained_tcp_udp_integrity_and_icmp_loss_at_most_one_percent')
    assert len(samples) >= 4 and samples[-1]['conntrack']['ct_packets'] > samples[0]['conntrack']['ct_packets']
    assert all(row['bsd_bytes']['failures'] == samples[0]['bsd_bytes']['failures'] for row in samples)
    # Leave room for steady-state pool/cache growth while catching unbounded growth.
    assert samples[-1]['bsd_bytes']['used'] <= samples[1]['bsd_bytes']['used'] + 8*1024**2, samples
    assert samples[-1]['memory_kib']['MemAvailable'] >= samples[1]['memory_kib']['MemAvailable'] - 16*1024, samples
    result['passed'].append('memory_and_allocator_failures_stay_within_soak_bounds')

    # The same clients keep sending probes while the emulated WAN loses carrier.
    for client in clients:
        shell(client, f'busybox ping -i 1 -c 20 -W 3 {PROBE} '
              '>/tmp/fault.ping 2>&1 </dev/null & true', (router, wan))
    time.sleep(2)
    carrier(result['qmp_port'], False)
    router.until('WANCTL: en3 carrier down', timeout=8, drain_peer=(wan, *clients))
    deadline = time.monotonic() + 5
    while True:
        route = shell(router, '/extra/busybox ip -4 route show', (wan, *clients))
        if b'default via' not in route:
            break
        assert time.monotonic() < deadline, route
        time.sleep(0.2)
    time.sleep(5)
    restored = time.monotonic()
    carrier(result['qmp_port'], True)
    router.until('WANCTL: en3 configured 10.20.0.10/24', timeout=20, drain_peer=(wan, *clients))
    result['wan_reconfigure_seconds'] = time.monotonic() - restored
    for client in clients:
        benchmark(client, '/tmp/recovered', 4, (router, wan))
    time.sleep(14)
    for index, client in enumerate(clients):
        metrics[index]['recovered'] = await_result(client, '/tmp/recovered', (router, wan))
        output = shell(client, 'cat /tmp/fault.ping', (router, wan))
        sequence = [int(number) for number in re.findall(rb'\bseq=(\d+)', output)]
        assert sequence and sequence[0] <= 1 and sequence[-1] >= 17, output
        assert max(b-a for a, b in zip(sequence, sequence[1:])) >= 4, output
        metrics[index]['fault_ping_replies'] = sequence
    result['passed'].append('carrier_drop_with_live_probes_and_dhcp_tcp_udp_recovery')

    before = sample(router, (wan, *clients), start)['conntrack']
    shell(router, "echo 'cttimeout 2000 2000 2000' >/proc/a20/netfilter", (wan, *clients))
    deadline = time.monotonic() + 12
    while True:
        after = sample(router, (wan, *clients), start)
        if after['conntrack']['ct_tracked'] == 0:
            break
        assert time.monotonic() < deadline, after
        time.sleep(0.4)
    assert after['conntrack']['ct_timeout'] > before['ct_timeout'], after
    result['reclaimed'] = after
    result['passed'].append('idle_conntrack_reclaims_all_test_flows')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    boot = parser.add_mutually_exclusive_group(required=True)
    boot.add_argument('--kernel', type=Path, help='direct QEMU kernel boot')
    boot.add_argument('--router-image', type=Path, help='native UEFI PC router image')
    parser.add_argument('--ovmf-code', type=Path, help='OVMF firmware for --router-image')
    parser.add_argument('--ovmf-vars', type=Path, help='OVMF variable store template for --router-image')
    for name in ('linux', 'peer-initrd', 'busybox', 'out'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--nic-model', choices=('virtio-net-pci', 'e1000e'), default='virtio-net-pci')
    parser.add_argument('--duration', type=int, default=1800, help='seconds of steady-state concurrent traffic')
    parser.add_argument('--interval', type=int, default=30, help='seconds between router memory samples')
    args = parser.parse_args()
    if args.router_image and not (args.ovmf_code and args.ovmf_vars):
        parser.error('--router-image requires both --ovmf-code and --ovmf-vars')
    if not args.router_image and (args.ovmf_code or args.ovmf_vars):
        parser.error('OVMF inputs require --router-image')
    if not (2 <= args.interval and args.duration >= max(12, 3*args.interval)):
        parser.error('duration must be >=12s and allow at least three samples, with interval >=2s')
    out = args.out.resolve()
    out.mkdir(parents=True)
    image = pd.make_payload(out, args.busybox, False, None, files={'wan.conf': WAN_CONFIG})
    initrd = peer_image(args, out)
    ports = backend.reserve_ports(socket.SOCK_DGRAM, 6)
    qmp, = backend.reserve_ports(socket.SOCK_STREAM, 1)
    common = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'kvm', '-m', '768',
              '-nographic', '-monitor', 'none', '-no-reboot']
    router = [*common, '-smp', '1' if args.router_image else '2']
    boot_inputs = [args.kernel]
    if args.router_image:
        variables = out/'OVMF_VARS.fd'
        shutil.copyfile(args.ovmf_vars, variables)
        router += ['-drive', f'if=pflash,unit=0,format=raw,readonly=on,file={args.ovmf_code.resolve()}',
                   '-drive', f'if=pflash,unit=1,format=raw,file={variables}',
                   '-drive', f'if=none,id=boot,format=raw,file={args.router_image.resolve()},snapshot=on',
                   '-device', 'ide-hd,bus=ide.1,drive=boot,bootindex=1']
        boot_inputs = [args.router_image, args.ovmf_code, args.ovmf_vars]
    else:
        router += ['-kernel', str(args.kernel.resolve()),
                   '-append', 'a20.ip=192.168.10.1 a20.netmask=255.255.255.0 a20.dhcp=0 '
                              'a20.wan_config=/extra/wan.conf']
    # The PC AHCI driver exposes the first ready disk on the controller.
    payload_device = 'ide-hd,bus=ide.0' if args.router_image else 'virtio-blk-pci'
    router += ['-drive', f'file={image},if=none,format=raw,id=payload',
               '-device', f'{payload_device},drive=payload',
               '-qmp', f'tcp:127.0.0.1:{qmp},server=on,wait=off']
    linux = [*common, '-smp', '2', '-cpu', 'host', '-kernel', str(args.linux.resolve()), '-initrd', str(initrd),
             '-append', 'console=ttyS0 quiet net.ifnames=0 a20_lan_peer=1']
    def link(local, remote, number, model, peer):
        mac = f'52:54:00:0{number}:00:0{peer}'
        return ['-netdev', f'socket,id=s{number},udp=127.0.0.1:{remote},localaddr=127.0.0.1:{local}',
                '-device', f'{model},id=net{number},netdev=s{number},mac={mac}']
    nodes = {'router': router}
    for index, name in enumerate(('lan-a', 'wan', 'lan-b')):
        a, b = ports[2*index:2*index+2]
        router.extend(link(a, b, index, args.nic_model, 1))
        nodes[name] = [*linux, *link(b, a, index, 'virtio-net-pci', 2)]
    result = dict(status='running', passed=[], qmp_port=qmp, duration_seconds=args.duration,
                  interval_seconds=args.interval, nic_model=args.nic_model, argv=nodes,
                  sha256={str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in (*boot_inputs, args.linux, initrd, args.busybox)})
    manifest = out/'manifest.json'
    def checkpoint():
        temporary = manifest.with_suffix('.tmp')
        temporary.write_text(json.dumps(result, indent=2)+'\n')
        temporary.replace(manifest)

    checkpoint()
    with ExitStack() as stack:
        guests = []
        try:
            for name in ('router', 'wan', 'lan-a', 'lan-b'):
                guest = backend.Guest(nodes[name], stack.enter_context((out/f'{name}.log').open('wb')))
                stack.callback(guest.close)
                guests.append(guest)
            exercise(args, guests, result, checkpoint)
            guests[0].send('poweroff')
            guests[0].until('System is going down for power-off NOW')
            assert guests[0].process.wait(timeout=15) == 0
            result['status'] = 'passed'
        except BaseException as error:
            result.update(status='failed', error=repr(error))
            raise
        finally:
            checkpoint()
    print('BSD_ROUTER_SOAK: PASS')


if __name__ == '__main__':
    main()
