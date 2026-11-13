"""Exercise callback lifetimes in the POSIX rtw89 platform runtime."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Rtw89RuntimeTest(unittest.TestCase):
    def test_callback_lifetimes(self):
        with tempfile.TemporaryDirectory(prefix="a20-rtw89-runtime-") as temporary:
            binary = Path(temporary) / "runtime"
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-pthread", str(ROOT / "tools/tests/rtw89_runtime_host.c"),
                            "-o", str(binary)], check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True,
                                    text=True, timeout=10)
            self.assertIn("drain/free and recursive ownership PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
