"""Execute production, architecture-independent ACPI readers on host fixtures."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

@unittest.skipUnless(shutil.which("cc"), "host C compiler required")
class AcpiTablesTests(unittest.TestCase):
    def test_production_readers(self):
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / "acpi-test"
            subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", "-fno-sanitize-recover=all",
                            "-I" + str(ROOT / "kernel/include"),
                            str(ROOT / "tools/tests/test_acpi_tables.c"),
                            str(ROOT / "kernel/firmware/acpi/tables.c"),
                            str(ROOT / "kernel/firmware/acpi/topology.c"),
                            "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], check=True, text=True, capture_output=True)
            self.assertIn("fixtures PASS", result.stdout)

if __name__ == "__main__":
    unittest.main()
