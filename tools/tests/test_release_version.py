"""Release publication must reject stale kernel identity and mismatched tags."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


CHECKER = Path(__file__).resolve().parents[1] / "check-release-version.py"


class ReleaseVersionTests(unittest.TestCase):
    def run_check(self, version="0.17", release="20.0.17", tag=None):
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / "version.h"
            header.write_text(
                f'#define A20OS_VERSION "{version}"\n'
                f'#define A20OS_RELEASE "{release}"\n'
            )
            command = [sys.executable, str(CHECKER), "--header", str(header)]
            if tag is not None:
                command.append(tag)
            return subprocess.run(command, capture_output=True, text=True)

    def test_matching_tag(self):
        self.assertEqual(self.run_check(tag="v0.17").returncode, 0)

    def test_development_checkout_without_tag(self):
        self.assertEqual(self.run_check().returncode, 0)

    def test_stale_linux_release(self):
        result = self.run_check(release="20.0.13", tag="v0.17")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("A20OS_RELEASE must be", result.stderr)

    def test_tag_does_not_match_header(self):
        result = self.run_check(tag="v0.18")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match kernel version", result.stderr)

    def test_invalid_version(self):
        self.assertNotEqual(self.run_check(version="release-candidate").returncode, 0)

    def test_missing_header(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                [sys.executable, str(CHECKER), "--header", str(Path(directory) / "missing.h")],
                capture_output=True, text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("release-version: FAIL", result.stderr)


if __name__ == "__main__":
    unittest.main()
