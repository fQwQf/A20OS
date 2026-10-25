#!/usr/bin/env python3
"""Ordinary pthreads execute on two isolated x86 A20OS kernels.

Only Ethernet frames cross the fixture; RAM and disks are separate. Existing
kernel/image inputs can be supplied together; otherwise build the current tree.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import select
import socket
import subprocess
import struct
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
CASES = ("off", "no-shared-memory", "no-automatic-threads", "automatic", "page-limit", "byte-limit", "thread-limit", "slice-limit", "disconnect")


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--timeout", type=float, default=100)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--accel", choices=("kvm", "tcg"), default="kvm")
    parser.add_argument("--case", action="append", choices=CASES)
    parser.add_argument("--debug-qemu", action="store_true")
    args = parser.parse_args()
    if bool(args.kernel) != bool(args.image):
        parser.error("--kernel and --image must be supplied together")
    parent = ROOT / ".kernel-build/cluster/ssi"
    parent.mkdir(parents=True, exist_ok=True)
    directory = args.out.resolve() if args.out else Path(tempfile.mkdtemp(prefix="network-", dir=parent))
    if args.out:
        directory.mkdir(parents=True, exist_ok=False)
    report = {"passed": False, "cases": [], "transport": "guest Ethernet/IPv4 TCP", "shared_ram": False}
    make = ["make", "--no-print-directory", "ARCH=x86_64", "ABI=both", "CLUSTER_SSI=1"]
    def run(command, **kwargs):
        return subprocess.run(command, cwd=ROOT, check=True, **kwargs)
    print(f"cluster SSI network: {directory}", flush=True)
    try:
        with (directory / "build.log").open("w") as log:
            run(["make", "-C", "user", "ARCH=x86_64", "-j4", "build/x86_64/a20ssi", "build/x86_64/ssi_pthread_test"], stdout=log, stderr=subprocess.STDOUT)
            if not args.kernel:
                run(make + ["-j4", "dev-build"], stdout=log, stderr=subprocess.STDOUT)
        service = ROOT / "user/build/x86_64/a20ssi"
        fixture = ROOT / "user/build/x86_64/ssi_pthread_test"
        if not args.kernel:
            build = ROOT / subprocess.check_output(make + ["-s", "print-build-dir"], cwd=ROOT, text=True).strip()
            args.kernel, args.image = build / "kernel.elf", build / "fat32.img"
        kernel, image = args.kernel.resolve(), args.image.resolve()
        report.update(kernel=str(kernel), kernel_sha256=sha(kernel), image=str(image),
                      image_sha256=sha(image), service_sha256=sha(service), fixture_sha256=sha(fixture))
        frozen = directory / "kernel.elf"
        run(["cp", "--reflink=auto", str(kernel), str(frozen)])
        for name in CASES:
            if args.case and name not in args.case:
                continue
            active = name not in CASES[:3]
            case = directory / name
            case.mkdir()
            disk = case / "fat32.img"
            run(["cp", "--reflink=auto", "--sparse=always", str(image), str(disk)])
            for source in (service, fixture):
                run(["mcopy", "-o", "-i", str(disk), str(source), "::/" + source.name], stdout=subprocess.DEVNULL)
            for role in ("home", "worker"):
                config = case / (role + ".conf")
                text = ("version 1\nrole " + ("off" if name == "off" and role == "home" else role) + "\n"
                        + f"shared-memory {int(name != 'no-shared-memory' or role == 'worker')}\n"
                        + f"automatic-threads {int(name != 'no-automatic-threads' or role == 'worker')}\n"
                        + "max-pages 32\nmax-threads 4\nmax-slices 64\nquantum-ticks 2\n"
                        + "timeout-ms 15000\nmax-bytes 524288\nport 47220\n"
                        + ("address 10.0.3.3\n" if role == "home" else "address 10.0.3.3\n")
                        + "token " + "a7" * 32 + "\n")
                if role == "home":
                    if name == "page-limit": text = text.replace("max-pages 32", "max-pages 2")
                    if name == "byte-limit": text = text.replace("max-bytes 524288", "max-bytes 8192")
                    if name == "thread-limit": text = text.replace("max-threads 4", "max-threads 1")
                    if name == "slice-limit": text = text.replace("max-slices 64", "max-slices 1")
                if name == "disconnect": text = text.replace("timeout-ms 15000", "timeout-ms 5000")
                config.write_text(text)
                run(["mcopy", "-o", "-i", str(disk), str(config), "::/" + config.name])
            sockets, reservations, ports, processes, logs, commands = [], [], [], [], [], []
            paths = [case / "client.log", case / "server.log"]
            try:
                for _ in range(2):
                    reserve = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    reserve.bind(("127.0.0.1", 0))
                    ports.append(reserve.getsockname()[1]); reservations.append(reserve)
                    lane = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    lane.bind(("127.0.0.1", 0)); sockets.append(lane)
                for reserve in reservations:
                    reserve.close()
                macs = [bytes.fromhex(f"52540012340{i+1}") for i in range(2)]
                for i in range(2):
                    cmd = ["qemu-system-x86_64", "-machine", "q35", "-accel", args.accel,
                           "-no-reboot", "-m", "512M", "-smp", "1", "-nographic", "-snapshot",
                           "-kernel", str(frozen), "-append", f"a20.tcpmode=lwip a20.dhcp=0 a20.ip=10.0.3.{i+2} a20.netmask=255.255.255.0", "-drive", f"file={disk},if=none,format=raw,id=disk",
                           "-device", "virtio-blk-pci,drive=disk", "-netdev",
                           f"socket,id=net,udp=127.0.0.1:{sockets[i].getsockname()[1]},localaddr=127.0.0.1:{ports[i]}",
                           "-device", f"virtio-net-pci,netdev=net,mac={macs[i].hex(':')}",
                           "-fw_cfg", f"name=opt/x86/cmdline,string=a20.tcpmode=lwip a20.dhcp=0 a20.ip=10.0.3.{i+2} a20.netmask=255.255.255.0"]
                    if args.debug_qemu:
                        cmd += ["-d", "int,guest_errors", "-D", str(case / f"qemu-{i}.log")]
                    log = paths[i].open("w"); logs.append(log); commands.append(cmd)
                    processes.append(subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT))
                (case / "commands.json").write_text(json.dumps(commands, indent=2)+"\n")
                pcap = (case / "ethernet.pcap").open("wb")
                pcap.write(struct.pack("<IHHIIII", 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))
                launched, forwarded, began = set(), 0, time.monotonic()
                disconnected = False
                while time.monotonic()-began < args.timeout:
                    ready, _, _ = select.select(sockets, [], [], 0.005)
                    for lane in ready:
                        frame, _ = lane.recvfrom(65536)
                        now = time.time()
                        pcap.write(struct.pack("<IIII", int(now), int(now % 1 * 1000000), len(frame), len(frame)) + frame)
                        pcap.flush()
                        source = sockets.index(lane)
                        # Partition the link at the first worker completion,
                        # after real user execution but before state commit.
                        if name == "disconnect" and source == 1 and b"SSI1\x06\x00\x00\x00" in frame:
                            disconnected = True
                        if disconnected:
                            continue
                        if frame[:6] == macs[1-source] or (frame and frame[0] & 1):
                            sockets[1-source].sendto(frame, ("127.0.0.1", ports[1-source]))
                            forwarded += 1
                    outputs = [p.read_text(errors="replace") for p in paths]
                    for i, out in enumerate(outputs):
                        if i not in launched and "\n# " in out and (i == 1 or "SSI_WORKER: READY" in outputs[1]):
                            command = f"/bin/{service.name} /bin/worker.conf" if i == 1 else f"/bin/{service.name} /bin/home.conf /bin/{fixture.name}"
                            processes[i].stdin.write((command + "\n").encode())
                            processes[i].stdin.flush(); launched.add(i)
                        if (name != "disconnect" and "Signal 9" in out) or "a20ssi exec:" in out or "a20ssi configure:" in out or "SSI_PTHREAD: FAIL" in out or "KERNEL PANIC" in out or processes[i].poll() is not None:
                            raise RuntimeError(f"{name} node {i} failed (exit={processes[i].poll()}); see {paths[i]}")
                    failed_closed = name == "disconnect" and "Signal 9" in outputs[0]
                    if failed_closed or "SSI_PTHREAD: PASS" in outputs[0]:
                        if (active and not forwarded) or "SSI_WORKER: READY" not in outputs[1]:
                            raise RuntimeError("missing independent server/network execution")
                        home = re.findall(r"home slice epoch=(\d+) tid=(\d+) pages=(\d+) bytes=(\d+) reason=(\d+) result=(-?\d+)", outputs[0])
                        worker = re.findall(r"worker slice epoch=(\d+).*user-ticks=(\d+) reason=(\d+) result=(-?\d+)", outputs[1])
                        if active and (not worker or not home):
                            raise RuntimeError("missing actual remote user execution")
                        if not active and home:
                            raise RuntimeError("disabled mode unexpectedly offloaded")
                        if name == "disconnect":
                            if not failed_closed or not disconnected or "SSI_PTHREAD: PASS" in outputs[0] or len(home) != 1 or home[0][-1] != "-110":
                                raise RuntimeError("partition did not terminate uncertain execution without replay")
                        elif any(int(h[-1]) for h in home):
                            raise RuntimeError("a slice failed during successful program run")
                        if name == "automatic" and not any(int(w[1]) for w in worker):
                            raise RuntimeError("missing remote user CPU ticks")
                        if name == "page-limit" and (any(int(h[2]) > 2 for h in home) or not any(h[4] == "2" for h in home)):
                            raise RuntimeError("page limit not enforced")
                        if name == "byte-limit" and (any(int(h[3]) > 8192 for h in home) or not any(h[4] == "2" for h in home)):
                            raise RuntimeError("byte limit not enforced")
                        if name == "thread-limit":
                            admitted = set()
                            for line in outputs[0].splitlines():
                                match = re.search(r"home slice epoch=\d+ tid=(\d+)", line)
                                released = re.search(r"home thread released tid=(\d+)", line)
                                if match: admitted.add(match[1])
                                if released: admitted.discard(released[1])
                                if len(admitted) > 1:
                                    raise RuntimeError("thread admission limit not enforced")
                        if name == "slice-limit" and len(home) != len({h[1] for h in home}):
                            raise RuntimeError("per-thread slice limit not enforced")
                        report["cases"].append({"mode": name, "passed": True, "ethernet_frames": forwarded,
                                                "home_slices": len(home), "worker_user_ticks": sum(int(w[1]) for w in worker),
                                                "partitioned": disconnected})
                        print(f"PASS {name}: two independent kernels, {forwarded} Ethernet frames, {len(home)} slices", flush=True)
                        break
                else:
                    raise TimeoutError(f"{name}: see {case}")
            finally:
                if "pcap" in locals():
                    pcap.close()
                for process in processes:
                    if process.poll() is None:
                        process.terminate()
                for process in processes:
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill(); process.wait()
                for log in logs:
                    log.close()
                for sock in sockets + reservations:
                    sock.close()
        if sha(image) != report["image_sha256"]:
            raise RuntimeError("input image changed during run")
        report["passed"] = True
    finally:
        (directory / "report.json").write_text(json.dumps(report, indent=2)+"\n")
    print("check-cluster-ssi-network: PASS", flush=True)


if __name__ == "__main__":
    main()
