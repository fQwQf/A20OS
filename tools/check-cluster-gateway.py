#!/usr/bin/env python3
"""Two independent kernels and the shipped static leaf over UDP/COM2.

RISC-V Native client -> Ethernet/UDP -> x86 Native relay -> COM2/SLIP -> leaf.
Injects control/data loss and leaf reboot, then replays old session traffic.
No guest reply is synthesized. This is emulation, not physical-board evidence.
"""
import argparse
from collections import deque
import hashlib
import json
from pathlib import Path
import platform
import select
import shlex
import socket
import subprocess
import tempfile
import time


def decode_slip(packet):
    result = bytearray()
    escape = False
    for b in packet:
        if escape:
            result.append({0xdc: 0xc0, 0xdd: 0xdb}.get(b, b))
            escape = False
        elif b == 0xdb:
            escape = True
        else:
            result.append(b)
    return result


def cl_payload(data):
    if len(data) >= 42 and data[12:14] == b"\x08\x00":
        ihl = (data[14] & 15) * 4
        if ihl >= 20 and data[23] == 17 and len(data) >= 22 + ihl:
            return data[22 + ihl:]
    return b""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    directory = root / ".kernel-build/cluster/gateway"
    directory.mkdir(parents=True, exist_ok=True)
    commands = []
    for arch in ("riscv64", "x86_64"):
        make = ["make", "--no-print-directory", f"ARCH={arch}", "ABI=both", "BRINGUP=0"]
        if arch == "x86_64":
            make += ["CLUSTER_UART_COM2=1"]
        if not args.skip_build:
            with (directory / f"build-{arch}.log").open("w") as log:
                subprocess.run(make + ["-j8", "dev-build"], cwd=root, stdout=log,
                               stderr=subprocess.STDOUT, check=True)
        cmd = shlex.split(subprocess.check_output(make + ["-s", "_qemu_argv"], cwd=root, text=True))
        for i in reversed(range(len(cmd)-1)):
            if cmd[i] == "-netdev" or (cmd[i] == "-device" and cmd[i+1].startswith("virtio-net")):
                del cmd[i:i+2]
        if "-accel" in cmd:
            cmd[cmd.index("-accel")+1] = "tcg"
        commands.append(cmd)
    binary = directory / "leaf-process"
    subprocess.run(["cc", "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-Ikernel/arch/riscv64/include", "-Ikernel/include", "-Ikernel",
                    "tools/fixtures/clx_leaf_process.c", "-o", str(binary)], cwd=root, check=True)
    udp = [socket.socket(socket.AF_INET, socket.SOCK_DGRAM) for _ in range(2)]
    reservations = [socket.socket(socket.AF_INET, socket.SOCK_DGRAM) for _ in range(2)]
    for sock in udp + reservations:
        sock.bind(("127.0.0.1", 0))
    ports = [s.getsockname()[1] for s in reservations]
    processes, logs, endpoints = [], [], list(udp)
    events, saved, keys = {}, {}, set()
    trace = deque(maxlen=1024)
    success = False
    started = time.monotonic()

    def once(key):
        if key in events:
            return False
        events[key] = 1
        return True

    def leaf_start(reboot):
        wire, stdio = socket.socketpair()
        endpoints.extend([wire, stdio])
        log = (directory / ("leaf-reboot.log" if reboot else "leaf.log")).open("w")
        logs.append(log)
        process = subprocess.Popen([str(binary)] + (["300"] if reboot else []), cwd=root,
                                   stdin=stdio, stdout=stdio, stderr=log)
        processes.append(process)
        stdio.close()
        wire.setblocking(False)
        return process, wire

    try:
        with tempfile.TemporaryDirectory(prefix="a20-gateway-") as temporary:
            path = str(Path(temporary) / "com2.sock")
            for i, cmd in enumerate(commands):
                boot = f"a20.dhcp=0 a20.ip=10.0.3.{i+2} a20.netmask=255.255.255.0"
                if "-append" in cmd:
                    cmd[cmd.index("-append")+1] += " " + boot
                else:
                    cmd += ["-append", boot]
                if i:
                    cmd += ["-fw_cfg", f"name=opt/x86/cmdline,string={boot}"]
                device = "virtio-net-device,bus=virtio-mmio-bus.4" if i == 0 else "virtio-net-pci"
                cmd += ["-snapshot", "-netdev", f"socket,id=net,udp=127.0.0.1:{udp[i].getsockname()[1]},localaddr=127.0.0.1:{ports[i]}",
                        "-device", f"{device},netdev=net,mac=52:54:00:12:34:0{i+1}"]
                if i:
                    cmd += ["-serial", "mon:stdio", "-chardev", f"socket,id=cluster,path={path},server=on,wait=off", "-serial", "chardev:cluster"]
                reservations[i].close()
                log = (directory / ("client.log" if i == 0 else "gateway.log")).open("w")
                logs.append(log)
                processes.append(subprocess.Popen(cmd, cwd=root, stdin=subprocess.PIPE,
                                                   stdout=log, stderr=subprocess.STDOUT))
            head = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            endpoints.append(head)
            while True:
                try:
                    head.connect(path)
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    if time.monotonic()-started > 10:
                        raise RuntimeError("COM2 socket unavailable")
                    time.sleep(0.02)
            head.setblocking(False)
            leaf, wire = leaf_start(False)
            serial = [head, wire]
            pending, packets = [bytearray(), bytearray()], [bytearray(), bytearray()]
            launched = rebooted = replayed = False
            while time.monotonic()-started < args.timeout:
                if any(p.poll() is not None for p in processes[:2]) or leaf.poll() is not None:
                    raise RuntimeError("kernel/leaf exited; inspect logs")
                ready, writable, _ = select.select(udp + serial,
                    [s for i, s in enumerate(serial) if pending[i]], [], 0.01)
                for sock in writable:
                    i = serial.index(sock)
                    n = sock.send(pending[i])
                    del pending[i][:n]
                for sock in ready:
                    if sock in udp:
                        side = udp.index(sock)
                        data, _ = sock.recvfrom(65536)
                        key = f"ethernet-{side}-{data[12:14].hex()}"
                        events[key] = events.get(key, 0) + 1
                        if len(trace) < 100:
                            trace.append({"lane": "ethernet", "side": side, "hex": data.hex()})
                        cl = cl_payload(data)
                        if len(cl) >= 34 and cl[:2] == b"CL":
                            trace.append({"lane": "udp", "side": side, "hex": cl.hex()})
                            if cl[32:35] == b"CS\x01":
                                key = f"udp-{side}-drop-cs-{cl[35]}"
                                if cl[35] == 2 and int.from_bytes(cl[64:72], "little") == 200:
                                    saved.setdefault("udp-old-reply", (side, data))
                                if once(key):
                                    continue
                        udp[1-side].sendto(data, ("127.0.0.1", ports[1-side]))
                    else:
                        side = serial.index(sock)
                        data = sock.recv(4096)
                        if not data:
                            raise RuntimeError("serial stream closed")
                        for b in data:
                            if b != 0xc0:
                                packets[side].append(b)
                                if len(packets[side]) > 1024:
                                    raise RuntimeError("unbounded serial packet")
                                continue
                            packet = bytes(packets[side])
                            packets[side].clear()
                            if not packet:
                                continue
                            cl = decode_slip(packet)
                            trace.append({"lane": "uart", "side": side, "hex": cl.hex()})
                            if len(cl) > 256:
                                raise RuntimeError("gateway exceeded UART path MTU")
                            if cl[32:35] == b"CS\x01":
                                if int.from_bytes(cl[36:40], "little") != 256:
                                    raise RuntimeError("CS did not clamp the UART path MTU")
                                if side == 0 and cl[30] != 7:
                                    raise RuntimeError("relay did not decrement TTL")
                                events["uart-path-mtu-256"] = events.get("uart-path-mtu-256", 0) + 1
                            work = len(cl) >= 82 and cl[48:50] == b"CW"
                            if side == 0 and work and cl[3] == 6 and cl[51] == 2:
                                if once("uart-drop-work"):
                                    continue
                                saved.setdefault("uart-old-call", packet)
                                if int.from_bytes(cl[64:68], "little") == 1:
                                    keys.add(bytes(cl[8:12] + cl[32:48]))
                            if side == 1 and cl[3] == 7 and cl[52:54] == b"CW" and once("uart-drop-reply"):
                                continue
                            pending[1-side] += b"\xc0" + packet + b"\xc0"
                output = (directory / "client.log").read_text(errors="replace")
                gateway = (directory / "gateway.log").read_text(errors="replace")
                if "F:" in output or "F:" in gateway or "PANIC" in output+gateway:
                    raise RuntimeError("Native gateway test failed; inspect logs")
                if not launched and time.monotonic()-started >= 8:
                    processes[1].stdin.write(b"/bin/native-cluster-x86_64 gateway-head\n")
                    processes[1].stdin.flush()
                    processes[0].stdin.write(b"/bin/native-cluster-rv gateway-client /bin/clusterd-rv /bin/cluster-agent-rv\n")
                    processes[0].stdin.flush()
                    launched = True
                if "GATEWAY: reboot-ready" in output and not rebooted:
                    leaf.terminate()
                    leaf.wait(timeout=5)
                    serial[1].close()
                    leaf, wire = leaf_start(True)
                    serial[1] = wire
                    pending[1].clear()
                    packets[1].clear()
                    rebooted = True
                    events["leaf-reboot"] = 1
                if "GATEWAY: reboot-pass" in output and not replayed:
                    if "udp-old-reply" not in saved or "uart-old-call" not in saved:
                        raise RuntimeError("old-session evidence missing")
                    side, data = saved["udp-old-reply"]
                    udp[1-side].sendto(data, ("127.0.0.1", ports[1-side]))
                    pending[1] += b"\xc0" + saved["uart-old-call"] + b"\xc0"
                    events["old-cs-reply-replayed"] = events["old-call-replayed"] = 1
                    replayed = True
                if "UART: PASS" in output:
                    executed = sum((directory / name).read_text().count("DOT-EXEC:")
                                   for name in ("leaf.log", "leaf-reboot.log"))
                    if (not rebooted or not replayed or executed != len(keys) or executed < 96
                            or "GATEWAY: mtu-pass" not in output
                            or not all(events.get(k) for k in ("udp-0-drop-cs-1", "udp-0-drop-cs-2",
                                      "udp-1-drop-cs-1", "udp-1-drop-cs-2", "uart-path-mtu-256"))):
                        raise RuntimeError(f"fault/dedup/reboot evidence incomplete: {executed=}, {len(keys)=}")
                    success = True
                    events.update({"dot_executions": executed, "unique_dot_transactions": len(keys)})
                    print("check-cluster-gateway: PASS (RISC-V -> UDP -> x86 COM2 -> static leaf, CS loss, reliable dedup, reboot/old replay, any/mixed tree)")
                    return
            raise TimeoutError("gateway test timed out; inspect logs")
    finally:
        for p in processes:
            if p.poll() is None:
                p.terminate()
        for p in processes:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        for s in endpoints + reservations:
            s.close()
        for log in logs:
            log.close()
        sources = ["kernel/cluster/session.c", "kernel/cluster/session_control.h",
                   "kernel/cluster/transport.c", "kernel/cluster/udp.c", "kernel/cluster/uart.c",
                   "kernel/mcu/leaf.c", "user/tests/test_native_cluster.c",
                   "tools/check-cluster-gateway.py", "tools/fixtures/clx_leaf_process.c"]
        events.update({"success": success, "elapsed_seconds": time.monotonic()-started,
                       "platform": platform.platform(), "scope": "two TCG kernels and host leaf, not physical hardware",
                       "source_sha256": {p: hashlib.sha256((root/p).read_bytes()).hexdigest() for p in sources}})
        (directory / "events.json").write_text(json.dumps(events, indent=2)+"\n")
        (directory / "trace.json").write_text(json.dumps(list(trace), indent=2)+"\n")


if __name__ == "__main__":
    main()
