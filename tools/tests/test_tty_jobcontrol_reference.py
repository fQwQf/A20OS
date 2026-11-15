"""Keep the guest's independent terminal expectations checked against Linux."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux terminal reference")
class TtyJobcontrolReference(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix="a20-tty-reference-")
        cls.binary = Path(cls.work.name) / "test"
        subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "user/cmds/core/tty_jobcontrol_test.c"),
                        "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def test_pair_identities_streams_lock_and_errno(self):
        result = subprocess.run([str(self.binary), "--identity"], timeout=15,
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("TTY_JOBCTRL: identity PASS", result.stdout)

    def test_ownership_background_access_and_detach(self):
        result = subprocess.run([str(self.binary)], timeout=15,
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for section in ("ownership", "background-read", "background-write", "detach"):
            self.assertIn("TTY_JOBCTRL: " + section + " PASS", result.stdout)
        self.assertIn("TTY_JOBCTRL: PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
