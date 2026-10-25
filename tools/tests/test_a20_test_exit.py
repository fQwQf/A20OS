"""A printed marker must not hide a hung or failed guest process."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import a20_test
from a20_instance import parse_instance


class GuestExitTest(unittest.TestCase):
    def run_guest(self, body, strict=True):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            manifest = root / "test.toml"
            manifest.write_text('name = "exit-test"\narch = "riscv64"\n'
                                '[test]\nexpect = ["MARKER"]\ntimeout = "0.5s"\n'
                                + ('require_clean_exit = true\n' if strict else ''))
            inst = parse_instance(manifest)
            with patch.object(a20_test, "SMOKE_LOG_DIR", root), \
                 patch.object(a20_test, "build_instance"), \
                 patch.object(a20_test, "preflight"), \
                 patch.object(a20_test, "_feed_commands"), \
                 patch.object(a20_test, "_qemu_cmdline", return_value=[sys.executable, "-c", body]), \
                 contextlib.redirect_stdout(io.StringIO()):
                return a20_test.run_test(inst, [], False)

    def test_clean_exit_with_marker(self):
        self.assertEqual(self.run_guest('print("MARKER", flush=True)'), 0)

    def test_nonzero_exit_after_marker_fails(self):
        self.assertNotEqual(self.run_guest('print("MARKER", flush=True); raise SystemExit(3)'), 0)

    def test_timeout_after_marker_fails(self):
        self.assertNotEqual(self.run_guest('import time; print("MARKER", flush=True); time.sleep(30)'), 0)

    def test_clean_exit_without_marker_fails(self):
        self.assertNotEqual(self.run_guest('pass'), 0)

    def test_legacy_marker_only_contract_is_preserved(self):
        self.assertEqual(self.run_guest('print("MARKER", flush=True); raise SystemExit(3)', strict=False), 0)


if __name__ == "__main__":
    unittest.main()
