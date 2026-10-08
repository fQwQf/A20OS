import importlib.util
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[1] / "check_net_lane_parallel_report.py"
SPEC = importlib.util.spec_from_file_location("check_net_lane_parallel_report", MODULE_PATH)
assert SPEC and SPEC.loader
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)


class NetLaneParallelReportTests(unittest.TestCase):
    def test_accepts_real_parallel_four_lane_row(self):
        self.assertEqual(
            report.parse_core_parallel(
                "core_parallel: peak_active_lanes=3 probe_hits=64 "
                "inputs: 23 18 21 17 timers: 8 8 8 8\n"),
            (3, 64, [23, 18, 21, 17], [8, 8, 8, 8]))

    def test_rejects_single_lane_peak(self):
        with self.assertRaisesRegex(ValueError, "concurrently"):
            report.parse_core_parallel(
                "core_parallel: peak_active_lanes=1 probe_hits=64 "
                "inputs: 23 18 21 17 timers: 8 8 8 8\n")

    def test_rejects_missing_lane_input(self):
        with self.assertRaisesRegex(ValueError, "all four lanes"):
            report.parse_core_parallel(
                "core_parallel: peak_active_lanes=2 probe_hits=64 "
                "inputs: 23 18 0 17 timers: 8 8 8 8\n")

    def test_rejects_missing_lane_timer(self):
        with self.assertRaisesRegex(ValueError, "all four lanes"):
            report.parse_core_parallel(
                "core_parallel: peak_active_lanes=2 probe_hits=64 "
                "inputs: 23 18 21 17 timers: 8 0 8 8\n")


if __name__ == "__main__":
    unittest.main()
