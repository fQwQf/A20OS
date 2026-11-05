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
    parser.add_argument("--send", action="store_true", help="probe reliable SEND through both kernels and the static leaf")
    parser.add_argument("--abandon", action="store_true", help="lose every reply of the first leaf MAP attempt")
    parser.add_argument("--gossip", action="store_true", help="discover the leaf route from a relay, then expire it on link loss")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--log-dir", type=Path)
    args = parser.parse_args()
    if args.send and args.gossip:
        parser.error("--send currently uses explicit routes; gossip is a separate scenario")
    root = Path(__file__).resolve().parent.parent.parent
    directory = root / (".kernel-build/cluster/gateway-send" if args.send else
                        ".kernel-build/cluster/gateway-abandon" if args.abandon else
                        ".kernel-build/cluster/gateway-gossip" if args.gossip else ".kernel-build/cluster/gateway")
    if args.log_dir:
        directory = (root / args.log_dir).resolve()
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
    map_keys = set()
    map_acks = set()
    send_keys = set()
    send_retirements = {}
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
        process = subprocess.Popen([str(binary)] + (["300"] if reboot else []) +
                                   (["--send"] if args.send else []), cwd=root,
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
            launched = rebooted = replayed = unplugged = False
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
                            txid = int.from_bytes(cl[8:12], "little")
                            seq = int.from_bytes(cl[24:28], "little")
                            if args.send and side == 0 and cl[3] == 5 and int.from_bytes(cl[20:24], "little") == 3:
                                if txid or not seq or not cl[4] & 1 or len(cl) > 256:
                                    raise RuntimeError("invalid business SEND on UART")
                                send_keys.add(seq)
                                if once("uart-drop-send"):
                                    continue
                                saved.setdefault("uart-old-send", packet)
                                if once("uart-duplicate-send"):
                                    pending[1] += b"\xc0" + packet + b"\xc0"
                            if args.send and side == 1 and txid in send_keys:
                                if cl[3] == 7 and once("uart-drop-send-completion"):
                                    continue
                                if cl[3] == 9 and once("uart-drop-send-ack"):
                                    continue
                            if (args.send and side == 0 and cl[3] == 6 and
                                    int.from_bytes(cl[20:24], "little") == 0 and txid in send_keys):
                                send_retirements[txid] = send_retirements.get(txid, 0) + 1
                                if once("uart-drop-send-retirement"):
                                    saved["lost-send-retirement"] = txid
                                    continue
                            if cl[32:35] == b"CS\x01":
                                if int.from_bytes(cl[36:40], "little") != 256:
                                    raise RuntimeError("CS did not clamp the UART path MTU")
                                if side == 0 and cl[30] != 7:
                                    raise RuntimeError("relay did not decrement TTL")
                                events["uart-path-mtu-256"] = events.get("uart-path-mtu-256", 0) + 1
                            if side == 1 and cl[3] == 7 and len(cl) >= 54 and cl[4] & 1 and int.from_bytes(cl[48:52], "little") == 13:
                                events["mcu-admission-busy"] = events.get("mcu-admission-busy", 0) + 1
                            if side == 1 and cl[3] == 9 and int.from_bytes(cl[16:20], "little") == 0x68691772:
                                map_acks.add(int.from_bytes(cl[8:12], "little"))
                            work = len(cl) >= 82 and cl[48:50] == b"CW"
                            if side == 0 and work and cl[3] == 6 and cl[51] == 2:
                                if once("uart-drop-work"):
                                    continue
                                job = int.from_bytes(cl[52:60], "little")
                                if job != 0x53454e44:
                                    saved.setdefault("uart-old-call", packet)
                                if int.from_bytes(cl[64:68], "little") == 1 and job != 0x53454e44:
                                    keys.add(bytes(cl[8:12] + cl[32:48]))
                                if int.from_bytes(cl[64:68], "little") == 3:
                                    map_keys.add(bytes(cl[8:12] + cl[32:48]))
                            if side == 1 and cl[3] == 7 and cl[52:54] == b"CW" and once("uart-drop-reply"):
                                continue
                            if (side == 1 and cl[3] == 7 and cl[52:54] == b"CW"
                                    and int.from_bytes(cl[68:72], "little") == 3):
                                if args.abandon:
                                    transaction = bytes(cl[8:12] + cl[32:48])
                                    saved.setdefault("abandoned-map", transaction)
                                    if transaction == saved["abandoned-map"]:
                                        events["uart-drop-map-reply"] = events.get("uart-drop-map-reply", 0) + 1
                                        continue
                                elif once("uart-drop-map-reply"):
                                    continue
                            if not unplugged:
                                pending[1-side] += b"\xc0" + packet + b"\xc0"
                output = (directory / "client.log").read_text(errors="replace")
                gateway = (directory / "gateway.log").read_text(errors="replace")
                if "F:" in output or "F:" in gateway or "PANIC" in output+gateway:
                    raise RuntimeError("Native gateway test failed; inspect logs")
                if not launched and time.monotonic()-started >= 8:
                    processes[1].stdin.write(b"/bin/native-cluster-x86_64 " +
                                             (b"gateway-send-head" if args.send else b"gateway-head") +
                                             (b" /bin/clusterd-x86_64" if args.gossip else b"") + b"\n")
                    processes[1].stdin.flush()
                    processes[0].stdin.write(b"/bin/native-cluster-rv " +
                                             (b"gateway-send" if args.send else
                                              b"gateway-gossip" if args.gossip else b"gateway-client") +
                                             b" /bin/clusterd-rv /bin/cluster-agent-rv\n")
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
                    if args.send:
                        if "uart-old-send" not in saved:
                            raise RuntimeError("no old SEND for reboot replay")
                        pending[1] += b"\xc0" + saved["uart-old-send"] + b"\xc0"
                        events["old-send-replayed"] = 1
                    events["old-cs-reply-replayed"] = events["old-call-replayed"] = 1
                    replayed = True
                if args.gossip and "GATEWAY: gossip-expire-ready" in output and not unplugged:
                    unplugged = True
                    pending[0].clear()
                    pending[1].clear()
                    events["serial-unplugged"] = 1
                if "UART: PASS" in output:
                    if args.send:
                        before = (directory / "leaf.log").read_text()
                        after = (directory / "leaf-reboot.log").read_text()
                        required = ("uart-drop-send", "uart-duplicate-send", "uart-drop-send-completion",
                                    "uart-drop-send-ack", "uart-drop-send-retirement", "old-send-replayed")
                        admitted = [line for line in before.splitlines() if line.startswith("SEND-EXEC:")]
                        records = [dict(field.split("=", 1) for field in line.split()[1:]) for line in admitted]
                        if (len(send_keys) != 128 or before.count("SEND-EXEC:") != 128 or
                                "SEND-EXEC:" in after or
                                {int(r["index"]) for r in records} != set(range(128)) or
                                any(r["duplicates"] != "0" or r["bad"] != "0" or
                                    int(r["bytes"]) != (8, 64, 128, 206)[int(r["index"]) % 4]
                                    for r in records) or
                                "GATEWAY: send-pass" not in output or
                                "GATEWAY: send-old-session-pass" not in output or
                                not all(events.get(k) for k in required) or
                                send_retirements.get(saved.get("lost-send-retirement"), 0) < 2 or
                                not events.get("mcu-admission-busy")):
                            raise RuntimeError("SEND delivery/dedup/retirement recovery/reboot evidence incomplete")
                        events["send_admitted"] = 128
                        events["send_retirement_recovery"] = True
                        events["lost_send_retirement_txid"] = saved["lost-send-retirement"]
                        events["recovered_send_retirement_frames"] = send_retirements[saved["lost-send-retirement"]]
                    if args.gossip and "GATEWAY: gossip-expire-pass" not in output:
                        raise RuntimeError("automatic route lease did not expire")
                    executed = sum((directory / name).read_text().count("DOT-EXEC:")
                                   for name in ("leaf.log", "leaf-reboot.log"))
                    if (not rebooted or not replayed or executed != len(keys) or executed < 96
                            or "UART: mixed-word-tree-pass" not in output
                            or "MAP-EXEC:" not in (directory / "leaf-reboot.log").read_text()
                            or "GATEWAY: mtu-pass" not in output
                            or not all(events.get(k) for k in ("udp-0-drop-cs-1", "udp-0-drop-cs-2",
                                      "udp-1-drop-cs-1", "udp-1-drop-cs-2", "uart-path-mtu-256"))):
                        raise RuntimeError(f"fault/dedup/reboot evidence incomplete: {executed=}, {len(keys)=}")
                    maps = sum((directory / name).read_text().count("MAP-EXEC:")
                               for name in ("leaf.log", "leaf-reboot.log"))
                    admitted_maps = sum(int.from_bytes(key[:4], "little") in map_acks for key in map_keys)
                    if not maps or maps != admitted_maps or "uart-drop-map-reply" not in events:
                        raise RuntimeError("word map loss/dedup evidence missing")
                    if ("UART: post-job-cache-pass" not in output or
                            (args.abandon and ("UART: word-retries=0000000000000000" in output
                                               or events["uart-drop-map-reply"] < 2))):
                        raise RuntimeError("attempt abandonment/cache retirement evidence missing")
                    events.update({"dot_executions": executed, "unique_dot_transactions": len(keys),
                                   "map_executions": maps, "admitted_map_transactions": admitted_maps, "requested_map_transactions": len(map_keys)})
                    success = True
                    print("check-cluster-gateway: PASS (RISC-V -> UDP -> x86 COM2 -> static leaf, CS loss, reliable dedup, reboot/old replay, any/mixed tree" +
                          (", 128 SENDs and lost-retirement recovery" if args.send else "") +
                          (", automatic route/daemon recovery/lease expiry" if args.gossip else "") + ")")
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
                   "kernel/mcu/leaf.c", "kernel/cluster/reliable.c", "kernel/cluster/remote_ep.c", "kernel/cluster/route.c",
                   "kernel/include/uapi/a20/cluster_gossip.h", "kernel/include/uapi/a20/cluster_control.h", "kernel/include/uapi/a20/cluster_job.h",
                   "kernel/include/uapi/a20/cluster_words.h", "kernel/include/uapi/a20/cluster_word_job.h", "user/liba20rt/a20_cluster_job.h",
                   "user/svc/clusterd.c", "user/tests/test_native_cluster.c", "tools/mk/targets-dev.mk",
                   "tools/cluster/check-cluster-gateway.py", "tools/fixtures/clx_leaf_process.c"]
        events.update({"success": success, "elapsed_seconds": time.monotonic()-started,
                       "platform": platform.platform(), "scope": "two TCG kernels and host leaf, not physical hardware",
                       "source_sha256": {p: hashlib.sha256((root/p).read_bytes()).hexdigest() for p in sources}})
        (directory / "events.json").write_text(json.dumps(events, indent=2)+"\n")
        (directory / "trace.json").write_text(json.dumps(list(trace), indent=2)+"\n")


if __name__ == "__main__":
    main()
