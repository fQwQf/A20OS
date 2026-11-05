#!/usr/bin/env python3
"""Real optional native memory service across two isolated x86 A20OS kernels.

Only Ethernet frames cross the fixture; RAM and disks are separate. Existing
kernel/image inputs can be supplied together; otherwise build the current tree.
"""
import argparse
import hashlib
import json
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


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
    args = parser.parse_args()
    if bool(args.kernel) != bool(args.image):
        parser.error("--kernel and --image must be supplied together")
    parent = ROOT / ".kernel-build/cluster/memory"
    parent.mkdir(parents=True, exist_ok=True)
    directory = args.out.resolve() if args.out else Path(tempfile.mkdtemp(prefix="network-", dir=parent))
    if args.out:
        directory.mkdir(parents=True, exist_ok=False)
    report = {"passed": False, "cases": [], "transport": "guest Ethernet/IPv4 UDP", "shared_ram": False}
    make = ["make", "--no-print-directory", "ARCH=x86_64", "ABI=both", "CLUSTER_MEMORY=1", "CLUSTER_MEMORY_PAGES=8"]
    def run(command, **kwargs):
        return subprocess.run(command, cwd=ROOT, check=True, **kwargs)
    print(f"cluster memory network: {directory}", flush=True)
    try:
        with (directory / "build.log").open("w") as log:
            run(make + ["native-cluster-memory"] + ([] if args.kernel else ["-j4", "dev-build"]),
                stdout=log, stderr=subprocess.STDOUT)
        service, fixture = [ROOT / p for p in subprocess.check_output(
            make + ["-s", "print-cluster-memory-paths"], cwd=ROOT, text=True).splitlines()]
        if not args.kernel:
            build = ROOT / subprocess.check_output(make + ["-s", "print-build-dir"], cwd=ROOT, text=True).strip()
            args.kernel, args.image = build / "kernel.elf", build / "fat32.img"
        kernel, image = args.kernel.resolve(), args.image.resolve()
        report.update(kernel=str(kernel), kernel_sha256=sha(kernel), image=str(image),
                      image_sha256=sha(image), service_sha256=sha(service), fixture_sha256=sha(fixture))
        frozen = directory / "kernel.elf"
        run(["cp", "--reflink=auto", str(kernel), str(frozen)])
        for mode, name in enumerate(("off", "read-only", "read-write")):
            case = directory / name
            case.mkdir()
            disk = case / "fat32.img"
            run(["cp", "--reflink=auto", "--sparse=always", str(image), str(disk)])
            for source in (service, fixture):
                run(["mcopy", "-o", "-i", str(disk), str(source), "::/" + source.name], stdout=subprocess.DEVNULL)
            config = case / "memory.conf"
            text = f"version 1\nmode {name}\nmax-pages 2\ntransaction-ms 3000\nbytes-per-second 0\n"
            if mode == 0:
                text += "initial-file /bin/does-not-exist\n"
            elif mode == 1:
                initial = case / "model.bin"
                initial.write_bytes(bytes([13]) * 4096 + bytes([29]) * 512)
                run(["mcopy", "-o", "-i", str(disk), str(initial), "::/model.bin"])
                text += "initial-file /bin/model.bin\n"
            config.write_text(text)
            run(["mcopy", "-o", "-i", str(disk), str(config), "::/memory.conf"])
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
                           "-kernel", str(frozen), "-drive", f"file={disk},if=none,format=raw,id=disk",
                           "-device", "virtio-blk-pci,drive=disk", "-netdev",
                           f"socket,id=net,udp=127.0.0.1:{sockets[i].getsockname()[1]},localaddr=127.0.0.1:{ports[i]}",
                           "-device", f"virtio-net-pci,netdev=net,mac={macs[i].hex(':')}",
                           "-fw_cfg", f"name=opt/x86/cmdline,string=a20.dhcp=0 a20.ip=10.0.3.{i+2} a20.netmask=255.255.255.0"]
                    log = paths[i].open("w"); logs.append(log); commands.append(cmd)
                    processes.append(subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT))
                (case / "commands.json").write_text(json.dumps(commands, indent=2)+"\n")
                launched, forwarded, began = set(), 0, time.monotonic()
                while time.monotonic()-began < args.timeout:
                    ready, _, _ = select.select(sockets, [], [], 0.005)
                    for lane in ready:
                        frame, _ = lane.recvfrom(65536)
                        source = sockets.index(lane)
                        if frame[:6] == macs[1-source] or (frame and frame[0] & 1):
                            sockets[1-source].sendto(frame, ("127.0.0.1", ports[1-source]))
                            forwarded += 1
                    outputs = [p.read_text(errors="replace") for p in paths]
                    for i, out in enumerate(outputs):
                        if i not in launched and "\n# " in out:
                            processes[i].stdin.write(f"/bin/{fixture.name} {i} {mode} /bin/{service.name}\n".encode())
                            processes[i].stdin.flush(); launched.add(i)
                        if "MEMORY_TEST: FAIL" in out or "KERNEL PANIC" in out or processes[i].poll() is not None:
                            raise RuntimeError(f"{name} node {i} failed; see {paths[i]}")
                    if "MEMORY_TEST: PASS" in outputs[0]:
                        if not forwarded or "MEMORY_TEST: SERVER" not in outputs[1]:
                            raise RuntimeError("missing independent server/network execution")
                        if mode == 2 and "MEMORY_TEST: SERVER restarted" not in outputs[1]:
                            raise RuntimeError("missing actual service restart")
                        report["cases"].append({"mode": name, "passed": True, "ethernet_frames": forwarded})
                        print(f"PASS {name}: two independent kernels, {forwarded} Ethernet frames", flush=True)
                        break
                else:
                    raise TimeoutError(f"{name}: see {case}")
            finally:
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
    print("check-cluster-memory-network: PASS", flush=True)


if __name__ == "__main__":
    main()
