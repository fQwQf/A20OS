"""Hub admission closes before a failed child or HCD cleanup can retain it."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest
from .test_native_bus_lifetime import ROOT, C_FIXTURE


class UsbHubPrepare(unittest.TestCase):
    def test_child_and_upstream_failures_keep_closed_hub_and_urb_until_retry(self):
        if platform.machine().lower() not in ("x86_64", "amd64"):
            self.skipTest("fixture uses x86 architecture headers")
        model = (ROOT / "tools/tests/usb_hub_prepare/fixture.c").read_text()
        marker = "int main(int argc,char **argv) {"
        source = C_FIXTURE.replace(marker, model + "\n" + marker)
        source = source.replace('if(!strcmp(argv[1],"pci-retired"))',
                                'if(!strcmp(argv[1],"hub-prepare"))hub_prepare_case();\n'
                                ' else if(!strcmp(argv[1],"pci-retired"))')
        with tempfile.TemporaryDirectory(prefix="a20-hub-prepare-") as directory:
            path = Path(directory) / "fixture.c"
            path.write_text(source)
            binary = Path(directory) / "test"
            result = subprocess.run([
                "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
                "-fno-builtin", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-DCONFIG_X86_64", "-DCONFIG_NR_CPUS=1", "-DCONFIG_DRIVER_LIFECYCLE_TEST",
                "-I" + str(ROOT / "kernel/arch/x86_64/include"),
                "-I" + str(ROOT / "kernel/include"), "-I" + str(ROOT / "kernel"),
                str(path), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary), "hub-prepare"], capture_output=True,
                                    text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
