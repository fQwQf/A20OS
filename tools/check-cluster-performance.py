#!/usr/bin/env python3
"""Measure the real Native fused RPC path; never substitute host echo code."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import platform
import shlex
import socket
import statistics
import subprocess
import time


def parse(text):
    rows = []
    for line in text.splitlines():
        if not line.startswith("CLUSTER_BENCH: mode="):
            continue
        fields = dict(item.split("=", 1) for item in line.split()[1:])
        row = {key: int(value) if key not in ("mode", "block_sums_ns") else value
               for key, value in fields.items()}
        blocks = [int(value) for value in row.pop("block_sums_ns").split(",")]
        if len(blocks) != 100 or sum(blocks) != row["sum_ns"]:
            raise RuntimeError("incomplete or inconsistent guest block samples")
        if (row["calls"] < 1000 or row["calls"] % 100 or
                not row["elapsed_ns"] or not row["min_ns"] or
                not row["min_ns"] <= row["p50_ns"] <= row["p95_ns"] <= row["p99_ns"] <= row["max_ns"]):
            raise RuntimeError("invalid guest measurement")
        means = [value / (row["calls"] // 100) for value in blocks]
        row["mean_ns"] = row["sum_ns"] / row["calls"]
        row["block_mean_ns"] = means
        row["block_mean_standard_error_ns"] = statistics.stdev(means) / math.sqrt(len(means))
        row["application_roundtrips_per_second"] = row["calls"] * 1e9 / row["elapsed_ns"]
        row["bidirectional_payload_MB_per_second"] = 2 * row["bytes"] * row["calls"] * 1000 / row["elapsed_ns"]
        rows.append(row)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--mode", choices=("local", "loopback", "udp", "all"), default="all")
    parser.add_argument("--calls", type=int, default=100000)
    parser.add_argument("--udp-calls", type=int, default=10000)
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--kernel", type=Path, help="reference kernel ELF; only with --mode local --skip-build")
    parser.add_argument("--log-dir", type=Path, default=Path(".kernel-build/cluster/performance"))
    args = parser.parse_args()
    if args.kernel and (args.mode != "local" or not args.skip_build):
        parser.error("a reference kernel requires --mode local --skip-build")
    for count in (args.calls, args.udp_calls):
        if count < 1000 or count > 100000 or count % 100:
            parser.error("call counts must be 1000..100000 and divisible by 100")
    root = Path(__file__).resolve().parent.parent
    directory = (root / args.log_dir).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    make = ["make", "--no-print-directory", "ARCH=riscv64", "ABI=both", "CLUSTER_PROFILE=3", "BRINGUP=0"]
    sources = ["user/tests/test_native_cluster_bench.c", "kernel/abi/native/sys_native_ipc.c",
               "kernel/ipc/a20_channel.c", "kernel/cluster/remote_ep.c", "kernel/cluster/export.c",
               "kernel/cluster/transport.c", "kernel/cluster/udp.c", "kernel/cluster/fragment.c",
               "kernel/drivers/net/virtio_net.c", "kernel/net/lwip_compat.c",
               "kernel/net/lwip_port/arch/cc.h", "kernel/net/lwip_port/arch/heap.h",
               "tools/check-cluster-performance.py"]
    hashes = {path: hashlib.sha256((root / path).read_bytes()).hexdigest() for path in sources}
    if not args.skip_build:
        with (directory / "build.log").open("w") as log:
            subprocess.run(make + ["-j8", "dev-build"], cwd=root, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
    base = shlex.split(subprocess.check_output(make + ["-s", "_qemu_argv"], cwd=root, text=True))
    if args.kernel:
        base[base.index("-kernel") + 1] = str(args.kernel.resolve(strict=True))
    kernel = (root / base[base.index("-kernel") + 1]).resolve()
    kernel_hash = hashlib.sha256(kernel.read_bytes()).hexdigest()
    for flag in ("-netdev", "-device"):
        for i in reversed(range(len(base) - 1)):
            if base[i] == flag and (flag == "-netdev" or base[i+1].startswith("virtio-net")):
                del base[i:i+2]
    modes = ["local", "loopback", "udp"] if args.mode == "all" else [args.mode]
    results, guest_commands = [], []
    started = time.monotonic()
    success = False
    try:
        for mode in modes:
            nodes = 2 if mode == "udp" else 1
            reservations, ports = [], []
            for _ in range(nodes):
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                sock.bind(("127.0.0.1", 0))
                reservations.append(sock)
                ports.append(sock.getsockname()[1])
            processes, logs = [], []
            try:
                for sock in reservations:
                    sock.close()
                for i in range(nodes):
                    cmd = list(base) + ["-snapshot"]
                    boot = f"a20.dhcp=0 a20.ip=10.0.3.{i+2} a20.netmask=255.255.255.0"
                    if "-append" in cmd:
                        cmd[cmd.index("-append") + 1] += " " + boot
                    else:
                        cmd += ["-append", boot]
                    if mode == "udp":
                        cmd += ["-netdev", f"socket,id=net,udp=127.0.0.1:{ports[1-i]},localaddr=127.0.0.1:{ports[i]}",
                                "-device", f"virtio-net-device,bus=virtio-mmio-bus.4,netdev=net,mac=52:54:00:12:34:0{i+1}"]
                    guest_commands.append(cmd)
                    log = (directory / f"{mode}-{i}.log").open("w")
                    logs.append(log)
                    processes.append(subprocess.Popen(cmd, cwd=root, stdin=subprocess.PIPE,
                                                     stdout=log, stderr=subprocess.STDOUT))
                sent = set()
                deadline = time.monotonic() + args.timeout
                while time.monotonic() < deadline:
                    outputs = [(directory / f"{mode}-{i}.log").read_text(errors="replace") for i in range(nodes)]
                    if any("CLUSTER_BENCH: FAIL" in text or "KERNEL PANIC" in text for text in outputs):
                        results.extend(parse(outputs[0]))
                        raise RuntimeError(f"guest failed in {mode}; inspect {directory}")
                    for i, text in enumerate(outputs):
                        if i in sent or "# " not in text:
                            continue
                        if mode == "udp" and i == 0 and "CLUSTER_BENCH: SERVER_READY" not in outputs[1]:
                            continue
                        role = mode if mode != "udp" else "udp-server" if i == 1 else "udp-client"
                        count = args.udp_calls if mode == "udp" else args.calls
                        processes[i].stdin.write(f"/bin/native-cluster-bench-rv {role} {count}\n".encode())
                        processes[i].stdin.flush()
                        sent.add(i)
                    if "CLUSTER_BENCH: PASS" in outputs[0]:
                        rows = parse(outputs[0])
                        expected = [("udp-client", 64, args.udp_calls), ("udp-client", 65536, 1000)] if mode == "udp" else [(mode, 64, args.calls)]
                        if [(r["mode"], r["bytes"], r["calls"]) for r in rows] != expected:
                            raise RuntimeError("guest skipped a required measurement")
                        executed = 0
                        for row in rows:
                            executed += row["calls"] + 100
                            if row["executed"] != executed:
                                raise RuntimeError("service execution count disagrees with calls")
                        results.extend(rows)
                        break
                    if any(process.poll() is not None for process in processes):
                        raise RuntimeError(f"QEMU exited in {mode}")
                    time.sleep(0.05)
                else:
                    raise TimeoutError(f"performance run timed out in {mode}; inspect {directory}")
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
                for sock in reservations:
                    sock.close()
        success = True
        for row in results:
            print(f'{row["mode"]} {row["bytes"]} B: mean {row["mean_ns"]/1000:.3f} us, '
                  f'p50 {row["p50_ns"]/1000:.3f} us, p99 {row["p99_ns"]/1000:.3f} us, '
                  f'{row["bidirectional_payload_MB_per_second"]:.3f} MB/s bidirectional payload')
        print("check-cluster-performance: measurement complete (targets reported separately)")
    finally:
        evidence = {"success": success, "measurements": results, "source_sha256": hashes,
                    "started_at_utc": datetime.fromtimestamp(time.time() - (time.monotonic()-started), timezone.utc).isoformat(),
                    "worktree_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
                    "worktree_dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root)),
                    "kernel_elf": str(kernel), "kernel_sha256": kernel_hash,
                    "guest_commands": guest_commands, "elapsed_seconds": time.monotonic()-started,
                    "platform": platform.platform(), "scope": "RISC-V Native fused channel_call in QEMU TCG; no physical throughput claim",
                    "qemu_version": subprocess.check_output(["qemu-system-riscv64", "--version"], text=True).splitlines()[0]}
        (directory / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")


if __name__ == "__main__":
    main()
