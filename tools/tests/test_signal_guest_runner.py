#!/usr/bin/env python3
"""Regression tests for interactive Signal guest and perf result validation."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from signal_guest_runner import validate_bench_output  # noqa: E402
from signal_perf_bind import check_log_identity, dirty_diff_sha256, perf_bind  # noqa: E402
from signal_perf_pair import bench_summary  # noqa: E402


GOOD = """\
SIGNAL_BENCH: killwake rounds=5 total_ns=500 ns_per_op=100
SIGNAL_BENCH: pingpong rounds=3 total_ns=600 ns_per_round=200
SIGNAL_BENCH: signalfd records=7 total_ns=700 ns_per_record=100 records_per_s=10000
SIGNAL_BENCH: DONE
"""

FAKE_GUEST = r'''#!/usr/bin/env python3
import os
import re
import sys

mode = os.environ.get("FAKE_SIGNAL_GUEST_MODE", "success")
print("# ", end="", flush=True)
for command in sys.stdin:
    if command.strip() == "poweroff":
        print("\nSystem is going down for power-off NOW", flush=True)
        if mode == "marker-stall":
            import time
            while True:
                time.sleep(1)
        break
    match = re.search(r"(__A20_SIGNAL_RUNNER_EXIT_\d+__)", command)
    if "signal_bench" in command:
        output = """SIGNAL_BENCH: killwake rounds=5 total_ns=500 ns_per_op=100
SIGNAL_BENCH: pingpong rounds=3 total_ns=600 ns_per_round=200
SIGNAL_BENCH: signalfd records=7 total_ns=700 ns_per_record=100 records_per_s=10000
"""
        if mode == "fail":
            output += "SIGNAL_BENCH: FAIL synthetic failure\n"
        elif mode == "missing-marker":
            pass
        elif mode == "missing-result":
            output = "SIGNAL_BENCH: killwake rounds=5 total_ns=500 ns_per_op=100\nSIGNAL_BENCH: DONE\n"
        else:
            output += "SIGNAL_BENCH: DONE\n"
        print(output, end="", flush=True)
    status = 7 if mode == "nonzero" else 0
    if match:
        print(f"{match.group(1)}:{status}\n# ", end="", flush=True)
if mode == "marker-exit7":
    sys.exit(7)
'''


class SignalGuestRunnerTests(unittest.TestCase):
    def test_synthetic_output_requires_success_complete_and_all_metrics(self):
        self.assertEqual(validate_bench_output(GOOD, 0), (True, "ok"))
        self.assertTrue(validate_bench_output(GOOD.replace("SIGNAL_BENCH: DONE\n", ""), 0)[0] is False)
        self.assertTrue(validate_bench_output(GOOD.replace("SIGNAL_BENCH: DONE\n", "SIGNAL_BENCH: FAIL\n"), 0)[0] is False)
        self.assertTrue(validate_bench_output(GOOD, 23)[0] is False)
        self.assertTrue(validate_bench_output(GOOD.replace("records=7", "records=0"), 0)[0] is False)
        with self.assertRaises(ValueError):
            bench_summary(GOOD.replace("SIGNAL_BENCH: DONE\n", ""))

    def run_fake_guest(self, mode: str) -> tuple[subprocess.CompletedProcess[str], str]:
        with tempfile.TemporaryDirectory(prefix="signal-runner-test-") as tmp:
            root = Path(tmp)
            lockdir = root / "locks"
            lockdir.mkdir()
            (lockdir / "activity.lock").touch()
            (lockdir / "qemu-1.lock").touch()
            fake = root / "fake-guest"
            fake.write_text(FAKE_GUEST)
            fake.chmod(0o755)
            log = root / "guest.log"
            env = os.environ.copy()
            env["A20OS_SIGNAL_RUNNER_LOCK_DIR"] = str(lockdir)
            env["FAKE_SIGNAL_GUEST_MODE"] = mode
            result = subprocess.run(
                [sys.executable, str(TOOLS / "signal_guest_runner.py"),
                 "--qemu", str(fake), "--require-bench", str(log), "3",
                 "fake-kernel", "fake-image", "signal_bench 5 3 7", "POWEROFF"],
                env=env, text=True, capture_output=True, check=False, timeout=6,
            )
            return result, log.read_text(errors="replace")

    def test_fake_guest_success_requires_all_results_and_shutdown(self):
        result, output = self.run_fake_guest("success")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("SIGNAL_BENCH: DONE", output)
        self.assertIn("System is going down for power-off NOW", output)

    def test_fake_guest_fail_marker_and_missing_done_are_rejected(self):
        for mode in ("fail", "missing-marker", "missing-result"):
            with self.subTest(mode=mode):
                result, output = self.run_fake_guest(mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("System is going down for power-off NOW", output)

    def test_fake_guest_nonzero_command_status_cannot_pass_with_done(self):
        result, output = self.run_fake_guest("nonzero")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SIGNAL_BENCH: DONE", output)
        self.assertNotIn("System is going down for power-off NOW", output)

    def test_shutdown_marker_does_not_hide_hung_or_failed_qemu(self):
        for mode in ("marker-stall", "marker-exit7"):
            with self.subTest(mode=mode):
                result, output = self.run_fake_guest(mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("System is going down for power-off NOW", output)


class PerfBindRecordTests(unittest.TestCase):
    """An unbound report cannot be rerun, and a directory that quietly mixes
    two trees' artifacts looks like evidence but qualifies as none: both must
    be caught at record time."""

    def _bind(self, root, kernel_digest, image_digest="same-image"):
        kernel = root / "kernel.elf"
        image = root / "fat32.img"
        kernel.write_bytes(kernel_digest.encode())
        image.write_bytes(image_digest.encode())
        return perf_bind(
            repo=root, kernel=kernel, image=image, impl="R", round_no=1,
            summary_lines=["SIGNAL_BENCH: killwake rounds=5 total_ns=500 ns_per_op=100"],
            bench_args="signal_bench 5000 2000 100000", timeout=400,
        )

    def test_record_contains_commit_input_and_config_facts(self):
        with tempfile.TemporaryDirectory(prefix="signal-perf-bind-") as tmp:
            root = Path(tmp)
            # `repo` does not have to be a real checkout for the input facts;
            # a real repo adds commit/dirty hashes when present.
            record = self._bind(root, "kernel-A")
            self.assertEqual(record["inputs"]["kernel_sha256"],
                             hashlib.sha256(b"kernel-A").hexdigest())
            self.assertEqual(record["inputs"]["image_sha256"],
                             hashlib.sha256(b"same-image").hexdigest())
            self.assertEqual(record["impl"], "R")
            self.assertEqual(record["round"], 1)
            self.assertEqual(record["config"]["cpus"], 2)
            self.assertEqual(record["config"]["mem"], "1G")
            self.assertIn("signal_bench 5000", record["bench_args"])

    def test_mismatched_artifact_rebind_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix="signal-perf-bind-") as tmp:
            root = Path(tmp)
            first = self._bind(root, "kernel-A")
            # A stale log from a different build must not be renamed into new
            # evidence silently.
            stale = root / "rewrite"
            stale.write_text("old kernel bytes")
            second = self._bind(root, "kernel-B")
            self.assertEqual(check_log_identity(first, second), "inputs.kernel_sha256")
            self.assertIsNone(check_log_identity(first, first))
            _ = stale

    def test_same_artifacts_rerun_never_flags(self):
        with tempfile.TemporaryDirectory(prefix="signal-perf-bind-") as tmp:
            root = Path(tmp)
            first = self._bind(root, "kernel-A")
            second = self._bind(root, "kernel-A")
            self.assertIsNone(check_log_identity(first, second))

    def test_dirty_digest_tracks_contents_but_ignores_evidence_output(self):
        with tempfile.TemporaryDirectory(prefix="signal-dirty-hash-") as tmp:
            root = Path(tmp)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            subprocess.run(["git", "-C", str(root), "-c", "user.email=test@example.invalid",
                            "-c", "user.name=Test", "commit", "--allow-empty", "-qm", "baseline"],
                           check=True)
            source = root / "new.rs"
            source.write_text("first\n")
            first = dirty_diff_sha256(root)
            source.write_text("second\n")
            second = dirty_diff_sha256(root)
            self.assertNotEqual(first, second)
            evidence = root / "out" / "round.json"
            evidence.parent.mkdir()
            evidence.write_text("generated record\n")
            self.assertEqual(second, dirty_diff_sha256(root))

    def test_binding_identity_rejects_source_or_userland_changes(self):
        with tempfile.TemporaryDirectory(prefix="signal-bind-identity-") as tmp:
            root = Path(tmp)
            first = self._bind(root, "kernel-A")
            next_record = dict(first)
            next_record["source"] = {"commit": "other", "dirty_diff_sha256": None}
            self.assertEqual(check_log_identity(first, next_record), "source")
            next_record = dict(first)
            next_record["inputs"] = dict(first["inputs"], user_program_sha256="other")
            self.assertEqual(check_log_identity(first, next_record), "inputs.user_program_sha256")


if __name__ == "__main__":
    unittest.main()
