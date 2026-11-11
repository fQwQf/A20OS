"""Production claim registry, managed mappings and teardown with real threads."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
if __package__:
    from .test_firmware_model import ROOT, STUBS
    from .test_driver_workqueue import LOCK_STUB
else:
    from test_firmware_model import ROOT, STUBS
    from test_driver_workqueue import LOCK_STUB


class DriverResources(unittest.TestCase):
    def test_claims_mapping_rollback_and_lifetime(self):
        with tempfile.TemporaryDirectory(prefix="a20-resources-") as folder:
            work = Path(folder)
            stubs = {**STUBS, "core/lock.h": LOCK_STUB,
                     "net/socket/socket_internal.h":
                     '#include "core/types.h"\n'
                     'void netlink_uevent_emit(const char *, const char *, const char *, uint64_t);\n'}
            for name, content in stubs.items():
                path = work / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            binary = work / "test"
            subprocess.run([
                "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-DCONFIG_NR_CPUS=2", "-I" + str(work),
                "-I" + str(ROOT / "kernel/include"), "-I" + str(ROOT / "kernel"),
                str(ROOT / "tools/tests/driver_resource_host.c"),
                str(ROOT / "kernel/drivers/core/driver_core.c"),
                str(ROOT / "kernel/drivers/core/driver_class.c"),
                str(ROOT / "kernel/firmware/node.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=15,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})


if __name__ == "__main__":
    unittest.main()
