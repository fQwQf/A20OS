import json
import os
import sys
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from tools.release.v1_compare import SpecError, _parse_marker, _safe_token, run


RUNNER_SCRIPT = r'''import json, os, subprocess, sys, time
case = os.environ["A20_COMPARE_CASE"]
version = os.environ["A20_COMPARE_VERSION"]
iteration = int(os.environ["A20_COMPARE_ITERATION"])
series = [float(x) for x in os.environ.get("samples", "100").split(",")]
offset = 1 if version != "candidate" and os.path.basename(os.environ["A20_COMPARE_RUN_DIR"]).endswith("-b") else 0
value = series[(iteration - 1 + offset) % len(series)]
if os.environ.get("fail_iteration") == str(iteration) and version == "candidate":
    print("candidate run failed")
    raise SystemExit(7)
if os.environ.get("spawn_child") == "1" and version == "candidate":
    ready_file = os.environ["child_pid_file"] + ".ready"
    child_code = ("import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                  f"open({ready_file!r}, 'w').write('ready'); time.sleep(30)")
    child = subprocess.Popen([sys.executable, "-c", child_code])
    open(os.environ["child_pid_file"], "w").write(str(child.pid))
    deadline = time.monotonic() + 3
    while not os.path.exists(ready_file) and time.monotonic() < deadline:
        time.sleep(0.01)
    time.sleep(30)
if os.environ.get("fatal") and version == "candidate":
    print(os.environ["fatal"])
contract = json.loads(os.environ.get("contract", '{"packet_hash":"abc","errors":0}'))
mode = os.environ.get("marker_mode", "normal")
if mode != "missing":
    metrics = {} if mode == "missing_metrics" else {os.environ.get("metric_name", "packet_ns"):value}
    record = "A20_COMPARE_JSON: " + json.dumps({"schema":1,"case":case,"contract":contract,
          "metrics":metrics})
    print(record)
    if mode == "duplicate":
        print(record)
'''


class V1CompareTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.script = self.root / "fixture.py"
        self.script.write_text(RUNNER_SCRIPT, encoding="utf-8")
        self.spec = self.root / "protocol.toml"
        self.out = self.root / "evidence"

    def tearDown(self):
        self.temp.cleanup()

    def write_spec(self, *, c0_validated=True, cn_validated=True,
                   c0_present=True, candidate_samples="100", repetitions=6,
                   failure=None, budget=None, kind="performance",
                   metric_name="packet_ns", direction="lower", reference_samples="100",
                   candidate_env=None, timeout_seconds=3, case_id="nat-rewrite/test"):
        versions = []
        if c0_present:
            versions.append(("c0", c0_validated))
        versions.extend((("cn", cn_validated), ("candidate", False)))
        chunks = [
            "schema_version = 1",
            "[comparison]",
            'id = "test-net"',
            'requirement_id = "V1-NET"',
            'protocol_id = "nat-rewrite-test-v1"',
            'inputs_sha256 = "' + "a" * 64 + '"',
            "order_seed = 71",
            'environment = { host = "unit-test" }',
            "",
        ]
        for name, validated in versions:
            chunks.extend((f"[versions.{name}]", f'id = "{name}-test"',
                           f'revision = "{name}-revision"',
                           f"validated = {'true' if validated else 'false'}",
                           f'evidence = "{name} fixture evidence"', ""))
        chunks.extend(("[[cases]]", f'id = "{case_id}"', f'kind = "{kind}"',
                       f"repetitions = {repetitions}", f"timeout_seconds = {timeout_seconds}",
                       'expected_contract = { packet_hash = "abc", errors = 0 }', ""))
        for name, _ in versions:
            samples = candidate_samples if name == "candidate" else reference_samples
            env = [f'samples = "{samples}"', f'metric_name = "{metric_name}"']
            if failure and name == "candidate":
                env.append(f'fail_iteration = "{failure}"')
            if name == "candidate" and candidate_env:
                env.extend(f'{key} = {json.dumps(value)}' for key, value in candidate_env.items())
            chunks.extend((f"[cases.commands.{name}]",
                           f"argv = [{json.dumps(sys.executable)}, {json.dumps(str(self.script))}]",
                           f"artifact_paths = [{json.dumps(str(self.script))}]",
                           f'env = {{ {", ".join(env)} }}', ""))
        if c0_present:
            chunks.extend((f"[cases.metrics.{metric_name}]", f'direction = "{direction}"',
                           f'unit = "{"ns/packet" if direction == "lower" else "packets/s"}"',
                           'calibration_sha256 = "' + "b" * 64 + '"',
                           'calibration_evidence = "separately collected C0 self-control fixture"',
                           "resolution_ratio = 1.0", "noninferiority_bound_ratio = 1.0"))
            if budget is not None:
                chunks.append(f"budget_max = {budget}")
        self.spec.write_text("\n".join(chunks) + "\n", encoding="utf-8")

    def test_identical_contract_and_performance_pass(self):
        self.write_spec()
        report = run(self.spec, self.out)
        self.assertEqual(report["status"], "PASS")
        metric = report["cases"][0]["performance"]["comparisons"]["c0"]["packet_ns"]
        self.assertEqual(metric["status"], "PASS")
        self.assertEqual(metric["reference_self_control_degradation_ratio_ci"]["upper"], 1.0)

    def test_reproducible_regression_fails_against_observed_resolution(self):
        self.write_spec(candidate_samples="125")
        report = run(self.spec, self.out)
        self.assertEqual(report["status"], "FAIL")
        metric = report["cases"][0]["performance"]["comparisons"]["c0"]["packet_ns"]
        self.assertEqual(metric["status"], "FAIL")

    def test_higher_is_better_regression_fails(self):
        self.write_spec(candidate_samples="50", metric_name="packets_per_second",
                        direction="higher", reference_samples="100")
        report = run(self.spec, self.out)
        self.assertEqual(report["status"], "FAIL")
        metric = report["cases"][0]["performance"]["comparisons"]["c0"]["packets_per_second"]
        self.assertEqual(metric["candidate_degradation_ratio_ci"]["lower"], 2.0)

    def test_live_self_control_noise_cannot_expand_preregistered_bound(self):
        self.write_spec(reference_samples="100,140", candidate_samples="100")
        report = run(self.spec, self.out)
        self.assertEqual(report["status"], "EVIDENCE_INSUFFICIENT")
        metric = report["cases"][0]["performance"]["comparisons"]["c0"]["packet_ns"]
        self.assertIn("noise exceeds", metric["reason"])

    def test_wide_candidate_interval_is_evidence_insufficient(self):
        self.write_spec(candidate_samples="100,100,100,100,100,130")
        report = run(self.spec, self.out)
        self.assertEqual(report["status"], "EVIDENCE_INSUFFICIENT")

    def test_failed_round_is_retained_and_cannot_be_dropped_from_pass(self):
        self.write_spec(failure=2)
        report = run(self.spec, self.out)
        case = report["cases"][0]
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(any(not round_["complete"] for round_ in case["rounds"]))
        self.assertTrue(any(run_["returncode"] == 7 for run_ in case["runs"]))
        self.assertTrue((self.out / "report.json").is_file())
        self.assertTrue(list((self.out / "runs").rglob("stdout.log")))

    def test_unvalidated_tag_identity_cannot_be_reported_as_pass(self):
        self.write_spec(c0_validated=False)
        report = run(self.spec, self.out)
        self.assertEqual(report["status"], "EVIDENCE_INSUFFICIENT")
        self.assertIn("baseline_note", report["cases"][0])

    def test_missing_c0_is_rejected_before_any_result_can_pass(self):
        self.write_spec(c0_present=False)
        with self.assertRaisesRegex(SpecError, "versions.c0"):
            run(self.spec, self.out)
        self.assertFalse(self.out.exists())

    def test_pre_registered_hard_budget_violation_fails(self):
        self.write_spec(candidate_samples="120", budget=110)
        report = run(self.spec, self.out)
        metric = report["cases"][0]["performance"]["comparisons"]["c0"]["packet_ns"]
        self.assertEqual(metric["status"], "FAIL")
        self.assertIn("budget", metric["reason"])

    def test_panic_fatal_and_lockstall_after_success_marker_fail(self):
        for text in ("KERNEL PANIC: injected", "FATAL: injected", "LOCKSTALL: injected"):
            with self.subTest(text=text):
                self.write_spec(kind="functional", repetitions=1,
                                candidate_env={"fatal": text})
                report = run(self.spec, self.out)
                candidate = next(result for result in report["cases"][0]["runs"]
                                 if result["version"] == "candidate")
                self.assertEqual(candidate["status"], "FAIL")
                self.assertTrue(candidate["fatal_matches"])
                self.temp.cleanup()
                self.setUp()

    def test_missing_duplicate_markers_and_missing_metrics_fail_cleanly(self):
        for mode in ("missing", "duplicate", "missing_metrics"):
            with self.subTest(mode=mode):
                self.write_spec(kind="functional", repetitions=1,
                                candidate_env={"marker_mode": mode})
                report = run(self.spec, self.out)
                candidate = next(result for result in report["cases"][0]["runs"]
                                 if result["version"] == "candidate")
                self.assertEqual(candidate["status"], "FAIL")
                self.assertTrue(candidate["errors"])
                self.temp.cleanup()
                self.setUp()

    def test_timeout_kills_descendant_that_ignores_term(self):
        child_pid_file = self.root / "child.pid"
        self.write_spec(kind="functional", repetitions=1, timeout_seconds=0.2,
                        candidate_env={"spawn_child": "1", "child_pid_file": str(child_pid_file)})
        report = run(self.spec, self.out)
        candidate = next(result for result in report["cases"][0]["runs"]
                         if result["version"] == "candidate")
        self.assertTrue(candidate["timed_out"])
        self.assertEqual(candidate["status"], "FAIL")
        child_pid = int(child_pid_file.read_text())
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            proc_stat = Path(f"/proc/{child_pid}/stat")
            if not proc_stat.exists() or proc_stat.read_text().split()[2] == "Z":
                break
            time.sleep(0.05)
        else:
            self.fail("timed-out command left its SIGTERM-ignoring child alive")

    def test_case_id_path_encoding_does_not_alias(self):
        self.assertNotEqual(_safe_token("a/b"), _safe_token("a_b"))

    def test_boolean_schema_values_are_not_integer_schema_one(self):
        self.write_spec(kind="functional", repetitions=1)
        self.spec.write_text(self.spec.read_text().replace(
            "schema_version = 1", "schema_version = true", 1), encoding="utf-8")
        with self.assertRaisesRegex(SpecError, "schema_version"):
            run(self.spec, self.out)
        self.assertFalse(self.out.exists())

        payload, error = _parse_marker(
            'A20_COMPARE_JSON: {"schema":true,"case":"nat-rewrite/test",'
            '"contract":{"packet_hash":"abc","errors":0},"metrics":{}}',
            "nat-rewrite/test",
        )
        self.assertIsNone(payload)
        self.assertIn("schema", error)

    def test_invalid_fatal_regex_is_rejected_before_evidence_directory(self):
        self.write_spec(kind="functional", repetitions=1)
        text = self.spec.read_text(encoding="utf-8").replace(
            'expected_contract = { packet_hash = "abc", errors = 0 }',
            'expected_contract = { packet_hash = "abc", errors = 0 }\n'
            'fatal_patterns = ["["]',
            1,
        )
        self.spec.write_text(text, encoding="utf-8")
        with self.assertRaisesRegex(SpecError, "invalid fatal pattern"):
            run(self.spec, self.out)
        self.assertFalse(self.out.exists())


if __name__ == "__main__":
    unittest.main()
