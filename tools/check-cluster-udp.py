#!/usr/bin/env python3
"""Run real production channel proxies between two independent QEMU kernels."""
import argparse
from collections import deque
import json
import hashlib
import platform
import select
from pathlib import Path
import shlex
import socket
import subprocess
import time


class FaultWire:
    """Operate on real Ethernet datagrams; never synthesize guest replies."""
    def __init__(self, ports):
        self.ports = ports
        self.sockets = []
        self.events = {}
        self.delayed = []
        self.calls = [0] * len(ports)
        self.saved = {}
        self.trace = deque(maxlen=4096)
        for _ in ports:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.bind(("127.0.0.1", 0))
            self.sockets.append(sock)

    def address(self, side):
        return self.sockets[side].getsockname()[1]

    def once(self, direction, event):
        key = f"{direction}:{event}"
        if key in self.events:
            return False
        self.events[key] = 1
        return True

    def pump(self):
        ready, _, _ = select.select(self.sockets, [], [], 0.05)
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
                kind = cl[3]
                self.trace.append({"time": time.monotonic(), "side": side,
                    "type": kind, "flags": int.from_bytes(cl[4:6], "little"),
                    "frag": int.from_bytes(cl[6:8], "little"),
                    "txid": int.from_bytes(cl[8:12], "little"),
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
                reliable = bool(int.from_bytes(cl[4:6], "little") & 1)
                slot = int.from_bytes(cl[20:24], "little")
                fragmented = bool(int.from_bytes(cl[4:6], "little") & 2)
                index = int.from_bytes(cl[6:8], "little") & 0xfff
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
            output.sendto(data, destination)
        now = time.monotonic()
        pending = []
        for due, output, data, destination in self.delayed:
            if due <= now:
                output.sendto(data, destination)
            else:
                pending.append((due, output, data, destination))
        self.delayed = pending

    def replay_old_session(self):
        for side, kind in ((0, "call"), (1, "hello-ack")):
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
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--log-dir", type=Path, default=Path(".kernel-build/cluster/udp"))
    parser.add_argument("--profile", type=int, choices=(2, 3), default=2)
    parser.add_argument("--peer-profile", type=int, choices=(2, 3))
    args = parser.parse_args()
    if args.routed and args.restart:
        parser.error("use the dedicated gateway reboot test for routed reboot coverage")
    root = Path(__file__).resolve().parent.parent
    log_dir = (root / args.log_dir).resolve()
    log_dir.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    success = False
    sources = ["kernel/cluster/fragment.c", "kernel/cluster/fragment.h",
               "kernel/cluster/frame.c", "kernel/cluster/frame.h", "kernel/cluster/transport.c",
               "kernel/cluster/reliable.c", "kernel/cluster/remote_ep.c", "kernel/cluster/udp.c",
               "kernel/cluster/session.c", "kernel/cluster/session_control.h",
               "kernel/cluster/udp_selftest.c", "kernel/net/socket_inet.c", "tools/check-cluster-udp.py"]
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
    ports = []
    reservations = []
    for _ in profiles:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        ports.append(sock.getsockname()[1])
        reservations.append(sock)
    processes, logs = [], []
    wire = FaultWire(ports) if args.faults or args.restart or args.routed else None
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
            cases = ("session-ready", "echo-8", "reliable-96-at-most-once", "fragmented-8-at-most-once", "shared-dot", "unified-work", "directory-discovery", "unknown-slot",
                     "reliable-unknown-slot", "empty-call", "oversize-completes")
            if wire:
                cases += ("bounded-blackhole",)
            if args.routed:
                cases += ("routed-fragment-session",)
            if all(all(f"node={side} {case}: PASS" in out for case in cases)
                   for side, out in zip("ab", outputs)):
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
                    (log_dir / "udp-faults.json").write_text(json.dumps(wire.events, indent=2) + "\n")
                if args.routed and (wire.events.get("relay-fragment-6", 0) < 16 or
                                    wire.events.get("relay-fragment-7", 0) < 16 or
                                    "relay-ready: PASS" not in (log_dir / "udp-r.log").read_text(errors="replace")):
                    raise RuntimeError("relay fragment forwarding evidence missing")
                success = True
                print("check-cluster-udp: PASS (" + ("three kernels via UDP relay" if args.routed else "two kernels") + ", 96 reliable calls each, "
                      "at-most-once service execution, fragmented echo, shared dot, bounded failures" +
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
                    "restart": args.restart, "routed": args.routed, "source_sha256": hashes,
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
