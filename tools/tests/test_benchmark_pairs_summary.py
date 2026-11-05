"""Tests for strict paired guest-output benchmark summaries."""
import importlib.util
import unittest
from pathlib import Path


TOOL = Path(__file__).resolve().parents[1] / "bench/summarize-benchmark-pairs.py"
SPEC = importlib.util.spec_from_file_location("summary_tool", TOOL)
summary_tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(summary_tool)


def sample(
    variant,
    case,
    output,
    pair=1,
    argv=None,
    audit=(
        "APK_COUNTER_STATE_1_2\n"
        "collection_enabled_before_snapshot: 0\n"
        "APK_COUNTER_STATE_END_1_2\n"
    ),
):
    return {
        "variant": variant,
        "pair": pair,
        "case": case,
        "output": output,
        "argv": argv,
        "exit_code": 0,
        "counter_audit_output": audit,
        "collection_enabled_after_case": 0,
        "wall_seconds": 999999,
    }


class BenchmarkPairsSummaryTest(unittest.TestCase):
    def test_latency_ratio_is_not_inverted(self):
        rows = [
            sample(
                "control",
                "getpid",
                "MICRO mode=getpid n=1 mb=0 elapsed_ns=100 "
                "ns_per_op=100.00 fork_ns=0 wait_ns=0",
            ),
            sample(
                "candidate",
                "getpid",
                "MICRO mode=getpid n=1 mb=0 elapsed_ns=50 "
                "ns_per_op=50.00 fork_ns=0 wait_ns=0",
            ),
        ]
        result = summary_tool.summarize(rows, "noisy")["case_results"][0]["pairs"][0]
        self.assertEqual(result["candidate_over_control"], 0.5)
        self.assertEqual(result["latency_decrease_percent"], 50.0)
        self.assertNotIn("throughput_increase_percent", result)

    def test_throughput_ratio(self):
        rows = [
            sample("control", "cpu-1", "events per second: 100.00\n"),
            sample("candidate", "cpu-1", "events per second: 200.00\n"),
        ]
        result = summary_tool.summarize(rows, "clean")["case_results"][0]["pairs"][0]
        self.assertEqual(result["throughput_increase_percent"], 100.0)

    def test_sha256_extracts_configured_bytes_per_second(self):
        argv = [
            "openssl", "speed", "-elapsed", "-seconds", "5",
            "-bytes", "16384", "-evp", "sha256", "-mr",
        ]
        control_output = (
            "+DT:sha256:5:16384\n"
            "+R:102304:sha256:5.000000\n"
            "+H:16384\n"
            "+F:25:sha256:335229747.20\n"
        )
        candidate_output = control_output.replace(
            "335229747.20", "670459494.40"
        )
        rows = [
            sample("control", "sha256", control_output, argv=argv),
            sample("candidate", "sha256", candidate_output, argv=argv),
        ]
        result = summary_tool.summarize(rows, "noisy")["case_results"][0]["pairs"][0]
        self.assertEqual(result["unit"], "bytes/s")
        self.assertEqual(result["control"], 335229747.20)
        self.assertEqual(result["throughput_increase_percent"], 100.0)

    def test_futex_mismatch_is_latency(self):
        rows = [
            sample(
                "control", "futex-mismatch",
                "MICRO mode=futex-mismatch n=300000 mb=0 "
                "elapsed_ns=1000 ns_per_op=100.00 fork_ns=0 wait_ns=0",
            ),
            sample(
                "candidate", "futex-mismatch",
                "MICRO mode=futex-mismatch n=300000 mb=0 "
                "elapsed_ns=500 ns_per_op=50.00 fork_ns=0 wait_ns=0",
            ),
        ]
        result = summary_tool.summarize(rows, "clean")["case_results"][0]["pairs"][0]
        self.assertEqual(result["latency_decrease_percent"], 50.0)

    def test_dense_fork_accepts_real_diagnostic_suffix_fields(self):
        row = (
            "MICRO mode=fork-touched n=80 mb=64 elapsed_ns=1179206992 "
            "ns_per_op=14740087.40 fork_ns=684180841 wait_ns=494977724"
        )
        rows = [
            sample("control", "dense-fork", row),
            sample("candidate", "dense-fork", row),
        ]
        result = summary_tool.summarize(rows, "noisy")["case_results"][0]["pairs"][0]
        self.assertEqual(result["control"], 14740087.40)

    def test_micro_rejects_duplicate_metric_rows(self):
        line = (
            "MICRO mode=getpid n=2 mb=0 elapsed_ns=20 "
            "ns_per_op=10.0 fork_ns=0 wait_ns=0"
        )
        rows = [
            sample("control", "getpid", line + "\n" + line),
            sample("candidate", "getpid", line),
        ]
        with self.assertRaises(ValueError):
            summary_tool.summarize(rows, "clean")

    def test_fileio_ignores_verify_read_and_checks_bytes(self):
        def output(rate, verified_bytes=67108864):
            return (
                "FILEIO_RUN_BEGIN\n"
                "Throughput:\n"
                f"    written, MiB/s: {rate}\n"
                "FILEIO_RUN_END\n"
                "FILEIO_VERIFY_BEGIN\n"
                "Throughput:\n"
                "    read, MiB/s: 99999.0\n"
                "FILEIO_VERIFY_END\n"
                f"FILEIO: PASS verified_bytes={verified_bytes} cleaned=1\n"
            )

        rows = [
            sample("control", "fileio-100", output(10)),
            sample("candidate", "fileio-100", output(20)),
        ]
        result = summary_tool.summarize(rows, "unclassified")["case_results"][0]["pairs"][0]
        self.assertEqual(result["candidate"], 20.0)
        self.assertEqual(result["verified_bytes"], 67108864)
        self.assertEqual(result["throughput_increase_percent"], 100.0)

        rows[1]["output"] = output(20, verified_bytes=123)
        with self.assertRaises(ValueError):
            summary_tool.summarize(rows, "clean")

    def test_fileio_rejects_reversed_or_missing_markers(self):
        bad_output = (
            "FILEIO_RUN_END\nFILEIO_RUN_BEGIN\n"
            "written, MiB/s: 2\n"
            "FILEIO: PASS verified_bytes=4 cleaned=1\n"
        )
        rows = [
            sample("control", "fileio-100", bad_output),
            sample("candidate", "fileio-100", bad_output),
        ]
        with self.assertRaises(ValueError):
            summary_tool.summarize(rows, "clean")

    def test_threads_uses_total_events_over_total_time(self):
        def output(events):
            return (
                "total time: 2.0s\n"
                f"    total number of events: {events}\n"
            )

        rows = [
            sample("control", "threads-2", output(100)),
            sample("candidate", "threads-2", output(200)),
        ]
        result = summary_tool.summarize(rows, "clean")["case_results"][0]["pairs"][0]
        self.assertEqual(result["control"], 50.0)
        self.assertEqual(result["candidate"], 100.0)

    def test_fork_uses_explicit_fork_real_time_column(self):
        control = (
            "stress-ng: metrc: [1] fork 10 5.00 0.10 0.20 2.00 1.00\n"
        )
        candidate = control.replace("2.00 1.00", "4.00 3.00")
        rows = [
            sample("control", "fork", control),
            sample("candidate", "fork", candidate),
        ]
        result = summary_tool.summarize(rows, "noisy")["case_results"][0]["pairs"][0]
        self.assertEqual(result["control"], 2.0)
        self.assertEqual(result["candidate"], 4.0)

    def test_fork_rejects_unrelated_metrics_row(self):
        output = "stress-ng: metrc: [1] getpid 10 5.0 0.1 0.2 2.0 1.0\n"
        rows = [
            sample("control", "fork", output),
            sample("candidate", "fork", output),
        ]
        with self.assertRaises(ValueError):
            summary_tool.summarize(rows, "clean")

    def test_counter_audit_pairs_case_index_not_enable_state(self):
        rows = [
            sample(
                "control",
                "cpu-1",
                "events per second: 1\n",
                audit=(
                    "APK_COUNTER_STATE_1_2\n"
                    "collection_enabled_before_snapshot: 0\n"
                    "APK_COUNTER_STATE_END_1_2\n"
                ),
            ),
            sample(
                "candidate",
                "cpu-1",
                "events per second: 2\n",
                audit=(
                    "APK_COUNTER_STATE_1_2\n"
                    "collection_enabled_before_snapshot: 0\n"
                    "APK_COUNTER_STATE_END_1_2\n"
                ),
            ),
        ]
        summary_tool.summarize(rows, "clean")

    def test_audit_rejects_mismatched_marker_order(self):
        bad_audit = (
            "APK_COUNTER_STATE_1_2\n"
            "collection_enabled_before_snapshot: 0\n"
            "APK_COUNTER_STATE_END_1_3\n"
        )
        rows = [
            sample("control", "cpu-1", "events per second: 1\n", audit=bad_audit),
            sample("candidate", "cpu-1", "events per second: 2\n", audit=bad_audit),
        ]
        with self.assertRaises(ValueError):
            summary_tool.summarize(rows, "clean")

    def test_rejects_missing_audit_duplicate_side_and_bad_rc(self):
        control = sample("control", "cpu-1", "events per second: 1")
        candidate = sample("candidate", "cpu-1", "events per second: 2")

        missing_audit = dict(control)
        del missing_audit["counter_audit_output"]
        with self.assertRaises(ValueError):
            summary_tool.summarize([missing_audit, candidate], "clean")

        with self.assertRaises(ValueError):
            summary_tool.summarize([control, control, candidate], "clean")

        bad_rc = dict(control)
        bad_rc["exit_code"] = False
        with self.assertRaises(ValueError):
            summary_tool.summarize([bad_rc, candidate], "clean")

    def test_rejects_missing_metric_side_and_inconsistent_pair_sets(self):
        control = sample("control", "cpu-1", "missing metric")
        candidate = sample("candidate", "cpu-1", "events per second: 2")
        with self.assertRaises(ValueError):
            summary_tool.summarize([control, candidate], "clean")

        with self.assertRaises(ValueError):
            summary_tool.summarize(
                [sample("control", "cpu-1", "events per second: 1")],
                "clean",
            )

        rows = [
            sample("control", "cpu-1", "events per second: 1", pair=1),
            sample("candidate", "cpu-1", "events per second: 2", pair=1),
            sample("control", "cpu-2", "events per second: 1", pair=2),
            sample("candidate", "cpu-2", "events per second: 2", pair=2),
        ]
        with self.assertRaises(ValueError):
            summary_tool.summarize(rows, "clean")


if __name__ == "__main__":
    unittest.main()
