"""Storage ownership against production USB/core; controller and locks modeled."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest
from .test_native_bus_lifetime import ROOT, C_FIXTURE


class UsbStorageLifetime(unittest.TestCase):
    def test_partial_probe_active_io_and_last_consumer_release(self):
        machine = platform.machine().lower()
        if machine not in ("x86_64", "amd64"):
            self.skipTest("fixture uses x86 hardware headers")
        model = (ROOT / "tools/tests/usb_storage_lifetime/fixture.c").read_text()
        marker = "int main(int argc,char **argv) {"
        source = C_FIXTURE.replace(marker, model + "\n" + marker)
        source = source.replace('if(!strcmp(argv[1],"pci-retired"))',
                                'if(!strncmp(argv[1],"storage-",8))storage_case(argv[1]+8);\n'
                                ' else if(!strcmp(argv[1],"pci-retired"))')
        with tempfile.TemporaryDirectory(prefix="a20-storage-life-") as directory:
            fixture = Path(directory) / "fixture.c"
            fixture.write_text(source)
            binary = Path(directory) / "test"
            result = subprocess.run([
                "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
                "-fno-builtin", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-DCONFIG_X86_64", "-DCONFIG_NR_CPUS=1", "-DCONFIG_DRIVER_LIFECYCLE_TEST",
                "-I" + str(ROOT / "kernel/arch/x86_64/include"),
                "-I" + str(ROOT / "kernel/include"), "-I" + str(ROOT / "kernel"),
                str(fixture), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for case in ("partial", "active", "reuse"):
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), "storage-" + case],
                                            capture_output=True, text=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
