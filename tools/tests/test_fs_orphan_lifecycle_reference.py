"""Exercise the guest's inode/open-unlink expectations on a Linux filesystem."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class OrphanLifecycleReference(unittest.TestCase):
    def test_hardlink_open_unlink_fork_and_last_close(self):
        with tempfile.TemporaryDirectory(prefix="a20-orphan-reference-") as directory:
            work = Path(directory)
            binary = work / "test"
            subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "user/cmds/stress/fs_orphan_lifecycle.c"),
                            "-o", str(binary)], check=True)
            before = set(work.iterdir())
            result = subprocess.run([str(binary), str(work)], timeout=20,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("FS_ORPHAN: PASS old_ino=", result.stdout)
            self.assertEqual(set(work.iterdir()), before)


if __name__ == "__main__":
    unittest.main()
