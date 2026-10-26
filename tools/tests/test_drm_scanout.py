"""Production KMS-to-retained-scanout conversion, with address/UB sanitizers."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DrmScanout(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="a20-drm-scanout-")
        cls.binary = Path(cls.tmp.name) / "scanout"
        subprocess.run([
            "cc", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-fno-pie", "-no-pie", "-fsanitize=address,undefined",
            "-I", str(ROOT / "kernel/include"),
            str(ROOT / "tools/tests/drm_scanout_host.c"), "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, case):
        result = subprocess.run([str(self.binary), str(case)], text=True,
                                capture_output=True,
                                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS", result.stdout)

    def test_padded_formats_rgb_bgr_565_555_24bit_10bit(self):
        self.run_case(0)

    def test_chunk_and_sparse_vmo_page_boundaries(self):
        self.run_case(1)

    def test_clipping_preserves_untouched_pixels(self):
        self.run_case(2)

    def test_bad_geometry_and_masks_cannot_touch_scanout(self):
        self.run_case(3)


if __name__ == "__main__":
    unittest.main()
