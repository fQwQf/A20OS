"""Host-side tests for tools/a20log, the persistent-log reader.

These do not need a disk image or mtools: they exercise the same directory
reader that a mounted FAT32 partition presents to the tool.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "a20log"


class A20LogTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="a20log-test-")
        self.root = Path(self.temp.name)
        (self.root / "CURRENT").write_text(
            "boot=abc123\nseq=3\nbytes=100\nstate=clean\n",
            encoding="utf-8",
        )
        records = [
            {"v": 1, "boot": "abc123", "seq": 0, "mono_ns": 1000,
             "rt": 0, "lvl": "info", "src": "INIT", "msg": "[INIT] System ready\n"},
            {"v": 1, "boot": "abc123", "seq": 1, "mono_ns": 2000,
             "rt": 0, "lvl": "warn", "src": "NVME",
             "msg": "[WARN] [NVME] reset timed out\n"},
            {"v": 1, "boot": "abc123", "seq": 2, "mono_ns": 3000,
             "rt": 0, "lvl": "err", "src": "I2C-DW",
             "msg": "[ERR] [I2C-DW] address NAK\n"},
        ]
        body = "\n".join(json.dumps(r) for r in records) + "\n"
        # A power cut can leave a half-written last record.
        body += '{"v":1,"boot":"abc123","seq":3'
        (self.root / "boot-abc123.jsonl").write_text(body, encoding="utf-8")
        early = [
            {"v": 1, "boot": "loader-7", "bootcount": 7, "seq": 0,
             "mono_ns": 0, "rt": 0, "lvl": "info", "src": "UEFI",
             "msg": "STAGE loader"},
            {"v": 1, "boot": "loader-7", "bootcount": 7, "seq": 1,
             "mono_ns": 0, "rt": 0, "lvl": "err", "src": "UEFI",
             "msg": "FAIL stage=ExitBootServices status=0x1"},
        ]
        (self.root / "early-7.jsonl").write_text(
            "\n".join(json.dumps(r) for r in early) + "\n", encoding="utf-8")

    def tearDown(self):
        self.temp.cleanup()

    def run_tool(self, *args):
        return subprocess.run(
            [sys.executable, str(TOOL), *args, "--dir", str(self.root)],
            capture_output=True, text=True, check=False,
        )

    def test_list_reports_current_state(self):
        result = self.run_tool("list")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("boot-abc123.jsonl", result.stdout)
        self.assertIn("state=clean", result.stdout)
        self.assertIn("early-7.jsonl", result.stdout)

    def test_show_filters_and_skips_torn_line(self):
        result = self.run_tool("show", "--level", "warn")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("NVME", result.stdout)
        self.assertIn("I2C-DW", result.stdout)
        self.assertNotIn("[INIT] System ready", result.stdout)
        self.assertIn("torn record skipped", result.stderr)

    def test_show_loader_early_log(self):
        result = self.run_tool("show", "--boot", "loader")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("STAGE loader", result.stdout)
        self.assertIn("FAIL stage=ExitBootServices", result.stdout)

    def test_show_json_is_parseable(self):
        result = self.run_tool("show", "--json", "--src", "I2C-DW")
        self.assertEqual(result.returncode, 0, result.stderr)
        line = result.stdout.strip()
        self.assertEqual(json.loads(line)["src"], "I2C-DW")

    def test_summary_counts_levels(self):
        result = self.run_tool("summary")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("records=3", result.stdout)
        self.assertIn("warn=1", result.stdout)
        self.assertIn("err=1", result.stdout)
        self.assertIn("early-7.jsonl", result.stdout)


if __name__ == "__main__":
    unittest.main()
