#!/usr/bin/env python3
"""Three real kernels (RV64, AArch64, x86): application launch, variant admission and lifecycle."""
import argparse
import json
from pathlib import Path
import select
import shlex
import socket
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--arches", default="riscv64,aarch64,x86_64")
    parser.add_argument("--timeout", type=float, default=360)
    parser.add_argument("--log-dir", type=Path, default=Path(".kernel-build/cluster/apps"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    directory = (root / args.log_dir).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "events.json").write_text(json.dumps({"passed": False, "phase": "build"})+"\n")
    arches = tuple(args.arches.split(","))
    if len(arches)!=3 or any(a not in ("riscv64","aarch64","x86_64") for a in arches):
        parser.error("--arches needs three supported architectures")
    tags = tuple("rv" if a=="riscv64" else a for a in arches)
    bases = []
    for arch in dict.fromkeys(arches):
        make = ["make", "--no-print-directory", "CCACHE=", f"ARCH={arch}", "ABI=both", "BRINGUP=0", "QEMU_ACCEL=tcg,thread=multi"]
        if not args.skip_build:
            with (directory / f"build-{arch}.log").open("w") as log:
                subprocess.run(make + ["-j12", "dev-build"], cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
        cmd = shlex.split(subprocess.check_output(make + ["-s", "_qemu_argv"], cwd=root, text=True))
        for i in reversed(range(len(cmd)-1)):
            if cmd[i] == "-netdev" or (cmd[i] == "-device" and cmd[i+1].startswith("virtio-net")):
                del cmd[i:i+2]
        # QEMU's AArch64 ELF loader does not supply the Linux x0 DTB handoff.
        # Boot the same linked image's raw bytes so /chosen/bootargs reaches A20.
        if arch == "aarch64":
            i = cmd.index("-kernel")+1
            cmd[i] = str(Path(cmd[i]).with_suffix(".bin"))
        bases.append((arch,cmd))
    reserve, lanes, ports = [], [], []
    for _ in arches:
        a, b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM), socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        a.bind(("127.0.0.1", 0)); b.bind(("127.0.0.1", 0))
        reserve.append(a); lanes.append(b); ports.append(a.getsockname()[1])
    processes, logs, commands, outputs, pending = [None]*3, [], [], [None]*3, {}
    trace = []
    events = {"passed": False, "arches": arches, "cd2": 0, "cm2": 0, "cd1": 0, "cm1": 0, "ca1": 0, "ca2": 0, "dropped_release_replies": 0, "dropped_reusable_start_replies": 0, "recycle_domains": 0, "real_workloads": [], "real_argument_workloads": [], "cli_exits": 0, "cli_cancel_exits": 0, "dropped_stop_requests": 0, "dropped_stop_replies": 0, "dropped_start_replies": 0, "reboots": []}
    macs = [bytes.fromhex(f"52540012340{i+1}") for i in range(3)]

    def stop(i):
        p = processes[i]
        if p and p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill(); p.wait()

    def start(i, mode):
        stop(i)
        path = directory / f"node-{i}-mode-{mode}.log"
        log = path.open("w"); logs.append(log); outputs[i] = path
        processes[i] = subprocess.Popen(commands[i], cwd=root, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
        pending[i] = (time.monotonic()+args.timeout, mode)
        events["reboots"].append([i, mode])

    try:
        for sock in reserve:
            sock.close()
        for i, arch in enumerate(arches):
            cmd = list(dict(bases)[arch]) + ["-snapshot"]
            boot = f"a20.dhcp=0 a20.ip=10.0.3.{i+2} a20.netmask=255.255.255.0"
            if arch == "x86_64":
                cmd += ["-fw_cfg", f"name=opt/x86/cmdline,string={boot}"]
            elif "-append" in cmd:
                cmd[cmd.index("-append")+1] += " " + boot
            else:
                cmd += ["-append", boot]
            device = "virtio-net-pci" if arch == "x86_64" else "virtio-net-device,bus=virtio-mmio-bus.4"
            cmd += ["-netdev", f"socket,id=net,udp=127.0.0.1:{lanes[i].getsockname()[1]},localaddr=127.0.0.1:{ports[i]}", "-device", f"{device},netdev=net,mac={macs[i].hex(':')}"]
            commands.append(cmd)
            start(i, 0)
        (directory / "commands.json").write_text(json.dumps(commands, indent=2)+"\n")
        bound = False
        begin = time.monotonic()
        while time.monotonic()-begin < args.timeout:
            readers, _, _ = select.select(lanes, [], [], 0.02)
            drop = "APPS: timeout-pass" not in outputs[0].read_text(errors="replace")
            for sock in readers:
                packet, _ = sock.recvfrom(65536)
                source = lanes.index(sock)
                # Count actual protocol versions, independent of guest assertions.
                if len(packet) >= 42 and packet[12:14] == b"\x08\x00" and packet[23] == 17:
                    cl = packet[22+(packet[14] & 15)*4:]
                    if cl[:2] == b"CL":
                        if len(trace)<20000:
                            trace.append([round(time.monotonic()-begin,3),source,packet[30:34].hex(),cl.hex()])
                        for proto in (b"CD\x01", b"CD\x02", b"CM\x01", b"CM\x02", b"CA\x01", b"CA\x02"):
                            if proto in cl[32:60]:
                                events[proto[:2].decode().lower()+str(proto[2])] += 1
                ca = cl.find(b"CA\x01\x02",32,60) if len(packet)>=42 and packet[12:14]==b"\x08\x00" and packet[23]==17 else -1
                if source==1 and drop and ca>=0 and int.from_bytes(cl[ca+16:ca+24],"little")==99 and len(cl)>=ca+192:
                    events["dropped_start_replies"] += 1
                    continue
                reusable_ca = cl.find(b"CA\x02\x02",32,60) if len(packet)>=42 and packet[12:14]==b"\x08\x00" and packet[23]==17 else -1
                if source==1 and reusable_ca>=0 and len(cl)>=reusable_ca+192 and int.from_bytes(cl[reusable_ca+16:reusable_ca+24],"little")==1300 and "APPS: reusable-start-timeout-pass" not in outputs[0].read_text(errors="replace"):
                    events["dropped_reusable_start_replies"]+=1
                    continue
                stop_ca = cl.find(b"CA\x02\x04",32,60) if len(packet)>=42 and packet[12:14]==b"\x08\x00" and packet[23]==17 else -1
                if stop_ca>=0 and len(cl)>=stop_ca+176 and int.from_bytes(cl[stop_ca+16:stop_ca+24],"little")==701:
                    if source==0 and "APPCTL: stop retry original owner" not in outputs[0].read_text(errors="replace"):
                        events["dropped_stop_requests"]+=1
                        continue
                    if source==1 and len(cl)>=stop_ca+192:
                        events["dropped_stop_replies"]+=1
                        continue
                release_ca = cl.find(b"CA\x02\x05",32,60) if len(packet)>=42 and packet[12:14]==b"\x08\x00" and packet[23]==17 else -1
                if source==1 and release_ca>=0 and len(cl)>=release_ca+192 and int.from_bytes(cl[release_ca+16:release_ca+24],"little")==1200:
                    events["dropped_release_replies"]+=1
                    continue
                targets = [macs.index(packet[:6])] if packet[:6] in macs else range(3)
                for target in targets:
                    if target != source:
                        lanes[target].sendto(packet, ("127.0.0.1", ports[target]))
            for i, (when, mode) in list(pending.items()):
                if "\n# " in outputs[i].read_text(errors="replace"):
                    processes[i].stdin.write(f"/bin/native-apps-{tags[i]} {i} {mode}\n".encode())
                    processes[i].stdin.flush(); del pending[i]
                elif time.monotonic() >= when:
                    raise TimeoutError(f"guest {i} did not reach shell")
            out = outputs[0].read_text(errors="replace")
            for i, path in enumerate(outputs):
                if path:
                    guest = path.read_text(errors="replace")
                    if "KERNEL PANIC" in guest or "APPS: FAIL" in guest:
                        raise RuntimeError(f"guest {i} failed; see {path}")
            if "APPS: reboot-ready" in out and not bound:
                start(1, 1); bound = True
            if "APPS: PASS" in out:
                if not (bound and events["dropped_start_replies"] and all(events[k] for k in ("cd1", "cd2", "ca1"))):
                    raise RuntimeError(f"missing exercised phases: {events}")
                events["real_workloads"]=[(directory / f"node-{i}-mode-0.log").read_text(errors="replace").count("APP-WORK: PASS sum=500500") for i in range(3)]
                if "APPS: balance-pass real-programs=6 active=2,2,2" not in out or any(n<4 for n in events["real_workloads"]):
                    raise RuntimeError(f"missing actual balanced workloads: {events}")
                all_logs=[(directory / f"node-{i}-mode-0.log").read_text(errors="replace") for i in range(3)]
                all_logs[1]+=(directory / "node-1-mode-1.log").read_text(errors="replace")
                events["real_argument_workloads"]=[s.count("APP-ARGS: PASS argc=4 envc=2") for s in all_logs]
                events["cli_exits"]=out.count("APPCTL: exit=00000025")
                if "APPS: cli-pass real-args-env exit=37 nodes=3" not in out or events["real_argument_workloads"]!=[10,1,1] or events["cli_exits"]!=12:
                    raise RuntimeError(f"missing actual configured CLI workloads: {events}")
                events["cli_cancel_exits"]=out.count("APPCTL: exit=00000089")
                if "APPS: cli-cancel-pass remote-exit=137 active=0" not in out or events["cli_cancel_exits"]!=1 or not events["dropped_stop_requests"] or not events["dropped_stop_replies"]:
                    raise RuntimeError(f"missing actual remote CLI cancellation: {events}")
                events["recycle_domains"]=out.count("APPS: recycle-pass real-exits=16 live-preserved old-start=expired slots=8")
                if events["recycle_domains"]!=3 or not events["dropped_reusable_start_replies"] or "APPS: reusable-start-timeout-pass original-root=live" not in out or not events["ca2"] or not events["dropped_release_replies"] or "APPS: release-timeout-pass old-start=expired" not in out or "APPS: cli-reuse-pass runs=9 no-daemon-restart" not in out:
                    raise RuntimeError(f"missing real reusable application lifecycle: {events}")
                events["passed"] = True
                print(f"check-cluster-apps: PASS ({','.join(arches)}, variant launch, dedup/lost reply, exit/stop, daemon restart and kernel reboot)")
                return
            for i, p in enumerate(processes):
                if p.poll() is not None :
                    raise RuntimeError(f"QEMU {i} exited unexpectedly")
        raise TimeoutError(f"application test timeout; logs: {directory}")
    except Exception as error:
        events["error"] = str(error)
        raise
    finally:
        (directory / "events.json").write_text(json.dumps(events, indent=2)+"\n")
        (directory / "wire.json").write_text(json.dumps(trace)+"\n")
        for i in range(3):
            stop(i)
        for log in logs:
            log.close()
        for sock in lanes:
            sock.close()


if __name__ == "__main__":
    main()
