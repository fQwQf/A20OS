"""Execute the production AMD-Vi probe with bounded read-only transports."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class AmdIommuProbe(unittest.TestCase):
    def test_pci_mmio_snapshots_and_failures(self):
        with tempfile.TemporaryDirectory(prefix="a20-amdvi-probe-") as folder:
            binary = Path(folder) / "test"
            subprocess.run([
                "cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I" + str(ROOT / "kernel/include"),
                str(ROOT / "tools/tests/amd_iommu_probe_host.c"),
                str(ROOT / "kernel/drivers/core/amd_iommu_probe.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
