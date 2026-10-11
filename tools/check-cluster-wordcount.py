#!/usr/bin/env python3
"""Eight independent execution nodes: Native servers, optionally two MCU leaves."""
import argparse
from collections import Counter, deque
import hashlib
import json
from pathlib import Path
import re
import select
import shlex
import socket
import struct
import subprocess
import tempfile
import time
from importlib.util import module_from_spec, spec_from_file_location


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mixed", action="store_true", help="six Native kernels plus two production MCU leaf processes")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    directory = root / (".kernel-build/cluster/wordcount-mixed" if args.mixed else ".kernel-build/cluster/wordcount-server")
    directory.mkdir(parents=True, exist_ok=True)
    spec = spec_from_file_location("gateway_fixture", root / "tools/check-cluster-gateway.py")
    gateway = module_from_spec(spec)
    spec.loader.exec_module(gateway)
    source = (root / "tools/fixtures/wordcount.txt").read_bytes()
    words = Counter(re.findall(rb"[a-z0-9]+", source.lower()))
    expected = struct.pack("<I", len(words)) + b"".join(w.ljust(12, b"\0") + struct.pack("<I", n) for w, n in sorted(words.items()))
    assert len(source) == 1014 and len(words) == 8 and all(len(w) == 12 for w in words)
    count = 6 if args.mixed else 8
    base = {}
    for arch in (("riscv64", "x86_64") if args.mixed else ("riscv64",)):
        make = ["make", "--no-print-directory", f"ARCH={arch}", "ABI=both", "BRINGUP=0"]
        if arch == "x86_64":
            make += ["CLUSTER_UART_COM2=1"]
        if not args.skip_build:
            with (directory / f"build-{arch}.log").open("w") as log:
                subprocess.run(make + ["-j8", "dev-build"], cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
        cmd = shlex.split(subprocess.check_output(make + ["-s", "_qemu_argv"], cwd=root, text=True))
        for i in reversed(range(len(cmd)-1)):
            if cmd[i] == "-netdev" or (cmd[i] == "-device" and cmd[i+1].startswith("virtio-net")):
                del cmd[i:i+2]
        if "-accel" in cmd:
            cmd[cmd.index("-accel")+1] = "tcg"
        images = [part[5:] for token in cmd for part in token.split(",") if part.startswith("file=") and part.endswith("fat32.img")]
        if len(images) != 1:
            raise RuntimeError("expected exactly one FAT root image")
        subprocess.run(["mcopy", "-o", "-i", images[0], "tools/fixtures/wordcount.txt", "::/wordcount.txt"], cwd=root, check=True)
        base[arch] = cmd
    leaf_binary = directory / "leaf-process"
    if args.mixed:
        subprocess.run(["cc", "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror", "-Ikernel/arch/riscv64/include", "-Ikernel/include", "-Ikernel", "tools/fixtures/clx_leaf_process.c", "-o", str(leaf_binary)], cwd=root, check=True)
    reservations, udp, nodes, processes, logs, serial, leaf_logs = [], [], [], [], [], [], []
    ports = []
    for _ in range(count):
        reserve, lane = socket.socket(socket.AF_INET, socket.SOCK_DGRAM), socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        reserve.bind(("127.0.0.1", 0))
        lane.bind(("127.0.0.1", 0))
        reservations.append(reserve)
        udp.append(lane)
        ports.append(reserve.getsockname()[1])
    macs = [bytes.fromhex(f"5254001234{i+1:02x}") for i in range(count)]
    events, trace, keys, admitted = {}, deque(maxlen=2048), [{}, {}], [set(), set()]
    root_hash = 2166136261
    for byte in bytes(15) + b"\x01":
        root_hash = ((root_hash ^ byte) * 16777619) & 0xffffffff
    success = False
    launched = killed = False
    started = time.monotonic()
    try:
        with tempfile.TemporaryDirectory(prefix="a20-wordcount-") as temporary:
            for reserve in reservations:
                reserve.close()
            for i in range(count):
                arch = "x86_64" if args.mixed and i >= 4 else "riscv64"
                cmd = list(base[arch]) + ["-snapshot"]
                boot = f"a20.dhcp=0 a20.ip=10.0.3.{i+2} a20.netmask=255.255.255.0"
                if arch == "x86_64":
                    cmd += ["-fw_cfg", f"name=opt/x86/cmdline,string={boot}"]
                elif "-append" in cmd:
                    cmd[cmd.index("-append")+1] += " " + boot
                else:
                    cmd += ["-append", boot]
                device = "virtio-net-pci" if arch == "x86_64" else "virtio-net-device,bus=virtio-mmio-bus.4"
                cmd += ["-netdev", f"socket,id=net,udp=127.0.0.1:{udp[i].getsockname()[1]},localaddr=127.0.0.1:{ports[i]}", "-device", f"{device},netdev=net,mac={macs[i].hex(':')}"]
                if args.mixed and i >= 4:
                    cmd += ["-serial", "mon:stdio", "-serial", f"unix:{temporary}/uart-{i}.sock,server=on,wait=off"]
                log = (directory / f"node-{i+1}.log").open("w")
                logs.append(log)
                node = subprocess.Popen(cmd, cwd=root, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
                nodes.append(node)
                processes.append(node)
            if args.mixed:
                for i in (4, 5):
                    head = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    deadline = time.monotonic() + 10
                    while True:
                        try:
                            head.connect(f"{temporary}/uart-{i}.sock")
                            break
                        except (FileNotFoundError, ConnectionRefusedError):
                            if time.monotonic() >= deadline:
                                raise TimeoutError("COM2 did not start")
                            time.sleep(0.02)
                    leaf, stdio = socket.socketpair()
                    log = (directory / f"leaf-{i+3}.log").open("w")
                    logs.append(log)
                    leaf_logs.append(directory / f"leaf-{i+3}.log")
                    process = subprocess.Popen([str(leaf_binary), "--node", str(i+3)], cwd=root, stdin=stdio, stdout=stdio, stderr=log)
                    processes.append(process)
                    stdio.close()
                    head.setblocking(False)
                    leaf.setblocking(False)
                    serial.extend([head, leaf])
            pending = [bytearray() for _ in serial]
            packets = [bytearray() for _ in serial]
            while time.monotonic() - started < args.timeout:
                readers, writers, _ = select.select(udp + serial, [s for i, s in enumerate(serial) if pending[i]], [], 0.01)
                for sock in writers:
                    side = serial.index(sock)
                    n = sock.send(pending[side])
                    del pending[side][:n]
                for sock in readers:
                    if sock in udp:
                        side = udp.index(sock)
                        data, _ = sock.recvfrom(65536)
                        destinations = [macs.index(data[:6])] if data[:6] in macs else range(count)
                        cl = gateway.cl_payload(data)
                        if len(cl) >= 34 and cl[:2] == b"CL":
                            trace.append({"side": side, "lane": "udp", "hex": cl.hex()})
                            if (side == 1 and killed and cl[3] == 7 and cl[52:56] == b"CW\x01\x02"
                                    and int.from_bytes(cl[56:64], "little") == 10001):
                                events["dropped-dead-worker-reply"] = events.get("dropped-dead-worker-reply", 0) + 1
                                continue
                            if (side == 1 and not killed and cl[3] == 7 and cl[52:56] == b"CW\x01\x02"
                                    and int.from_bytes(cl[56:64], "little") == 10001
                                    and int.from_bytes(cl[68:72], "little") == 3):
                                events["killed-after-map-execution"] = side + 1
                                events["lost-shard"] = int.from_bytes(cl[64:68], "little")
                                nodes[side].terminate()
                                nodes[side].wait(timeout=5)
                                killed = True
                                continue
                        for target in destinations:
                            if target != side and not (target == 1 and killed):
                                udp[target].sendto(data, ("127.0.0.1", ports[target]))
                    else:
                        side = serial.index(sock)
                        data = sock.recv(4096)
                        if not data:
                            raise RuntimeError("leaf byte stream closed")
                        for byte in data:
                            if byte != 0xc0:
                                packets[side].append(byte)
                                if len(packets[side]) > 1024:
                                    raise RuntimeError("unbounded SLIP frame")
                                continue
                            packet = bytes(packets[side])
                            packets[side].clear()
                            if not packet:
                                continue
                            cl = gateway.decode_slip(packet)
                            trace.append({"side": side, "lane": "uart", "hex": cl.hex()})
                            if len(cl) > 256:
                                raise RuntimeError("MCU MTU exceeded")
                            if side % 2 == 0 and cl[3] == 6 and cl[48:52] == b"CW\x01\x02" and int.from_bytes(cl[64:68], "little") == 3:
                                keys[side // 2][bytes(cl[8:12] + cl[32:48])] = (int.from_bytes(cl[52:60], "little"), int.from_bytes(cl[60:64], "little"), int.from_bytes(cl[76:80], "little"))
                            if side % 2 == 1 and cl[3] == 9 and int.from_bytes(cl[16:20], "little") == root_hash:
                                # This fixture has one root boot and monotonic
                                # txids; ACK is correlated with its MAP calls.
                                admitted[side // 2].add(int.from_bytes(cl[8:12], "little"))
                            pending[side ^ 1] += b"\xc0" + packet + b"\xc0"
                if not launched and time.monotonic() - started >= 10:
                    for i, node in enumerate(nodes):
                        tag = "x86_64" if args.mixed and i >= 4 else "rv"
                        mode = "mixed" if args.mixed else "servers"
                        node.stdin.write(f"/bin/native-cluster-word8-{tag} {i} {mode} /bin/cluster-agent-{tag} /bin/clusterd-{tag} /bin/cluster-wordcount-{tag}\n".encode())
                        node.stdin.flush()
                    launched = True
                outputs = [(directory / f"node-{i+1}.log").read_text(errors="replace") for i in range(count)]
                if any("NATIVE_CLUSTER_WORD8: FAIL" in out or "PANIC" in out for out in outputs):
                    raise RuntimeError("guest failed; inspect node logs")
                out = outputs[0]
                if "NATIVE_CLUSTER_WORD8: PASS" in out:
                    results = re.findall(r"WORD8: result=([0-9a-f]+)", out)
                    assignments = [bytes.fromhex(value) for value in re.findall(r"WORD8: assignments=([0-9a-f]+)", out)]
                    if len(results) != 2 or any(bytes.fromhex(value) != expected for value in results) or not killed or "WORD8: baseline-pass" not in out or "WORD8: failure-pass" not in out:
                        raise RuntimeError("missing exact results or failure evidence")
                    lost = events["lost-shard"]
                    if (len(assignments) != 2 or any(len(row) != 62 for row in assignments)
                            or set(assignments[0][::2]) != set(range(1, 9))
                            or assignments[1][2 * lost] == 2 or assignments[1][2 * lost + 1] <= 1):
                        raise RuntimeError("missing all-node execution or exact lost-shard reassignment")
                    events["assignments_hex"] = [row.hex() for row in assignments]
                    if any(f"{word.decode()} {n}\n" not in out for word, n in words.items()):
                        raise RuntimeError("file CLI output differs from host reference")
                    for i, path in enumerate(leaf_logs):
                        executions = [tuple(map(int, row)) for row in re.findall(r"MAP-EXEC: job=(\d+) shard=(\d+) attempt=(\d+)", path.read_text())]
                        executed = len(executions)
                        accepted = [key for key in keys[i] if int.from_bytes(key[:4], "little") in admitted[i]]
                        if (not executed or executed != len(accepted) or len(set(executions)) != executed
                                or not set(executions).issubset(set(keys[i].values()))):
                            raise RuntimeError(f"leaf execution/dedup mismatch: {executed=}, {len(accepted)=}, {len(keys[i])=}")
                        events[f"leaf-{i+7}-executions"] = executed
                        events[f"leaf-{i+7}-admitted-transactions"] = len(accepted)
                        events[f"leaf-{i+7}-requested-transactions"] = len(keys[i])
                    success = True
                    print("check-cluster-wordcount: PASS (16 shards, 31 tasks, " + ("six Native kernels + two MCU leaves" if args.mixed else "eight Native kernels") + ", executed-node loss/reassignment, exact host reference and file CLI)")
                    return
                for i, process in enumerate(nodes):
                    if process.poll() is not None and not (i == 1 and killed):
                        raise RuntimeError(f"kernel {i+1} exited unexpectedly")
            raise TimeoutError("wordcount timeout; inspect logs")
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
        for sock in udp + serial + reservations:
            sock.close()
        sources = ["tools/check-cluster-wordcount.py", "tools/fixtures/wordcount.txt", "tools/fixtures/clx_leaf_process.c", "user/tests/test_native_cluster_word8.c", "user/liba20rt/a20_cluster_job.h", "kernel/include/uapi/a20/cluster_job.h", "kernel/include/uapi/a20/cluster_word_job.h", "kernel/include/uapi/a20/cluster_words.h", "kernel/mcu/leaf.c", "user/svc/clusterd.c", "user/svc/jobd.c", "user/svc/cluster_wordcount.c"]
        events.update({"success": success, "mixed": args.mixed, "elapsed_seconds": time.monotonic()-started, "shards": 16, "tasks": 31, "expected_hex": expected.hex(), "scope": "independent TCG kernels and optional production leaf host processes, not physical hardware", "source_sha256": {p: hashlib.sha256((root/p).read_bytes()).hexdigest() for p in sources}})
        (directory / "evidence.json").write_text(json.dumps(events, indent=2) + "\n")
        (directory / "trace.json").write_text(json.dumps(list(trace), indent=2) + "\n")


if __name__ == "__main__":
    main()
