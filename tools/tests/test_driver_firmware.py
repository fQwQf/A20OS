"""Production firmware loader/VFS adapter: bounded copies and owned streams."""
from pathlib import Path
import subprocess
import tempfile
import unittest

if __package__:
    from .test_firmware_model import ROOT, STUBS
else:
    from test_firmware_model import ROOT, STUBS


class DriverFirmware(unittest.TestCase):
    def test_images_providers_and_anchored_vfs(self):
        with tempfile.TemporaryDirectory(prefix="a20-driver-firmware-") as temp:
            work = Path(temp)
            stubs = {**STUBS, "arch/fcntl.h": """#pragma once
#define ARCH_O_DIRECTORY 0x10000
#define ARCH_O_NOFOLLOW 0x20000
#define ARCH_O_DIRECT 0x4000
#define ARCH_O_LARGEFILE 0x8000
"""}
            for name, content in stubs.items():
                path = work / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            binary = work / "test"
            subprocess.run([
                "cc", "-std=gnu99", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I" + str(work), "-I" + str(ROOT / "kernel/include"),
                "-I" + str(ROOT / "kernel"),
                str(ROOT / "tools/tests/driver_firmware_host.c"),
                str(ROOT / "kernel/drivers/core/firmware.c"),
                str(ROOT / "kernel/drivers/core/firmware_vfs.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
