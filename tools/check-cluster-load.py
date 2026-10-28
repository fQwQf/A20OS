#!/usr/bin/env python3
"""Measure receiver-admitted SENDs on four independent Native QEMU nodes."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import random
import select
import shlex
import socket
import subprocess
import time


class EthernetHub:
    """Forward guest Ethernet bytes; never implement a guest service or reply."""
    def __init__(self, ports, loss, seed):
        self.ports = ports
        self.loss = loss
        self.random = random.Random(seed)
        self.sockets = []
        self.macs = [bytes.fromhex(f"52540012340{i+1}") for i in range(4)]
        self.forwarded = [0] * 4
        self.dropped = [0] * 4
        for _ in ports:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
            sock.bind(("127.0.0.1", 0))
            sock.setblocking(False)
            self.sockets.append(sock)

    def pump(self):
        ready, _, _ = select.select(self.sockets, [], [], 0.002)
        for sock in ready:
            side = self.sockets.index(sock)
            # Bound each turn so a busy source cannot starve other directions.
            for _ in range(256):
                try:
                    data, _ = sock.recvfrom(65536)
                except BlockingIOError:
                    break
                if len(data) < 14:
                    raise RuntimeError("truncated guest Ethernet frame")
                cluster = False
                if len(data) >= 42 and data[12:14] == b"\x08\x00":
                    ihl = (data[14] & 15) * 4
                    udp = 14 + ihl
                    cluster = (ihl >= 20 and data[23] == 17 and len(data) >= udp + 10
                               and data[udp+2:udp+4] == b"\xab\xf4"
                               and data[udp+8:udp+10] == b"CL")
                if cluster and self.random.random() < self.loss:
                    self.dropped[side] += 1
                    continue
                if data[:6] in self.macs:
                    targets = [self.macs.index(data[:6])]
                elif data[0] & 1:
                    targets = [i for i in range(4) if i != side]
                else:
                    raise RuntimeError("guest used an unknown unicast destination")
                for target in targets:
                    self.sockets[target].sendto(data, ("127.0.0.1", self.ports[target]))
                    self.forwarded[side] += 1

    def close(self):
        for sock in self.sockets:
            sock.close()


def parse_result(text, node, seconds, min_rate):
    rows = [line for line in text.splitlines() if line.startswith("CLUSTER_LOAD: RESULT ")]
    if len(rows) != 1:
        raise RuntimeError(f"node {node}: expected exactly one result")
    row = {key: int(value) for key, value in
           (item.split("=", 1) for item in rows[0].split()[2:])}
    if (row["node"] != node or row["receiver"] != node % 4 + 1
            or row["submitted"] != row["delivered"]
            or row["submitted"] <= 0 or row["duplicates"] or row["bad"]
            or row["window_ns"] != seconds * 10**9
            or row["producer_ns"] < row["window_ns"]
            or not 0 < row["window_admitted"] <= row["delivered"]):
        raise RuntimeError(f"node {node}: invalid delivery accounting: {row}")
    row["receiver_messages_per_second"] = row["window_admitted"] / seconds
    # The 1.0 contract is the measured reference rate, not an aspirational
    # line-rate number: every admitted message is delivered exactly once and
    # the rate stays at or above the declared floor for the whole window.
    row["min_rate"] = min_rate
    row["contract_rate_passed"] = row["receiver_messages_per_second"] >= min_rate
    resources = {}
    for line in text.splitlines():
        if line.startswith("CLUSTER_LOAD: resources "):
            fields = dict(item.split("=", 1) for item in line.split()[2:])
            kind = int(fields["kind"])
            values = [int(value) for value in fields["values"].split(",")]
            if int(fields["node"]) != node or kind in resources or len(values) != 20:
                raise RuntimeError(f"node {node}: malformed resource snapshot")
            resources[kind] = values
    if (set(resources) != {3, 4, 5} or resources[3][5] or resources[3][9]
            or resources[4][1] or resources[4][2]
            or resources[5][1:5] != [3, 3, 0, 0] or resources[5][9]):
        raise RuntimeError(f"node {node}: resources did not drain or links failed")
    row["resource_snapshots"] = resources
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--random-loss", type=float, default=0)
    parser.add_argument("--min-rate", type=float, default=100.0,
                        help="per-direction admitted SEND/s floor (0 disables)")
    parser.add_argument("--seed", type=int, default=20261011)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--log-dir", type=Path, default=Path(".kernel-build/cluster/load"))
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600 or not 0 <= args.random_loss <= 0.5:
        parser.error("seconds must be 1..3600 and random loss 0..0.5")
    if args.timeout <= args.seconds:
        parser.error("timeout must exceed the measurement window")
    root = Path(__file__).resolve().parent.parent
    directory = (root / args.log_dir).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    make = ["make", "--no-print-directory", "ARCH=riscv64", "ABI=both", "CLUSTER_PROFILE=3", "BRINGUP=0"]
    sources = ["user/tests/test_native_cluster_load.c", "kernel/cluster/transport.c",
               "kernel/cluster/remote_ep.c", "kernel/cluster/export.c", "kernel/cluster/reliable.c",
               "kernel/cluster/udp.c", "kernel/net/backends/lwip/socket_inet.c", "kernel/net/backends/lwip/lwip_compat.c", "kernel/mm/slab.c",
               "kernel/net/backends/lwip/port/arch/heap.h", "kernel/drivers/net/virtio_net.c",
               "kernel/include/uapi/a20/cluster_snapshot.h", "kernel/cluster/transport.h",
               "tools/check-cluster-load.py"]
    hashes = {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in sources}
    if not args.skip_build:
        with (directory / "build.log").open("w") as log:
            subprocess.run(make + ["-j8", "dev-build"], cwd=root, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
    base = shlex.split(subprocess.check_output(make + ["-s", "_qemu_argv"], cwd=root, text=True))
    kernel = (root / base[base.index("-kernel") + 1]).resolve()
    kernel_hash = hashlib.sha256(kernel.read_bytes()).hexdigest()
    for flag in ("-netdev", "-device"):
        for i in reversed(range(len(base) - 1)):
            if base[i] == flag and (flag == "-netdev" or base[i+1].startswith("virtio-net")):
                del base[i:i+2]
    reservations = []
    for _ in range(4):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        reservations.append(sock)
    ports = [sock.getsockname()[1] for sock in reservations]
    hub = EthernetHub(ports, args.random_loss, args.seed)
    commands, processes, logs, results = [], [], [], []
    started = time.monotonic()
    success = False
    try:
        for sock in reservations:
            sock.close()
        for i in range(4):
            cmd = list(base) + ["-snapshot"]
            boot = f"a20.dhcp=0 a20.ip=10.0.3.{i+2} a20.netmask=255.255.255.0"
            if "-append" in cmd:
                cmd[cmd.index("-append") + 1] += " " + boot
            else:
                cmd += ["-append", boot]
            cmd += ["-netdev", f"socket,id=net,udp=127.0.0.1:{hub.sockets[i].getsockname()[1]},localaddr=127.0.0.1:{ports[i]}",
                    "-device", f"virtio-net-device,bus=virtio-mmio-bus.4,netdev=net,mac=52:54:00:12:34:0{i+1}"]
            commands.append(cmd)
            log = (directory / f"node-{i+1}.log").open("w")
            logs.append(log)
            processes.append(subprocess.Popen(cmd, cwd=root, stdin=subprocess.PIPE,
                                             stdout=log, stderr=subprocess.STDOUT))
        sent = False
        next_log_read = 0
        while time.monotonic() - started < args.timeout:
            hub.pump()
            if time.monotonic() < next_log_read:
                continue
            next_log_read = time.monotonic() + 0.05
            outputs = [(directory / f"node-{i+1}.log").read_text(errors="replace") for i in range(4)]
            if any("CLUSTER_LOAD: FAIL" in out or "KERNEL PANIC" in out for out in outputs):
                raise RuntimeError(f"guest failed; inspect {directory}")
            if any(process.poll() is not None for process in processes):
                raise RuntimeError("a QEMU node exited before completion")
            if not sent and all("# " in out for out in outputs):
                for i, process in enumerate(processes):
                    process.stdin.write(f"/bin/native-cluster-load-rv {i+1} {args.seconds}\n".encode())
                    process.stdin.flush()
                sent = True
            if all("CLUSTER_LOAD: RESULT " in out for out in outputs):
                results = [parse_result(out, i+1, args.seconds, args.min_rate)
                           for i, out in enumerate(outputs)]
                success = True
                break
        else:
            raise TimeoutError(f"four-node load timed out; inspect {directory}")
        for row in results:
            print(f'node {row["node"]} -> {row["receiver"]}: {row["receiver_messages_per_second"]:.3f} receiver-admitted SEND/s, '
                  f'floor {args.min_rate:.1f}: {row["contract_rate_passed"]}')
        failed = [row["node"] for row in results if not row["contract_rate_passed"]]
        if failed:
            raise RuntimeError(f"four-node rate below contract on nodes {failed}")
        print(f"check-cluster-load: PASS ({args.seconds}s, exact delivery, "
              f"rate >= {args.min_rate:.1f} SEND/s per direction)")
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
        for process in processes:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        for log in logs:
            log.close()
        hub.close()
        for sock in reservations:
            sock.close()
        evidence = {"success": success, "seconds": args.seconds, "random_loss": args.random_loss,
                    "min_rate": args.min_rate,
                    "seed": args.seed, "results": results, "source_sha256": hashes,
                    "kernel_elf": str(kernel), "kernel_sha256": kernel_hash,
                    "guest_commands": commands, "forwarded": hub.forwarded, "dropped": hub.dropped,
                    "elapsed_seconds": time.monotonic() - started,
                    "started_at_utc": datetime.fromtimestamp(time.time() - (time.monotonic()-started), timezone.utc).isoformat(),
                    "worktree_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
                    "platform": platform.platform(),
                    "qemu_version": subprocess.check_output(["qemu-system-riscv64", "--version"], text=True).splitlines()[0],
                    "scope": "four independent Native RISC-V kernels, receiver-owned fixed windows, QEMU TCG; no physical throughput claim"}
        (directory / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")


if __name__ == "__main__":
    main()
