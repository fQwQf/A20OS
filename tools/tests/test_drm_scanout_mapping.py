"""Production scanout mapper and actual compact-PFN helpers; GOP is not RAM."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DrmScanoutMapping(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="a20-drm-mapping-")
        cls.binary = Path(cls.tmp.name) / "mapping"
        subprocess.run([
            "cc", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-builtin",
            "-fno-pie", "-no-pie", "-fsanitize=address,undefined",
            "-DCONFIG_X86_64", "-DCONFIG_64BIT", "-DCONFIG_NR_CPUS=1",
            "-I", str(ROOT / "kernel/arch/x86_64/include"),
            "-I", str(ROOT / "kernel/include"), "-I", str(ROOT / "kernel"),
            str(ROOT / "tools/tests/drm_scanout_mapping_host.c"),
            "-o", str(cls.binary),
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

    def test_reserved_gop_with_no_pfa_frames_can_present(self):
        self.run_case(0)

    def test_allocated_gpu_ram_uses_compact_pfn_mapping(self):
        self.run_case(1)

    def test_non_ram_vram_pairs_device_map_and_unmap(self):
        self.run_case(2)

    def test_overflow_and_spans_crossing_ram_rejected_before_mapping(self):
        self.run_case(3)


if __name__ == "__main__":
    unittest.main()
