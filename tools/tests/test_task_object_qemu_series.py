#!/usr/bin/env python3
"""End-to-end tests for the prepared task-object QEMU series driver.

The fake checker emits the same report schema as check-native-sync.py. These
tests exercise the full runner, ordering, partial evidence, and hash guards
without starting QEMU.
"""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "tools/native/run-task-object-qemu-series.py"
CONTRACT = {
    "warmup_calls": 1000,
    "calls": 100000,
    "errors": 0,
    "status_hash_fnv1a32": 235836997,
}

spec = importlib.util.spec_from_file_location("task_object_series", DRIVER)
series_driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(series_driver)


FAKE_CHECKER = r'''#!/usr/bin/env python3
import hashlib, json, os, pathlib, sys

args = sys.argv[1:]
def value(name): return args[args.index(name) + 1]
impl = value("--task-objects-impl")
prepared = pathlib.Path(value("--prepared"))
output = pathlib.Path(value("--output"))
output.mkdir(parents=True, exist_ok=True)
mode = os.environ.get("FAKE_SERIES_MODE", "pass")
counter = output.parent / ".fake-checker-count"
count = int(counter.read_text()) + 1 if counter.exists() else 1
counter.write_text(str(count))
if mode == "timeout":
    raise SystemExit(124)
if mode == "failure":
    raise SystemExit(1)
manifest = json.loads((prepared / "manifest.json").read_text())
contract = {"warmup_calls": 1000, "calls": 100000, "errors": 0,
            "status_hash_fnv1a32": 235836997}
elapsed = 100 if impl == "c" else 110
case = {
    "task_identity_contract": contract,
    "task_identity_metrics": {"elapsed_ns": elapsed,
                               "ns_per_transaction": elapsed // 100000},
    "kernel_sha256": "1" * 64,
    "app_sha256": manifest["artifacts_sha256"]["native-futex"],
    "run_image_sha256": "2" * 64,
    "audit_errors": 0,
    "fatal_log_scan_passed": True,
    "prepared_manifest_sha256": hashlib.sha256(
        (prepared / "manifest.json").read_bytes()).hexdigest(),
    "log": str(output / "guest.log"),
}
report = {"passed": True, "cases": [case]}
impl_output = output / impl
impl_output.mkdir(parents=True, exist_ok=True)
(impl_output / "report.json").write_text(json.dumps(report))
if mode == "hash-drift" and count == 1:
    pathlib.Path(__file__).write_text(pathlib.Path(__file__).read_text() + "\n# drift\n")
'''


class TaskObjectSeriesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="task-object-series-test-")
        self.root = Path(self.temp.name)
        self.snapshots = {}
        source_hashes = {
            "user/tests/test_native_futex.c": "a" * 64,
            "user/tests/native-thread-contract.h": "b" * 64,
        }
        for impl in ("c", "rust"):
            path = self.root / f"snapshot-{impl}"
            path.mkdir()
            manifest = {
                "arch": "x86_64",
                "task_objects_impl": impl,
                "cpus": 2,
                "source_inputs_sha256": source_hashes,
                "source_tree_state": "c" * 64,
                "artifacts_sha256": {"native-futex": "d" * 64},
            }
            (path / "manifest.json").write_text(json.dumps(manifest))
            self.snapshots[impl] = path
        self.checker = self.root / "fake-checker.py"
        self.checker.write_text(FAKE_CHECKER)
        self.protocol = self.root / "protocol.md"
        self.protocol.write_text("test protocol v1\n")
        allowed = sorted(os.sched_getaffinity(0))
        if len(allowed) < 2:
            self.skipTest("at least two allowed host CPUs are needed for taskset")
        self.cpu_set = f"{allowed[0]},{allowed[1]}"

    def tearDown(self):
        self.temp.cleanup()

    def run_driver(self, output, *, compare=False, mode="pass"):
        argv = [
            str(DRIVER), "--snapshot", str(self.snapshots["c"]),
            "--output", str(output), "--impl", "c", "--pairs", "6",
            "--seed", "20261013", "--cpu-set", self.cpu_set, "--timeout", "1",
        ]
        if compare:
            argv += ["--compare-rust-snapshot", str(self.snapshots["rust"])]
        with patch.object(series_driver, "CHECKER", self.checker), \
                patch.object(series_driver, "PROTOCOL", self.protocol), \
                patch.object(series_driver.sys, "argv", argv), \
                patch.dict(os.environ, {"FAKE_SERIES_MODE": mode}):
            return series_driver.main()

    def read_series(self, output):
        return json.loads((output / "series.json").read_text())

    def test_complete_cc_series_writes_summary_and_ci(self):
        output = self.root / "cc-series"
        self.assertEqual(self.run_driver(output), 0)
        report = self.read_series(output)
        self.assertEqual(report["status"], "complete")
        self.assertEqual(len(report["samples"]), 12)
        self.assertEqual(len(report["pairs"]), 6)
        self.assertEqual(report["paired_ratio_min"], 1.0)
        self.assertEqual(report["paired_ratio_max"], 1.0)
        self.assertEqual(report["max_pair_spread_ratio"], 1.0)
        self.assertEqual(report["protocol_sha256"], series_driver.sha(self.protocol))
        self.assertEqual(report["series_driver_sha256"], series_driver.sha(DRIVER))
        self.assertTrue(all(pair["ratio_b_over_a"] == 1.0 for pair in report["pairs"]))

    def test_complete_cr_series_is_seeded_and_balanced(self):
        output = self.root / "cr-series"
        self.assertEqual(self.run_driver(output, compare=True), 0)
        report = self.read_series(output)
        self.assertEqual(report["status"], "complete")
        self.assertEqual(report["pair_order"], [1, 6, 3, 2, 4, 5])
        sides = report["pair_sides"]
        self.assertEqual(sides, {
            "1": ["c", "rust"], "6": ["rust", "c"],
            "3": ["c", "rust"], "2": ["rust", "c"],
            "4": ["c", "rust"], "5": ["rust", "c"],
        })
        self.assertEqual(sum(v == ["c", "rust"] for v in sides.values()), 3)
        self.assertEqual(sum(v == ["rust", "c"] for v in sides.values()), 3)
        self.assertEqual(len(report["samples"]), 12)
        self.assertEqual(len(report["pairs"]), 6)
        self.assertEqual(report["paired_ratio_median"], 1.1)
        self.assertEqual(report["paired_median_ci95_exact"], [1.1, 1.1])

    def test_timeout_keeps_partial_attempt_and_marks_failure(self):
        output = self.root / "timeout-series"
        self.assertEqual(self.run_driver(output, compare=True, mode="timeout"), 1)
        report = self.read_series(output)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(len(report["samples"]), 1)
        self.assertEqual(report["samples"][0]["returncode"], 124)
        self.assertEqual(report["samples"][0]["status"], "failed")
        self.assertTrue(Path(report["samples"][0]["runner_log"]).is_file())

    def test_checker_failure_keeps_partial_attempt_and_marks_failure(self):
        output = self.root / "failed-series"
        self.assertEqual(self.run_driver(output, compare=True, mode="failure"), 1)
        report = self.read_series(output)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(len(report["samples"]), 1)
        self.assertEqual(report["samples"][0]["returncode"], 1)
        self.assertEqual(report["samples"][0]["status"], "failed")

    def test_checker_hash_drift_refuses_next_run_and_preserves_partial(self):
        output = self.root / "drift-series"
        self.assertEqual(self.run_driver(output, compare=True, mode="hash-drift"), 1)
        report = self.read_series(output)
        self.assertEqual(report["status"], "failed")
        self.assertIn("changed mid-series", report["failure"])
        self.assertEqual(len(report["samples"]), 1)
        self.assertTrue(Path(report["samples"][0]["report"]).is_file())

    def test_existing_output_is_not_overwritten(self):
        output = self.root / "existing-series"
        output.mkdir()
        sentinel = output / "sentinel"
        sentinel.write_text("preserve")
        with self.assertRaises(SystemExit):
            self.run_driver(output, compare=True)
        self.assertEqual(sentinel.read_text(), "preserve")


if __name__ == "__main__":
    unittest.main()
