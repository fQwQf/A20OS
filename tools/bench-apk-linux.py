#!/usr/bin/env python3
"""Compare identical Alpine executables on host Linux and A20OS under KVM.

Build first with tools/a20 build benchmark-x86_64. See the accompanying
measurement report for the protocol and interpretation limits.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shlex
import statistics
import subprocess
import time

REPO = Path(__file__).resolve().parents[1]


def cases(seconds):
    sb = [f"--time={seconds}", "--events=0", "--rand-seed=1"]
    return {
        "cpu-1": (["sysbench", "cpu", *sb, "--threads=1", "--cpu-max-prime=20000", "run"], "events/s"),
        "cpu-2": (["sysbench", "cpu", *sb, "--threads=2", "--cpu-max-prime=20000", "run"], "events/s"),
        "memory-write": (["sysbench", "memory", *sb, "--threads=1", "--memory-block-size=1M", "--memory-total-size=100T", "--memory-oper=write", "run"], "MiB/s"),
        "threads-2": (["sysbench", "threads", *sb, "--threads=2", "--thread-yields=100", "--thread-locks=1", "run"], "events/s"),
        "pipe": (["stress-ng", "--pipe", "1", "--timeout", f"{seconds}s", "--metrics-brief"], "bogo ops/s"),
        # Default switch uses pipe2(O_DIRECT), which A20OS rejects and stress-ng
        # silently replaces with a byte-stream pipe. Select equal semantics.
        "switch": (["stress-ng", "--switch", "1", "--switch-method", "sem-sysv", "--timeout", f"{seconds}s", "--metrics-brief"], "bogo ops/s"),
        "fork": (["stress-ng", "--fork", "1", "--timeout", f"{seconds}s", "--metrics-brief"], "bogo ops/s"),
        "sha256": (["openssl", "speed", "-elapsed", "-seconds", str(seconds), "-bytes", "16384", "-evp", "sha256", "-mr"], "MB/s"),
    }


def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def command(argv):
    return subprocess.check_output(argv, cwd=REPO, text=True).strip()


class Guest:
    def __init__(self, argv, logfile,
                 ready_pattern=rb"A20_BENCH_READY\r?\n", ready_timeout=60):
        self.log = logfile.open("wb")
        self.p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, cwd=REPO)
        self.buffer = b""
        if ready_pattern is None:
            return
        try:
            self.until(ready_pattern, ready_timeout)
        except BaseException:
            self.close()
            raise

    def until(self, pattern, timeout):
        deadline = time.monotonic() + timeout
        while True:
            match = re.search(pattern, self.buffer)
            if match:
                result = self.buffer[:match.end()]
                self.buffer = self.buffer[match.end():]
                return result, match
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("guest serial deadline expired")
            if not select.select([self.p.stdout], [], [], min(remaining, 0.5))[0]:
                continue
            data = os.read(self.p.stdout.fileno(), 65536)
            if not data:
                raise RuntimeError(f"QEMU exited: {self.p.poll()}")
            self.log.write(data)
            self.log.flush()
            self.buffer += data

    def result(self, token, timeout):
        marker = f"APK_BENCH_END_{token}"
        self.until(rb"(?:^|\n)APK_BENCH_BEGIN_" + token.encode() + rb"\r?\n", 10)
        started = time.monotonic()
        raw, match = self.until(rb"(?:^|\n)" + marker.encode() + rb" (\d+)\r?\n", timeout)
        return raw.decode(errors="replace"), int(match.group(1)), time.monotonic() - started

    def close(self):
        self.p.terminate()
        try:
            self.p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
        self.log.close()


def metric(name, output, seconds):
    if name.startswith("cpu"):
        m = re.search(r"events per second:\s*([\d.]+)", output)
    elif name == "threads-2":
        count = re.search(r"total number of events:\s*(\d+)", output)
        elapsed = re.search(r"total time:\s*([\d.]+)s", output)
        return float(count[1]) / float(elapsed[1]) if count and elapsed else None
    elif name == "memory-write":
        m = re.search(r"\(([\d.]+) MiB/sec\)", output)
    elif name == "sha256":
        m = re.search(r"^\+F:\d+:[^:]+:([\d.]+)", output, re.M)
        return float(m[1]) / 1e6 if m else None
    else:
        if "successful run completed" not in output or not re.search(r"failed:\s*0", output):
            return None
        if re.search(r"(?:skipped|metrics untrustworthy):\s*[1-9]", output):
            return None
        m = re.search(r"\]\s+" + name + r"\s+\d+\s+[\d.]+\s+[\d.]+\s+[\d.]+\s+([\d.]+)", output)
    return float(m[1]) if m else None


COUNTERS_OFF = "echo 0 > /proc/a20/perf || exit 1"


def counter_audit_script(token):
    # Outside the workload's timing window. The first header records state
    # before the read arms diagnostics; old kernels have no such header.
    return [f"echo APK_COUNTER_STATE_{token}",
            "/bin/busybox head -n 1 /proc/a20/perf || exit 1", COUNTERS_OFF,
            f"echo APK_COUNTER_STATE_END_{token}"]


def read_counter_audit(guest, token):
    data, _ = guest.until(rb"(?:^|\n)APK_COUNTER_STATE_END_" + token.encode() + rb"\r?\n", 15)
    output = data.decode(errors="replace")
    found = re.search(r"^collection_enabled_before_snapshot: ([01])\s*$", output, re.M)
    return {"collection_enabled_after_case": int(found[1]) if found else None,
            "counter_audit_output": output}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True, help="new evidence directory")
    ap.add_argument("--host-root", type=Path, default=REPO / "build/benchmark-host-root")
    ap.add_argument("--build-dir", type=Path, required=True)
    ap.add_argument("--image", type=Path, default=REPO / "build/images/benchmark-x86_64.img")
    ap.add_argument("--cpus", default="4,6", help="same host CPU set for Linux and all QEMU threads")
    ap.add_argument("--seconds", type=int, default=5)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--timeout", type=int, default=30)
    # The optional pipe diagnostic has the same O_DIRECT fallback discrepancy
    # as default switch; do not put it in the comparable default suite.
    ap.add_argument("--cases", default=",".join(k for k in cases(1) if k != "pipe"))
    args = ap.parse_args()
    if args.seconds < 1 or args.runs < 1:
        ap.error("seconds and runs must be positive")
    chosen = {k: cases(args.seconds)[k] for k in args.cases.split(",")}
    root, bd, image = args.host_root.resolve(), args.build_dir.resolve(), args.image.resolve()
    loader = root / "lib/ld-musl-x86_64.so.1"
    kernel, fat = bd / "kernel.elf", bd / "fat32.img"
    for path in (loader, kernel, fat, image):
        if not path.is_file():
            ap.error(f"missing input: {path}")
    args.out.mkdir(parents=True, exist_ok=False)
    out = args.out.resolve()
    env = dict(os.environ, LC_ALL="C", OPENSSL_CONF="/dev/null", OPENSSL_ia32cap="0")
    # No global package installation or privileged chroot is needed on the host.
    host_prefix = ["taskset", "-c", args.cpus, str(loader), "--library-path", f"{root}/lib:{root}/usr/lib"]
    qemu = ["taskset", "-c", args.cpus, "qemu-system-x86_64", "-machine", "q35",
            "-accel", "kvm", "-cpu", "host", "-no-reboot", "-m", "2G", "-smp", "2",
            "-display", "none", "-monitor", "none", "-serial", "stdio", "-snapshot",
            "-kernel", str(kernel), "-drive", f"file={fat},if=none,format=raw,id=x0",
            "-device", "virtio-blk-pci,drive=x0", "-drive", f"file={image},if=none,format=raw,id=xworld",
            "-device", "virtio-blk-pci,drive=xworld", "-netdev", "user,id=net",
            "-device", "virtio-net-pci,netdev=net"]
    bins = ["usr/bin/sysbench", "usr/bin/stress-ng", "usr/bin/openssl"]
    bins += sorted(str(p.relative_to(root)) for directory in (root / "lib", root / "usr/lib")
                   for p in directory.glob("*.so*") if p.is_file() and not p.is_symlink())
    hashes = {"/" + x: sha(root / x) for x in bins}
    manifest = {
        "created": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "git_head": command(["git", "rev-parse", "HEAD"]),
        "git_status": command(["git", "status", "--short"]), "uname": command(["uname", "-a"]),
        "lscpu": command(["lscpu"]), "clocksource": Path("/sys/devices/system/clocksource/clocksource0/current_clocksource").read_text().strip(),
        "governor": Path(f"/sys/devices/system/cpu/cpu{args.cpus.split(',')[0]}/cpufreq/scaling_governor").read_text().strip(),
        "qemu_version": command(["qemu-system-x86_64", "--version"]), "qemu_argv": qemu,
        "host_prefix": host_prefix, "cpu_affinity": args.cpus, "seconds": args.seconds, "runs": args.runs,
        "openssl_ia32cap": "0", "binary_sha256": hashes,
        "input_sha256": {str(p): sha(p) for p in (kernel, fat, image)},
        "runner_sha256": sha(__file__), "cases": chosen, "samples": [], "order": [],
        "counter_policy": "disable_before_each_case_and_audit_after",
    }

    def save():
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    save()
    guest_boot = 0

    def boot_guest(run, index):
        nonlocal guest_boot
        guest_boot += 1
        private_image = out / f"guest-{guest_boot}.img"
        subprocess.run(["cp", "--reflink=auto", "--sparse=always", str(image), str(private_image)], check=True)
        lines = ["#!/bin/sh", "export LC_ALL=C OPENSSL_CONF=/dev/null OPENSSL_ia32cap=0",
                 "uname -a", "sha256sum " + " ".join(hashes), "echo APK_BENCH_INFO_DONE"]
        for offset, (case, (argv, _)) in enumerate(list(chosen.items())[index:], index):
            token = f"r{run}_{offset}"
            lines += [COUNTERS_OFF, f"echo APK_BENCH_BEGIN_{token}", shlex.join(argv),
                      f"r=$?; echo APK_BENCH_END_{token} $r"]
            lines += counter_audit_script(token)
        lines += ["echo APK_BENCH_SUITE_DONE"]
        script = out / f"guest-{guest_boot}.sh"
        script.write_text("\n".join(lines) + "\n")
        debugfs = "/usr/sbin/debugfs"
        subprocess.run([debugfs, "-w", "-R", f"write {script} /etc/a20-benchmark.sh", str(private_image)],
                       check=True, capture_output=True)
        installed = subprocess.check_output([debugfs, "-R", "cat /etc/a20-benchmark.sh", str(private_image)], stderr=subprocess.DEVNULL)
        if installed.decode() != script.read_text():
            raise RuntimeError("benchmark script was not installed into private image")
        argv = [a.replace(str(image), str(private_image)) for a in qemu]
        manifest.setdefault("guest_boots", []).append({"argv": argv, "script_sha256": sha(script), "image_sha256": sha(private_image)})
        save()
        guest = Guest(argv, out / f"{run}-qemu-{index}.log")
        try:
            info, _ = guest.until(rb"(?:^|\n)APK_BENCH_INFO_DONE\r?\n", 15)
            info = info.decode(errors="replace")
            (out / f"{run}-guest-info-{index}.log").write_text(info)
            if any(f"{h}  {p}" not in info for p, h in hashes.items()):
                raise RuntimeError("guest/host executable hash mismatch")
        except BaseException:
            guest.close()
            raise
        return guest

    for run in range(1, args.runs + 1):
        order = ["linux", "a20"] if run % 2 else ["a20", "linux"]
        for side in order:
            manifest["order"].append([run, side])
            guest = None
            try:
                for index, (name, (argv, unit)) in enumerate(chosen.items()):
                    token = f"r{run}_{index}"
                    logfile = out / f"{run}-{side}-{name}.log"
                    row = {"run": run, "side": side, "case": name, "unit": unit, "argv": argv}
                    try:
                        if side == "linux":
                            started = time.monotonic()
                            proc = subprocess.run(host_prefix + [str(root / "usr/bin" / argv[0]), *argv[1:]],
                                                  env=env, capture_output=True, text=True, timeout=args.timeout)
                            output, rc, wall = proc.stdout + proc.stderr, proc.returncode, time.monotonic() - started
                        else:
                            if guest is None:
                                guest = boot_guest(run, index)
                            output, rc, wall = guest.result(token, args.timeout)
                            row.update(read_counter_audit(guest, token))
                        logfile.write_text(output)
                        value = metric(name, output, args.seconds)
                        row.update(exit_code=rc, wall_seconds=wall, value=value,
                                   status="ok" if rc == 0 and value is not None and value > 0 else "failed")
                        # Check the guest's own elapsed clock against the Linux observer.
                        elapsed = re.search(r"total time:\s*([\d.]+)s", output)
                        if elapsed:
                            row["reported_seconds"] = float(elapsed[1])
                            if abs(wall - float(elapsed[1])) > max(1, args.seconds * 0.25):
                                row.update(status="invalid-clock", value=None)
                    except (TimeoutError, subprocess.TimeoutExpired, RuntimeError) as exc:
                        row.update(status="timeout" if isinstance(exc, (TimeoutError, subprocess.TimeoutExpired)) else "failed",
                                   error=str(exc), value=None)
                        if guest:
                            guest.close()
                            guest = None
                    manifest["samples"].append(row)
                    save()
                    print(f"run={run} {side} {name}: {row['status']} {row.get('value')} {unit}", flush=True)
            finally:
                if guest:
                    guest.close()
    summary = {}
    for name, (_, unit) in chosen.items():
        summary[name] = {"unit": unit, "comparable": name != "pipe"}
        if name == "pipe":
            summary[name]["reason"] = "Linux uses packet-mode pipe2(O_DIRECT); A20OS falls back to a stream pipe"
        for side in ("linux", "a20"):
            rows = [r for r in manifest["samples"] if r["case"] == name and r["side"] == side]
            values = [r["value"] for r in rows if r["status"] == "ok"]
            summary[name][side] = {"values": values, "complete": len(values) == args.runs,
                                   "median": statistics.median(values) if values else None,
                                   "min": min(values) if values else None, "max": max(values) if values else None}
        if summary[name]["comparable"] and all(summary[name][s]["complete"] for s in ("linux", "a20")):
            summary[name]["a20_over_linux"] = summary[name]["a20"]["median"] / summary[name]["linux"]["median"]
    manifest["summary"] = summary
    manifest["inputs_unchanged"] = all(sha(p) == h for p, h in manifest["input_sha256"].items())
    save()
    with (out / "samples.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["run", "side", "case", "status", "value", "unit", "wall_seconds"], extrasaction="ignore")
        writer.writeheader()
        writer.writerows(manifest["samples"])
    print(json.dumps(summary, indent=2), flush=True)
    return 0 if manifest["inputs_unchanged"] and all(r["status"] == "ok" for r in manifest["samples"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
