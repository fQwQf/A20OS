#!/usr/bin/env python3
"""Run controlled workload variants and collect A20OS perf/lock snapshots.

The input config is a JSON list of {name, argv, [host_argv]} workloads.
Use a separate output directory for every experiment; no base image is changed.
"""
from __future__ import annotations
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("apk_bench", ROOT / "tests/tools/bench/bench-apk-linux.py")
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


def counters(text):
    return {m[1]: int(m[2]) for m in re.finditer(r"^([a-zA-Z0-9_]+): (\d+)\s*$", text, re.M)}


def locks(text):
    result = {}
    for m in re.finditer(r"^(\w+): (\d+) (\d+) max=(\d+)\s*$", text, re.M):
        row = result.setdefault(m[1], {"contended": 0, "spins": 0, "max": 0})
        row["contended"] += int(m[2])
        row["spins"] += int(m[3])
        row["max"] = max(row["max"], int(m[4]))
    return result


def host_utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="milliseconds")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--config", type=Path, required=True)
    ap.add_argument("--baseline", type=Path, default=ROOT / "build/evidence/apk-bench-2026-10-22-final/manifest.json")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--timeout", type=int, default=40)
    ap.add_argument("--kernel", type=Path)
    ap.add_argument("--install", action="append", default=[], help="hostfile:/guest/path")
    ap.add_argument("--replace-install", action="append", default=[],
                    help="guest path to remove from the private image before installing it")
    ap.add_argument("--no-counters", action="store_true")
    ap.add_argument("--preserve-counter-state", action="store_true",
                    help="with --no-counters, leave state untouched for lifecycle diagnostics")
    args = ap.parse_args()
    if args.preserve_counter_state and not args.no_counters:
        ap.error("--preserve-counter-state requires --no-counters")
    install_paths = {item.split(":", 1)[1] for item in args.install if ":" in item}
    if any(path not in install_paths for path in args.replace_install):
        ap.error("--replace-install must name a path supplied by --install")
    config = json.loads(args.config.read_text())
    source = json.loads(args.baseline.read_text())
    args.out.mkdir(parents=True, exist_ok=False)
    out = args.out.resolve()
    (out / "config.json").write_text(json.dumps(config, indent=2) + "\n")
    qemu = source["qemu_argv"]
    if args.kernel:
        qemu[qemu.index("-kernel") + 1] = str(args.kernel.resolve())
    base = next(Path(a.split("file=", 1)[1].split(",", 1)[0]) for a in qemu if "id=xworld" in a)
    root = ROOT / "build/benchmark-host-root"
    env = dict(os.environ, LC_ALL="C", OPENSSL_CONF="/dev/null", OPENSSL_ia32cap="0")
    manifest = {"created": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "baseline": str(args.baseline),
                "config": config, "runner_sha256": bench.sha(__file__), "samples": [], "boots": [],
                "kernel_sha256": bench.sha(qemu[qemu.index("-kernel") + 1]), "base_image_sha256": bench.sha(base),
                "counters_enabled": not args.no_counters,
                "replace_install": args.replace_install,
                "counter_policy": ("preserve" if args.preserve_counter_state else
                                   "disable_before_each_case_and_audit_after" if args.no_counters else
                                   "diagnostic_snapshots")}
    (out / "runner.py.txt").write_text(Path(__file__).read_text())

    def save():
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    for run in range(1, args.runs + 1):
        for side in (["linux", "a20"] if run % 2 else ["a20", "linux"]):
            if side == "linux":
                for case in config:
                    argv = case.get("host_argv") or source["host_prefix"] + [str(root / "usr/bin" / case["argv"][0]), *case["argv"][1:]]
                    started_utc = host_utc_now()
                    started_ns = time.monotonic_ns()
                    start = time.monotonic()
                    proc = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=args.timeout)
                    wall_seconds = time.monotonic() - start
                    finished_ns = time.monotonic_ns()
                    finished_utc = host_utc_now()
                    output = proc.stdout + proc.stderr
                    (out / f"{run}-linux-{case['name']}.log").write_text(output)
                    row = {"run": run, "side": side, "case": case["name"], "exit_code": proc.returncode,
                           "wall_seconds": wall_seconds, "output": output, "argv": argv,
                           "host_started_utc": started_utc, "host_finished_utc": finished_utc,
                           "host_started_monotonic_ns": started_ns,
                           "host_finished_monotonic_ns": finished_ns,
                           "host_timestamp_semantics": "subprocess_roundtrip"}
                    manifest["samples"].append(row)
                    save()
                    print(f"{run} linux {case['name']}: exit={proc.returncode}", flush=True)
                continue
            private = out / f"guest-{run}.img"
            subprocess.run(["cp", "--reflink=auto", "--sparse=always", str(base), str(private)], check=True)
            script = ["#!/bin/sh", "export LC_ALL=C OPENSSL_CONF=/dev/null OPENSSL_ia32cap=0", "uname -a",
                      "sha256sum /usr/bin/sysbench /usr/bin/stress-ng /lib/ld-musl-x86_64.so.1"]
            for i, case in enumerate(config):
                token = f"{run}_{i}"
                if not args.no_counters:
                    script += ["echo stop > /proc/a20/perf", "echo start > /proc/a20/perf", "echo reset > /proc/a20/lock_contention",
                               f"echo DIAG_PRE_{token}", "cat /proc/a20/perf", "cat /proc/stat",
                               f"echo DIAG_PRE_END_{token}"]
                elif not args.preserve_counter_state:
                    script += [bench.COUNTERS_OFF]
                script += [f"echo APK_BENCH_BEGIN_{token}", shlex.join(case["argv"]),
                           f"r=$?; echo APK_BENCH_END_{token} $r"]
                if not args.no_counters:
                    script += ["echo stop > /proc/a20/perf", f"echo DIAG_POST_{token}", "cat /proc/a20/perf", "cat /proc/stat",
                               f"echo DIAG_LOCKS_{token}", "cat /proc/a20/lock_contention",
                               "echo 0 > /proc/a20/perf", f"echo DIAG_POST_END_{token}"]
                elif not args.preserve_counter_state:
                    script += bench.counter_audit_script(token)
            script += ["echo DIAG_SUITE_DONE"]
            script_path = out / f"guest-{run}.sh"
            script_path.write_text("\n".join(script) + "\n")
            installs = [(script_path, "/etc/a20-benchmark.sh")]
            for install in args.install:
                src, dst = install.split(":", 1)
                installs.append((Path(src).resolve(), dst))
            for dst in args.replace_install:
                # Replacement is deliberately scoped to this run's private
                # image.  debugfs write cannot replace an existing inode and
                # paths such as /bin/echo may be BusyBox symlinks in the base.
                subprocess.run(["/usr/sbin/debugfs", "-w", "-R", f"rm {dst}",
                                str(private)], check=True, capture_output=True)
            for src, dst in installs:
                subprocess.run(["/usr/sbin/debugfs", "-w", "-R", f"write {src} {dst}", str(private)],
                               check=True, capture_output=True)
                actual = subprocess.check_output(["/usr/sbin/debugfs", "-R", f"cat {dst}", str(private)], stderr=subprocess.DEVNULL)
                if hashlib.sha256(actual).hexdigest() != bench.sha(src):
                    raise RuntimeError(f"installed image file mismatch: {dst}")
            argv = [s.replace(str(base), str(private)) for s in qemu]
            manifest["boots"].append({"run": run, "argv": argv, "image_sha256": bench.sha(private),
                                       "installed": {dst: bench.sha(src) for src, dst in installs}})
            save()
            guest = bench.Guest(argv, out / f"{run}-qemu.log")
            try:
                for i, case in enumerate(config):
                    token = f"{run}_{i}"
                    if not args.no_counters:
                        pre, _ = guest.until(f"\nDIAG_PRE_END_{token}\\r?\\n".encode(), 20)
                        pre = pre.decode(errors="replace")
                    started_utc = host_utc_now()
                    started_ns = time.monotonic_ns()
                    output, rc, wall = guest.result(token, args.timeout)
                    finished_ns = time.monotonic_ns()
                    finished_utc = host_utc_now()
                    row = {"run": run, "side": side, "case": case["name"], "exit_code": rc,
                           "wall_seconds": wall, "output": output, "argv": case["argv"],
                           "host_started_utc": started_utc, "host_finished_utc": finished_utc,
                           "host_started_monotonic_ns": started_ns,
                           "host_finished_monotonic_ns": finished_ns,
                           "host_timestamp_semantics": "collection_roundtrip_not_command_boundary"}
                    if not args.no_counters:
                        post, _ = guest.until(f"\nDIAG_POST_END_{token}\\r?\\n".encode(), 20)
                        post = post.decode(errors="replace")
                        before, after = counters(pre), counters(post.split("DIAG_LOCKS_")[0])
                        row.update(perf_before=before, perf_after=after,
                                   perf_delta={k: v - before.get(k, 0) for k, v in after.items()},
                                   locks=locks(post), pre_output=pre, post_output=post)
                    elif not args.preserve_counter_state:
                        row.update(bench.read_counter_audit(guest, token))
                    (out / f"{run}-a20-{case['name']}.log").write_text(output)
                    manifest["samples"].append(row)
                    save()
                    print(f"{run} a20 {case['name']}: exit={rc} wall={wall:.3f}", flush=True)
                guest.until(rb"(?:^|\n)DIAG_SUITE_DONE\r?\n", 10)
            finally:
                guest.close()
    manifest["base_unchanged"] = bench.sha(base) == manifest["base_image_sha256"]
    save()
    return 0 if all(r["exit_code"] == 0 for r in manifest["samples"]) and manifest["base_unchanged"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
