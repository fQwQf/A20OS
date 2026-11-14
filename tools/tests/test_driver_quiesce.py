"""Offline -> quiesce -> drain -> remove -> devres using production C."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

if __package__:
    from .test_firmware_model import ROOT, STUBS
else:
    from test_firmware_model import ROOT, STUBS


class DriverQuiesce(unittest.TestCase):
    def test_pending_io_and_publish_failure(self):
        with tempfile.TemporaryDirectory(prefix="a20-quiesce-") as temp:
            work = Path(temp)
            stubs = {**STUBS, "net/socket/socket_internal.h":
                     '#include "core/types.h"\n'
                     'void netlink_uevent_emit(const char *, const char *, '
                     'const char *, uint64_t);\n'}
            for name, content in stubs.items():
                path = work / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            binary = work / "test"
            subprocess.run([
                "cc", "-std=gnu99", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-DCONFIG_NR_CPUS=2", "-DCONFIG_DRIVER_LIFECYCLE_TEST", "-I" + str(work),
                "-I" + str(ROOT / "kernel/include"), "-I" + str(ROOT / "kernel"),
                str(ROOT / "tools/tests/driver_quiesce_host.c"),
                str(ROOT / "kernel/drivers/core/driver_core.c"),
                str(ROOT / "kernel/drivers/core/driver_class.c"), str(ROOT / "kernel/drivers/core/driver_protocol.c"), "-o", str(binary),
                str(ROOT / "kernel/firmware/node.c"),
                str(ROOT / "kernel/drivers/core/driver_registry_test.c"),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})


if __name__ == "__main__":
    unittest.main()
