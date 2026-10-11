#!/usr/bin/env python3
"""Execute Native calls and a mixed job through the real x86 COM2 driver.

The remote process runs the shipped leaf, substituting byte I/O and clock.
QEMU executes a separate full kernel. This is not physical MCU evidence.
"""
import argparse
from collections import deque
import hashlib
import json
import platform
import re
import select
import shlex
import socket
import subprocess
import tempfile
import time
from pathlib import Path


def decode_slip(packet):
    decoded = bytearray()
    escape = False
    for byte in packet:
        if escape:
            decoded.append({0xdc: 0xc0, 0xdd: 0xdb}.get(byte, byte))
            escape = False
        elif byte == 0xdb:
            escape = True
        else:
            decoded.append(byte)
    return decoded


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--soak-calls", type=int, default=96)
    args = parser.parse_args()
    if not 96 <= args.soak_calls <= 10000:
        parser.error("--soak-calls must be between 96 and 10000")
    root = Path(__file__).resolve().parent.parent
    directory = root / ".kernel-build/cluster/uart"
    directory.mkdir(parents=True, exist_ok=True)
    make = ["make", "--no-print-directory", "ARCH=x86_64", "ABI=both",
            "BRINGUP=0", "CLUSTER_UART_COM2=1"]
    if not args.skip_build:
        with (directory / "build.log").open("w") as log:
            subprocess.run(make + ["-j8", "dev-build"], cwd=root, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
    binary = directory / "leaf-process"
    subprocess.run(["cc", "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-Ikernel/arch/riscv64/include", "-Ikernel/include", "-Ikernel",
                    "tools/tests/clx_leaf_process.c", "-o", str(binary)],
                   cwd=root, check=True)
    command = shlex.split(subprocess.check_output(make + ["-s", "_qemu_argv"],
                                                  cwd=root, text=True))
    if "-accel" in command:
        command[command.index("-accel") + 1] = "tcg"
    processes, streams, endpoints = [], [], []
    events = {"dropped-request": 0, "dropped-reply": 0, "frames": [0, 0]}
    dot_keys = set()
    trace = deque(maxlen=256)
    started = time.monotonic()
    try:
        with tempfile.TemporaryDirectory(prefix="a20-uart-") as temporary:
            path = str(Path(temporary) / "com2.sock")
            guest_log = (directory / "kernel.log").open("w")
            leaf_log = (directory / "leaf.log").open("w")
            streams.extend([guest_log, leaf_log])
            command += ["-snapshot", "-serial", "mon:stdio", "-chardev",
                        f"socket,id=cluster,path={path},server=on,wait=off",
                        "-serial", "chardev:cluster"]
            guest = subprocess.Popen(command, cwd=root, stdin=subprocess.PIPE,
                                     stdout=guest_log, stderr=subprocess.STDOUT)
            processes.append(guest)
            head = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            endpoints.append(head)
            while True:
                try:
                    head.connect(path)
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    if time.monotonic() - started > 10 or guest.poll() is not None:
                        raise RuntimeError("COM2 chardev unavailable; inspect kernel.log")
                    time.sleep(0.02)
            leaf_wire, leaf_stdio = socket.socketpair()
            endpoints.extend([leaf_wire, leaf_stdio])
            leaf = subprocess.Popen([str(binary)], cwd=root, stdin=leaf_stdio,
                                    stdout=leaf_stdio, stderr=leaf_log)
            processes.append(leaf)
            leaf_stdio.close()
            sockets = [head, leaf_wire]
            for sock in sockets:
                sock.setblocking(False)
            pending = [bytearray(), bytearray()]
            packets = [bytearray(), bytearray()]
            launched = False
            while time.monotonic() - started < args.timeout:
                if any(p.poll() is not None for p in processes):
                    raise RuntimeError("guest/leaf exited; inspect logs")
                readable, writable, _ = select.select(
                    sockets, [s for i, s in enumerate(sockets) if pending[i]], [], 0.01)
                for sock in writable:
                    i = sockets.index(sock)
                    n = sock.send(pending[i])
                    del pending[i][:n]
                for sock in readable:
                    side = sockets.index(sock)
                    data = sock.recv(4096)
                    if not data:
                        raise RuntimeError("COM2 stream closed")
                    for byte in data:
                        if byte != 0xc0:
                            packets[side].append(byte)
                            if len(packets[side]) > 1024:
                                raise RuntimeError("unbounded SLIP packet")
                            continue
                        packet = bytes(packets[side])
                        packets[side].clear()
                        if not packet:
                            continue
                        frame = decode_slip(packet)
                        events["frames"][side] += 1
                        trace.append({"time": round(time.monotonic()-started, 6),
                                      "side": side, "hex": frame.hex()})
                        work = len(frame) >= 82 and frame[48:50] == b"CW"
                        if side == 0 and work and frame[3] == 6 and frame[51] == 2:
                            if not events["dropped-request"]:
                                events["dropped-request"] = 1
                                continue
                            if int.from_bytes(frame[64:68], "little") == 1:
                                dot_keys.add(bytes(frame[8:12] + frame[32:48]))
                        if side == 1 and frame[3] == 7 and frame[52:54] == b"CW" and not events["dropped-reply"]:
                            events["dropped-reply"] = 1
                            continue
                        pending[1-side] += b"\xc0" + packet + b"\xc0"
                if not launched and time.monotonic() - started >= 8:
                    guest.stdin.write(f"/bin/native-cluster-x86_64 uart /bin/clusterd-x86_64 /bin/cluster-agent-x86_64 {args.soak_calls}\n".encode())
                    guest.stdin.flush()
                    launched = True
                output = (directory / "kernel.log").read_text(errors="replace")
                if "F:uart-" in output or "PANIC" in output:
                    raise RuntimeError("Native UART test failed; inspect kernel.log")
                if "UART: PASS" in output:
                    executed = (directory / "leaf.log").read_text().count("DOT-EXEC:")
                    if executed < args.soak_calls or executed != len(dot_keys) or not all(events[k] for k in ("dropped-request", "dropped-reply")):
                        raise RuntimeError(f"dedup/fault evidence missing: {executed=}, {len(dot_keys)=}, {events=}")
                    timings = {name: int(value, 16) for name, value in
                               re.findall(r"UART: ([a-z0-9-]+)=([0-9a-f]{16})", output)}
                    events["client_metrics"] = timings
                    events["platform"] = platform.platform()
                    events["qemu_version"] = subprocess.check_output(
                        ["qemu-system-x86_64", "--version"], text=True).splitlines()[0]
                    inputs = ["kernel/arch/x86_64/platform/cluster_uart.c",
                              "kernel/cluster/uart.c", "kernel/cluster/transport.c",
                              "kernel/proc/proc.c", "kernel/proc/task.c", "kernel/proc/exit.c",
                              "kernel/mcu/leaf.c", "tools/tests/clx_leaf_process.c",
                              "user/tests/test_native_cluster.c", "user/svc/clusterd.c",
                              "user/liba20rt/a20_cluster_job.h"]
                    events["source_sha256"] = {p: hashlib.sha256((root/p).read_bytes()).hexdigest()
                                               for p in inputs}
                    events.update({"dot_executions": executed, "unique_dot_transactions": len(dot_keys),
                                   "elapsed_seconds": round(time.monotonic()-started, 6),
                                   "scope": "x86 TCG kernel + host MCU leaf, emulated COM2, not physical throughput"})
                    (directory / "events.json").write_text(json.dumps(events, indent=2) + "\n")
                    print("check-cluster-uart: PASS (Native COM2 directory, reliable DOT calls, loss/dedup, clusterd any, mixed tree)")
                    return
            raise TimeoutError("UART checker timed out; inspect kernel.log and leaf.log")
    finally:
        (directory / "trace.json").write_text(json.dumps(list(trace), indent=2) + "\n")
        for process in processes:
            if process.poll() is None:
                process.terminate()
        for process in processes:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        for endpoint in endpoints:
            endpoint.close()
        for stream in streams:
            stream.close()


if __name__ == "__main__":
    main()
