#!/usr/bin/env python3
"""Three independent Native kernels: any, indirect probe, jobs, death and reboot."""
import argparse
import json
from pathlib import Path
import select
import shlex
import socket
import subprocess
import time


class Wire:
    def __init__(self, ports):
        self.ports = ports
        self.sockets = []
        self.block_directory = False
        self.lost_shard = None
        self.lost_source = None
        self.job_fault_finished = False
        self.indirect_cookies = set()
        self.events = {"blocked-directory": 0, "indirect-request": 0,
                       "indirect-reply": 0, "lost-job-reply": 0, "job-new-attempt": 0}
        for _ in ports:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.bind(("127.0.0.1", 0))
            self.sockets.append(sock)

    def pump(self):
        ready, _, _ = select.select(self.sockets, [], [], 0.02)
        macs = [bytes.fromhex(f"52540012340{i+1}") for i in range(3)]
        for sock in ready:
            data, _ = sock.recvfrom(65536)
            source = self.sockets.index(sock)
            destinations = [macs.index(data[:6])] if data[:6] in macs else list(range(3))
            cl = b""
            if len(data) >= 42 and data[12:14] == b"\x08\x00":
                ihl = (data[14] & 15) * 4
                if ihl >= 20 and data[23] == 17 and len(data) >= 22 + ihl:
                    cl = data[22 + ihl:]
            if len(cl) >= 34 and cl[:2] == b"CL":
                kind = cl[3]
                if self.block_directory and source == 0 and kind == 6 and cl[48:52] == b"CM\x01\x03" and cl[80:96] == bytes(range(0x20, 0x30)):
                    self.events["indirect-request"] += 1
                    self.indirect_cookies.add(cl[72:80])
                if self.block_directory and source == 2 and kind == 7 and cl[52:56] == b"CM\x01\x03" and cl[56:60] == bytes(4) and cl[116:124] in self.indirect_cookies:
                    self.events["indirect-reply"] += 1
                if not self.job_fault_finished and kind == 7 and cl[52:56] == b"CW\x01\x02" and int.from_bytes(cl[56:64], "little") == 1000:
                    shard = int.from_bytes(cl[64:68], "little")
                    attempt = int.from_bytes(cl[80:84], "little")
                    if self.lost_shard is None:
                        self.lost_shard = (shard, attempt)
                        self.lost_source = source
                    if (shard, attempt) == self.lost_shard:
                        self.events["lost-job-reply"] += 1
                        continue
                    if shard == self.lost_shard[0] and attempt > self.lost_shard[1]:
                        self.events["job-new-attempt"] += 1
                        self.job_fault_finished = True
                slot = int.from_bytes(cl[20:24], "little")
                if self.block_directory and source == 0 and 1 in destinations and kind == 6 and slot == 0xfffffffe:
                    self.events["blocked-directory"] += 1
                    continue
            for target in destinations:
                if target != source:
                    self.sockets[target].sendto(data, ("127.0.0.1", self.ports[target]))

    def close(self):
        for sock in self.sockets:
            sock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--log-dir", type=Path, default=Path(".kernel-build/cluster/mesh"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent.parent
    directory = (root / args.log_dir).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    make = ["make", "--no-print-directory", "ARCH=riscv64", "ABI=both", "BRINGUP=0"]
    if not args.skip_build:
        with (directory / "build.log").open("w") as log:
            subprocess.run(make + ["-j8", "dev-build"], cwd=root, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
    base = shlex.split(subprocess.check_output(make + ["-s", "_qemu_argv"], cwd=root, text=True))
    # Replace only this command's network devices; retain its verified block bus.
    for flag in ("-netdev", "-device"):
        for i in reversed(range(len(base) - 1)):
            if base[i] == flag and (flag == "-netdev" or base[i+1].startswith("virtio-net")):
                del base[i:i+2]
    reservations = []
    ports = []
    for _ in range(3):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        reservations.append(sock)
        ports.append(sock.getsockname()[1])
    wire = Wire(ports)
    processes, logs, commands = [None] * 3, [], []
    killed = restarted = launched = False

    def start(i, reboot=False):
        log = (directory / f"mesh-{i}{'-reboot' if reboot else ''}.log").open("w")
        logs.append(log)
        processes[i] = subprocess.Popen(commands[i], cwd=root, stdin=subprocess.PIPE,
                                         stdout=log, stderr=subprocess.STDOUT)

    def run_test(i):
        processes[i].stdin.write(f"/bin/native-cluster-mesh-rv {i}\n".encode())
        processes[i].stdin.flush()

    try:
        for sock in reservations:
            sock.close()
        for i in range(3):
            cmd = list(base) + ["-snapshot"]
            boot = f"a20.dhcp=0 a20.ip=10.0.3.{i+2} a20.netmask=255.255.255.0"
            if "-append" in cmd:
                cmd[cmd.index("-append") + 1] += " " + boot
            else:
                cmd += ["-append", boot]
            cmd += ["-netdev", f"socket,id=net,udp=127.0.0.1:{wire.sockets[i].getsockname()[1]},localaddr=127.0.0.1:{ports[i]}",
                    "-device", f"virtio-net-device,bus=virtio-mmio-bus.4,netdev=net,mac=52:54:00:12:34:0{i+1}"]
            commands.append(cmd)
            start(i)
        begin = time.monotonic()
        reboot_at = None
        while time.monotonic() - begin < args.timeout:
            wire.pump()
            elapsed = time.monotonic() - begin
            if not launched and elapsed >= 8:
                for i in range(3):
                    run_test(i)
                launched = True
            out = (directory / "mesh-0.log").read_text(errors="replace")
            if "NATIVE_CLUSTER_MESH: FAIL" in out:
                raise RuntimeError("Native client reported FAIL; inspect mesh-0.log")
            if "MESH: job-retry-pass" in out:
                wire.job_fault_finished = True
            if "MESH: indirect-ready" in out and not killed:
                wire.block_directory = True
            if "MESH: indirect-pass" in out and not killed:
                if not all(wire.events[k] for k in ("blocked-directory", "indirect-request", "indirect-reply", "lost-job-reply")) or "MESH: job-retry-pass" not in out:
                    raise RuntimeError(f"indirect path was not exercised: {wire.events}")
                wire.block_directory = False
                processes[1].terminate()
                processes[1].wait(timeout=5)
                killed = True
            if "MESH: reboot-ready" in out and not restarted:
                start(1, reboot=True)
                reboot_at = time.monotonic()
                restarted = True
            if reboot_at is not None and time.monotonic() - reboot_at >= 8:
                run_test(1)
                reboot_at = None
            if "NATIVE_CLUSTER_MESH: PASS" in out:
                if not killed or not restarted:
                    raise RuntimeError("missing death/reboot phases")
                (directory / "events.json").write_text(json.dumps(wire.events, indent=2) + "\n")
                print("check-cluster-mesh: PASS (three Native kernels, any, remote binary tree, indirect probe, death, reboot and daemon recovery)")
                return
            for i, process in enumerate(processes):
                if process.poll() is not None and not (i == 1 and killed and not restarted):
                    raise RuntimeError(f"QEMU {i} exited unexpectedly")
        raise TimeoutError(f"Native mesh timeout; logs: {directory}")
    finally:
        for process in processes:
            if process and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for log in logs:
            log.close()
        wire.close()


if __name__ == "__main__":
    main()
