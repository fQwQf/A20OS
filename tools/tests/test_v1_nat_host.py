from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path

from tools.release.v1_nat_host import HeaderClosureError, _verify_header_closure


ROOT = Path(__file__).resolve().parents[2]


class FixedC0AdapterTests(unittest.TestCase):
    def test_header_drift_is_rejected(self) -> None:
        def historical(_revision: str, _path: str) -> bytes:
            return b"historical header\n"

        with self.assertRaisesRegex(HeaderClosureError, "header drift"):
            _verify_header_closure(
                "0" * 40,
                ["kernel/include/net/netfilter.h"],
                git_file=historical,
            )

    def test_matching_historical_header_is_recorded(self) -> None:
        current = (ROOT / "kernel/include/net/netfilter.h").read_bytes()

        def historical(_revision: str, path: str) -> bytes:
            self.assertEqual(path, "kernel/include/net/netfilter.h")
            return current

        records = _verify_header_closure(
            "0" * 40,
            ["kernel/include/net/netfilter.h"],
            git_file=historical,
        )
        self.assertEqual(records[0]["sha256"], records[0]["revision_sha256"])

    def test_revision_argument_is_required(self) -> None:
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/release/v1_nat_host.py")],
            cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"--revision", result.stderr)


if __name__ == "__main__":
    unittest.main()
