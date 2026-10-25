"""Environment errors must be actionable and produce a failing status."""
import contextlib
import io
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_quickstart as check


class QuickstartEnvironmentTest(unittest.TestCase):
    def test_missing_tools_are_reported_together(self):
        output = io.StringIO()
        with patch.object(check, "find_tool", return_value=None), contextlib.redirect_stdout(output):
            self.assertEqual(check.main(), 1)
        self.assertIn("gcc-riscv64-unknown-elf", output.getvalue())
        self.assertIn("rustup", output.getvalue())
        self.assertIn("quickstart prerequisites: FAIL", output.getvalue())

    def test_compiler_error_is_not_hidden(self):
        output = io.StringIO()
        result = subprocess.CompletedProcess([], 1, "", "can't find crate for core")
        with patch.object(check.subprocess, "run", return_value=result), contextlib.redirect_stdout(output):
            self.assertFalse(check.probe("core", ["rustc"], "rustup target add TARGET"))
        self.assertIn("can't find crate for core", output.getvalue())
        self.assertIn("rustup target add TARGET", output.getvalue())

    def test_wrong_version_fails_even_with_successful_command(self):
        result = subprocess.CompletedProcess([], 0, "rustc 1.85.0 (test)\n", "")
        with patch.object(check.subprocess, "run", return_value=result), contextlib.redirect_stdout(io.StringIO()):
            self.assertFalse(check.probe("version", ["rustc"], "fix PATH", "rustc 1.96.0 "))


if __name__ == "__main__":
    unittest.main()
