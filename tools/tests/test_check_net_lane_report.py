import tempfile
import unittest
from pathlib import Path

from tools.check_net_lane_report import read_report


class NetLaneReportTests(unittest.TestCase):
    def read(self, text, lanes):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "qemu.log"
            path.write_text(text, encoding="utf-8")
            return read_report(path, lanes)

    def test_four_lane_report_requires_both_counters_and_columns(self):
        report = """memp lane         alloc   freed
0                  311     296
1                    2       2
2                    0       1
3                    1       1
rx lane            rx   drop
0                    0      0
1                    1      2
2                    0      0
3                    0      1
rx staged (not yet processed): 0
"""
        self.read(report, 4)

    def test_literal_percent_format_row_is_rejected(self):
        report = """memp lane         alloc   freed
0 1 1
1 1 1
2 1 1
3 1 1
rx lane            rx   drop
%-15lu 0 0
1 0 0
2 0 0
3 0 0
rx staged (not yet processed): 0
"""
        with self.assertRaisesRegex(ValueError, "expected lane rows"):
            self.read(report, 4)

    def test_one_lane_report_has_no_per_lane_blocks(self):
        self.read("PBUF_POOL 10 0 0 0\nTCP_SEG 8 0 0 0\n", 1)


if __name__ == "__main__":
    unittest.main()
