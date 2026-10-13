#!/usr/bin/env python3
"""Run a reproducible series of prepared Native task-object QEMU samples.

This driver never builds. Each invocation gets a unique report directory and
the underlying checker copies the sealed base disk into that run directory.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
CHECKER = ROOT / "tools/check-native-sync.py"
PROTOCOL = ROOT / "docs/rust/task-object-performance-protocol.md"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def atomic_json(path, data):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def mark_failed(series, output, reason):
    series["status"] = "failed"
    series["failure"] = reason
    series["failed_unix_ns"] = time.time_ns()
    atomic_json(output / "series.json", series)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--compare-rust-snapshot", type=Path,
                        help="pair C runs with Rust runs from this second snapshot")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--impl", choices=["c", "rust"], required=True)
    parser.add_argument("--pairs", type=int, default=6)
    parser.add_argument("--seed", type=int, default=20261013)
    parser.add_argument("--cpu-set", default="2,4")
    parser.add_argument("--timeout", type=float, default=90)
    args = parser.parse_args()
    if args.pairs < 1 or args.timeout <= 0:
        parser.error("pairs and timeout must be positive")
    if shutil.which("taskset") is None:
        parser.error("taskset is required for fixed host CPU affinity")

    snapshot = args.snapshot if args.snapshot.is_absolute() else ROOT / args.snapshot
    output = args.output if args.output.is_absolute() else ROOT / args.output
    if not snapshot.is_dir() or not (snapshot / "manifest.json").is_file():
        parser.error(f"prepared snapshot is missing: {snapshot}")
    if output.exists():
        parser.error(f"refusing to overwrite series output: {output}")
    output.mkdir(parents=True)

    snapshot_manifest = json.loads((snapshot / "manifest.json").read_text())
    compare_snapshot = None
    compare_manifest = None
    if args.compare_rust_snapshot:
        compare_snapshot = (args.compare_rust_snapshot if
                            args.compare_rust_snapshot.is_absolute() else
                            ROOT / args.compare_rust_snapshot)
        if not compare_snapshot.is_dir() or not (compare_snapshot / "manifest.json").is_file():
            parser.error(f"comparison snapshot is missing: {compare_snapshot}")
        compare_manifest = json.loads((compare_snapshot / "manifest.json").read_text())
        if args.impl != "c":
            parser.error("--compare-rust-snapshot requires --impl c")
        if (compare_manifest.get("arch") != "x86_64" or
                compare_manifest.get("task_objects_impl") != "rust" or
                compare_manifest.get("cpus") != 2):
            parser.error("comparison snapshot must be Rust x86_64 guest SMP2")
        if (compare_manifest.get("source_inputs_sha256") !=
                snapshot_manifest.get("source_inputs_sha256") or
                compare_manifest.get("source_tree_state") !=
                snapshot_manifest.get("source_tree_state") or
                compare_manifest.get("artifacts_sha256", {}).get("native-futex") !=
                snapshot_manifest.get("artifacts_sha256", {}).get("native-futex")):
            parser.error("C and Rust snapshots do not contain the same fixture/source state")
    if (snapshot_manifest.get("arch") != "x86_64" or
            snapshot_manifest.get("task_objects_impl") != args.impl or
            snapshot_manifest.get("cpus") != 2):
        parser.error("snapshot must match requested x86_64 implementation with guest SMP2")
    checker_sha = sha(CHECKER)
    protocol_sha = sha(PROTOCOL)
    manifest_path = snapshot / "manifest.json"
    snapshot_sha = sha(manifest_path)
    compare_manifest_path = compare_snapshot / "manifest.json" if compare_snapshot else None
    compare_snapshot_sha = sha(compare_manifest_path) if compare_manifest_path else None
    cpu_set = ",".join(str(int(cpu)) for cpu in args.cpu_set.split(","))
    allowed = sorted(os.sched_getaffinity(0))
    requested = {int(cpu) for cpu in cpu_set.split(",")}
    if not requested.issubset(allowed):
        parser.error(f"requested cpuset {cpu_set} is not within allowed CPUs {allowed}")

    rng = random.Random(args.seed)
    indices = list(range(1, args.pairs + 1))
    rng.shuffle(indices)
    pair_sides = {}
    if compare_snapshot:
        if args.pairs % 2:
            parser.error("candidate comparison requires an even pair count for balanced order")
        orders = [["c", "rust"]] * (args.pairs // 2)
        orders += [["rust", "c"]] * (args.pairs // 2)
        rng.shuffle(orders)
        pair_sides = {str(pair_index): orders[offset]
                      for offset, pair_index in enumerate(indices)}
    else:
        pair_sides = {str(pair_index): ["a", "b"] for pair_index in indices}
    series = {
        "schema": 1,
        "status": "running",
        "impl": "c-vs-rust" if compare_snapshot else args.impl,
        "snapshot": {"c": str(snapshot), "rust": str(compare_snapshot)}
                    if compare_snapshot else str(snapshot),
        "snapshot_manifest_sha256": {"c": snapshot_sha, "rust": compare_snapshot_sha}
                                     if compare_snapshot else snapshot_sha,
        "checker_sha256": checker_sha,
        "series_driver_sha256": sha(Path(__file__)),
        "protocol_sha256": protocol_sha,
        "workload_sha256": hashlib.sha256((
            snapshot_manifest["source_inputs_sha256"]["user/tests/test_native_futex.c"] +
            snapshot_manifest["source_inputs_sha256"]["user/tests/native-thread-contract.h"]
        ).encode()).hexdigest(),
        "contract": {"warmup_calls": 1000, "calls": 100000, "errors": 0,
                     "status_hash_fnv1a32": 235836997},
        "host_cpu_set": cpu_set,
        "allowed_host_cpus": allowed,
        "guest_cpus": 2,
        "seed": args.seed,
        "pair_order": indices,
        "pair_sides": pair_sides,
        "timeout_seconds": args.timeout,
        "samples": [],
        "pairs": [],
    }
    atomic_json(output / "series.json", series)
    start_unix = time.time_ns()
    start_monotonic = time.monotonic_ns()
    for pair_index in indices:
        pair = {"pair": pair_index, "runs": []}
        sides = pair_sides[str(pair_index)]
        for side in sides:
            if (sha(CHECKER) != checker_sha or sha(PROTOCOL) != protocol_sha or
                    sha(manifest_path) != snapshot_sha or
                    (compare_manifest_path and
                     sha(compare_manifest_path) != compare_snapshot_sha)):
                mark_failed(series, output,
                            "checker, protocol, or sealed snapshot manifest changed mid-series")
                return 1
            if compare_snapshot:
                side_impl = side
                side_snapshot = snapshot if side == "c" else compare_snapshot
                side_manifest_sha = snapshot_sha if side == "c" else compare_snapshot_sha
                label = side
            else:
                side_impl = args.impl
                side_snapshot = snapshot
                side_manifest_sha = snapshot_sha
                label = side
            run_dir = output / f"pair-{pair_index:02d}-{label}"
            command = [
                "taskset", "--cpu-list", cpu_set, sys.executable, str(CHECKER),
                "--arches", "x86_64", "--cpus", "2", "--jobs", "1",
                "--timeout", str(args.timeout), "--task-objects-impl", side_impl,
                "--prepared", str(side_snapshot), "--output", str(run_dir),
            ]
            before = time.monotonic_ns()
            load_before = os.getloadavg()
            with (run_dir.with_suffix(".runner.log")).open("w") as log:
                result = subprocess.run(command, cwd=ROOT, stdout=log,
                                        stderr=subprocess.STDOUT, check=False)
            load_after = os.getloadavg()
            runner_log = run_dir.with_suffix(".runner.log")
            run_report_path = run_dir / side_impl / "report.json"
            if result.returncode != 0 or not run_report_path.is_file():
                sample = {"pair": pair_index, "label": label, "impl": side_impl,
                          "command": command, "returncode": result.returncode,
                          "runner_log": str(runner_log), "status": "failed"}
                series["samples"].append(sample)
                mark_failed(series, output,
                            f"checker invocation failed or omitted report for pair {pair_index} side {label}")
                return 1
            report = json.loads(run_report_path.read_text())
            case = report["cases"][0]
            if not report.get("passed") or case.get("task_identity_contract") != series["contract"]:
                series["status"] = "failed"
                sample = {"pair": pair_index, "label": label, "impl": side_impl,
                          "status": "invalid-report", "report": str(run_report_path)}
                series["samples"].append(sample)
                mark_failed(series, output,
                            f"invalid checker report for pair {pair_index} side {label}")
                return 1
            sample = {
                "pair": pair_index, "label": label, "impl": side_impl,
                "status": "passed",
                "command": command, "returncode": result.returncode,
                "elapsed_wall_ns": time.monotonic_ns() - before,
                "host_load_before": load_before, "host_load_after": load_after,
                "runner_log": str(runner_log), "report": str(run_report_path),
                "guest_log": case["log"], "kernel_sha256": case["kernel_sha256"],
                "app_sha256": case["app_sha256"], "run_disk_sha256": case["run_image_sha256"],
                "task_identity_metrics": case["task_identity_metrics"],
                "audit_errors": case["audit_errors"],
                "fatal_log_scan_passed": case["fatal_log_scan_passed"],
                "prepared_manifest_sha256": case["prepared_manifest_sha256"],
            }
            series["samples"].append(sample)
            pair["runs"].append(sample)
            if len(pair["runs"]) == len(sides):
                if compare_snapshot:
                    by_impl = {run["impl"]: run for run in pair["runs"]}
                    ct = by_impl["c"]["task_identity_metrics"]["elapsed_ns"]
                    rt = by_impl["rust"]["task_identity_metrics"]["elapsed_ns"]
                    pair["rust_over_c_ratio"] = rt / ct
                else:
                    at = pair["runs"][0]["task_identity_metrics"]["elapsed_ns"]
                    bt = pair["runs"][1]["task_identity_metrics"]["elapsed_ns"]
                    pair["ratio_b_over_a"] = bt / at
                series["pairs"].append(pair)
            atomic_json(output / "series.json", series)

    if (sha(CHECKER) != checker_sha or sha(PROTOCOL) != protocol_sha or
            sha(manifest_path) != snapshot_sha or
            (compare_manifest_path and sha(compare_manifest_path) != compare_snapshot_sha)):
        mark_failed(series, output,
                    "checker, protocol, or sealed snapshot manifest changed during series")
        return 1
    series["status"] = "complete"
    series["started_unix_ns"] = start_unix
    series["completed_unix_ns"] = time.time_ns()
    series["elapsed_wall_ns"] = time.monotonic_ns() - start_monotonic
    if compare_snapshot:
        ratios = [pair["rust_over_c_ratio"] for pair in series["pairs"]]
        series["paired_ratio_median"] = statistics.median(ratios)
        series["paired_median_ci95_exact"] = [min(ratios), max(ratios)]
    else:
        ratios = [pair["ratio_b_over_a"] for pair in series["pairs"]]
        series["paired_ratio_min"] = min(ratios)
        series["paired_ratio_max"] = max(ratios)
        series["max_pair_spread_ratio"] = max(
            max(ratio, 1 / ratio) for ratio in ratios)
    atomic_json(output / "series.json", series)
    print(json.dumps({key: series[key] for key in
                      ("status", "impl", "seed", "host_cpu_set", "elapsed_wall_ns",
                       "paired_ratio_min", "paired_ratio_max", "max_pair_spread_ratio",
                       "paired_ratio_median", "paired_median_ci95_exact")
                      if key in series},
                     sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
