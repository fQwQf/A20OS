"""Production IVRS validation and typed views on unaligned, hostile fixtures."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class AcpiIvrs(unittest.TestCase):
    def test_whole_table_validation_and_views(self):
        with tempfile.TemporaryDirectory(prefix="a20-ivrs-") as folder:
            work = Path(folder)
            binary = work / "test"
            subprocess.run([
                "cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I" + str(ROOT / "kernel/include"),
                str(ROOT / "tools/tests/acpi_ivrs_host.c"),
                str(ROOT / "kernel/firmware/acpi/ivrs.c"),
                str(ROOT / "kernel/firmware/acpi/tables.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
