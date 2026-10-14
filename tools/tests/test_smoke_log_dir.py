import unittest
from pathlib import Path

from tools import smoke


class SmokeLogDirectoryTests(unittest.TestCase):
    def test_default_keeps_configured_log_path(self):
        configured = ".kernel-build/smoke/native-ipc-riscv64.log"
        self.assertEqual(smoke.resolve_log_path(configured), configured)

    def test_absolute_log_dir_uses_unique_case_basename(self):
        configured = ".kernel-build/smoke/native-ipc-riscv64.log"
        directory = "/tmp/a20-smoke-run-unique"
        self.assertEqual(
            smoke.resolve_log_path(configured, directory),
            str(Path(directory) / "native-ipc-riscv64.log"),
        )

    def test_relative_log_dir_is_repo_relative(self):
        configured = ".kernel-build/smoke/native-ipc-riscv64.log"
        self.assertEqual(
            smoke.resolve_log_path(configured, "build/evidence/run-01"),
            str(smoke.REPO / "build/evidence/run-01/native-ipc-riscv64.log"),
        )


if __name__ == "__main__":
    unittest.main()
