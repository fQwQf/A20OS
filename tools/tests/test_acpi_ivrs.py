"""Production IVRS validation and typed views on unaligned, hostile fixtures."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class AcpiIvrs(unittest.TestCase):
    def run_fixture(self, name):
        with tempfile.TemporaryDirectory(prefix="a20-ivrs-") as folder:
            work = Path(folder)
            binary = work / "test"
            subprocess.run([
                "cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I" + str(ROOT / "kernel/include"),
                str(ROOT / ("tools/tests/" + name + ".c")),
                str(ROOT / "kernel/firmware/acpi/ivrs.c"),
                str(ROOT / "kernel/firmware/acpi/ivrs_policy.c"),
                str(ROOT / "kernel/firmware/acpi/tables.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_whole_table_validation_and_views(self):
        self.run_fixture("acpi_ivrs_host")

    def test_routing_candidates_and_memory_constraints(self):
        self.run_fixture("acpi_ivrs_policy_host")

    def test_complete_requester_sets_and_shared_constraints(self):
        self.run_fixture("acpi_ivrs_requesters_host")


if __name__ == "__main__":
    unittest.main()
