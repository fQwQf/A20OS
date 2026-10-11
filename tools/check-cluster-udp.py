#!/usr/bin/env python3
"""Run real production channel proxies between two independent QEMU kernels."""
import argparse
import binascii
from collections import deque
import json
import hashlib
import platform
import random
import select
from pathlib import Path
import shlex
import socket
import subprocess
import time


class FaultWire:
    """Operate on real Ethernet datagrams; never synthesize guest replies."""
    def __init__(self, ports, loss=0, delay=False, corrupt=False, seed=20261011, flood=False, oneway=False):
        self.ports = ports
        self.sockets = []
        self.events = {}
        self.delayed = []
        self.calls = [0] * len(ports)
        self.saved = {}
        self.trace = deque(maxlen=4096)
        self.loss, self.delay, self.corrupt = loss, delay, corrupt
        self.random = random.Random(seed)
        self.flood = flood
        self.flood_queue = deque()
        self.next_flood = 0
        self.partition_until = 0
        self.oneway = oneway
        self.send_sequences = set()

        for _ in ports:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.bind(("127.0.0.1", 0))
            self.sockets.append(sock)

    def partition(self, seconds):
        self.partition_until = time.monotonic() + seconds
        self.events["partition-seconds"] = seconds

    def address(self, side):
        return self.sockets[side].getsockname()[1]

    def once(self, direction, event):
        key = f"{direction}:{event}"
        if key in self.events:
            return False
        self.events[key] = 1
        return True

    def count(self, side, event):
        key = f"{side}:{event}"
        self.events[key] = self.events.get(key, 0) + 1

    @staticmethod
    def corrupt_cluster(data, udp):
        # Preserve a valid UDP checksum so the production CL CRC rejects it.
        packet = bytearray(data)
        packet[udp + 8 + 55] ^= 1
        return FaultWire.udp_checksum(packet, udp)

    @staticmethod
    def udp_checksum(packet, udp):
        packet = bytearray(packet)
        length = int.from_bytes(packet[udp+4:udp+6], "big")
        packet[udp+6:udp+8] = bytes(2)
        pseudo = packet[26:34] + bytes((0, 17)) + length.to_bytes(2, "big")
        payload = pseudo + packet[udp:udp+length]
        if len(payload) & 1:
            payload += bytes(1)
        total = sum(int.from_bytes(payload[i:i+2], "big") for i in range(0, len(payload), 2))
        while total >> 16:
            total = (total & 0xffff) + (total >> 16)
        packet[udp+6:udp+8] = ((~total & 0xffff) or 0xffff).to_bytes(2, "big")
        return bytes(packet)

    def pump(self):
        ready, _, _ = select.select(self.sockets, [], [], 0.002 if self.flood_queue else 0.05)
        for sock in ready:
            data, _ = sock.recvfrom(65536)
            side = self.sockets.index(sock)
            macs = [bytes.fromhex(f"52540012340{i+1}") for i in range(len(self.ports))]
            if data[:6] in macs:
                target = macs.index(data[:6])
            else:
                # Broadcast ARP/IPv6 packets do not carry CL fault injection.
                for target in range(len(self.ports)):
                    if target != side:
                        self.sockets[target].sendto(data, ("127.0.0.1", self.ports[target]))
                continue
            destination = ("127.0.0.1", self.ports[target])
            output = self.sockets[target]
            cl = b""
            if len(data) >= 42 and data[12:14] == b"\x08\x00":
                ip = 14
                ihl = (data[ip] & 15) * 4
                if ihl >= 20 and data[ip + 9] == 17 and len(data) >= ip + ihl + 8:
                    udp = ip + ihl
                    if data[udp + 2:udp + 4] == b"\xab\xf4":
                        cl = data[udp + 8:]
            if len(cl) >= 34 and cl[:2] == b"CL":
                if time.monotonic() < self.partition_until:
                    self.count(side, "partition-drop")
                    continue
                kind = cl[3]
                self.trace.append({"time": time.monotonic(), "side": side,
                    "type": kind, "flags": int.from_bytes(cl[4:6], "little"),
                    "frag": int.from_bytes(cl[6:8], "little"),
                    "txid": int.from_bytes(cl[8:12], "little"),
                    "seq": int.from_bytes(cl[24:28], "little"),
                    "length": len(cl), "ttl": cl[30], "target": target,
                    "prefix": cl[32:56].hex()})
                if side >= 2:
                    if int.from_bytes(cl[4:6], "little") & 2:
                        if cl[30] != 7 or len(cl) > 1472:
                            raise RuntimeError("relay violated fragment TTL/MTU")
                        event = f"relay-fragment-{kind}"
                        self.events[event] = self.events.get(event, 0) + 1
                    output.sendto(data, destination)
                    continue
                if kind == 2:
                    self.saved.setdefault((side, "hello-ack"), data)
                if self.flood and kind == 1 and self.once(side, "hello-flood-queued"):
                    for index in range(1000):
                        node = (0x10000 + index).to_bytes(16, "little")
                        node_hash = 2166136261
                        for byte in node:
                            node_hash = ((node_hash ^ byte) * 16777619) & 0xffffffff
                        frame = bytearray(cl)
                        frame[12:16] = node_hash.to_bytes(4, "little")
                        frame[32:48] = node
                        frame[-2:] = binascii.crc_hqx(frame[:-2], 0xffff).to_bytes(2, "little")
                        packet = data[:udp+8] + frame
                        self.flood_queue.append((side, output,
                            self.udp_checksum(packet, udp), destination))
                reliable = bool(int.from_bytes(cl[4:6], "little") & 1)
                slot = int.from_bytes(cl[20:24], "little")
                fragmented = bool(int.from_bytes(cl[4:6], "little") & 2)
                index = int.from_bytes(cl[6:8], "little") & 0xfff
                src = int.from_bytes(cl[12:16], "little")
                dst = int.from_bytes(cl[16:20], "little")
                txid = int.from_bytes(cl[8:12], "little")
                seq = int.from_bytes(cl[24:28], "little")
                if self.oneway and reliable and kind == 5:
                    if txid or not seq:
                        raise RuntimeError("business SEND violated zero txid/nonzero seq")
                    self.send_sequences.add((src, dst, seq))
                    self.saved.setdefault((side, "send"), data)
                    if not fragmented and self.once(side, "drop-send"):
                        continue
                    if not fragmented and self.once(side, "duplicate-send"):
                        self.delayed.append((time.monotonic() + 0.2, output, data, destination))
                    if fragmented and index == 0 and self.once(side, "drop-send-fragment"):
                        continue
                    if fragmented and index == 1 and self.once(side, "reorder-send-fragment"):
                        self.delayed.append((time.monotonic() + 0.06, output, data, destination))
                        continue
                if self.oneway and (dst, src, txid) in self.send_sequences:
                    if kind == 7 and self.once(side, "drop-send-completion"):
                        continue
                    if kind == 9 and self.once(side, "drop-send-ack"):
                        continue
                if (self.oneway and reliable and kind == 6 and not slot and
                        (src, dst, txid) in self.send_sequences and
                        self.once(side, "drop-send-retirement")):
                    continue
                if fragmented:
                    if kind == 6 and index == 0 and self.once(side, "drop-fragment"):
                        continue
                    if kind == 6 and index == 1 and self.once(side, "reorder-fragment"):
                        self.delayed.append((time.monotonic() + 0.06, output, data, destination))
                        continue
                    if kind == 7 and index == 1 and self.once(side, "drop-reply-fragment"):
                        continue
                    if kind == 7 and index == 0 and self.once(side, "duplicate-fragment"):
                        self.delayed.append((time.monotonic() + 0.01, output, data, destination))
                if reliable and kind == 6 and slot:
                    # Replay a counted business request after reboot; a ready
                    # probe would not exercise the service execution counter.
                    if not fragmented and len(cl) >= 54 and cl[48:52] == b"yler":
                        self.saved.setdefault((side, "call"), data)
                    if len(cl) >= 54 and cl[48:52] == b"kalb":
                        key = f"{side}:blackhole-call"
                        self.events[key] = self.events.get(key, 0) + 1
                        continue
                    if self.once(side, "drop-request"):
                        continue
                    self.calls[side] += 1
                    if self.calls[side] == 2:
                        self.once(side, "duplicate-call")
                        self.once(side, "delay-call")
                        self.delayed.append((time.monotonic() + 0.2, output, data, destination))
                elif reliable and kind == 7 and self.once(side, "drop-reply"):
                    continue
                elif kind == 9 and self.once(side, "drop-ack"):
                    continue
                elif reliable and kind == 6 and not slot and self.once(side, "drop-receipt"):
                    continue
                if reliable:
                    if (self.corrupt and kind == 6 and slot and not fragmented and
                            len(cl) > 57 and self.once(side, "corrupt-crc")):
                        data = self.corrupt_cluster(data, udp)
                        # Deliver this separate fault so random loss cannot
                        # bypass the guest's CRC-counter assertion.
                        output.sendto(data, destination)
                        continue
                    self.count(side, "random-eligible")
                    if self.random.random() < self.loss:
                        self.count(side, "random-drop")
                        continue
                    if self.delay:
                        milliseconds = self.random.randint(50, 200)
                        self.count(side, "random-delay")
                        self.delayed.append((time.monotonic() + milliseconds / 1000,
                                             output, data, destination))
                        continue
            output.sendto(data, destination)
        now = time.monotonic()
        if self.flood_queue and now >= self.next_flood:
            for _ in range(min(4, len(self.flood_queue))):
                side, output, data, destination = self.flood_queue.popleft()
                output.sendto(data, destination)
                self.count(side, "hello-flood-sent")
            self.next_flood = now + 0.002
        pending = []
        for due, output, data, destination in self.delayed:
            if due <= now:
                output.sendto(data, destination)
            else:
                pending.append((due, output, data, destination))
        self.delayed = pending

    def replay_old_session(self):
        frames = [(0, "call"), (1, "hello-ack")]
        if self.oneway:
            frames.append((0, "send"))
        for side, kind in frames:
            data = self.saved.get((side, kind))
            if data is None:
                raise RuntimeError(f"no saved {kind} for old-session replay")
            self.sockets[1-side].sendto(data, ("127.0.0.1", self.ports[1-side]))
            self.events[f"{side}:old-{kind}"] = 1

    def close(self):
        for sock in self.sockets:
            sock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--routed", action="store_true", help="three kernels through a UDP relay, with wire fault injection")
    parser.add_argument("--faults", action="store_true", help="drop, duplicate and delay real wire frames")
    parser.add_argument("--restart", action="store_true", help="restart node b and replay old session frames")
    parser.add_argument("--stress", action="store_true", help="1000 verified reliable calls per endpoint")
    parser.add_argument("--send", action="store_true", help="264 reliable business SENDs and 96 concurrent CALLs per endpoint")
    parser.add_argument("--random-loss", type=float, default=0, help="seeded loss probability for reliable frames")
    parser.add_argument("--random-delay", action="store_true", help="delay reliable frames by 50-200 ms")
    parser.add_argument("--corrupt", action="store_true", help="flip one CL payload bit per endpoint, preserving UDP checksum")
    parser.add_argument("--seed", type=int, default=20261011)
    parser.add_argument("--hello-flood", action="store_true", help="send 1000 valid-CRC HELLOs with forged node IDs per endpoint")
    parser.add_argument("--partition", action="store_true", help="isolate both endpoints for 30 seconds, then verify a fresh call")
    parser.add_argument("--slow-consumer", action="store_true", help="hold a real service reply for 10 seconds and verify bounded cancellation")
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--log-dir", type=Path, default=Path(".kernel-build/cluster/udp"))
    parser.add_argument("--profile", type=int, choices=(2, 3), default=2)
    parser.add_argument("--peer-profile", type=int, choices=(2, 3))
    args = parser.parse_args()
    if not 0 <= args.random_loss <= 0.5:
        parser.error("--random-loss must be between 0 and 0.5")
    if args.routed and args.restart:
        parser.error("use the dedicated gateway reboot test for routed reboot coverage")
    if args.routed and args.hello_flood:
        parser.error("--hello-flood currently requires the direct-link topology")
    if args.routed and args.corrupt:
        parser.error("--corrupt checks endpoint counters and requires direct links")
    if args.partition and (args.restart or args.routed):
        parser.error("--partition requires direct endpoints without --restart")
    if args.slow_consumer and args.restart:
        parser.error("--slow-consumer must run before a separate reboot test")
    root = Path(__file__).resolve().parent.parent
    log_dir = (root / args.log_dir).resolve()
    log_dir.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    success = False
    sources = ["kernel/cluster/fragment.c", "kernel/cluster/fragment.h",
               "kernel/cluster/frame.c", "kernel/cluster/frame.h", "kernel/cluster/transport.c",
               "kernel/cluster/reliable.c", "kernel/cluster/remote_ep.c", "kernel/cluster/udp.c",
               "kernel/cluster/session.c", "kernel/cluster/session_control.h",
               "kernel/include/uapi/a20/cluster_snapshot.h",
               "kernel/cluster/udp_selftest.c", "kernel/net/socket_inet.c",
               "kernel/net/lwip_compat.c", "kernel/net/lwip_port/arch/cc.h",
               "kernel/net/lwip_port/arch/heap.h", "tools/check-cluster-udp.py"]
    hashes = {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in sources}
    profiles = [args.profile, args.peer_profile or args.profile] + ([2] if args.routed else [])
    commands = {}
    for profile in set(profiles):
        make = ["make", "--no-print-directory", "ARCH=riscv64", "BRINGUP=1",
                "RAMFS_USER=1", "DRIVER_DEPLOYMENT=embedded", f"CLUSTER_PROFILE={profile}"]
        if not args.skip_build:
            with (log_dir / f"udp-build-{profile}.log").open("w") as log:
                subprocess.run(make + ["-j8", "kernel-only"], cwd=root,
                               stdout=log, stderr=subprocess.STDOUT, check=True)
        commands[profile] = shlex.split(subprocess.check_output(make + ["_qemu_argv"],
                                                               cwd=root, text=True))
    kernel_hashes = {
        str(profile): hashlib.sha256((root / cmd[cmd.index("-kernel") + 1]).read_bytes()).hexdigest()
        for profile, cmd in commands.items()
    }
    ports = []
    reservations = []
    for _ in profiles:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        ports.append(sock.getsockname()[1])
        reservations.append(sock)
    processes, logs = [], []
    wire = (FaultWire(ports, args.random_loss, args.random_delay, args.corrupt, args.seed, args.hello_flood, args.send)
            if args.faults or args.restart or args.routed or args.random_loss or
            args.random_delay or args.corrupt or args.hello_flood or args.partition else None)
    rounds = 1000 if args.stress else 96
    restarted, replayed = False, False
    guest_commands = []
    try:
        for sock in reservations:
            sock.close()
        for i, side in enumerate("abr" if args.routed else "ab"):
            cmd = list(commands[profiles[i]])
            bootargs = (f"clxudptest={side} a20.dhcp=0 a20.ip=10.0.3.{i+2} "
                        "a20.netmask=255.255.255.0")
            if args.routed:
                bootargs += " clxudprouted=1"
            if wire:
                bootargs += " clxudpfaults=1"
            if args.restart:
                bootargs += " clxudprestart=1"
            if args.stress:
                bootargs += " clxudpstress=1"
            if args.send:
                bootargs += " clxudpsend=1"
            if args.corrupt:
                bootargs += " clxudpcorrupt=1"
            if args.hello_flood:
                bootargs += " clxudpflood=1"
            if args.partition:
                bootargs += " clxudppartition=1"
            if args.slow_consumer:
                bootargs += " clxudpslow=1"
            if "-append" in cmd:
                cmd[cmd.index("-append") + 1] += " " + bootargs
            else:
                cmd += ["-append", bootargs]
            destination = wire.address(i) if wire else ports[1-i]
            cmd += ["-netdev", f"socket,id=net,udp=127.0.0.1:{destination},"
                    f"localaddr=127.0.0.1:{ports[i]}", "-device",
                    f"virtio-net-device,bus=virtio-mmio-bus.4,netdev=net,"
                    f"mac=52:54:00:12:34:0{i+1}"]
            guest_commands.append(cmd)
            path = log_dir / f"udp-{side}.log"
            log = path.open("w")
            logs.append(log)
            processes.append(subprocess.Popen(cmd, cwd=root, stdin=subprocess.DEVNULL,
                                               stdout=log, stderr=subprocess.STDOUT))
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            outputs = [(log_dir / f"udp-{side}.log").read_text(errors="replace")
                       for side in "ab"]
            if any("[CLX-UDP-TEST]" in out and ": FAIL" in out for out in outputs):
                raise RuntimeError("guest reported FAIL")
            if args.partition and not wire.partition_until and "partition-ready: PASS" in outputs[0]:
                wire.partition(30)
            cases = ("session-ready", "echo-8", f"reliable-{rounds}-at-most-once", "fragmented-8-at-most-once", "shared-dot", "unified-work", "directory-discovery", "unknown-slot",
                     "reliable-unknown-slot", "empty-call", "oversize-completes")
            if wire:
                cases += ("bounded-blackhole",)
            if args.routed:
                cases += ("routed-fragment-session",)
            if args.send:
                cases += ("oneway-264-concurrent-call",)
            if args.corrupt:
                cases += ("crc-counter",)
            if args.hello_flood:
                cases += ("hello-flood-bounded",)
            if all(all(f"node={side} {case}: PASS" in out for case in cases)
                   for side, out in zip("ab", outputs)):
                if args.slow_consumer and "slow-consumer-recovered: PASS" not in outputs[0]:
                    if wire:
                        wire.pump()
                    else:
                        time.sleep(0.05)
                    continue
                if args.partition:
                    if "partition-recovered: PASS" not in outputs[0]:
                        wire.pump()
                        continue
                    if (time.monotonic() < wire.partition_until or
                            not all(wire.events.get(f"{side}:partition-drop") for side in range(2))):
                        raise RuntimeError("partition did not isolate both endpoints for 30 seconds")
                if args.restart and not restarted:
                    if "reboot-armed: PASS" not in outputs[0]:
                        wire.pump()
                        continue
                    processes[1].terminate()
                    processes[1].wait(timeout=5)
                    cmd = list(guest_commands[1])
                    cmd[cmd.index("-append") + 1] += " clxudpreboot=1"
                    log = (log_dir / "udp-b-reboot.log").open("w")
                    logs.append(log)
                    processes[1] = subprocess.Popen(cmd, cwd=root, stdin=subprocess.DEVNULL,
                                                     stdout=log, stderr=subprocess.STDOUT)
                    restarted = True
                if args.restart:
                    if "reboot-session-ready: PASS" in outputs[0] and not replayed:
                        wire.replay_old_session()
                        replayed = True
                    if "restart-old-session-rejected: PASS" not in outputs[0]:
                        wire.pump()
                        continue
                    if not replayed:
                        raise RuntimeError("old-session replay did not run")
                if wire:
                    expected = {f"{side}:{event}" for side in range(2)
                                for event in ("drop-request", "drop-reply", "drop-ack",
                                              "drop-receipt", "duplicate-call", "delay-call",
                                              "drop-fragment", "reorder-fragment",
                                              "drop-reply-fragment", "duplicate-fragment")}
                    if any(wire.events.get(f"{side}:blackhole-call", 0) != 9 for side in range(2)):
                        raise RuntimeError("blackhole did not exercise exactly 8 retries per caller")
                    if not expected.issubset(wire.events):
                        raise RuntimeError(f"fault scenarios missing: {expected - wire.events.keys()}")
                    if args.send:
                        expected_send = {f"{side}:{event}" for side in range(2)
                                         for event in ("drop-send", "duplicate-send",
                                                       "drop-send-fragment", "reorder-send-fragment",
                                                       "drop-send-completion", "drop-send-ack",
                                                       "drop-send-retirement")}
                        if not expected_send.issubset(wire.events):
                            raise RuntimeError(f"SEND faults missing: {expected_send - wire.events.keys()}")
                    for side in range(2):
                        for enabled, event in ((args.corrupt, "corrupt-crc"),
                                               (args.random_loss, "random-drop"),
                                               (args.random_delay, "random-delay")):
                            if enabled and not wire.events.get(f"{side}:{event}"):
                                raise RuntimeError(f"missing injected {event} on node {side}")
                        if args.hello_flood and wire.events.get(f"{side}:hello-flood-sent") != 1000:
                            raise RuntimeError("HELLO flood did not send all 1000 forged identities")
                    (log_dir / "udp-faults.json").write_text(json.dumps(wire.events, indent=2) + "\n")
                if args.routed and (wire.events.get("relay-fragment-6", 0) < 16 or
                                    wire.events.get("relay-fragment-7", 0) < 16 or
                                    "relay-ready: PASS" not in (log_dir / "udp-r.log").read_text(errors="replace")):
                    raise RuntimeError("relay fragment forwarding evidence missing")
                success = True
                print("check-cluster-udp: PASS (" + ("three kernels via UDP relay" if args.routed else "two kernels") + f", {rounds} reliable calls each, "
                      "at-most-once service execution, fragmented echo, shared dot, bounded failures" +
                      (", 264 SENDs and 96 concurrent CALLs each" if args.send else "") +
                      (", real wire faults" if wire else "") +
                      (", reboot and old-session rejection)" if args.restart else ")"))
                print(f"logs: {log_dir}")
                return 0
            if any(proc.poll() is not None for proc in processes):
                raise RuntimeError("QEMU exited before acceptance completed")
            if wire:
                wire.pump()
            else:
                time.sleep(0.1)
        raise RuntimeError("guest acceptance timed out")
    except RuntimeError as error:
        print(f"check-cluster-udp: FAIL: {error}")
        for side in "ab":
            path = log_dir / f"udp-{side}.log"
            if path.exists():
                print(f"--- {path} ---\n" + "\n".join(path.read_text(
                    errors="replace").splitlines()[-35:]))
        return 1
    finally:
        evidence = {"success": success, "profiles": profiles, "faults": wire is not None,
                    "restart": args.restart, "routed": args.routed, "calls_each": rounds,
                    "send_each": 264 if args.send else 0,
                    "concurrent_calls_each": 96 if args.send else 0,
                    "random_loss": args.random_loss, "random_delay_ms": [50, 200] if args.random_delay else None,
                    "corrupt": args.corrupt, "seed": args.seed, "source_sha256": hashes,
                    "guest_commands": guest_commands, "kernel_sha256": kernel_hashes,
                    "hello_flood_each": 1000 if args.hello_flood else 0,
                    "partition_seconds": 30 if args.partition else 0,
                    "slow_consumer_ms": 10000 if args.slow_consumer else 0,
                    "platform": platform.platform(), "elapsed_seconds": round(time.monotonic()-started, 6),
                    "qemu_version": subprocess.check_output(["qemu-system-riscv64", "--version"], text=True).splitlines()[0],
                    "scope": f"{len(profiles)} independent RISC-V kernels, TCG, not physical throughput"}
        (log_dir / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        if wire:
            (log_dir / "wire-trace.json").write_text(json.dumps(list(wire.trace), indent=2) + "\n")
            (log_dir / "udp-faults.json").write_text(json.dumps(wire.events, indent=2) + "\n")
        for proc in processes:
            if proc.poll() is None:
                proc.terminate()
        for proc in processes:
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        for log in logs:
            log.close()
        if wire:
            wire.close()


if __name__ == "__main__":
    raise SystemExit(main())
